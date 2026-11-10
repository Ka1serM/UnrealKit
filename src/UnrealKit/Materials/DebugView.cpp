#include "UnrealKit/Materials/DebugView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UnrealKit::Materials {
namespace {

using ue5parse::conversion::EngineSymbols;
using ue5parse::conversion::EngineValues;
using ue5parse::conversion::ExpressionGraph;
using ue5parse::conversion::ExprId;
using ue5parse::conversion::ExprNode;
using ue5parse::conversion::ExprOp;
using ue5parse::conversion::GeometryAttribute;
using ue5parse::conversion::MaterialEvaluator;
using ue5parse::conversion::ShaderOutputs;
using ue5parse::conversion::ShaderReflection;
using ue5parse::conversion::ShaderStage;
using ue5parse::conversion::SurfaceAttributes;
using ue5parse::conversion::VertexProjection;
using ue5parse::conversion::viewDefaults;

ExprId unexpectedEncoding(ExpressionGraph& graph) {
    graph.fail("The debug view has an unexpected encoding.");
    return graph.constant(0.0f);
}

ExprId decode(ExpressionGraph& graph, const ExprId encoded) {
    if (graph.isConstant(encoded)) {
        const float linear = graph.node(encoded).value;
        const float value = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        constexpr float debugFloor = 1.0f / 16384.0f;
        return graph.constant(value <= debugFloor * 1.001f ? 0.0f : value);
    }
    const ExprNode& select = graph.node(encoded);
    if (select.op != ExprOp::Select)
        return unexpectedEncoding(graph);
    const ExprNode& low = graph.node(graph.node(select.c).op == ExprOp::Mul ? select.c : select.b);
    if (low.op != ExprOp::Mul)
        return unexpectedEncoding(graph);
    const ExprNode& max = graph.node(graph.isConstant(low.a) ? low.b : low.a);
    if (max.op != ExprOp::Max)
        return unexpectedEncoding(graph);
    return graph.isConstant(max.a) ? max.b : max.a;
}

ExprId decodeNormal(ExpressionGraph& graph, const ExprId encoded) {
    const ExprId decoded = decode(graph, encoded);
    if (graph.isConstant(decoded))
        return graph.constant(graph.node(decoded).value * 2.0f - 1.0f);
    const ExprNode& add = graph.node(decoded);
    if (add.op != ExprOp::Add)
        return unexpectedEncoding(graph);
    const bool constantFirst = graph.isConstant(add.a);
    const ExprId offset = constantFirst ? add.a : add.b;
    if (!graph.isConstant(offset) || graph.node(offset).value != 0.5f)
        return unexpectedEncoding(graph);
    return graph.mul(constantFirst ? add.b : add.a, graph.constant(2.0f));
}

constexpr std::string_view diffuseOverride = "View_DiffuseOverrideParameter";
constexpr std::string_view specularOverride = "View_SpecularOverrideParameter";

struct LightInputs {
    EngineSymbols symbols;
    std::set<std::string, std::less<>> names;
};

bool isLightInput(const std::string_view name) {
    for (const std::string_view prefix :
         {"IndirectLightingCache_", "MobileDirectionalLight_", "MobileReflectionCapture_", "ReflectionCubemap", "View_ReflectionEnvironment",
          "View_PrecomputedIndirect", "View_DirectionalLight", "View_AtmosphereLight", "View_DistantSkyLight", "LightmapResourceCluster_"})
        if (name.starts_with(prefix))
            return true;
    return name == "View_IndirectLightingColorScale" || name == "View_SkyLightColor" || name == "ReflectionBrightness";
}

void addLightInputs(const ShaderReflection& reflection, LightInputs& inputs) {
    for (const ShaderReflection::ConstantBuffer& buffer : reflection.getConstantBuffers())
        for (std::size_t i = 0; i < buffer.members.size(); ++i) {
            const ShaderReflection::Member& member = buffer.members[i];
            if (!isLightInput(member.name))
                continue;
            inputs.names.insert(member.name);
            const uint32_t end = i + 1 < buffer.members.size() ? buffer.members[i + 1].offset : member.offset + 64;
            for (uint32_t word = 0; member.offset + word * 4 < end; ++word)
                inputs.symbols.insert(member.name + "[" + std::to_string(word) + "]");
        }
    for (const ShaderReflection::Binding& texture : reflection.getTextures())
        if (isLightInput(texture.name)) {
            inputs.names.insert(texture.name);
            inputs.symbols.insert(texture.name);
        }
}

LightInputs lightInputs(const ShaderReflection& reflection) {
    LightInputs inputs;
    addLightInputs(reflection, inputs);
    return inputs;
}

bool isCondition(const ExprOp op) {
    return op >= ExprOp::Less && op <= ExprOp::Not;
}

bool variesOverSurface(const ExprNode& node) {
    switch (node.op) {
    case ExprOp::Texture:
    case ExprOp::GeometryDx:
    case ExprOp::GeometryDy:
    case ExprOp::Time:
    case ExprOp::Frame:
        return true;
    case ExprOp::Geometry: {
        const auto attribute = static_cast<GeometryAttribute>(node.index);
        return attribute <= GeometryAttribute::VertexColor || attribute == GeometryAttribute::SceneDepth || attribute == GeometryAttribute::ViewDirection
               || attribute == GeometryAttribute::LocalPosition;
    }
    default:
        return false;
    }
}

class LightSeparation {
public:
    LightSeparation(ExpressionGraph& graph, const std::set<std::string, std::less<>>& lights) : graph(graph), lights(lights) {}

    bool isLit(const ExprId id) {
        if (const auto known = lit.find(id); known != lit.end())
            return known->second;
        bool result = graph.node(id).op == ExprOp::Unresolved && isLight(id);
        for (uint32_t i = 0; i < graph.operandCount(id) && !result; ++i)
            result = isLit(operand(id, i));
        lit.emplace(id, result);
        return result;
    }

    ExprId darkened(const ExprId id) {
        if (!isLit(id))
            return id;
        if (const auto known = dark.find(id); known != dark.end())
            return known->second;
        ExprId result = graph.constant(0.0f);
        if (graph.node(id).op != ExprOp::Unresolved) {
            std::array<ExprId, 3> operands{};
            for (uint32_t i = 0; i < graph.operandCount(id); ++i)
                operands[i] = darkened(operand(id, i));
            result = graph.rebuild(id, operands);
        }
        dark.emplace(id, result);
        return result;
    }

    ExprId withoutLight(const ExprId id) {
        if (!isLit(id))
            return id;
        if (const auto known = stripped.find(id); known != stripped.end())
            return known->second;
        const ExprId result = strip(id);
        stripped.emplace(id, result);
        return result;
    }

    ExprId occlusion(const ExprId coefficient, bool& hasLight) {
        std::vector<std::pair<ExprId, bool>> factors;
        collectFactors(coefficient, false, factors);
        ExprId product = graph.constant(1.0f);
        hasLight = false;
        for (const auto& [factor, divisor] : factors) {
            if (isLit(factor)) {
                hasLight = true;
                continue;
            }
            product = divisor ? graph.div(product, factor) : graph.mul(product, factor);
        }
        return product;
    }

private:
    bool isLight(const ExprId id) const {
        const std::string& name = graph.unresolvedName(id);
        return lights.contains(std::string_view(name).substr(0, name.find('[')));
    }

    ExprId operand(const ExprId id, const uint32_t i) const {
        const ExprNode& node = graph.node(id);
        return i == 0 ? node.a : i == 1 ? node.b : node.c;
    }

    bool varies(const ExprId id) {
        if (const auto known = surfaceVarying.find(id); known != surfaceVarying.end())
            return known->second;
        bool result = variesOverSurface(graph.node(id));
        for (uint32_t i = 0; i < graph.operandCount(id) && !result; ++i)
            result = varies(operand(id, i));
        surfaceVarying.emplace(id, result);
        return result;
    }

    ExprId strip(const ExprId id) {
        const ExprNode node = graph.node(id);
        if (node.op == ExprOp::Unresolved)
            return graph.constant(0.0f);
        if (isCondition(node.op))
            return darkened(id);
        if (node.op == ExprOp::Select)
            return graph.select(darkened(node.a), withoutLight(node.b), withoutLight(node.c));
        if (node.op != ExprOp::Mul && node.op != ExprOp::Div && !varies(id))
            return graph.constant(graph.isConstant(darkened(id), 0.0f) ? 0.0f : 1.0f);
        std::array<ExprId, 3> operands{};
        for (uint32_t i = 0; i < graph.operandCount(id); ++i)
            operands[i] = withoutLight(operand(id, i));
        if (node.op == ExprOp::Div && graph.isConstant(operands[1], 0.0f))
            operands[1] = graph.constant(1.0f);
        return graph.rebuild(id, operands);
    }

    void collectFactors(const ExprId id, const bool divisor, std::vector<std::pair<ExprId, bool>>& factors) const {
        const ExprNode& node = graph.node(id);
        if (node.op == ExprOp::Mul || node.op == ExprOp::Div) {
            const ExprId a = node.a, b = node.b;
            const bool divides = node.op == ExprOp::Div;
            collectFactors(a, divisor, factors);
            collectFactors(b, divides != divisor, factors);
            return;
        }
        factors.emplace_back(id, divisor);
    }

    ExpressionGraph& graph;
    const std::set<std::string, std::less<>>& lights;
    std::unordered_map<ExprId, bool> lit;
    std::unordered_map<ExprId, bool> surfaceVarying;
    std::unordered_map<ExprId, ExprId> dark;
    std::unordered_map<ExprId, ExprId> stripped;
};

EngineValues withoutLighting() {
    EngineValues values = viewDefaults();
    values[std::string(diffuseOverride)] = {0.0f, 0.0f, 0.0f, 0.0f};
    values[std::string(specularOverride)] = {0.0f, 0.0f, 0.0f, 0.0f};
    return values;
}

std::array<std::string, 3> overrideWords(const std::string_view parameter, const std::array<uint32_t, 3>& words) {
    std::array<std::string, 3> names;
    for (uint32_t c = 0; c < 3; ++c)
        names[c] = std::string(parameter) + "[" + std::to_string(words[c]) + "]";
    return names;
}

std::optional<std::array<ExprId, 3>> overrideCoefficients(MaterialEvaluator& evaluator, const CompiledShader& basePass, const EngineValues& view,
                                                          EngineSymbols symbols, const std::array<std::string, 3>& names) {
    ExpressionGraph& graph = evaluator.getGraph();
    symbols.insert(names.begin(), names.end());
    const ShaderOutputs outputs = evaluator.evaluate(basePass, view, symbols);
    if (outputs.targets.empty()) {
        graph.fail("The base pass writes no render target.");
        return std::nullopt;
    }
    std::array<ExprId, 3> coefficients{};
    for (uint32_t c = 0; c < 3; ++c) {
        const std::optional<ExprId> coefficient = graph.coefficient(outputs.targets[0][c], graph.unresolved(names[c]));
        if (!coefficient) {
            graph.fail("The base pass mixes " + names[c] + " into its output in a way the translation cannot separate.");
            return std::nullopt;
        }
        coefficients[c] = *coefficient;
    }
    return coefficients;
}

bool allZero(const ExpressionGraph& graph, const std::array<ExprId, 3>& values) {
    return std::ranges::all_of(values, [&](const ExprId value) { return graph.isConstant(value, 0.0f); });
}

void applyLightResponse(MaterialEvaluator& evaluator, const CompiledShader& basePass, const LightInputs& lights, LightSeparation& separation,
                        SurfaceAttributes& surface) {
    ExpressionGraph& graph = evaluator.getGraph();
    EngineValues unlitView = withoutLighting();
    unlitView["View_UnlitViewmodeMask"] = {1.0f};
    const std::array<std::string, 3> diffuseWords = overrideWords(diffuseOverride, {0, 1, 2});
    const std::optional<std::array<ExprId, 3>> present = overrideCoefficients(evaluator, basePass, unlitView, {}, diffuseWords);
    if (!present)
        return;
    if (allZero(graph, *present)) {
        surface.baseColor.fill(graph.constant(0.0f));
        surface.specular = graph.constant(0.0f);
        return;
    }
    const std::optional<std::array<ExprId, 3>> coefficients = overrideCoefficients(evaluator, basePass, withoutLighting(), lights.symbols, diffuseWords);
    if (!coefficients)
        return;
    if (allZero(graph, *coefficients)) {
        graph.fail("The light of the diffuse term reads as zero outside the pixel shader.");
        return;
    }
    std::array<ExprId, 3> occlusion{};
    std::array<bool, 3> lit{};
    for (uint32_t c = 0; c < 3; ++c)
        occlusion[c] = separation.occlusion((*coefficients)[c], lit[c]);
    if (lit[0] != lit[1] || lit[0] != lit[2]) {
        graph.fail("The base pass lights the channels of the diffuse color differently.");
        return;
    }
    if (!lit[0])
        return;
    if (occlusion[0] != occlusion[1] || occlusion[0] != occlusion[2]) {
        graph.fail("The ambient occlusion of the base pass differs per channel.");
        return;
    }
    for (uint32_t c = 0; c < 3; ++c)
        surface.baseColor[c] = graph.mul(surface.baseColor[c], occlusion[c]);
    surface.specular = graph.mul(surface.specular, occlusion[0]);
}

std::optional<std::array<ExprId, 3>> sceneColorShare(MaterialEvaluator& evaluator, const CompiledShader& basePass, const LightInputs& lights,
                                                     LightSeparation& separation, const std::array<ExprId, 3>& emission) {
    ExpressionGraph& graph = evaluator.getGraph();
    EngineValues view = withoutLighting();
    view["SceneColor"] = {1.0f};
    const ShaderOutputs outputs = evaluator.evaluate(basePass, view, lights.symbols);
    if (outputs.targets.empty()) {
        graph.fail("The base pass writes no render target.");
        return std::nullopt;
    }
    std::array<ExprId, 3> share{};
    for (uint32_t c = 0; c < 3; ++c)
        share[c] = graph.sub(separation.darkened(outputs.targets[0][c]), emission[c]);
    if (std::ranges::all_of(share, [&](const ExprId channel) { return graph.isConstant(channel, 0.0f); }))
        return std::nullopt;
    return share;
}

SurfaceAttributes debugViewAttributes(MaterialEvaluator& evaluator, const CompiledShader& basePass, const CompiledShader& vertexShader,
                                      const CompiledShader& debugView, const BlendMode blendMode) {
    ExpressionGraph& graph = evaluator.getGraph();
    for (const char* parameters : {"DebugParameters0", "DebugParameters1", "DebugParameters2"}) {
        if (debugView.reflection.hasMember(parameters))
            continue;
        graph.fail(std::string("The layout of the debug view is unknown: its shader keeps no reflection naming ") + parameters + ".");
        return {};
    }
    const std::string normal = "DebugParameters0[0]", roughness = "DebugParameters0[1]", metallic = "DebugParameters0[2]", specular = "DebugParameters0[3]",
                      baseColor = "DebugParameters1[2]";
    const LightInputs vertexLights = lightInputs(vertexShader.reflection);
    evaluator.evaluate(vertexShader, viewDefaults(), vertexLights.symbols, VertexProjection::Camera);
    EngineValues selection = viewDefaults();
    selection["DebugParameters0"] = {0.0f, 0.0f, 0.0f, 0.0f};
    selection["DebugParameters1"] = {0.0f, 0.0f, 0.0f, 0.0f};
    selection["DebugParameters2"] = {1.0f, 0.0f, 0.0f, 0.0f};
    LightInputs debugLights = vertexLights;
    addLightInputs(debugView.reflection, debugLights);
    EngineSymbols debugSymbols = debugLights.symbols;
    debugSymbols.insert({normal, roughness, metallic, specular, baseColor});
    const ShaderOutputs debugOutputs = evaluator.evaluate(debugView, selection, debugSymbols);
    if (debugOutputs.targets.empty()) {
        graph.fail("The debug view writes no render target.");
        return {};
    }
    const auto& output = debugOutputs.targets[0];
    LightSeparation debugSeparation(graph, debugLights.names);
    const auto selected = [&](const std::string& selector, const uint32_t c) {
        const std::optional<ExprId> coefficient = graph.coefficient(output[c], graph.unresolved(selector));
        if (coefficient)
            return *coefficient;
        graph.fail("The debug view mixes the attribute " + selector + " in a way the translation cannot separate.");
        return graph.constant(0.0f);
    };

    SurfaceAttributes surface;
    for (uint32_t c = 0; c < 3; ++c) {
        surface.baseColor[c] = debugSeparation.withoutLight(selected(baseColor, c));
        surface.normal[c] = debugSeparation.withoutLight(decodeNormal(graph, selected(normal, c)));
    }
    if (std::ranges::all_of(surface.normal, [&](const ExprId n) { return graph.isConstant(n); }))
        for (uint32_t c = 0; c < 3; ++c)
            surface.normal[c] = graph.geometry(GeometryAttribute::Normal, c);
    surface.roughness = debugSeparation.withoutLight(decode(graph, selected(roughness, 0)));
    surface.metallic = debugSeparation.withoutLight(decode(graph, selected(metallic, 0)));
    surface.specular = debugSeparation.withoutLight(decode(graph, selected(specular, 0)));

    LightInputs lights = vertexLights;
    addLightInputs(basePass.reflection, lights);
    LightSeparation separation(graph, lights.names);
    const ShaderOutputs basePassOutputs = evaluator.evaluate(basePass, withoutLighting(), lights.symbols);
    if (basePassOutputs.targets.empty()) {
        graph.fail("The base pass writes no render target.");
        return {};
    }
    const auto& color = basePassOutputs.targets[0];
    for (uint32_t c = 0; c < 3; ++c)
        surface.emission[c] = separation.darkened(color[c]);
    if (blendMode == BlendMode::Masked)
        surface.opacity = graph.select(separation.darkened(basePassOutputs.discard), graph.constant(0.0f), graph.constant(1.0f));
    const std::optional<std::array<ExprId, 3>> refraction =
        blendMode == BlendMode::Translucent ? sceneColorShare(evaluator, basePass, lights, separation, surface.emission) : std::nullopt;
    applyLightResponse(evaluator, basePass, lights, separation, surface);
    if (blendMode < BlendMode::Translucent || blendMode == BlendMode::Additive)
        return surface;
    surface.opacity = graph.clamp(separation.withoutLight(color[3]), graph.constant(0.0f), graph.constant(1.0f));
    surface.refraction = refraction;
    return surface;
}

}

std::optional<SurfaceAttributes> debugViewSurface(MaterialEvaluator& evaluator, const std::vector<CompiledShader>& shaders, const BlendMode blendMode) {
    if (shaders.size() < 2)
        return std::nullopt;
    //! ShaderMaterials puts the debug view pixel shader after the base pass pixel shader.
    const auto debugView = std::ranges::find(shaders.begin() + 1, shaders.end(), ShaderStage::Pixel, &CompiledShader::stage);
    if (debugView == shaders.end())
        return std::nullopt;
    const auto vertexShader = std::ranges::find(shaders, ShaderStage::Vertex, &CompiledShader::stage);
    if (vertexShader == shaders.end())
        return std::nullopt;
    return debugViewAttributes(evaluator, shaders.front(), *vertexShader, *debugView, blendMode);
}

}
