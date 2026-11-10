import itertools
import math

import bpy

from . import scene_generated as fb
from .node_layout import layout

Geometry = fb.MaterialGeometry
MIRROR = (1.0, -1.0, 1.0)
CENTIMETRES = (100.0, -100.0, 100.0)
METRES = (0.01, -0.01, 0.01)
GROUP = "unrealkit.group"
#! Unreal's reverse culling flips which face a one-sided surface draws without touching the shared mesh.
REVERSE_CULLING = "unrealkit_reverse_culling"
SOCKET_TYPES = {"FLOAT": "NodeSocketFloat", "VECTOR": "NodeSocketVector", "COLOR": "NodeSocketColor",
                "SHADER": "NodeSocketShader"}
ADDRESS_NAMES = {fb.TextureAddress.Wrap: "Wrap", fb.TextureAddress.Clamp: "Clamp", fb.TextureAddress.Mirror: "Mirror"}


class Value:

    def __init__(self, width, constant=None, socket=None, alpha=None, planar=False):
        self.width = width
        self.constant = constant
        self.socket = socket
        self.alpha = alpha
        self.planar = planar


class GroupSocket:

    def __init__(self, node, name, output):
        self.node = node
        self.identifier = name
        self.output = output

    def resolve(self):
        return (self.node.outputs if self.output else self.node.inputs)[self.identifier]


class NodeWriter:

    def __init__(self, tree, groups=None):
        self.tree = tree
        self.nodes = tree.nodes
        self.links = tree.links
        self.groups = groups
        self.pending = []

    def new(self, kind, **settings):
        node = self.nodes.new(kind)
        for name, setting in settings.items():
            setattr(node, name, setting)
        return node

    def link(self, source, target):
        self.connect(target, Value(1, socket=source))

    def connect(self, socket, value):
        if isinstance(socket, GroupSocket) or isinstance(value.socket, GroupSocket):
            self.pending.append((socket, value))
            return
        if value.constant is None:
            self.links.new(value.socket, socket)
            return
        constant = value.constant
        size = len(socket.default_value) if hasattr(socket.default_value, "__len__") else 1
        if size == 1:
            if len(constant) != 1:
                raise RuntimeError(f"a constant of {len(constant)} components goes to the scalar input {socket.name}")
            socket.default_value = constant[0]
            return
        values = constant * size if len(constant) == 1 else (constant + (0.0,) * size)[:size]
        if size == 4 and len(constant) < 4:
            values = values[:3] + (1.0,)
        socket.default_value = values

    def link_groups(self):
        pending, self.pending = self.pending, []
        for socket, value in pending:
            if isinstance(socket, GroupSocket):
                socket = socket.resolve()
            if isinstance(value.socket, GroupSocket):
                value = Value(value.width, socket=value.socket.resolve())
            self.connect(socket, value)

    def math(self, operation, *operands):
        node = self.new("ShaderNodeMath", operation=operation)
        for socket, operand in zip(node.inputs, operands):
            self.connect(socket, operand)
        return Value(1, socket=node.outputs[0])

    def vector_math(self, operation, *operands, output="Vector", width=3):
        node = self.new("ShaderNodeVectorMath", operation=operation)
        for socket, operand in zip(node.inputs, operands):
            self.connect(socket, operand)
        return Value(1 if output == "Value" else width, socket=node.outputs[output])

    def vector_transform(self, kind, source, target, vector):
        node = self.new("ShaderNodeVectorTransform", vector_type=kind, convert_from=source, convert_to=target)
        node.inputs[0].default_value = vector
        return Value(3, socket=node.outputs[0])

    def group(self, name, *operands):
        definition = DEFINITIONS[name]
        node = self.new("ShaderNodeGroup", node_tree=self.groups[name])
        for (input_name, _), operand in zip(definition.inputs, operands):
            self.connect(GroupSocket(node, input_name, output=False), operand)
        return [GroupSocket(node, output_name, output=True) for output_name, _ in definition.outputs]


class Definition:

    def __init__(self, name, inputs, outputs, body):
        self.name = name
        self.inputs = inputs
        self.outputs = outputs
        self.body = body

    def build(self):
        tree = bpy.data.node_groups.new(self.name, "ShaderNodeTree")
        tree[GROUP] = self.name
        for name, kind in self.inputs:
            tree.interface.new_socket(name, in_out="INPUT", socket_type=SOCKET_TYPES[kind])
        for name, kind in self.outputs:
            tree.interface.new_socket(name, in_out="OUTPUT", socket_type=SOCKET_TYPES[kind])
        writer = NodeWriter(tree)
        source = writer.new("NodeGroupInput")
        output = writer.new("NodeGroupOutput")
        inputs = {name: Value(3 if kind == "VECTOR" else 1, socket=source.outputs[name]) for name, kind in self.inputs}
        for (name, _), socket in zip(self.outputs, self.body(writer, inputs)):
            writer.link(socket, output.inputs[name])
        layout(tree, [source], output)
        return tree


def shared_groups():
    groups = {tree[GROUP]: tree for tree in bpy.data.node_groups if tree.get(GROUP) in DEFINITIONS}
    for name, definition in DEFINITIONS.items():
        if name not in groups:
            groups[name] = definition.build()
    return groups


def less_equal(w, inputs):
    return [w.math("SUBTRACT", Value(1, (1.0,)), w.math("GREATER_THAN", inputs["A"], inputs["B"])).socket]


def not_equal(w, inputs):
    equal = w.math("COMPARE", inputs["A"], inputs["B"], Value(1, (0.0,)))
    return [w.math("SUBTRACT", Value(1, (1.0,)), equal).socket]


def clamp_vector(w, inputs):
    return [w.vector_math("MINIMUM", w.vector_math("MAXIMUM", inputs["Vector"], inputs["Min"]), inputs["Max"]).socket]


def normal_z(w, inputs):
    vector = w.new("ShaderNodeCombineXYZ")
    w.connect(vector.inputs[0], inputs["X"])
    w.connect(vector.inputs[1], inputs["Y"])
    length = w.vector_math("DOT_PRODUCT", Value(3, socket=vector.outputs[0]), Value(3, socket=vector.outputs[0]),
                           output="Value")
    rest = w.math("SUBTRACT", Value(1, (1.0,)), length)
    rest.socket.node.use_clamp = True
    return [w.math("SQRT", rest).socket]


def flip_v(w, inputs):
    return [w.vector_math("MULTIPLY_ADD", inputs["Vector"], Value(3, MIRROR), Value(3, (0.0, 1.0, 0.0))).socket]


def tangent_to_world(w, inputs):
    color = w.vector_math("MULTIPLY_ADD", inputs["Vector"], Value(3, (0.5, -0.5, 0.5)), Value(3, (0.5, 0.5, 0.5)))
    normal_map = w.new("ShaderNodeNormalMap", uv_map="UV0")
    w.link(color.socket, normal_map.inputs["Color"])
    normal = normal_map.outputs["Normal"]
    return [normal, w.vector_math("MULTIPLY", Value(3, socket=normal), Value(3, MIRROR)).socket]


def address(modes):

    def body(w, inputs):
        separate = w.new("ShaderNodeSeparateXYZ")
        w.connect(separate.inputs[0], inputs["Vector"])
        combine = w.new("ShaderNodeCombineXYZ")
        for axis, mode in enumerate(modes):
            value = Value(1, socket=separate.outputs[axis])
            if mode == fb.TextureAddress.Wrap:
                value = w.math("FRACT", value)
            elif mode == fb.TextureAddress.Mirror:
                value = w.math("PINGPONG", value, Value(1, (1.0,)))
            w.connect(combine.inputs[axis], value)
        return [combine.outputs[0]]

    return body


def address_name(modes):
    return "Address " + " ".join(ADDRESS_NAMES[mode] for mode in modes)


def displacement(w, inputs):
    node = w.new("ShaderNodeVectorDisplacement", space="WORLD")
    node.inputs["Scale"].default_value = 1.0
    w.connect(node.inputs["Vector"], w.vector_math("MULTIPLY", inputs["Offset"], Value(3, METRES)))
    return [node.outputs["Displacement"]]


def mix_transparent(w, surface, factor):
    mix = w.new("ShaderNodeMixShader")
    w.link(factor, mix.inputs[0])
    w.connect(mix.inputs[1], surface)
    w.link(w.new("ShaderNodeBsdfTransparent").outputs[0], mix.inputs[2])
    return mix.outputs[0]


def one_sided(w, inputs):
    reverse = w.new("ShaderNodeAttribute", attribute_type="OBJECT", attribute_name=REVERSE_CULLING)
    back = w.math("ABSOLUTE", w.math("SUBTRACT", Value(1, socket=w.new("ShaderNodeNewGeometry").outputs["Backfacing"]),
                                     Value(1, socket=reverse.outputs["Fac"])))
    return [mix_transparent(w, inputs["Shader"], back.socket)]


def additive(w, inputs):
    add = w.new("ShaderNodeAddShader")
    w.link(w.new("ShaderNodeBsdfTransparent").outputs[0], add.inputs[0])
    w.connect(add.inputs[1], inputs["Shader"])
    return [mix_transparent(w, Value(1, socket=add.outputs[0]), w.new("ShaderNodeLightPath").outputs["Is Shadow Ray"])]


def refraction(w, inputs):
    through = w.new("ShaderNodeBsdfTransparent")
    w.connect(through.inputs["Color"], inputs["Color"])
    add = w.new("ShaderNodeAddShader")
    w.connect(add.inputs[0], inputs["Shader"])
    w.link(through.outputs[0], add.inputs[1])
    return [add.outputs[0]]


DRIVER_VARIABLES = {"fps": "render.fps", "angle": "camera.data.angle", "width": "render.resolution_x",
                    "height": "render.resolution_y", "scale": "render.resolution_percentage",
                    "camera_x": "camera.matrix_world[3][0]", "camera_y": "camera.matrix_world[3][1]",
                    "camera_z": "camera.matrix_world[3][2]"}
ANGLE = f"(angle if angle > 0 else {2.0 * math.atan(18.0 / 50.0)!r})"


def drive(socket, expression):
    driver = socket.driver_add("default_value").driver
    driver.expression = expression
    for name, path in DRIVER_VARIABLES.items():
        if name not in expression:
            continue
        variable = driver.variables.new()
        variable.name = name
        variable.targets[0].id_type = "SCENE"
        variable.targets[0].id = bpy.context.scene
        variable.targets[0].data_path = path


def driven(expression):
    def body(w, _):
        node = w.new("ShaderNodeValue")
        drive(node.outputs[0], expression)
        return [node.outputs[0]]

    return body


def driven_pair(expressions):

    def body(w, _):
        node = w.new("ShaderNodeCombineXYZ")
        for socket, expression in zip(node.inputs, expressions):
            drive(socket, expression)
        return [node.outputs[0]]

    return body


def texture_coordinate_name(channel):
    return f"Texture Coordinate {channel}"


def texture_coordinate(channel):

    def body(w, _):
        uv = w.new("ShaderNodeUVMap", uv_map=f"UV{channel}")
        return flip_v(w, {"Vector": Value(2, socket=uv.outputs["UV"])})

    return body


def lower_hemisphere(w, inputs):
    axes = w.new("ShaderNodeSeparateXYZ")
    w.link(w.new("ShaderNodeTexCoord").outputs["Generated"], axes.inputs["Vector"])
    below = w.math("LESS_THAN", Value(1, socket=axes.outputs["Z"]), Value(1, (0.0,)))
    mix = w.new("ShaderNodeMix", data_type="RGBA")
    w.connect(mix.inputs["Factor"], w.math("MULTIPLY", below, inputs["Lower Alpha"]))
    w.connect(mix.inputs["A"], inputs["Color"])
    w.connect(mix.inputs["B"], inputs["Lower Color"])
    return [mix.outputs["Result"]]


def cube_strip(w, inputs):
    one = Value(1, (1.0,))
    zero = Value(1, (0.0,))
    axes = w.new("ShaderNodeSeparateXYZ")
    w.connect(axes.inputs["Vector"], inputs["Vector"])
    x = Value(1, socket=axes.outputs["X"])
    y = w.math("MULTIPLY", Value(1, socket=axes.outputs["Y"]), Value(1, (-1.0,)))
    z = Value(1, socket=axes.outputs["Z"])
    ax, ay, az = (w.math("ABSOLUTE", value) for value in (x, y, z))
    on_x = w.math("MULTIPLY", w.math("GREATER_THAN", ax, w.math("MULTIPLY", ay, Value(1, (0.9999,)))),
                  w.math("GREATER_THAN", ax, w.math("MULTIPLY", az, Value(1, (0.9999,)))))
    on_y = w.math("MULTIPLY", w.math("SUBTRACT", one, on_x),
                  w.math("GREATER_THAN", ay, w.math("MULTIPLY", az, Value(1, (0.9999,)))))
    on_z = w.math("SUBTRACT", w.math("SUBTRACT", one, on_x), on_y)
    below_x, below_y, below_z = (w.math("LESS_THAN", value, zero) for value in (x, y, z))
    sign_x, sign_y, sign_z = (w.math("MULTIPLY_ADD", below, Value(1, (-2.0,)), one) for below in (below_x, below_y, below_z))
    face = w.math("ADD", w.math("MULTIPLY", on_x, below_x),
                  w.math("ADD", w.math("MULTIPLY", on_y, w.math("ADD", below_y, Value(1, (2.0,)))),
                         w.math("MULTIPLY", on_z, w.math("ADD", below_z, Value(1, (4.0,))))))
    minus_z = w.math("MULTIPLY", z, Value(1, (-1.0,)))
    minus_y = w.math("MULTIPLY", y, Value(1, (-1.0,)))
    s = w.math("ADD", w.math("MULTIPLY", on_x, w.math("MULTIPLY", minus_z, sign_x)),
               w.math("ADD", w.math("MULTIPLY", on_y, x), w.math("MULTIPLY", on_z, w.math("MULTIPLY", x, sign_z))))
    t = w.math("ADD", w.math("MULTIPLY", w.math("ADD", on_x, on_z), minus_y), w.math("MULTIPLY", on_y, w.math("MULTIPLY", z, sign_y)))
    major = w.math("ADD", w.math("MULTIPLY", on_x, ax), w.math("ADD", w.math("MULTIPLY", on_y, ay), w.math("MULTIPLY", on_z, az)))
    half_texel = w.math("DIVIDE", Value(1, (0.5,)), inputs["Face Size"])
    upper = w.math("SUBTRACT", one, half_texel)

    def unit(value):
        spread = w.math("MULTIPLY_ADD", w.math("DIVIDE", value, major), Value(1, (0.5,)), Value(1, (0.5,)))
        return w.math("MINIMUM", w.math("MAXIMUM", spread, half_texel), upper)

    #! Blender shows the first row of a DDS at the top, so the strip's first face is its top sixth.
    v = w.math("SUBTRACT", one, w.math("DIVIDE", w.math("ADD", face, unit(t)), Value(1, (6.0,))))
    uv = w.new("ShaderNodeCombineXYZ")
    w.connect(uv.inputs["X"], unit(s))
    w.connect(uv.inputs["Y"], v)
    return [uv.outputs["Vector"]]


def sky_strength(w, inputs):
    rays = w.new("ShaderNodeLightPath").outputs
    bounced = w.math("GREATER_THAN", Value(1, socket=rays["Ray Depth"]), Value(1, (0.5,)))
    indirect = w.math("SUBTRACT", inputs["Indirect Intensity"], Value(1, (1.0,)))
    scale = w.math("MULTIPLY_ADD", bounced, indirect, Value(1, (1.0,)))
    unseen = w.math("SUBTRACT", Value(1, (1.0,)), Value(1, socket=rays["Is Camera Ray"]))
    return [w.math("MULTIPLY", w.math("MULTIPLY", scale, unseen), inputs["Intensity"]).socket]


def scaled(socket, scale):
    return lambda w: w.vector_math("MULTIPLY", Value(3, socket=socket(w)), Value(3, scale)).socket


def geometry_output(name):
    return lambda w: w.new("ShaderNodeNewGeometry").outputs[name]


def tangent_axis(color):
    def socket(w):
        node = w.new("ShaderNodeNormalMap", uv_map="UV0")
        node.inputs["Color"].default_value = color
        return node.outputs["Normal"]

    return socket


#! A displacement and a bounced ray have no camera ray to trace back, so they read the scene camera instead.
def camera_position(w):
    geometry = w.new("ShaderNodeNewGeometry")
    camera = w.new("ShaderNodeCameraData")
    traced = w.vector_math("MULTIPLY_ADD", Value(3, socket=geometry.outputs["Incoming"]),
                           Value(1, socket=camera.outputs["View Distance"]),
                           Value(3, socket=geometry.outputs["Position"]))
    scene = Value(3, socket=driven_pair(("camera_x", "camera_y", "camera_z"))(w, None)[0])
    seen = Value(1, socket=w.new("ShaderNodeLightPath").outputs["Is Camera Ray"])
    return w.vector_math("MULTIPLY_ADD", w.vector_math("SUBTRACT", traced, scene), seen, scene).socket


def transformed(kind, source, target, vector):
    return lambda w: w.vector_transform(kind, source, target, vector).socket


def axis_unit(axis):
    unit = [0.0, 0.0, 0.0]
    unit[axis] = MIRROR[axis]
    return tuple(unit)


GEOMETRY = {
    Geometry.Position: ("Position", scaled(geometry_output("Position"), CENTIMETRES)),
    Geometry.Normal: ("Normal", scaled(geometry_output("Normal"), MIRROR)),
    Geometry.LocalPosition: ("Local Position",
                             scaled(lambda w: w.new("ShaderNodeTexCoord").outputs["Object"], CENTIMETRES)),
    #! Unreal's bitangent points along V, which is down where Blender's is up.
    Geometry.Tangent: ("Tangent", scaled(tangent_axis((1.0, 0.5, 0.5, 1.0)), MIRROR)),
    Geometry.Bitangent: ("Bitangent", scaled(tangent_axis((0.5, 1.0, 0.5, 1.0)), (-1.0, 1.0, -1.0))),
    Geometry.CameraPosition: ("Camera Position", scaled(camera_position, CENTIMETRES)),
    Geometry.CameraForward: ("Camera Forward", scaled(transformed("VECTOR", "CAMERA", "WORLD", (0.0, 0.0, 1.0)), MIRROR)),
    Geometry.CameraRight: ("Camera Right", scaled(transformed("VECTOR", "CAMERA", "WORLD", (1.0, 0.0, 0.0)), MIRROR)),
    Geometry.CameraUp: ("Camera Up", scaled(transformed("VECTOR", "CAMERA", "WORLD", (0.0, 1.0, 0.0)), MIRROR)),
    Geometry.ViewDirection: ("View Direction", scaled(geometry_output("Incoming"), (-1.0, 1.0, -1.0))),
    Geometry.ObjectOrigin: ("Object Origin", scaled(transformed("POINT", "OBJECT", "WORLD", (0.0, 0.0, 0.0)), CENTIMETRES)),
    Geometry.WorldOrigin: ("World Origin", scaled(transformed("POINT", "WORLD", "OBJECT", (0.0, 0.0, 0.0)), CENTIMETRES)),
}
for _axis, _letter in enumerate("XYZ"):
    GEOMETRY[Geometry.ObjectAxisX + _axis] = (f"Object Axis {_letter}",
                                              scaled(transformed("VECTOR", "OBJECT", "WORLD", axis_unit(_axis)), MIRROR))
    GEOMETRY[Geometry.WorldAxisX + _axis] = (f"World Axis {_letter}",
                                             scaled(transformed("VECTOR", "WORLD", "OBJECT", axis_unit(_axis)), MIRROR))

VECTOR = (("Vector", "VECTOR"),)
VALUE = (("Value", "FLOAT"),)
SHADER = (("Shader", "SHADER"),)
SCALARS = (("A", "FLOAT"), ("B", "FLOAT"))
DEFINITIONS = {definition.name: definition for definition in [
    Definition("Less Equal", SCALARS, (("Value", "FLOAT"),), less_equal),
    Definition("Not Equal", SCALARS, (("Value", "FLOAT"),), not_equal),
    Definition("Clamp Vector", VECTOR + (("Min", "VECTOR"), ("Max", "VECTOR")), VECTOR, clamp_vector),
    Definition("Flip V", VECTOR, VECTOR, flip_v),
    Definition("Normal Z", (("X", "FLOAT"), ("Y", "FLOAT")), (("Z", "FLOAT"),), normal_z),
    Definition("Tangent To World", VECTOR, (("Normal", "VECTOR"), ("Unreal Normal", "VECTOR")), tangent_to_world),
    Definition("Displacement", (("Offset", "VECTOR"),), (("Displacement", "VECTOR"),), displacement),
    Definition("One Sided", SHADER, SHADER, one_sided),
    Definition("Additive", SHADER, SHADER, additive),
    Definition("Refraction", SHADER + (("Color", "COLOR"),), SHADER, refraction),
    *(Definition(address_name(modes), VECTOR, VECTOR, address(modes))
      for modes in itertools.permutations(ADDRESS_NAMES, 2)),
    *(Definition(name, (), VECTOR, lambda w, _, socket=socket: [socket(w)]) for name, socket in GEOMETRY.values()),
    *(Definition(texture_coordinate_name(channel), (), VECTOR, texture_coordinate(channel)) for channel in range(8)),
    Definition("Time", (), VALUE, driven("frame / fps")),
    Definition("Frame", (), VALUE, driven("frame")),
    Definition("Film Scale", (), VECTOR, driven_pair((f"tan({ANGLE} / 2) * min(1, width / height)",
                                                      f"tan({ANGLE} / 2) * min(1, height / width)"))),
    Definition("View Size", (), VECTOR, driven_pair(("width * scale / 100", "height * scale / 100"))),
    Definition("Lower Hemisphere", (("Color", "COLOR"), ("Lower Color", "COLOR"), ("Lower Alpha", "FLOAT")),
               (("Color", "COLOR"),), lower_hemisphere),
    Definition("Cube Strip", VECTOR + (("Face Size", "FLOAT"),), VECTOR, cube_strip),
    Definition("Sky Strength", (("Intensity", "FLOAT"), ("Indirect Intensity", "FLOAT")), VALUE, sky_strength),
]}
