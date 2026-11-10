import math
import traceback

import bpy
import numpy as np
from mathutils import Matrix, Quaternion

from . import instancing
from . import material_program
from . import scene_generated as fb
from . import space
from .node_groups import REVERSE_CULLING, NodeWriter, Value
from .product import NAME
from .session import text

KEY = "unrealkit.key"
#! foreach_set takes the values of CONSTANT and LINEAR in Blender's enum.
CONSTANT, LINEAR = 0, 1
LOOP_TOLERANCE = 1e-3
LIGHT_TYPES = {fb.LightType.Point: "POINT", fb.LightType.Spot: "SPOT", fb.LightType.Rect: "AREA",
               fb.LightType.Directional: "SUN"}
HIDDEN_PATHS = ("hide_viewport", "hide_render")
LIGHT_RAY_PATHS = ("visible_shadow", "visible_diffuse", "visible_glossy", "visible_transmission", "visible_volume_scatter")
OBJECT_PROPERTIES = ("location", "rotation", "scale", "visibility", "field_of_view")
BONES = "bones/"
BONE_ATTRIBUTES = ("location", "rotation", "scale")
MATERIALS = "materials/"
MAX_SKINNED_UV_SETS = 4
VIEWER = "unrealkit.viewer"
VIEWER_VISIBILITY = "unrealkit.viewer_visibility"
VIEWER_VISIBILITIES = {fb.ViewerVisibility.OnlyViewer: "only_viewer", fb.ViewerVisibility.NotViewer: "not_viewer"}


class DatablockIndex:

    def __init__(self, collection):
        self.collection = collection
        self.by_key = {}
        for datablock in collection:
            key = datablock.get(KEY)
            if key is not None:
                self.by_key.setdefault(key, datablock)

    def get(self, key):
        return self.by_key.get(key)

    def add(self, key, datablock):
        datablock[KEY] = key
        self.by_key[key] = datablock
        return datablock


def keys(record):
    if record is None or record.FramesLength() == 0:
        return None
    frames = record.FramesAsNumpy().astype(np.float32)
    return frames, record.ValuesAsNumpy().astype(np.float64).reshape(len(frames), record.Components())


def loops_whole(frames, loop):
    if loop is None or len(frames) < 2:
        return False
    start, length = loop
    return abs(frames[0] - start) < LOOP_TOLERANCE and abs(frames[-1] - (start + length)) < LOOP_TOLERANCE


def bone_path(name, prop):
    escaped = name.replace("\\", "\\\\").replace('"', '\\"')
    return f'pose.bones["{escaped}"].{prop}'


def properties(record):
    for index in range(record.PropertiesLength()):
        prop = record.Properties(index)
        yield text(prop.Name()), text(prop.Value())


def euler_keys(rotations):
    result = np.empty((len(rotations), 3))
    previous = Quaternion(rotations[0].tolist()).to_euler("XYZ")
    for i, rotation in enumerate(rotations):
        previous = Quaternion(rotation.tolist()).to_euler("XYZ", previous)
        result[i] = previous
    return result


class Track:

    def __init__(self, record):
        self.channels = {}
        self.loop = None
        for index in range(record.ChannelsLength()):
            channel = record.Channels(index)
            recorded = channel.Keys()
            self.channels[text(channel.Property())] = keys(recorded)
            if recorded is not None and recorded.LoopFrames() > 0:
                self.loop = (recorded.LoopStart(), recorded.LoopFrames())

    def get(self, prop):
        return self.channels.get(prop)

    def bones(self):
        bones = {}
        for prop, keyed in self.channels.items():
            if not prop.startswith(BONES):
                continue
            bone, _, attribute = prop[len(BONES):].rpartition("/")
            bones.setdefault(bone, {})[attribute] = keyed
        return [(bone, attributes.get("location"), attributes.get("rotation"), attributes.get("scale"))
                for bone, attributes in bones.items()]

    def material_parameters(self):
        for prop, keyed in self.channels.items():
            if prop.startswith(MATERIALS):
                slot, _, parameter = prop[len(MATERIALS):].partition("/")
                yield int(slot), parameter, keyed

    def unknown(self):
        return [prop for prop in self.channels if prop not in OBJECT_PROPERTIES and not prop.startswith((BONES, MATERIALS))
                or prop.startswith(BONES) and prop.rpartition("/")[2] not in BONE_ATTRIBUTES]


class SceneBuilder:
    def __init__(self, title):
        self.title = title
        self.images = DatablockIndex(bpy.data.images)
        self.materials = DatablockIndex(bpy.data.materials)
        self.programs = material_program.ProgramMaterials()
        self.meshes = DatablockIndex(bpy.data.meshes)
        self.armatures = DatablockIndex(bpy.data.armatures)
        self.lights = DatablockIndex(bpy.data.lights)
        self.worlds = DatablockIndex(bpy.data.worlds)
        self.cameras = DatablockIndex(bpy.data.cameras)
        self.actions = DatablockIndex(bpy.data.actions)
        self.skeletons = {}
        self.mesh_skeletons = {}
        self.animations = {}
        self.collections = {}
        self.objects = {}
        self.object_bones = {}
        self.mesh_objects = []
        self.instanced_materials = {}
        self.missing_materials = set()
        self.frame_end = None
        self.diagnostics = []
        self.helper_scene = None
        self.builders = {
            fb.Record.Image: self.image,
            fb.Record.Material: self.material,
            fb.Record.Skeleton: self.skeleton,
            fb.Record.Mesh: self.mesh,
            fb.Record.Light: self.light,
            fb.Record.Camera: self.camera,
            fb.Record.Instances: self.instances,
            fb.Record.Animation: self.animation,
            fb.Record.Scene: self.scene,
            fb.Record.Collection: self.collection,
            fb.Record.Object: self.object,
        }

    def build(self, kind, record):
        try:
            self.builders[kind](record)
        except Exception as error:
            traceback.print_exc()
            name = text(record.Name()) if hasattr(record, "Name") else ""
            self.diagnostics.append((fb.Severity.Error, "import.record", str(error), name))

    def edit_scene(self):
        if self.helper_scene is None:
            self.helper_scene = bpy.data.scenes.new(f"{self.title} edit")
        return self.helper_scene

    def finish(self):
        apply_viewers(bpy.context.scene)
        self.extend_skinned_uvs()
        if self.helper_scene is not None:
            bpy.data.scenes.remove(self.helper_scene)
            self.helper_scene = None
        if bpy.ops.ed.undo_push.poll():
            bpy.ops.ed.undo_push(message=f"Receive {self.title}")
        return self.diagnostics

    def extend_skinned_uvs(self):
        skinned = {self.meshes.get(key) for key in self.mesh_skeletons} - {None}
        needed = {}
        read = {}
        for obj in self.mesh_objects:
            if obj.data not in skinned:
                continue
            for slot in obj.material_slots:
                if slot.material and slot.material.node_tree:
                    if slot.material not in read:
                        read[slot.material] = uv_sets_read(slot.material.node_tree)
                    needed[obj.data] = max(needed.get(obj.data, 0), read[slot.material])
        for mesh, count in needed.items():
            layers = mesh.uv_layers
            if not layers or not layers[-1].name.startswith("UV"):
                continue
            uvs = np.empty(len(mesh.loops) * 2, dtype=np.float32)
            layers[-1].data.foreach_get("uv", uvs)
            for channel in range(len(layers), min(count, MAX_SKINNED_UV_SETS)):
                layers.new(name=f"UV{channel}").data.foreach_set("uv", uvs)

    def image(self, record):
        key = text(record.Key())
        if self.images.get(key) is not None:
            return
        data = record.DataAsNumpy().tobytes()
        image = bpy.data.images.new(text(record.Name()), width=1, height=1, alpha=True)
        image.source = "FILE"
        image.filepath_raw = f"//textures/{image.name}.dds"
        image.pack(data=data, data_len=len(data))
        image.alpha_mode = "CHANNEL_PACKED"
        image.colorspace_settings.name = "sRGB" if record.Srgb() else "Non-Color"
        self.images.add(key, image)

    def material(self, record):
        key = text(record.Key())
        if self.materials.get(key) is not None:
            return
        material, warnings = self.programs.material(text(record.Name()), record.Program(), self.images.get,
                                                    text(record.BlendMode()), record.TwoSided(), record.Unlit())
        for warning in dict.fromkeys(warnings):
            self.diagnostics.append((fb.Severity.Warning, "import.program_incomplete", warning, key))
        self.materials.add(key, material)

    def skeleton(self, record):
        key = text(record.Key())
        bones = []
        for index in range(record.BonesLength()):
            bone = record.Bones(index)
            bones.append((text(bone.Name()), bone.Parent(), bone.LocationAsNumpy(), bone.RotationAsNumpy(), bone.ScaleAsNumpy()))
        rest = space.RestBones(bones)
        self.skeletons[key] = rest
        if self.armatures.get(key) is not None:
            return
        armature = bpy.data.armatures.new(text(record.Name()))
        armature.display_type = "STICK"
        scene = self.edit_scene()
        holder = bpy.data.objects.new(armature.name, armature)
        scene.collection.objects.link(holder)
        view_layer = scene.view_layers[0]
        view_layer.objects.active = holder
        try:
            with bpy.context.temp_override(scene=scene, view_layer=view_layer, active_object=holder, object=holder):
                bpy.ops.object.mode_set(mode="EDIT")
                try:
                    edit_bones = []
                    for i, name in enumerate(rest.names):
                        bone = armature.edit_bones.new(name)
                        bone.head = (0.0, 0.0, 0.0)
                        bone.tail = (0.0, rest.lengths[i], 0.0)
                        bone.matrix = Matrix(rest.bones[i].tolist())
                        local = Matrix(rest.bind_local(i).tolist())
                        bind_rotation = local.to_quaternion()
                        bone["orig_loc"] = list(local.to_translation())
                        bone["orig_quat"] = bind_rotation.conjugated()
                        bone["post_quat"] = bind_rotation @ Matrix(rest.axis_changes[i].tolist()).to_quaternion()
                        if rest.parents[i] >= 0:
                            bone.parent = edit_bones[rest.parents[i]]
                        edit_bones.append(bone)
                finally:
                    bpy.ops.object.mode_set(mode="OBJECT")
        finally:
            bpy.data.objects.remove(holder)
        self.armatures.add(key, armature)

    def mesh(self, record):
        key = text(record.Key())
        skeleton = text(record.Skeleton())
        if skeleton:
            self.mesh_skeletons[key] = skeleton
        if self.meshes.get(key) is not None:
            return
        vertex_count = record.PositionsLength() // 3
        corners = record.IndicesAsNumpy().view(np.int32)
        mesh = bpy.data.meshes.new(text(record.Name()))
        mesh.vertices.add(vertex_count)
        positions = record.PositionsAsNumpy().reshape(-1, 3)
        mesh.vertices.foreach_set("co", space.positions(positions).ravel())
        if vertex_count:
            mesh[material_program.BOUNDS_MIN] = positions.min(axis=0).tolist()
            mesh[material_program.BOUNDS_MAX] = positions.max(axis=0).tolist()
        mesh.loops.add(len(corners))
        mesh.loops.foreach_set("vertex_index", corners)
        mesh.polygons.add(len(corners) // 3)
        mesh.polygons.foreach_set("loop_start", np.arange(0, len(corners), 3, dtype=np.int32))
        mesh.polygons.foreach_set("material_index", record.TriangleMaterialsAsNumpy().astype(np.int32))
        if record.UvCount():
            channels = record.UvsAsNumpy().reshape(record.UvCount(), vertex_count, 2)
            for channel in range(record.UvCount()):
                uvs = channels[channel][corners]
                uvs[:, 1] = 1 - uvs[:, 1]
                mesh.uv_layers.new(name=f"UV{channel}").data.foreach_set("uv", uvs.ravel())
        colors = record.ColorsAsNumpy().astype(np.float32) / np.float32(255)
        mesh.color_attributes.new("COL0", "FLOAT_COLOR", "POINT").data.foreach_set("color", colors)
        for _ in range(record.MaterialSlotsLength()):
            mesh.materials.append(None)
        mesh.update()
        mesh.shade_smooth()
        normals = space.directions(record.NormalsAsNumpy())
        if skeleton:
            #! The Armature modifier turns custom normals in corner fan space, but leaves a free custom_normal attribute as it is.
            mesh.normals_split_custom_set_from_vertices(normals)
            self.add_vertex_groups(mesh, record, self.skeletons[skeleton].names)
        else:
            mesh.attributes.new("custom_normal", "FLOAT_VECTOR", "POINT").data.foreach_set("vector", normals.ravel())
        self.meshes.add(key, mesh)

    @staticmethod
    def add_vertex_groups(mesh, record, bone_names):
        influences = record.Influences()
        vertices = np.repeat(np.arange(record.PositionsLength() // 3, dtype=np.int32), influences)
        bones = record.BoneIndicesAsNumpy()
        weights = record.BoneWeightsAsNumpy()
        used = weights > 0
        vertices, bones, weights = vertices[used], bones[used], weights[used]
        order = np.lexsort((weights, bones))
        vertices, bones, weights = vertices[order], bones[order], weights[order]
        starts = np.flatnonzero(np.r_[True, (bones[1:] != bones[:-1]) | (weights[1:] != weights[:-1])])
        ends = np.r_[starts[1:], len(order)].tolist()
        holder = bpy.data.objects.new(mesh.name, mesh)
        try:
            groups = {}
            for start, end in zip(starts.tolist(), ends):
                bone = int(bones[start])
                group = groups.get(bone)
                if group is None:
                    group = groups[bone] = holder.vertex_groups.new(name=bone_names[bone])
                group.add(vertices[start:end].tolist(), float(weights[start]) / 65535, "REPLACE")
        finally:
            bpy.data.objects.remove(holder)

    def light(self, record):
        key = text(record.Key())
        if record.Type() == fb.LightType.Sky:
            if self.worlds.get(key) is None:
                self.worlds.add(key, self.sky_world(record))
            return
        if self.lights.get(key) is not None:
            return
        light_type = LIGHT_TYPES[record.Type()]
        light = bpy.data.lights.new(text(record.Name()), light_type)
        light.color = tuple(float(value) for value in record.ColorAsNumpy())
        light.energy = space.light_energy(light_type, record.Intensity())
        light.shadow_soft_size = record.SourceRadius() * space.METRES_PER_CENTIMETRE
        if light_type == "SPOT":
            outer = math.cos(math.radians(record.OuterConeAngle()))
            inner = math.cos(math.radians(min(record.InnerConeAngle(), record.OuterConeAngle())))
            light.spot_size = 2 * math.radians(record.OuterConeAngle())
            light.spot_blend = min(max((inner - outer) / (1 - outer), 0.0), 1.0) if outer < 1 else 0.0
        elif light_type == "SUN":
            light.angle = math.radians(record.SourceAngle())
        elif light_type == "AREA":
            light.shape = "RECTANGLE"
            light.size = record.SourceWidth() * space.METRES_PER_CENTIMETRE
            light.size_y = record.SourceHeight() * space.METRES_PER_CENTIMETRE
        if light_type != "SUN" and record.AttenuationRadius() > 0:
            self.light_falloff(light, record)
        self.lights.add(key, light)

    def light_falloff(self, light, record):
        radius = record.AttenuationRadius() * space.METRES_PER_CENTIMETRE
        if hasattr(light, "use_custom_distance"):
            light.use_custom_distance = True
            light.cutoff_distance = radius
        light.use_nodes = True
        tree = light.node_tree
        emission = next(node for node in tree.nodes if node.type == "EMISSION")
        writer = NodeWriter(tree)
        distance = Value(1, socket=writer.new("ShaderNodeLightPath").outputs["Ray Length"])
        ratio = writer.math("DIVIDE", distance, Value(1, (radius,)))
        squared = writer.math("MULTIPLY", ratio, ratio)
        if record.InverseSquaredFalloff():
            window = writer.math("SUBTRACT", Value(1, (1.0,)), writer.math("MULTIPLY", squared, squared))
            window.socket.node.use_clamp = True
            strength = writer.math("MULTIPLY", window, window)
        else:
            exponent = record.LightFalloffExponent()
            window = writer.math("SUBTRACT", Value(1, (1.0,)), squared)
            window.socket.node.use_clamp = True
            curve = writer.math("POWER", window, Value(1, (exponent,)))
            strength = writer.math("MULTIPLY", curve, writer.math("MULTIPLY", squared, Value(1, (4.0 / 0.75 ** exponent,))))
        writer.link(strength.socket, emission.inputs["Strength"])

    def sky_world(self, record):
        name = text(record.Name())
        world = bpy.data.worlds.new(name)
        tree = world.node_tree
        tree.nodes.clear()
        writer = NodeWriter(tree, self.programs.groups)
        image_key = text(record.Image())
        if image_key:
            image = self.images.get(image_key)
            if image is None:
                raise ValueError(f"the image {image_key} of the sky light was not sent")
            turn = writer.new("ShaderNodeMapping", vector_type="VECTOR")
            turn.inputs["Rotation"].default_value = (0.0, 0.0, -math.radians(record.SourceCubemapAngle()))
            writer.link(writer.new("ShaderNodeTexCoord").outputs["Generated"], turn.inputs["Vector"])
            strip = writer.group("Cube Strip", Value(3, socket=turn.outputs["Vector"]), Value(1, (float(image.size[0]),)))[0]
            texture = writer.new("ShaderNodeTexImage", image=image, extension="EXTEND")
            writer.connect(texture.inputs["Vector"], Value(3, socket=strip))
            color = Value(4, socket=texture.outputs["Color"])
        else:
            self.diagnostics.append((fb.Severity.Warning, "import.sky_capture",
                                     "The sky light captures the scene, which Blender cannot do, so it gives no light "
                                     "from above the horizon.", name))
            color = Value(4, (0.0, 0.0, 0.0, 1.0))
        if record.LowerHemisphereIsBlack():
            lower = tuple(float(value) for value in record.LowerHemisphereColorAsNumpy())
            color = Value(4, socket=writer.group("Lower Hemisphere", color, Value(4, lower[:3] + (1.0,)),
                                                 Value(1, lower[3:]))[0])
        tint = writer.new("ShaderNodeMix", data_type="RGBA", blend_type="MULTIPLY")
        tint.inputs["Factor"].default_value = 1.0
        writer.connect(tint.inputs["A"], color)
        writer.connect(tint.inputs["B"], Value(4, tuple(float(value) for value in record.ColorAsNumpy()) + (1.0,)))
        strength = writer.group("Sky Strength", Value(1, (space.sky_strength(record.Intensity()),)),
                                Value(1, (record.IndirectLightingIntensity(),)))[0]

        background = writer.new("ShaderNodeBackground")
        writer.link(tint.outputs["Result"], background.inputs["Color"])
        writer.link(strength, background.inputs["Strength"])
        output = writer.new("ShaderNodeOutputWorld")
        writer.link(background.outputs["Background"], output.inputs["Surface"])
        writer.link_groups()
        return world

    def camera(self, record):
        key = text(record.Key())
        if self.cameras.get(key) is not None:
            return
        camera = bpy.data.cameras.new(text(record.Name()))
        camera.sensor_fit = "HORIZONTAL"
        camera.sensor_width = 36
        camera.lens = space.camera_lens(record.FieldOfView())
        camera.clip_start = record.NearClip() * space.METRES_PER_CENTIMETRE
        self.cameras.add(key, camera)

    def instances(self, record):
        key = text(record.Key())
        if self.meshes.get(key) is not None:
            return
        mesh = self.meshes.get(text(record.Mesh()))
        name = f"{mesh.name if mesh is not None else text(record.Mesh())} instances"
        self.meshes.add(key, instancing.points(name, record))

    def built_material(self, key):
        if not key:
            return None
        material = self.materials.get(key)
        if material is None and key not in self.missing_materials:
            self.missing_materials.add(key)
            message = "The material of a slot was not built, so the slot has no material."
            print(f"[{NAME}] {message} ({key})")
            self.diagnostics.append((fb.Severity.Warning, "import.material_missing", message, key))
        return material

    def instanced_material(self, key, parameters, facing, sprite):
        material = self.built_material(key)
        if material is None or (not parameters and not sprite and facing == fb.Facing.None_):
            return material
        cache_key = (key, tuple(sorted(parameters)), facing, sprite)
        copy = self.instanced_materials.get(cache_key)
        if copy is None:
            copy = instancing.instanced_material(material, parameters, facing, sprite)
            if copy is not material:
                for prop in (KEY, material_program.STRUCTURE):
                    if prop in copy:
                        del copy[prop]
            self.instanced_materials[cache_key] = copy
        return copy

    def animation(self, record):
        tracks = {}
        for index in range(record.TracksLength()):
            track = record.Tracks(index)
            tracks[text(track.Name())] = Track(track)
        self.animations[text(record.Key())] = (text(record.Name()), tracks)

    def scene(self, record):
        scene = bpy.context.scene
        scene.render.fps = record.FramesPerSecond()
        scene.render.fps_base = 1.0
        self.frame_end = record.FrameEnd()

    def collection(self, record):
        parent = record.Parent()
        target = bpy.context.scene.collection if parent < 0 else self.collections[parent]
        name = text(record.Name())
        collection = target.children.get(name)
        if collection is None:
            collection = bpy.data.collections.new(name)
            target.children.link(collection)
        for prop, value in properties(record):
            collection[prop] = value
        self.collections[record.Id()] = collection

    def object(self, record):
        kind = record.Kind()
        data_key = text(record.Data())
        world = self.worlds.get(data_key) if kind == fb.ObjectKind.Light else None
        if world is not None:
            bpy.context.scene.world = world
            return
        data = None
        rest = None
        if kind == fb.ObjectKind.Mesh:
            data = self.meshes.get(data_key)
            if record.Posed():
                rest = self.skeletons.get(self.mesh_skeletons.get(data_key, ""))
                if rest is None:
                    raise ValueError(f"the skeleton of {data_key} was not sent")
        elif kind == fb.ObjectKind.Light:
            data = self.lights.get(data_key)
        elif kind == fb.ObjectKind.Camera:
            data = self.cameras.get(data_key)
        elif kind == fb.ObjectKind.Instances:
            data = self.meshes.get(data_key)
        if kind != fb.ObjectKind.Empty and data is None:
            raise ValueError(f"the data {data_key} was not built")
        name = text(record.Name())
        collection = self.collections[record.Collection()]
        mesh_object = None
        if rest is not None:
            obj = bpy.data.objects.new(name, self.armatures.get(self.mesh_skeletons[data_key]))
            obj.show_in_front = True
            mesh_object = bpy.data.objects.new(data.name, data)
            modifier = mesh_object.modifiers.new("Armature", "ARMATURE")
            modifier.object = obj
            mesh_object.parent = obj
        else:
            obj = bpy.data.objects.new(name, data)
        collection.objects.link(obj)
        if mesh_object is not None:
            collection.objects.link(mesh_object)

        pre = Matrix.Identity(4)
        post = space.VIEW_BASIS if kind in (fb.ObjectKind.Light, fb.ObjectKind.Camera) else Matrix.Identity(4)
        if record.Parent() >= 0:
            obj.parent = self.objects[record.Parent()]
            parent_bone = text(record.ParentBone())
            if parent_bone:
                bones = self.object_bones[record.Parent()]
                bone = bones.names.index(parent_bone)
                obj.parent_type = "BONE"
                obj.parent_bone = parent_bone
                pre = bones.child_basis(bone, Matrix(bones.poses[bone].tolist()))
        obj.matrix_basis = pre @ space.matrix(record.MatrixAsNumpy()) @ post

        shaded = mesh_object or obj
        material_keys = [text(record.Materials(index)) for index in range(record.MaterialsLength())]
        if kind == fb.ObjectKind.Instances:
            slots = self.instancer(obj, data, material_keys)
        else:
            slots = shaded.material_slots
            for slot, material_key in zip(slots, material_keys):
                slot.link = "OBJECT"
                slot.material = self.built_material(material_key)
            if kind == fb.ObjectKind.Mesh:
                self.mesh_objects.append(shaded)
        for target in (obj,) if mesh_object is None else (obj, mesh_object):
            for path in HIDDEN_PATHS:
                setattr(target, path, not record.Visible())
        if record.ReverseCulling():
            shaded[REVERSE_CULLING] = 1
            for slot in slots:
                if slot.material is not None:
                    slot.material.use_backface_culling = False
        if not record.CastShadow():
            for path in LIGHT_RAY_PATHS:
                setattr(shaded, path, False)
        for prop, value in properties(record):
            shaded[prop] = value
        animation_key = text(record.Animation())
        if record.Viewer() >= 0:
            self.decide_by_viewer(collection, self.objects[record.Viewer()], record.ViewerVisibility())
        if animation_key:
            self.animate(obj, mesh_object, slots, rest, animation_key, text(record.Track()), pre, post)
        if rest is not None:
            self.object_bones[record.Id()] = rest
        self.objects[record.Id()] = obj

    def instancer(self, obj, points, material_keys):
        mesh_key = points[instancing.MESH]
        mesh = self.meshes.get(mesh_key)
        if mesh is None:
            raise ValueError(f"the mesh {mesh_key} of the instances was not built")
        if len(material_keys) != len(mesh.materials):
            raise ValueError(f"{obj.name} binds {len(material_keys)} materials to the {len(mesh.materials)} slots of {mesh_key}")
        parameters = points[instancing.PARAMETERS].to_dict()
        facing = points[instancing.FACING]
        sprite = mesh_key == instancing.QUAD
        materials = [self.instanced_material(key, parameters, facing, sprite) for key in material_keys]
        return instancing.instancer(obj, instancing.source(obj, mesh, mesh_key), materials, bool(points[instancing.ANIMATED]),
                                     self.frame_end, tuple(points[instancing.LOOP]))

    def decide_by_viewer(self, collection, viewer, visibility):
        decided = VIEWER_VISIBILITIES.get(visibility)
        if decided is None:
            return
        if collection.get(VIEWER, viewer.name) != viewer.name or collection.get(VIEWER_VISIBILITY, decided) != decided:
            self.diagnostics.append((fb.Severity.Warning, "import.viewer_replaced",
                                     f"The view of {viewer.name} now decides the collection, in place of {collection[VIEWER]}.",
                                     collection.name))
        collection[VIEWER] = viewer.name
        collection[VIEWER_VISIBILITY] = decided

    def animate(self, obj, mesh_object, slots, rest, animation_key, track_name, pre, post):
        name, tracks = self.animations[animation_key]
        track = tracks.get(track_name)
        if track is None:
            raise ValueError(f"the animation {animation_key} has no track {track_name}")
        action = self.actions.get(animation_key)
        if action is None:
            action = self.actions.add(animation_key, bpy.data.actions.new(name))
            action.layers.new("Layer").strips.new(type="KEYFRAME")

        for prop in track.unknown():
            self.diagnostics.append((fb.Severity.Warning, "import.animation_property",
                                     f"The importer does not animate the property {prop}.", track_name))
        curves = []
        if (location := track.get("location")) is not None:
            frames, values = location
            located = space.positions(values).astype(np.float64) @ np.array(pre.to_3x3()).T + np.array(pre.to_translation())
            curves += [("location", axis, frames, located[:, axis], LINEAR) for axis in range(3)]
        if (rotation := track.get("rotation")) is not None:
            frames, values = rotation
            turned = space.multiply(space.multiply(np.array(pre.to_quaternion()), space.quaternions(values)),
                                    np.array(post.to_quaternion()))
            angles = euler_keys(turned)
            curves += [("rotation_euler", axis, frames, angles[:, axis], LINEAR) for axis in range(3)]
        if (scale := track.get("scale")) is not None:
            frames, values = scale
            curves += [("scale", axis, frames, values[:, axis], LINEAR) for axis in range(3)]
        visibility = []
        if (shown := track.get("visibility")) is not None:
            frames, values = shown
            visibility = [(path, 0, frames, 1 - values[:, 0], CONSTANT) for path in HIDDEN_PATHS]
        bones = track.bones()
        if bones and rest is not None:
            converter = space.BoneKeys(rest)
            for bone, location, rotation, scale in bones:
                location, rotation, scale = converter.convert(bone, location, rotation, scale)
                for prop, keyed in (("location", location), ("rotation_quaternion", rotation), ("scale", scale)):
                    if keyed is None:
                        continue
                    frames, values = keyed
                    curves += [(bone_path(bone, prop), axis, frames, values[:, axis], LINEAR) for axis in range(values.shape[1])]
        self.assign(action, obj, track_name, curves + visibility, track.loop)
        if mesh_object is not None and visibility:
            self.assign(action, mesh_object, f"{track_name} mesh", visibility, track.loop)
        if (field_of_view := track.get("field_of_view")) is not None:
            frames, values = field_of_view
            self.assign(action, obj.data, track_name, [("lens", 0, frames, space.camera_lens(values[:, 0]), LINEAR)], track.loop)
        self.animate_materials(action, slots, (mesh_object or obj).name, track, track_name)

    def animate_materials(self, action, slots, owner, track, track_name):
        by_slot = {}
        for slot, parameter, keyed in track.material_parameters():
            by_slot.setdefault(slot, []).append((parameter, keyed))
        for slot, parameters in by_slot.items():
            if slot >= len(slots) or slots[slot].material is None:
                raise ValueError(f"the track {track_name} animates the material of slot {slot}, which {owner} does not have")
            material = slots[slot].material.copy()
            if KEY in material:
                del material[KEY]
            slots[slot].material = material
            nodes = material.node_tree.nodes
            curves = []
            for parameter, (frames, values) in parameters:
                for name, components in ((material_program.parameter_input_name(parameter), range(min(values.shape[1], 3))),
                                         (material_program.parameter_input_name(parameter, alpha=True), (3,))):
                    node = nodes.get(name)
                    if node is None or material_program.PARAMETER not in node or (components == (3,) and values.shape[1] < 4):
                        continue
                    path = f'nodes["{name}"].{material_program.parameter_value_path(node)}'
                    if values.shape[1] == 1 or components == (3,):
                        curves.append((path, 0, frames, values[:, components[0]], LINEAR))
                    else:
                        curves += [(path, axis, frames, values[:, axis], LINEAR) for axis in components]
            if curves:
                self.assign(action, material.node_tree, f"{track_name} slot {slot}", curves, track.loop)

    @staticmethod
    def assign(action, target, slot_name, curves, loop=None):
        slot = next((slot for slot in action.slots if slot.target_id_type == target.id_type and slot.name_display == slot_name), None)
        if slot is None:
            slot = action.slots.new(id_type=target.id_type, name=slot_name)
            channelbag = action.layers[0].strips[0].channelbag(slot, ensure=True)
            for path, index, frames, values, interpolation in curves:
                curve = channelbag.fcurves.new(path, index=index)
                points = curve.keyframe_points
                points.add(len(frames))
                points.foreach_set("co", np.column_stack((frames, values)).astype(np.float32).ravel())
                points.foreach_set("interpolation", np.full(len(frames), interpolation, dtype=np.int32))
                if loops_whole(frames, loop):
                    curve.modifiers.new("CYCLES")
        data = target.animation_data_create()
        if data.action not in (None, action):
            raise ValueError(f"{target.name} has another action already")
        data.action = action
        #! Blender crashes when it gets the same material slot again.
        if data.action_slot != slot:
            data.action_slot = slot


def uv_sets_read(tree, seen=None):
    seen = set() if seen is None else seen
    if tree in seen:
        return 0
    seen.add(tree)
    count = 0
    for node in tree.nodes:
        if node.bl_idname == "ShaderNodeUVMap" and node.uv_map[2:].isdigit() and node.uv_map.startswith("UV"):
            count = max(count, int(node.uv_map[2:]) + 1)
        elif node.bl_idname == "ShaderNodeGroup" and node.node_tree:
            count = max(count, uv_sets_read(node.node_tree, seen))
    return count


def layer_collections(layer_collection):
    yield layer_collection
    for child in layer_collection.children:
        yield from layer_collections(child)


def apply_viewers(scene):
    decided = [collection for collection in scene.collection.children_recursive if VIEWER in collection]
    for name in sorted({collection[VIEWER] for collection in decided}):
        if scene.view_layers.get(name) is None:
            scene.view_layers.new(name)
    for view_layer in scene.view_layers:
        for layer_collection in layer_collections(view_layer.layer_collection):
            collection = layer_collection.collection
            if VIEWER not in collection:
                continue
            own_view = collection[VIEWER] == view_layer.name
            layer_collection.exclude = own_view if collection[VIEWER_VISIBILITY] == "not_viewer" else not own_view
