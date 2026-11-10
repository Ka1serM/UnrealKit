#include "UnrealKit/Materials/GlslWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

namespace UnrealKit::Materials {

using Op = scene::stream::MaterialOp;
using Geometry = scene::stream::MaterialGeometry;
using Instruction = scene::stream::MaterialInstruction;

const char* const glslFragmentInputs = R"(
in vec3 materialPosition;
in vec3 materialNormal;
in vec3 materialLocalPosition;
in vec4 materialTangent;
in vec4 materialTexCoords01;
in vec4 materialTexCoords23;
in vec4 materialVertexColor;
)";

const char* const glslVertexOutputs = R"(
out vec3 materialPosition;
out vec3 materialNormal;
out vec3 materialLocalPosition;
out vec4 materialTangent;
out vec4 materialTexCoords01;
out vec4 materialTexCoords23;
out vec4 materialVertexColor;
vec3 materialFrameOrigin;
mat3 materialFrameAxes;
)";

const char* const glslCommon = R"(
// Object to world and back, in Unreal space.
uniform mat4 materialObjectToWorld;
uniform mat4 materialWorldToObject;
uniform vec3 materialBoundsMin;
uniform vec3 materialBoundsMax;
uniform vec3 materialCameraPosition;
uniform vec3 materialCameraForward;
uniform vec3 materialCameraRight;
uniform vec3 materialCameraUp;
uniform vec2 materialFilmScale;
uniform vec2 materialViewSize;
uniform float materialTime;
uniform float materialFrame;
uniform bool materialTwoSided;

struct Surface {
    vec3 baseColor;
    vec3 normal;
    vec3 emission;
    float roughness;
    float metallic;
    float specular;
    float opacity;
    vec3 refraction;
};

// The unit normal of the triangle's side that the eye sees, for a two-sided surface.
vec3 materialFacingNormal() {
    vec3 normal = normalize(materialNormal);
    if (materialTwoSided && dot(normal, materialCameraPosition - materialPosition) < 0.0)
        normal = -normal;
    return normal;
}

vec3 materialTangentAxis() {
    vec3 normal = materialFacingNormal();
    vec3 tangent = materialTangent.xyz - normal * dot(materialTangent.xyz, normal);
    return dot(tangent, tangent) > 0.0 ? normalize(tangent) : vec3(0.0);
}

// Unreal's TangentY: cross(TangentZ, TangentX) * TangentZ.w.
vec3 materialBitangentAxis() {
    return cross(materialFacingNormal(), materialTangentAxis()) * (materialTangent.w < 0.0 ? -1.0 : 1.0);
}

vec3 materialTangentToWorld(vec3 vector) {
    vec3 world = vector.x * materialTangentAxis() + vector.y * materialBitangentAxis() + vector.z * materialFacingNormal();
    return dot(world, world) > 0.0 ? normalize(world) : materialFacingNormal();
}

vec4 materialDecodeSrgb(vec4 color) {
    vec3 low = color.rgb / 12.92;
    vec3 high = pow((color.rgb + 0.055) / 1.055, vec3(2.4));
    return vec4(mix(high, low, vec3(lessThanEqual(color.rgb, vec3(0.04045)))), color.a);
}

// HLSL's pow, which is NaN for a negative base; zero here, so that one bad lane does not blacken the pixel.
vec4 materialPow(vec4 base, vec4 exponent) {
    return mix(pow(max(base, vec4(1e-30)), exponent), vec4(0.0), lessThanEqual(base, vec4(0.0)));
}

vec4 materialSmoothstep(vec4 low, vec4 high, vec4 value) {
    vec4 t = clamp((value - low) / (high - low), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float materialRandom(vec4 seed) {
    return fract(sin(dot(seed, vec4(12.9898, 78.233, 37.719, 4.581))) * 43758.5453);
}
)";

namespace {

std::string literal(float value) {
    if (std::isnan(value))
        value = 0.0f;
    value = std::clamp(value, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max());
    std::string text = std::format("{}", value);
    if (text.find_first_of(".e") == std::string::npos)
        text += ".0";
    return text;
}

std::string lanes(const std::string& value, const uint32_t width) {
    static constexpr std::string_view swizzles[] = {"x", "x", "xy", "xyz", "xyzw"};
    return std::format("{}.{}", value, swizzles[std::min<uint32_t>(width, 4)]);
}

std::string widen(const std::string& value, const uint32_t width) {
    switch (width) {
    case 1:
        return std::format("vec4({})", value);
    case 2:
        return std::format("vec4({}, 0.0, 0.0)", value);
    case 3:
        return std::format("vec4({}, 0.0)", value);
    default:
        return value;
    }
}

std::string geometry(const Geometry attribute) {
    const auto index = static_cast<int>(attribute);
    if (attribute >= Geometry::TexCoord0 && attribute <= Geometry::TexCoord7) {
        const int set = index - static_cast<int>(Geometry::TexCoord0);
        if (set >= 4)
            return "vec4(0.0)";
        return std::format("vec4(materialTexCoords{}.{}, 0.0, 0.0)", set < 2 ? "01" : "23", set % 2 == 0 ? "xy" : "zw");
    }
    if (attribute >= Geometry::CustomData0 && attribute <= Geometry::CustomData8)
        return "vec4(0.0)";
    switch (attribute) {
    case Geometry::Position:
        return "vec4(materialPosition, 0.0)";
    case Geometry::Normal:
        return "vec4(materialFacingNormal(), 0.0)";
    case Geometry::Tangent:
        return "vec4(materialTangentAxis(), 0.0)";
    case Geometry::Bitangent:
        return "vec4(materialBitangentAxis(), 0.0)";
    case Geometry::VertexColor:
        return "materialVertexColor";
    case Geometry::ObjectAxisX:
        return "vec4(materialObjectToWorld[0].xyz, 0.0)";
    case Geometry::ObjectAxisY:
        return "vec4(materialObjectToWorld[1].xyz, 0.0)";
    case Geometry::ObjectAxisZ:
        return "vec4(materialObjectToWorld[2].xyz, 0.0)";
    case Geometry::ObjectOrigin:
        return "vec4(materialObjectToWorld[3].xyz, 0.0)";
    case Geometry::WorldAxisX:
        return "vec4(materialWorldToObject[0].xyz, 0.0)";
    case Geometry::WorldAxisY:
        return "vec4(materialWorldToObject[1].xyz, 0.0)";
    case Geometry::WorldAxisZ:
        return "vec4(materialWorldToObject[2].xyz, 0.0)";
    case Geometry::WorldOrigin:
        return "vec4(materialWorldToObject[3].xyz, 0.0)";
    case Geometry::ObjectBoundsMin:
        return "vec4(materialBoundsMin, 0.0)";
    case Geometry::ObjectBoundsMax:
        return "vec4(materialBoundsMax, 0.0)";
    case Geometry::CameraPosition:
        return "vec4(materialCameraPosition, 0.0)";
    case Geometry::CameraForward:
        return "vec4(materialCameraForward, 0.0)";
    case Geometry::CameraRight:
        return "vec4(materialCameraRight, 0.0)";
    case Geometry::CameraUp:
        return "vec4(materialCameraUp, 0.0)";
    case Geometry::SceneDepth:
        return "vec4(1e6)";
    case Geometry::ViewDirection:
        return "vec4(normalize(materialPosition - materialCameraPosition), 0.0)";
    case Geometry::PrimitiveId:
        return "vec4(0.0)";
    case Geometry::CameraFilmScale:
        return "vec4(materialFilmScale, 0.0, 0.0)";
    case Geometry::ViewSize:
        return "vec4(materialViewSize, 0.0, 0.0)";
    case Geometry::LocalPosition:
        return "vec4(materialLocalPosition, 0.0)";
    default:
        return "vec4(0.0)";
    }
}

//! The evaluator runs the vertex shader with the object at the origin and world axes.
std::string vertexGeometry(const Geometry attribute) {
    switch (attribute) {
    case Geometry::Position:
    case Geometry::LocalPosition:
        return "vec4(materialPosition - materialFrameOrigin, 0.0)";
    case Geometry::Normal:
        return "vec4(normalize(materialNormal), 0.0)";
    case Geometry::Tangent:
        return "vec4(normalize(materialTangent.xyz), 0.0)";
    case Geometry::Bitangent:
        return "vec4(cross(normalize(materialNormal), normalize(materialTangent.xyz)) * (materialTangent.w < 0.0 ? -1.0 : 1.0), 0.0)";
    case Geometry::ObjectBoundsMin:
        return "vec4(materialFrameAxes * materialBoundsMin, 0.0)";
    case Geometry::ObjectBoundsMax:
        return "vec4(materialFrameAxes * materialBoundsMax, 0.0)";
    case Geometry::CameraPosition:
        return "vec4(materialCameraPosition - materialFrameOrigin, 0.0)";
    default:
        return geometry(attribute);
    }
}

std::vector<uint32_t> operands(const Instruction& instruction) {
    switch (instruction.op()) {
    case Op::Constant:
    case Op::Parameter:
    case Op::Geometry:
    case Op::GeometryDx:
    case Op::GeometryDy:
    case Op::Time:
    case Op::Frame:
        return {};
    case Op::Texture:
    case Op::Extract:
    case Op::Abs:
    case Op::Floor:
    case Op::Ceil:
    case Op::Fract:
    case Op::Round:
    case Op::Trunc:
    case Op::Sign:
    case Op::Sqrt:
    case Op::Exp2:
    case Op::Log2:
    case Op::Sin:
    case Op::Cos:
    case Op::Tan:
    case Op::Asin:
    case Op::Acos:
    case Op::Atan:
    case Op::Random:
    case Op::Length:
    case Op::Normalize:
    case Op::TangentToWorld:
        return {instruction.a()};
    case Op::Combine:
        if (instruction.width() >= 3)
            return {instruction.a(), instruction.b(), instruction.c()};
        return {instruction.a(), instruction.b()};
    case Op::Select:
    case Op::Clamp:
    case Op::Mix:
    case Op::Smoothstep:
    case Op::MulAdd:
        return {instruction.a(), instruction.b(), instruction.c()};
    default:
        return {instruction.a(), instruction.b()};
    }
}

enum class Stage { Vertex, Fragment };

class Writer {
public:
    Writer(const MaterialProgram& program, const std::vector<bool>& srgb, const std::vector<bool>& perInstance, const Stage stage)
        : program(program), srgb(srgb), perInstance(perInstance), stage(stage) {}

    std::string writeSurface() const {
        std::string source = declarations();
        source += "Surface evaluateMaterial() {\n";
        std::vector<uint32_t> outputs{program.baseColor, program.normal, program.emission, program.roughness, program.metallic, program.specular};
        for (const int32_t output : {program.opacity, program.refraction})
            if (output >= 0)
                outputs.push_back(static_cast<uint32_t>(output));
        source += statements(outputs);
        const auto value = [&](const uint32_t index) { return index < program.instructions.size() ? std::format("v{}", index) : std::string("vec4(0.0)"); };
        source += "    Surface surface;\n";
        source += std::format("    surface.baseColor = {}.rgb;\n", value(program.baseColor));
        source += std::format("    surface.normal = {}.xyz;\n", value(program.normal));
        source += std::format("    surface.emission = {}.rgb;\n", value(program.emission));
        source += std::format("    surface.roughness = {}.x;\n", value(program.roughness));
        source += std::format("    surface.metallic = {}.x;\n", value(program.metallic));
        source += std::format("    surface.specular = {}.x;\n", value(program.specular));
        source += std::format("    surface.opacity = {};\n", program.opacity >= 0 ? value(static_cast<uint32_t>(program.opacity)) + ".x" : std::string("1.0"));
        source += std::format("    surface.refraction = {};\n",
                              program.refraction >= 0 ? value(static_cast<uint32_t>(program.refraction)) + ".rgb" : std::string("vec3(0.0)"));
        source += "    return surface;\n}\n";
        return source;
    }

    std::string writeDisplacement() const {
        if (program.displacement < 0 || static_cast<std::size_t>(program.displacement) >= program.instructions.size())
            return "vec3 evaluateDisplacement() {\n    return vec3(0.0);\n}\n";
        const auto output = static_cast<uint32_t>(program.displacement);
        return std::format("{}vec3 evaluateDisplacement() {{\n{}    return v{}.xyz;\n}}\n", declarations(), statements({output}), output);
    }

private:
    std::string declarations() const {
        std::string source;
        for (std::size_t i = 0; i < program.textures.size(); ++i)
            source += std::format("uniform sampler2D materialTexture{};\n", i);
        for (std::size_t i = 0; i < program.parameters.size(); ++i)
            if (!perInstance[i])
                source += std::format("uniform vec4 materialParameter{};\n", i);
            else if (stage == Stage::Fragment)
                source += std::format("flat in vec4 materialParameter{};\n", i);
        return source;
    }

    std::string statements(const std::vector<uint32_t>& outputs) const {
        std::vector<bool> used(program.instructions.size(), false);
        std::vector<uint32_t> pending;
        for (const uint32_t output : outputs)
            if (output < program.instructions.size())
                pending.push_back(output);
        while (!pending.empty()) {
            const uint32_t index = pending.back();
            pending.pop_back();
            if (used[index])
                continue;
            used[index] = true;
            for (const uint32_t operand : operands(program.instructions[index]))
                if (operand < program.instructions.size())
                    pending.push_back(operand);
        }
        std::string source;
        for (uint32_t i = 0; i < program.instructions.size(); ++i)
            if (used[i])
                source += std::format("    vec4 v{} = {};\n", i, expression(program.instructions[i]));
        return source;
    }

    std::string v(const uint32_t index) const { return std::format("v{}", index); }
    uint32_t width(const uint32_t index) const { return index < program.instructions.size() ? program.instructions[index].width() : 1; }

    std::string expression(const Instruction& instruction) const {
        std::string result = vector(instruction);
        if (instruction.width() == 1 && instruction.op() != Op::Constant && instruction.op() != Op::Parameter)
            result = std::format("vec4(({}).x)", result);
        return result;
    }

    std::string vector(const Instruction& instruction) const {
        const uint32_t w = instruction.width();
        const std::string a = v(instruction.a());
        const std::string b = v(instruction.b());
        const std::string c = v(instruction.c());
        const auto unary = [&](const std::string_view function) { return std::format("{}({})", function, a); };
        const auto binary = [&](const std::string_view function) { return std::format("{}({}, {})", function, a, b); };
        const auto infix = [&](const std::string_view symbol) { return std::format("({} {} {})", a, symbol, b); };
        const auto compare = [&](const std::string_view function) { return std::format("vec4({}({}, {}))", function, a, b); };
        const uint32_t operandWidth = std::max(width(instruction.a()), width(instruction.b()));
        switch (instruction.op()) {
        case Op::Constant: {
            std::string components;
            for (uint32_t i = 0; i < 4; ++i) {
                const std::size_t at = instruction.index() + (w == 1 ? 0 : i);
                const bool inside = w == 1 || i < w;
                components += (i ? ", " : "") + literal(inside && at < program.constants.size() ? program.constants[at] : 0.0f);
            }
            return std::format("vec4({})", components);
        }
        case Op::Parameter:
            if (instruction.index() >= program.parameters.size())
                return "vec4(0.0)";
            return std::format("materialParameter{}", instruction.index());
        case Op::Texture: {
            if (instruction.index() >= program.textures.size())
                return "vec4(0.0)";
            const std::string sample = std::format("texture(materialTexture{}, {}.xy)", instruction.index(), a);
            return instruction.index() < srgb.size() && srgb[instruction.index()] ? std::format("materialDecodeSrgb({})", sample) : sample;
        }
        case Op::Geometry:
            return stage == Stage::Vertex ? vertexGeometry(static_cast<Geometry>(instruction.index())) : geometry(static_cast<Geometry>(instruction.index()));
        case Op::GeometryDx:
            return stage == Stage::Vertex ? "vec4(0.0)" : std::format("dFdx({})", geometry(static_cast<Geometry>(instruction.index())));
        case Op::GeometryDy:
            return stage == Stage::Vertex ? "vec4(0.0)" : std::format("dFdy({})", geometry(static_cast<Geometry>(instruction.index())));
        case Op::Time:
            return "vec4(materialTime)";
        case Op::Frame:
            return "vec4(materialFrame)";
        case Op::Extract:
            return std::format("vec4({}[{}])", a, std::min<int>(instruction.index(), 3));
        case Op::Combine:
            return std::format("vec4({}.x, {}.x, {}, 0.0)", a, b, w >= 3 ? c + ".x" : std::string("0.0"));
        case Op::Add:
            return infix("+");
        case Op::Sub:
            return infix("-");
        case Op::Mul:
            return infix("*");
        case Op::Div:
            return infix("/");
        case Op::Min:
            return binary("min");
        case Op::Max:
            return binary("max");
        case Op::Pow:
            return binary("materialPow");
        case Op::Atan2:
            return binary("atan");
        case Op::Mod:
            return std::format("({0} - {1} * floor({0} / {1}))", a, b);
        case Op::Abs:
            return unary("abs");
        case Op::Floor:
            return unary("floor");
        case Op::Ceil:
            return unary("ceil");
        case Op::Fract:
            return unary("fract");
        case Op::Round:
            return unary("roundEven");
        case Op::Trunc:
            return unary("trunc");
        case Op::Sign:
            return unary("sign");
        case Op::Sqrt:
            return std::format("sqrt(max({}, vec4(0.0)))", a);
        case Op::Exp2:
            return unary("exp2");
        case Op::Log2:
            return unary("log2");
        case Op::Sin:
            return unary("sin");
        case Op::Cos:
            return unary("cos");
        case Op::Tan:
            return unary("tan");
        case Op::Asin:
            return std::format("asin(clamp({}, -1.0, 1.0))", a);
        case Op::Acos:
            return std::format("acos(clamp({}, -1.0, 1.0))", a);
        case Op::Atan:
            return unary("atan");
        case Op::Random:
            return std::format("vec4(materialRandom({}))", widen(lanes(a, width(instruction.a())), width(instruction.a())));
        case Op::Less:
            return compare("lessThan");
        case Op::LessEqual:
            return compare("lessThanEqual");
        case Op::Equal:
            return compare("equal");
        case Op::NotEqual:
            return compare("notEqual");
        case Op::Select:
            return std::format("mix({}, {}, notEqual({}, vec4(0.0)))", c, b, a);
        case Op::Dot:
            return std::format("vec4(dot({}, {}))", lanes(a, operandWidth), lanes(b, operandWidth));
        case Op::Length:
            return std::format("vec4(length({}))", lanes(a, width(instruction.a())));
        case Op::Distance:
            return std::format("vec4(distance({}, {}))", lanes(a, operandWidth), lanes(b, operandWidth));
        case Op::Normalize: {
            const std::string vector = lanes(a, w);
            return widen(std::format("({0} / max(length({0}), 1e-30))", vector), w);
        }
        case Op::Cross:
            return std::format("vec4(cross({}.xyz, {}.xyz), 0.0)", a, b);
        case Op::Reflect:
            return widen(std::format("reflect({}, {})", lanes(a, w), lanes(b, w)), w);
        case Op::TangentToWorld:
            return std::format("vec4(materialTangentToWorld({}.xyz), 0.0)", a);
        case Op::Clamp:
            return std::format("min(max({}, {}), {})", a, b, c);
        case Op::Mix:
            return std::format("({0} + ({1} - {0}) * {2})", a, b, c);
        case Op::Smoothstep:
            return std::format("materialSmoothstep({}, {}, {})", b, c, a);
        case Op::MulAdd:
            return std::format("({} * {} + {})", a, b, c);
        }
        return "vec4(0.0)";
    }

    const MaterialProgram& program;
    const std::vector<bool>& srgb;
    const std::vector<bool>& perInstance;
    const Stage stage;
};

}

std::string glslMaterial(const MaterialProgram& program, const std::vector<bool>& srgb, const std::vector<bool>& perInstance) {
    return Writer(program, srgb, perInstance, Stage::Fragment).writeSurface();
}

std::string glslDisplacement(const MaterialProgram& program, const std::vector<bool>& srgb, const std::vector<bool>& perInstance) {
    return Writer(program, srgb, perInstance, Stage::Vertex).writeDisplacement();
}

}
