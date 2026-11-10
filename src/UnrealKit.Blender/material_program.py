import hashlib

import bpy

from . import deferred_update
from . import scene_generated as fb
from .node_groups import DEFINITIONS, GEOMETRY, MIRROR, NodeWriter, Value, address_name, shared_groups, texture_coordinate_name
from .node_layout import VECTOR_NODE, layout
from .session import text

Op = fb.MaterialOp
Geometry = fb.MaterialGeometry

MATH = {
    Op.Add: "ADD", Op.Sub: "SUBTRACT", Op.Mul: "MULTIPLY", Op.Div: "DIVIDE", Op.Min: "MINIMUM", Op.Max: "MAXIMUM",
    Op.Pow: "POWER", Op.Atan2: "ARCTAN2", Op.Mod: "FLOORED_MODULO", Op.Abs: "ABSOLUTE", Op.Floor: "FLOOR",
    Op.Ceil: "CEIL", Op.Fract: "FRACT", Op.Round: "ROUND", Op.Trunc: "TRUNC", Op.Sign: "SIGN", Op.Sqrt: "SQRT",
    Op.Sin: "SINE", Op.Cos: "COSINE", Op.Tan: "TANGENT", Op.Asin: "ARCSINE", Op.Acos: "ARCCOSINE",
    Op.Atan: "ARCTANGENT", Op.Less: "LESS_THAN",
}
VECTOR_MATH = {
    Op.Add: "ADD", Op.Sub: "SUBTRACT", Op.Mul: "MULTIPLY", Op.Div: "DIVIDE", Op.Min: "MINIMUM", Op.Max: "MAXIMUM",
    Op.Mod: "WRAP", Op.Abs: "ABSOLUTE", Op.Floor: "FLOOR", Op.Ceil: "CEIL", Op.Fract: "FRACTION", Op.Sin: "SINE",
    Op.Cos: "COSINE", Op.Tan: "TANGENT",
}
PLANAR_KEEPING = {Op.Add, Op.Sub, Op.Min, Op.Max, Op.Abs, Op.Floor, Op.Ceil, Op.Fract, Op.Sin, Op.Tan}
LEAVES = {Op.Constant, Op.Parameter, Op.Geometry, Op.GeometryDx, Op.GeometryDy, Op.Time, Op.Frame}
TERNARY = {Op.Select, Op.Clamp, Op.Mix, Op.Smoothstep, Op.MulAdd}
UNARY = {Op.Abs, Op.Floor, Op.Ceil, Op.Fract, Op.Round, Op.Trunc, Op.Sign, Op.Sqrt, Op.Sin, Op.Cos, Op.Tan, Op.Asin,
         Op.Acos, Op.Atan, Op.Exp2, Op.Log2, Op.Random, Op.Length, Op.Normalize, Op.Extract, Op.Texture,
         Op.TangentToWorld}
EXTENSIONS = {fb.TextureAddress.Wrap: "REPEAT", fb.TextureAddress.Clamp: "EXTEND", fb.TextureAddress.Mirror: "MIRROR"}
BOUNDS_MIN = "UnrealKitBoundsMin"
BOUNDS_MAX = "UnrealKitBoundsMax"
FAR_AWAY = 1.0e6
STRUCTURE = "unrealkit.program"
WARNINGS = "unrealkit.warnings"
TEXTURE_INDEX = "unrealkit.texture"
PARAMETER = "unrealkit.parameter"
INSTRUCTION_BYTES = 16


def parameter_input_name(name, alpha=False):
    return f"{name} A" if alpha else name


def is_direction(parameter):
    return parameter.ValueLength() > 1 and parameter.ValueAsNumpy()[:3].min() < 0.0


def parameter_value_path(node):
    return "vector" if node.bl_idname == VECTOR_NODE else "outputs[0].default_value"


def structure(program, blend_mode, two_sided, unlit):
    digest = hashlib.sha1()
    table = program._tab
    start = table.Vector(table.Offset(4))
    digest.update(bytes(table.Bytes[start:start + INSTRUCTION_BYTES * program.InstructionsLength()]))
    if program.ConstantsLength():
        digest.update(program.ConstantsAsNumpy().tobytes())
    for index in range(program.ParametersLength()):
        parameter = program.Parameters(index)
        digest.update(f"{text(parameter.Name())}/{parameter.ValueLength()}/{is_direction(parameter)};".encode())
    for index in range(program.TexturesLength()):
        texture = program.Textures(index)
        digest.update(f"{text(texture.Name())}/{texture.AddressU()}/{texture.AddressV()}/{texture.Nearest()};".encode())
    outputs = (program.BaseColor(), program.Normal(), program.Emission(), program.Roughness(), program.Metallic(),
               program.Specular(), program.Opacity(), program.Displacement(), program.Refraction())
    digest.update(repr((outputs, blend_mode, two_sided, unlit)).encode())
    return digest.hexdigest()


def image_keys(program):
    return [text(program.Textures(index).Image()) for index in range(program.TexturesLength())]


class ProgramMaterials:

    def __init__(self):
        self.groups = shared_groups()
        self.by_structure = {}
        for material in bpy.data.materials:
            shape = material.get(STRUCTURE)
            if shape is not None:
                self.by_structure.setdefault(shape, material)

    def material(self, name, program, images, blend_mode, two_sided, unlit):
        shape = structure(program, blend_mode, two_sided, unlit)
        template = self.by_structure.get(shape)
        if template is None:
            material = build_material(name, program, blend_mode, two_sided, unlit, self.groups)
            material[STRUCTURE] = shape
            self.by_structure[shape] = material
        else:
            material = template.copy()
            material.name = name
            animation = material.node_tree.animation_data
            if animation is not None:
                animation.action = None
        tree = material.node_tree
        warnings = [warning for warning in material[WARNINGS].split("\n") if warning]
        set_images(tree, image_keys(program), images, warnings)
        deferred_update.run(lambda: set_parameters(tree, program), tree)
        return material, warnings


def set_images(tree, keys, images, warnings):
    for node in tree.nodes:
        index = node.get(TEXTURE_INDEX)
        if index is None:
            continue
        image = images(keys[index])
        if image is None:
            warnings.append(f"The image {keys[index]} was not built, so it reads as black.")
        if node.image != image:
            node.image = image


def set_parameters(tree, program):
    for node in tree.nodes:
        found = node.get(PARAMETER)
        if found is None:
            continue
        index, component = found
        value = program.Parameters(index).ValueAsNumpy()
        if node.bl_idname == VECTOR_NODE:
            node.vector = tuple(float(v) for v in value[:3])
            continue
        node.outputs[0].default_value = float(value[component]) if component >= 0 else tuple(float(v) for v in value)


def build_material(name, program, blend_mode, two_sided, unlit, groups):
    material = bpy.data.materials.new(name)
    tree = material.node_tree
    tree.nodes.clear()
    warnings = []
    builder = ProgramTree(tree, program, warnings, unlit, groups)
    output = tree.nodes.new("ShaderNodeOutputMaterial")

    def edits():
        builder.declare()
        builder.build(output, blend_mode, two_sided)

    deferred_update.run(edits, tree)
    if builder.pending:
        deferred_update.run(builder.link_groups, tree)
    layout(tree, builder.parameter_column(), output)
    material_settings(material, blend_mode, two_sided, program.Displacement() >= 0)
    material[WARNINGS] = "\n".join(dict.fromkeys(warnings))
    return material


class ProgramTree(NodeWriter):
    def __init__(self, tree, program, warnings, unlit, groups):
        super().__init__(tree, groups)
        self.unlit = unlit
        self.program = program
        self.warnings = warnings
        self.constants = program.ConstantsAsNumpy() if program.ConstantsLength() else ()
        self.values = {}
        self.parameters = {}
        self.shared = {}
        self.uses = {}
        self.parameter_nodes = {}
        self.texture_nodes = []
        self.used = self.surface_instructions()

    def surface_instructions(self):
        program = self.program
        outputs = [program.Emission()] if self.unlit else [program.BaseColor(), program.Normal(), program.Emission(),
                                                           program.Roughness(), program.Metallic(), program.Specular()]
        for output in (program.Opacity(), program.Refraction(), program.Displacement()):
            if output >= 0:
                outputs.append(output)
        normal = program.Normal()
        tangent_normal = not self.unlit and program.Instructions(normal).Op() == Op.TangentToWorld
        if not self.unlit and (self.is_geometry(normal, Geometry.Normal) or tangent_normal):
            outputs.remove(normal)
        if tangent_normal:
            outputs.append(program.Instructions(normal).A())
        self.outputs = outputs
        used = set()
        pending = list(outputs)
        while pending:
            index = pending.pop()
            if index in used:
                continue
            used.add(index)
            pending.extend(self.operands(program.Instructions(index)))
        return used

    def declare(self):
        program = self.program
        parameters = {}
        alpha = set()
        for index in self.used:
            instruction = program.Instructions(index)
            if instruction.Op() == Op.Parameter:
                parameters.setdefault(instruction.Index(), index)
        for index in self.used:
            instruction = program.Instructions(index)
            if instruction.Op() != Op.Extract or instruction.Index() != 3:
                continue
            source = program.Instructions(instruction.A())
            if source.Op() == Op.Parameter:
                alpha.add(source.Index())
        for index in sorted(parameters, key=lambda index: text(program.Parameters(index).Name()).lower()):
            name = text(program.Parameters(index).Name())
            if program.Parameters(index).ValueLength() == 1:
                node = self.parameter_node("ShaderNodeValue", name, index, 0)
            elif is_direction(program.Parameters(index)):
                node = self.parameter_node(VECTOR_NODE, name, index, -1)
            else:
                node = self.parameter_node("ShaderNodeRGB", name, index, -1)
            alpha_node = None
            if index in alpha:
                alpha_node = self.parameter_node("ShaderNodeValue", parameter_input_name(name, alpha=True), index, 3)
            self.parameter_nodes[index] = (node, alpha_node)

    def parameter_node(self, kind, name, index, component):
        node = self.nodes.new(kind)
        node.name = node.label = name
        node[PARAMETER] = (index, component)
        return node

    def parameter_column(self):
        parameters = [node for pair in self.parameter_nodes.values() for node in pair if node is not None]
        return parameters + sorted(self.texture_nodes, key=lambda node: node.label.lower())

    def build(self, output, blend_mode, two_sided):
        program = self.program
        used = self.used
        for index in self.outputs:
            self.uses[index] = self.uses.get(index, 0) + 1
        for index in used:
            for operand in self.operands(program.Instructions(index)):
                self.uses[operand] = self.uses.get(operand, 0) + 1
        fused, inside = self.fusions()
        for index in sorted(used - inside):
            fusion = fused.get(index)
            self.values[index] = self.instruction(program.Instructions(index)) if fusion is None else self.fuse(*fusion)
        normal = program.Normal()
        tangent_normal = not self.unlit and program.Instructions(normal).Op() == Op.TangentToWorld
        principled = self.nodes.new("ShaderNodeBsdfPrincipled")
        inputs = principled.inputs
        self.connect(inputs["Emission Color"], self.values[self.program.Emission()])
        inputs["Emission Strength"].default_value = 1.0
        if self.unlit:
            inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1.0)
            inputs["Specular IOR Level"].default_value = 0.0
        else:
            self.connect(inputs["Base Color"], self.values[self.program.BaseColor()])
            self.connect(inputs["Metallic"], self.values[self.program.Metallic()])
            self.connect(inputs["Roughness"], self.values[self.program.Roughness()])
            self.connect(inputs["Specular IOR Level"], self.values[self.program.Specular()])
        if tangent_normal:
            self.link(self.group("Tangent To World", self.values[program.Instructions(normal).A()])[0], inputs["Normal"])
        elif not self.unlit and not self.is_geometry(normal, Geometry.Normal):
            self.connect(inputs["Normal"], self.vector_math("MULTIPLY", self.values[normal], Value(3, MIRROR)))
        if self.program.Opacity() >= 0:
            self.connect(inputs["Alpha"], self.values[self.program.Opacity()])
        surface = principled.outputs["BSDF"]
        if self.program.Refraction() >= 0:
            surface = self.refract(surface)
        self.link(self.blend(surface, blend_mode, two_sided), output.inputs["Surface"])
        if program.Displacement() >= 0:
            self.link(self.group("Displacement", self.values[program.Displacement()])[0], output.inputs["Displacement"])

    def fusions(self):
        program = self.program
        fused = {}
        inside = set()

        def single(index, op):
            instruction = program.Instructions(index)
            return instruction if instruction.Op() == op and self.uses.get(index) == 1 else None

        def is_constant(index, value):
            instruction = program.Instructions(index)
            return instruction.Op() == Op.Constant and instruction.Width() == 1 and self.constants[instruction.Index()] == value

        for index in self.used:
            instruction = program.Instructions(index)
            op, a = instruction.Op(), instruction.A()
            if instruction.Width() != 1:
                continue
            if op == Op.Exp2:
                product = single(a, Op.Mul)
                if product is None:
                    continue
                for logarithm, exponent in ((product.A(), product.B()), (product.B(), product.A())):
                    if single(logarithm, Op.Log2) is None:
                        continue
                    fused[index] = ("POWER", (program.Instructions(logarithm).A(), exponent))
                    inside |= {a, logarithm}
                    break
            elif op == Op.Sub:
                positive, negative = single(a, Op.Less), single(instruction.B(), Op.Less)
                if positive is None or negative is None or positive.B() != negative.A():
                    continue
                if is_constant(positive.A(), 0.0) and is_constant(negative.B(), 0.0):
                    fused[index] = ("SIGN", (positive.B(),))
                    inside |= {a, instruction.B()}
            elif op == Op.Sqrt:
                clamp = single(a, Op.Clamp)
                if clamp is None or not (is_constant(clamp.B(), 0.0) and is_constant(clamp.C(), 1.0)):
                    continue
                difference = single(clamp.A(), Op.Sub)
                if difference is None or not is_constant(difference.A(), 1.0):
                    continue
                dot = single(difference.B(), Op.Dot)
                if dot is None or dot.A() != dot.B():
                    continue
                vector = program.Instructions(dot.A())
                if vector.Op() != Op.Combine or vector.Width() != 2 or self.uses.get(dot.A()) != 2:
                    continue
                fused[index] = ("Normal Z", (vector.A(), vector.B()))
                inside |= {a, clamp.A(), difference.B(), dot.A()}
        return fused, inside

    def fuse(self, kind, operands):
        values = [self.values[operand] for operand in operands]
        if kind in DEFINITIONS:
            return Value(1, socket=self.group(kind, *values)[0])
        return self.math(kind, *values)

    def refract(self, surface):
        refraction = self.values[self.program.Refraction()]
        if self.program.Opacity() >= 0:
            refraction = self.vector_math("MULTIPLY", self.values[self.program.Opacity()], refraction)
        return self.group("Refraction", Value(1, socket=surface), refraction)[0]

    def blend(self, surface, blend_mode, two_sided):
        if blend_mode == "BLEND_Additive":
            surface = self.group("Additive", Value(1, socket=surface))[0]
        elif blend_mode == "BLEND_Modulate":
            modulate = self.nodes.new("ShaderNodeBsdfTransparent")
            self.connect(modulate.inputs["Color"], self.values[self.program.Emission()])
            surface = modulate.outputs[0]
        if not two_sided:
            surface = self.group("One Sided", Value(1, socket=surface))[0]
        return surface

    def operands(self, instruction):
        op = instruction.Op()
        if op in LEAVES:
            return []
        if op == Op.Combine:
            return [instruction.A(), instruction.B(), instruction.C()][:instruction.Width()]
        if op == Op.Texture and self.texture_coordinate(instruction.A()) is not None:
            return []
        if op in UNARY:
            return [instruction.A()]
        if op in TERNARY:
            return [instruction.A(), instruction.B(), instruction.C()]
        return [instruction.A(), instruction.B()]

    def texture_coordinate(self, index):
        instruction = self.program.Instructions(index)
        if instruction.Op() == Op.Geometry and Geometry.TexCoord0 <= instruction.Index() <= Geometry.TexCoord7:
            return instruction.Index() - Geometry.TexCoord0
        return None

    def is_geometry(self, index, attribute):
        instruction = self.program.Instructions(index)
        return instruction.Op() == Op.Geometry and instruction.Index() == attribute

    def instruction(self, instruction):
        op, width = instruction.Op(), instruction.Width()
        index, a, b, c = instruction.Index(), instruction.A(), instruction.B(), instruction.C()
        values = self.values
        if op == Op.Constant:
            return Value(width, tuple(float(v) for v in self.constants[index:index + width]))
        if op == Op.Parameter:
            return self.parameter(index)
        if op == Op.Texture:
            return self.texture(index, a)
        if op == Op.Geometry:
            return self.geometry(index, width)
        if op in (Op.GeometryDx, Op.GeometryDy):
            self.warnings.append("A screen-space derivative of the geometry reads as zero.")
            return Value(width, (0.0,) * width)
        if op == Op.Time:
            return Value(1, socket=self.shared_group("Time"))
        if op == Op.Frame:
            return Value(1, socket=self.shared_group("Frame"))
        if op == Op.Extract:
            return self.component(values[a], index)
        if op == Op.Combine:
            node = self.nodes.new("ShaderNodeCombineXYZ")
            for socket, source in zip(node.inputs, (a, b, c)[:width]):
                self.connect(socket, values[source])
            return Value(width, socket=node.outputs[0], planar=width == 2)
        if op in (Op.Exp2, Op.Log2):
            if op == Op.Exp2:
                return self.math("POWER", Value(1, (2.0,)), values[a])
            return self.math("LOGARITHM", values[a], Value(1, (2.0,)))
        if op == Op.Random:
            node = self.nodes.new("ShaderNodeTexWhiteNoise")
            node.noise_dimensions = "1D"
            self.connect(node.inputs["W"], values[a])
            return Value(1, socket=node.outputs["Value"])
        if op == Op.LessEqual:
            return Value(1, socket=self.group("Less Equal", values[a], values[b])[0])
        if op == Op.Equal:
            return self.math("COMPARE", values[a], values[b], Value(1, (0.0,)))
        if op == Op.NotEqual:
            return Value(1, socket=self.group("Not Equal", values[a], values[b])[0])
        if op == Op.Select:
            return self.mix(width, values[a], values[c], values[b], clamp=True)
        if op == Op.Mix:
            return self.mix(width, values[c], values[a], values[b], clamp=False)
        if op == Op.MulAdd:
            if width == 1:
                return self.math("MULTIPLY_ADD", values[a], values[b], values[c])
            result = self.vector_math("MULTIPLY_ADD", values[a], values[b], values[c], width=width)
            result.planar = (is_planar(values[a]) or is_planar(values[b])) and is_planar(values[c])
            return result
        if op == Op.Clamp:
            if width == 1 and self.saturates(a, b, c):
                values[a].socket.node.use_clamp = True
                return values[a]
            if width == 1:
                node = self.nodes.new("ShaderNodeClamp")
                for socket, source in zip(node.inputs, (a, b, c)):
                    self.connect(socket, values[source])
                return Value(1, socket=node.outputs[0])
            return Value(width, socket=self.group("Clamp Vector", values[a], values[b], values[c])[0])
        if op == Op.Smoothstep:
            return self.smoothstep(width, values[a], values[b], values[c])
        if op == Op.Dot:
            return self.vector_math("DOT_PRODUCT", self.planar(values[a]), self.planar(values[b]), output="Value")
        if op == Op.Length:
            return self.vector_math("LENGTH", self.planar(values[a]), output="Value")
        if op == Op.Distance:
            return self.vector_math("DISTANCE", self.planar(values[a]), self.planar(values[b]), output="Value")
        if op == Op.Normalize:
            return self.vector_math("NORMALIZE", self.planar(values[a]), width=width)
        if op == Op.Cross:
            return self.vector_math("CROSS_PRODUCT", values[a], values[b])
        if op == Op.Reflect:
            return self.vector_math("REFLECT", values[a], values[b])
        if op == Op.TangentToWorld:
            return Value(3, socket=self.group("Tangent To World", values[a])[1])
        operands = (values[a],) if op in UNARY else (values[a], values[b])
        if width == 1:
            return self.math(MATH[op], *operands)
        if op == Op.Mod:
            return self.vector_math("WRAP", values[a], values[b], Value(1, (0.0,)), width=width)
        result = self.vector_math(VECTOR_MATH[op], *operands, width=width)
        planar = [is_planar(operand) for operand in operands]
        result.planar = any(planar) if op == Op.Mul else all(planar) and op in PLANAR_KEEPING
        return result

    def saturates(self, value, low, high):
        source = self.values[value].socket
        return (self.values[low].constant == (0.0,) and self.values[high].constant == (1.0,) and source is not None
                and source.node.bl_idname == "ShaderNodeMath" and self.uses.get(value) == 1)

    def planar(self, value):
        if value.width != 2 or value.constant is not None or value.planar:
            return value
        return self.vector_math("MULTIPLY", value, Value(3, (1.0, 1.0, 0.0)), width=2)

    def mix(self, width, factor, a, b, clamp):
        node = self.nodes.new("ShaderNodeMix")
        kind = "Float" if width == 1 else "Vector"
        node.data_type = "FLOAT" if width == 1 else "VECTOR"
        node.clamp_factor = clamp
        if width > 1 and factor.width > 1:
            node.factor_mode = "NON_UNIFORM"
            self.connect(by_identifier(node.inputs, "Factor_Vector"), factor)
        else:
            self.connect(by_identifier(node.inputs, "Factor_Float"), factor)
        self.connect(by_identifier(node.inputs, f"A_{kind}"), a)
        self.connect(by_identifier(node.inputs, f"B_{kind}"), b)
        return Value(width, socket=by_identifier(node.outputs, f"Result_{kind}"))

    def smoothstep(self, width, value, low, high):
        node = self.nodes.new("ShaderNodeMapRange")
        node.interpolation_type = "SMOOTHSTEP"
        if width == 1:
            names = ("Value", "From Min", "From Max")
            output = "Result"
        else:
            node.data_type = "FLOAT_VECTOR"
            names = ("Vector", "From_Min_FLOAT3", "From_Max_FLOAT3")
            output = "Vector"
            self.connect(by_identifier(node.inputs, "To_Min_FLOAT3"), Value(1, (0.0,)))
            self.connect(by_identifier(node.inputs, "To_Max_FLOAT3"), Value(1, (1.0,)))
        for name, operand in zip(names, (value, low, high)):
            self.connect(by_identifier(node.inputs, name), operand)
        return Value(width, socket=by_identifier(node.outputs, output))

    def component(self, value, index):
        if value.constant is not None:
            return Value(1, (value.constant[index],))
        if value.width == 1:
            return value
        if index == 3:
            if value.alpha is None:
                raise RuntimeError("the fourth component of a vector without one is read")
            return Value(1, socket=value.alpha() if callable(value.alpha) else value.alpha)
        key = ("separate", value.socket.node.name, value.socket.identifier)
        node = self.shared.get(key)
        if node is None:
            node = self.shared[key] = self.nodes.new("ShaderNodeSeparateXYZ")
            self.link(value.socket, node.inputs[0])
        return Value(1, socket=node.outputs[index])

    def node(self, key, kind, **settings):
        node = self.shared.get(key)
        if node is None:
            node = self.shared[key] = self.new(kind, **settings)
        return node

    def shared_group(self, name):
        key = ("group", name)
        socket = self.shared.get(key)
        if socket is None:
            socket = self.shared[key] = self.group(name)[0]
        return socket

    def parameter(self, index):
        node, alpha = self.parameter_nodes[index]
        if node.bl_idname == "ShaderNodeValue":
            return Value(1, socket=node.outputs[0])
        return Value(4, socket=node.outputs[0], alpha=alpha.outputs[0] if alpha is not None else None)

    def texture(self, index, coordinate):
        texture = self.program.Textures(index)
        key = text(texture.Image())
        node = self.nodes.new("ShaderNodeTexImage")
        node.label = text(texture.Name()) or key.rsplit(".", 1)[-1]
        node[TEXTURE_INDEX] = index
        if text(texture.Name()):
            self.texture_nodes.append(node)
        addresses = (texture.AddressU(), texture.AddressV())
        if addresses[0] == addresses[1]:
            node.extension = EXTENSIONS[addresses[0]]
        else:
            node.extension = "EXTEND"
        if texture.Nearest():
            node.interpolation = "Closest"
        self.connect(node.inputs["Vector"], self.texture_uv(coordinate, addresses))
        return Value(4, socket=node.outputs["Color"], alpha=node.outputs["Alpha"])

    def texture_uv(self, coordinate, addresses):
        key = ("uv of", coordinate, addresses[0] != addresses[1] and addresses)
        found = self.shared.get(key)
        if found is not None:
            return found
        channel = self.texture_coordinate(coordinate)
        if channel is not None:
            uv = Value(2, socket=self.node(("uv", channel), "ShaderNodeUVMap", uv_map=f"UV{channel}").outputs["UV"],
                       planar=True)
        else:
            uv = self.shared.get(("flipped", coordinate))
            if uv is None:
                uv = self.shared[("flipped", coordinate)] = self.flip_v(self.values[coordinate])
        if addresses[0] != addresses[1]:
            uv = self.address_axes(uv, addresses)
        self.shared[key] = uv
        return uv

    def address_axes(self, uv, addresses):
        return Value(2, socket=self.group(address_name(addresses), uv)[0], planar=True)

    def flip_v(self, value):
        return Value(2, socket=self.group("Flip V", value)[0], planar=value.planar)

    def geometry(self, attribute, width):
        if attribute in GEOMETRY:
            return Value(3, socket=self.shared_group(GEOMETRY[attribute][0]))
        if Geometry.TexCoord0 <= attribute <= Geometry.TexCoord7:
            return Value(2, socket=self.shared_group(texture_coordinate_name(attribute - Geometry.TexCoord0)), planar=True)
        if attribute == Geometry.VertexColor:
            node = self.node("color", "ShaderNodeVertexColor", layer_name="COL0")
            return Value(4, socket=node.outputs["Color"], alpha=node.outputs["Alpha"])
        if attribute == Geometry.SceneDepth:
            return Value(1, (FAR_AWAY,))
        if attribute in (Geometry.CameraFilmScale, Geometry.ViewSize):
            name = "Film Scale" if attribute == Geometry.CameraFilmScale else "View Size"
            return Value(2, socket=self.shared_group(name), planar=True)
        if attribute in (Geometry.ObjectBoundsMin, Geometry.ObjectBoundsMax):
            name = BOUNDS_MIN if attribute == Geometry.ObjectBoundsMin else BOUNDS_MAX
            node = self.node(("bounds", name), "ShaderNodeAttribute", attribute_type="OBJECT", attribute_name=name)
            return Value(3, socket=node.outputs["Vector"])
        if Geometry.CustomData0 <= attribute <= Geometry.CustomData8:
            return Value(width, (0.0,) * width)
        self.warnings.append(f"The geometry input {attribute} has no Blender counterpart and reads as zero.")
        return Value(width, (0.0,) * width)


def is_planar(value):
    if value.constant is not None:
        return value.width == 2 or value.constant == (0.0,)
    return value.planar


def by_identifier(sockets, identifier):
    return next(socket for socket in sockets if socket.identifier == identifier)


def material_settings(material, blend_mode, two_sided, displaced):
    if not two_sided:
        material.use_backface_culling = True
    if blend_mode in ("BLEND_Masked", "BLEND_AlphaHoldout"):
        material.surface_render_method = "DITHERED"
    elif blend_mode not in ("", "BLEND_Opaque"):
        material.surface_render_method = "BLENDED"
    if displaced:
        material.displacement_method = "DISPLACEMENT"
