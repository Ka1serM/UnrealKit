import bpy
import numpy as np

from . import scene_generated as fb
from . import space
from .material_program import PARAMETER, VECTOR_NODE, parameter_input_name
from .node_groups import CENTIMETRES, GROUP as SHARED_GROUP
from .product import NAME

ROTATION = "unrealkit.rotation"
SCALE = "unrealkit.scale"
FRAME = "unrealkit.frame"
ROLL = "unrealkit.roll"
MESH = "unrealkit.instanced_mesh"
FACING = "unrealkit.facing"
ANIMATED = "unrealkit.animated"
LOOP = "unrealkit.loop"
PARAMETERS = "unrealkit.instance_parameters"
GROUP = "unrealkit.instances"
FACING_GROUP = "unrealkit.facing_group"
SOURCE = "unrealkit.instance_source"
PARTICLE_POSITION = "unrealkit.particle_position"
QUAD = "unrealkit:quad"
NO_END = 1.0e9
FACING_NAMES = {fb.Facing.CameraPlane: "Camera Plane", fb.Facing.CameraPosition: "Camera Position", fb.Facing.CameraAxis: "Camera Axis"}
ATTRIBUTES = {1: ("FLOAT", "value"), 2: ("FLOAT2", "vector"), 3: ("FLOAT_VECTOR", "vector"), 4: ("FLOAT_COLOR", "color")}
OUTPUTS = {"ShaderNodeValue": "Fac", "ShaderNodeRGB": "Color", VECTOR_NODE: "Vector"}


def points(name, record):
    count = record.LocationsLength() // 3
    if record.LocationsLength() != 3 * count or record.RotationsLength() != 4 * count or record.ScalesLength() != 3 * count:
        raise ValueError(f"the instances {name} have {record.LocationsLength()} location, {record.RotationsLength()} rotation, "
                         f"and {record.ScalesLength()} scale values, which are not of the same samples")
    animated = record.FirstFramesLength() > 0 or record.FrameCountsLength() > 0
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(count)
    if count:
        positions = space.positions(record.LocationsAsNumpy()).ravel()
        mesh.vertices.foreach_set("co", positions)
        #! Instancing passes no point position to the instances, so a sprite's particle position goes along as an attribute.
        if record.Mesh() == QUAD.encode():
            mesh.attributes.new(PARTICLE_POSITION, "FLOAT_VECTOR", "POINT").data.foreach_set("vector", positions)
        mesh.attributes.new(ROTATION, "QUATERNION", "POINT").data.foreach_set(
            "value", space.quaternions(record.RotationsAsNumpy()).astype(np.float32).ravel())
        mesh.attributes.new(SCALE, "FLOAT_VECTOR", "POINT").data.foreach_set("vector", record.ScalesAsNumpy())
        if record.Facing() != fb.Facing.None_:
            q = space.quaternions(record.RotationsAsNumpy())
            #! A camera axis sprite carries its roll as the twist of its rotation about its Z axis.
            twist = 3 if record.Facing() == fb.Facing.CameraAxis else 1
            roll = 2 * np.arctan2(q[:, twist], q[:, 0])
            mesh.attributes.new(ROLL, "FLOAT", "POINT").data.foreach_set("value", roll.astype(np.float32))
    if animated:
        if record.FirstFramesLength() != record.FrameCountsLength():
            raise ValueError(f"the instances {name} have {record.FirstFramesLength()} first frames and "
                             f"{record.FrameCountsLength()} frame counts")
        counts = record.FrameCountsAsNumpy().astype(np.int64)
        if int(counts.sum()) != count:
            raise ValueError(f"the frames of the instances {name} give {int(counts.sum())} samples, not {count}")
        starts = np.cumsum(counts) - counts
        frames = np.repeat(record.FirstFramesAsNumpy().astype(np.int64) - starts, counts) + np.arange(count)
        mesh.attributes.new(FRAME, "FLOAT", "POINT").data.foreach_set("value", frames.astype(np.float32))
    parameters = {}
    for index in range(record.ParametersLength()):
        parameter = record.Parameters(index)
        parameter_name, components = parameter.Name().decode("utf-8"), parameter.Components()
        if components not in ATTRIBUTES or parameter.ValuesLength() != components * count:
            raise ValueError(f"the parameter {parameter_name} of the instances {name} has {parameter.ValuesLength()} values of "
                             f"{components} components for {count} samples")
        attribute_type, element = ATTRIBUTES[components]
        if count:
            mesh.attributes.new(parameter_name, attribute_type, "POINT").data.foreach_set(element, parameter.ValuesAsNumpy())
        parameters[parameter_name] = components
    mesh[MESH] = record.Mesh().decode("utf-8")
    mesh[FACING] = int(record.Facing())
    mesh[ANIMATED] = animated
    mesh[LOOP] = (record.LoopStart(), record.LoopFrames())
    mesh[PARAMETERS] = parameters
    return mesh


def source(obj, mesh, key):
    holder = bpy.data.objects.new(f"{obj.name} instance", mesh)
    holder[SOURCE] = key
    for slot in holder.material_slots:
        slot.link = "OBJECT"
    return holder


def instanced_material(material, parameters, facing, sprite):
    replaced = []
    for node in material.node_tree.nodes:
        if PARAMETER not in node:
            continue
        for name in parameters:
            if node.name == parameter_input_name(name, alpha=True) and node[PARAMETER][1] == 3:
                replaced.append((node.name, name, "Alpha"))
            elif node.name == parameter_input_name(name):
                replaced.append((node.name, name, OUTPUTS[node.bl_idname]))
    #! Unreal's sprite vertex factory gives every corner of a sprite its particle's position as the local position.
    local_positions = [node for node in material.node_tree.nodes if sprite and node.bl_idname == "ShaderNodeGroup"
                       and node.node_tree is not None and node.node_tree.get(SHARED_GROUP) == "Local Position"]
    if not replaced and not local_positions and facing == fb.Facing.None_:
        return material
    copy = material.copy()
    copy.name = f"{material.name} instanced"
    nodes, links = copy.node_tree.nodes, copy.node_tree.links
    for node_name, parameter, output in replaced:
        node = nodes[node_name]
        attribute = nodes.new("ShaderNodeAttribute")
        attribute.attribute_type = "INSTANCER"
        attribute.attribute_name = parameter
        attribute.label = node.label
        attribute.location = node.location
        for link in list(node.outputs[0].links):
            links.new(attribute.outputs[output], link.to_socket)
        nodes.remove(node)
    for original in local_positions:
        node = nodes[original.name]
        attribute = nodes.new("ShaderNodeAttribute")
        attribute.attribute_type = "INSTANCER"
        attribute.attribute_name = PARTICLE_POSITION
        attribute.location = node.location
        scaled = nodes.new("ShaderNodeVectorMath")
        scaled.operation = "MULTIPLY"
        scaled.label = "Particle Local Position"
        scaled.inputs[1].default_value = CENTIMETRES
        scaled.location = (node.location.x + 160, node.location.y)
        links.new(attribute.outputs["Vector"], scaled.inputs[0])
        for link in list(node.outputs[0].links):
            links.new(scaled.outputs[0], link.to_socket)
        nodes.remove(node)
    if facing != fb.Facing.None_:
        face(copy, facing)
    return copy


def face(material, facing):
    nodes, links = material.node_tree.nodes, material.node_tree.links
    output = next(node for node in nodes if node.bl_idname == "ShaderNodeOutputMaterial" and node.is_active_output)
    turned = nodes.new("ShaderNodeGroup")
    turned.node_tree = facing_shader_group(facing)
    turned.location = (output.location.x - 200, output.location.y - 300)
    socket = output.inputs["Displacement"]
    if socket.links:
        added = nodes.new("ShaderNodeVectorMath")
        added.operation = "ADD"
        added.location = (output.location.x - 200, output.location.y - 450)
        links.new(socket.links[0].from_socket, added.inputs[0])
        links.new(turned.outputs[0], added.inputs[1])
        links.new(added.outputs[0], socket)
    else:
        links.new(turned.outputs[0], socket)
    material.displacement_method = "DISPLACEMENT"


class TreeWriter:

    def __init__(self, tree):
        self.tree = tree

    def new(self, kind, inputs=(), **settings):
        node = self.tree.nodes.new(kind)
        for name, setting in settings.items():
            setattr(node, name, setting)
        for index, value in enumerate(inputs):
            if value is None:
                continue
            if isinstance(value, bpy.types.NodeSocket):
                self.tree.links.new(value, node.inputs[index])
            else:
                node.inputs[index].default_value = value
        return node

    def output(self, kind, inputs=(), name=0, **settings):
        return self.new(kind, inputs, **settings).outputs[name]


def instances_group():
    for group in bpy.data.node_groups:
        if group.get(GROUP) and "Loop Frames" in group.interface.items_tree:
            return group
    group = bpy.data.node_groups.new(f"{NAME} Instances", "GeometryNodeTree")
    group[GROUP] = True
    group.interface.new_socket("Points", in_out="INPUT", socket_type="NodeSocketGeometry")
    group.interface.new_socket("Instance", in_out="INPUT", socket_type="NodeSocketGeometry")
    group.interface.new_socket("Animated", in_out="INPUT", socket_type="NodeSocketBool")
    end = group.interface.new_socket("End", in_out="INPUT", socket_type="NodeSocketFloat")
    end.default_value = NO_END
    group.interface.new_socket("Loop Start", in_out="INPUT", socket_type="NodeSocketFloat")
    group.interface.new_socket("Loop Frames", in_out="INPUT", socket_type="NodeSocketFloat")
    group.interface.new_socket("Instances", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    w = TreeWriter(group)
    inputs = w.new("NodeGroupInput")
    rotation = w.output("GeometryNodeInputNamedAttribute", (ROTATION,), "Attribute", data_type="QUATERNION")
    scale = w.output("GeometryNodeInputNamedAttribute", (SCALE,), "Attribute", data_type="FLOAT_VECTOR")
    frame = w.output("GeometryNodeInputNamedAttribute", (FRAME,), "Attribute", data_type="FLOAT")
    time = w.output("GeometryNodeInputSceneTime", name="Frame")
    start, length = inputs.outputs["Loop Start"], inputs.outputs["Loop Frames"]
    into_loop = w.output("ShaderNodeMath", (time, start), "Value", operation="SUBTRACT")
    wrapped = w.output("ShaderNodeMath", (start, w.output("ShaderNodeMath", (into_loop, length), "Value", operation="FLOORED_MODULO")),
                       "Value", operation="ADD")
    loops = w.output("FunctionNodeBooleanMath", (w.output("FunctionNodeCompare", (length, 0.0), data_type="FLOAT", operation="GREATER_THAN"),
                                                 w.output("FunctionNodeCompare", (into_loop, 0.0), data_type="FLOAT", operation="GREATER_EQUAL")),
                     operation="AND")
    looped = w.output("GeometryNodeSwitch", (loops, time, wrapped), input_type="FLOAT")
    now = w.output("ShaderNodeMath", (looped, inputs.outputs["End"]), "Value", operation="MINIMUM")
    on_frame = w.new("FunctionNodeCompare", data_type="FLOAT", operation="EQUAL")
    w.tree.links.new(frame, on_frame.inputs["A"])
    w.tree.links.new(now, on_frame.inputs["B"])
    on_frame.inputs["Epsilon"].default_value = 0.499
    shown = w.output("FunctionNodeBooleanMath", (inputs.outputs["Animated"], on_frame.outputs[0]), operation="IMPLY")
    placed = w.new("GeometryNodeInstanceOnPoints")
    links = w.tree.links
    links.new(inputs.outputs["Points"], placed.inputs["Points"])
    links.new(shown, placed.inputs["Selection"])
    links.new(inputs.outputs["Instance"], placed.inputs["Instance"])
    links.new(rotation, placed.inputs["Rotation"])
    links.new(scale, placed.inputs["Scale"])
    links.new(placed.outputs["Instances"], w.new("NodeGroupOutput").inputs["Instances"])
    return group


def facing_shader_group(facing):
    for group in bpy.data.node_groups:
        if group.get(FACING_GROUP) == int(facing):
            return group
    group = bpy.data.node_groups.new(f"{NAME} Facing {FACING_NAMES[facing]}", "ShaderNodeTree")
    group[FACING_GROUP] = int(facing)
    group.interface.new_socket("Displacement", in_out="OUTPUT", socket_type="NodeSocketVector")
    w = TreeWriter(group)

    def math(operation, *inputs):
        return w.output("ShaderNodeVectorMath", inputs, operation=operation)

    def scaled(vector, value):
        return w.output("ShaderNodeVectorMath", (vector, None, None, value), "Vector", operation="SCALE")

    def attribute(name, output):
        return w.output("ShaderNodeAttribute", (), output, attribute_type="INSTANCER", attribute_name=name)

    def transform(vector, vector_type, source, target):
        return w.output("ShaderNodeVectorTransform", (vector,), vector_type=vector_type, convert_from=source, convert_to=target)

    def along(axes, vector):
        parts = w.new("ShaderNodeSeparateXYZ", (vector,))
        terms = [scaled(axis, parts.outputs[index]) for index, axis in enumerate(axes)]
        return math("ADD", math("ADD", terms[0], terms[1]), terms[2])

    def unit_without(vector, axis):
        return math("NORMALIZE", math("SUBTRACT", vector, scaled(axis, w.output("ShaderNodeVectorMath", (vector, axis), "Value",
                                                                                   operation="DOT_PRODUCT"))))

    local = math("MULTIPLY", w.output("ShaderNodeTexCoord", name="Object"), attribute(SCALE, "Vector"))
    position = w.output("ShaderNodeNewGeometry", name="Position")
    origin = w.output("ShaderNodeObjectInfo", name="Location")
    view = transform((0.0, 0.0, 0.0), "POINT", "CAMERA", "WORLD")
    away = math("SUBTRACT", origin, view)
    if facing != fb.Facing.None_:
        rolled = w.output("ShaderNodeVectorRotate", (local, None, None, attribute(ROLL, "Fac")), rotation_type="X_AXIS")
    if facing == fb.Facing.CameraPlane:
        parts = w.new("ShaderNodeSeparateXYZ", (rolled,))
        negated = w.output("ShaderNodeMath", (parts.outputs["Y"], -1.0), operation="MULTIPLY")
        in_view = w.output("ShaderNodeCombineXYZ", (negated, parts.outputs["Z"], parts.outputs["X"]))
        offset = transform(in_view, "VECTOR", "CAMERA", "WORLD")
    elif facing == fb.Facing.CameraPosition:
        forward = math("NORMALIZE", away)
        up = unit_without((0.0, 0.0, 1.0), forward)
        offset = along((forward, math("CROSS_PRODUCT", up, forward), up), rolled)
    else:
        up = math("NORMALIZE", transform((0.0, 0.0, 1.0), "VECTOR", "OBJECT", "WORLD"))
        forward = unit_without(away, up)
        offset = along((forward, math("CROSS_PRODUCT", up, forward), up), rolled)
    moved = math("SUBTRACT", math("ADD", origin, offset), position)
    displacement = w.new("ShaderNodeVectorDisplacement", (moved, 0.0, 1.0), space="WORLD")
    w.tree.links.new(displacement.outputs[0], w.new("NodeGroupOutput").inputs[0])
    return group


class SlotMaterial:

    def __init__(self, node, slots):
        self.node = node
        self.slots = slots
        self.material = node.inputs["Material"].default_value

    @property
    def material(self):
        return self.node.inputs["Material"].default_value

    @material.setter
    def material(self, material):
        self.node.inputs["Material"].default_value = material
        for slot in self.slots:
            slot.material = material


def instancer(obj, holder, materials, animated, end=None, loop=(0, 0)):
    while len(obj.data.materials) < len(materials):
        obj.data.materials.append(None)
    for slot in obj.material_slots:
        slot.link = "OBJECT"
    tree = bpy.data.node_groups.new(obj.name, "GeometryNodeTree")
    tree.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    w = TreeWriter(tree)
    instance = w.output("GeometryNodeObjectInfo", (holder, False), "Geometry", transform_space="ORIGINAL")
    slots = []
    for index, material in enumerate(materials):
        selection = None
        if len(materials) > 1:
            selection = w.output("FunctionNodeCompare", (w.output("GeometryNodeInputMaterialIndex"), index), data_type="INT",
                                 operation="EQUAL")
        node = w.new("GeometryNodeSetMaterial", (instance, selection, material))
        instance = node.outputs["Geometry"]
        slots.append(SlotMaterial(node, (obj.material_slots[index], holder.material_slots[index])))
    shown = w.new("GeometryNodeGroup", node_tree=instances_group())
    w.tree.links.new(w.new("NodeGroupInput").outputs[0], shown.inputs["Points"])
    w.tree.links.new(instance, shown.inputs["Instance"])
    shown.inputs["Animated"].default_value = animated
    shown.inputs["End"].default_value = NO_END if end is None else end
    shown.inputs["Loop Start"].default_value, shown.inputs["Loop Frames"].default_value = loop
    w.tree.links.new(shown.outputs["Instances"], w.new("NodeGroupOutput").inputs[0])
    obj.modifiers.new("Instances", "NODES").node_group = tree
    return slots
