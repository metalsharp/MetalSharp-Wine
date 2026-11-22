#include "core/shader/compiler.h"
#include "core/shader/link_file_cache.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "core/shader/border.h"
#include "core/shader/fp64.h"
#include "core/shader/glsl_upgrade.h"
#include "core/limits.h"
#include "core/shader/geometry.h"
#include "core/shader/tessellation.h"
#include "core/shader/spirv_passes.h"
#include "core/shader/subroutines.h"
#include "core/shader/transform_feedback.h"
#include "core/util/log.h"

namespace wmgl::shader {
namespace {

constexpr const char* kDefaultBlockName = "wmgl_DefaultUniforms";
constexpr int kDefaultBlockBinding = 1000;
constexpr unsigned kNoLocation = 0xFFF;  // glslang's TQualifier::layoutLocationEnd

// GLSL built-in constants (gl_Max*): glslang's defaults, with the limits
// the core reports through glGet (core/limits.h).
const TBuiltInResource* resources() {
    static const TBuiltInResource r = [] {
        TBuiltInResource x = *GetDefaultResources();
        namespace L = wmgl::limits;
        x.maxVertexAttribs = L::kMaxVertexAttribs;
        x.maxDrawBuffers = L::kMaxDrawBuffers;
        x.maxTextureImageUnits = x.maxVertexTextureImageUnits = x.maxGeometryTextureImageUnits = L::kMaxTextureUnitsPerStage;
        x.maxTessControlTextureImageUnits = x.maxTessEvaluationTextureImageUnits = L::kMaxTextureUnitsPerStage;
        x.maxCombinedTextureImageUnits = L::kMaxCombinedTextureUnits;
        x.maxVaryingComponents = 64;
        x.maxVaryingVectors = 16;
        // GLSL ES 3.00's gl_MaxVertexOutputVectors / gl_MaxFragmentInputVectors.
        x.maxVertexOutputVectors = L::kMaxVertexOutputComponents / 4;
        x.maxFragmentInputVectors = L::kMaxInterfaceComponents / 4;
        x.maxImageUnits = L::kMaxImageUnits;
        x.maxCombinedImageUnitsAndFragmentOutputs = x.maxCombinedShaderOutputResources = L::kMaxCombinedShaderOutputResources;
        x.maxImageSamples = 0;
        x.maxVertexImageUniforms = x.maxTessControlImageUniforms = x.maxTessEvaluationImageUniforms = L::kMaxImageUniformsPerStage;
        x.maxGeometryImageUniforms = x.maxFragmentImageUniforms = x.maxComputeImageUniforms = L::kMaxImageUniformsPerStage;
        x.maxCombinedImageUniforms = L::kMaxCombinedImageUniforms;
        x.maxTessControlAtomicCounters = x.maxTessEvaluationAtomicCounters = 0;
        x.maxVertexAtomicCounters = x.maxGeometryAtomicCounters = x.maxFragmentAtomicCounters = x.maxComputeAtomicCounters =
            L::kMaxStageAtomicCounters;
        x.maxCombinedAtomicCounters = L::kMaxStageAtomicCounters;
        x.maxTessControlAtomicCounterBuffers = x.maxTessEvaluationAtomicCounterBuffers = 0;
        x.maxVertexAtomicCounterBuffers = L::kMaxVertexAtomicCounterBuffers;
        x.maxGeometryAtomicCounterBuffers = x.maxFragmentAtomicCounterBuffers = x.maxComputeAtomicCounterBuffers =
            L::kMaxStageAtomicCounterBuffers;
        x.maxCombinedAtomicCounterBuffers = L::kMaxStageAtomicCounterBuffers;
        x.maxAtomicCounterBindings = L::kMaxAtomicCounterBufferBindings;
        x.maxAtomicCounterBufferSize = L::kMaxAtomicCounterBufferSize;
        x.maxViewports = L::kMaxViewports;
        x.maxPatchVertices = L::kMaxPatchVertices;
        x.maxTessGenLevel = L::kMaxTessGenLevel;
        x.maxSamples = 4;
        // Default-block uniform components of every stage (GL_MAX_*_UNIFORM_COMPONENTS).
        x.maxVertexUniformComponents = x.maxFragmentUniformComponents = x.maxGeometryUniformComponents =
            x.maxTessControlUniformComponents = x.maxTessEvaluationUniformComponents = x.maxComputeUniformComponents =
                L::kMaxDefaultUniformComponents;
        // gl_MaxVertexUniformVectors and gl_MaxFragmentUniformVectors match the
        // API's GL_MAX_*_UNIFORM_VECTORS (KHR-GL46.limits).
        x.maxVertexUniformVectors = x.maxFragmentUniformVectors = L::kMaxDefaultUniformComponents / 4;
        x.maxComputeTextureImageUnits = L::kMaxTextureUnitsPerStage;
        x.maxComputeWorkGroupCountX = x.maxComputeWorkGroupCountY = x.maxComputeWorkGroupCountZ = L::kMaxComputeWorkGroupCount;
        x.maxComputeWorkGroupSizeX = L::kMaxComputeWorkGroupSize[0];
        x.maxComputeWorkGroupSizeY = L::kMaxComputeWorkGroupSize[1];
        x.maxComputeWorkGroupSizeZ = L::kMaxComputeWorkGroupSize[2];
        return x;
    }();
    return &r;
}

void initGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage language(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return EShLangVertex;
    case Stage::TessControl: return EShLangTessControl;
    case Stage::TessEval: return EShLangTessEvaluation;
    case Stage::Geometry: return EShLangGeometry;
    case Stage::Fragment: return EShLangFragment;
    default: return EShLangCompute;
    }
}

spv::ExecutionModel executionModel(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::TessControl: return spv::ExecutionModelTessellationControl;
    case Stage::TessEval: return spv::ExecutionModelTessellationEvaluation;
    case Stage::Geometry: return spv::ExecutionModelGeometry;
    case Stage::Fragment: return spv::ExecutionModelFragment;
    default: return spv::ExecutionModelGLCompute;
    }
}

const EShMessages kMessages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

// Macros of extensions glslang implements without knowing their names
// (raiseComputeVersion), predefined like its own.
constexpr const char* kPreamble = "#define GL_ARB_cull_distance 1\n#define GL_EXT_gpu_shader4 1\n";

// Configures a shader for codegen: relaxed Vulkan rules, one default block.
void configure(glslang::TShader& shader, EShLanguage lang) {
    shader.setPreamble(kPreamble);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setGlobalUniformBlockName(kDefaultBlockName);
    shader.setGlobalUniformSet(0);
    shader.setGlobalUniformBinding(kDefaultBlockBinding);
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);
}

// MSL has no rectangle textures: rectangle images become 2D images (the
// driver samples them with unnormalized-coordinate samplers and texelFetch
// maps to read()).
std::vector<uint32_t> rectImagesAs2D(std::vector<uint32_t> spirv) {
    constexpr uint32_t kOpTypeImage = 25, kDimRect = 4, kDim2D = 1;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, opcode = spirv[i] & 0xffff;
        if (count == 0) break;
        if (opcode == kOpTypeImage && count > 3 && spirv[i + 3] == kDimRect) spirv[i + 3] = kDim2D;
        i += count;
    }
    return spirv;
}

// A vertex shader without outputs becomes a void function, which Metal only
// accepts with rasterization disabled: give it a position output (GL leaves
// the position undefined when the shader does not write it).
void ensureVertexOutput(std::string& msl) {
    const size_t fn = msl.find("vertex void main0(");
    if (fn == std::string::npos) return;
    std::string head = msl.substr(0, fn) + "struct main0_out\n{\n    float4 gl_Position [[position]];\n};\n\n";
    std::string main = msl.substr(fn);
    main.replace(0, std::string("vertex void main0(").size(), "vertex main0_out main0(");
    const size_t open = main.find('{');
    main.insert(open + 1, "\n    main0_out out = {};");
    for (size_t at = main.find("return;"); at != std::string::npos; at = main.find("return;", at + 11))
        main.replace(at, 7, "return out;");
    const size_t close = main.rfind('}');
    main.insert(close, "    return out;\n");
    msl = head + main;
}

// Cull distances (the gl_CullDistance output is masked, so SPIRV-Cross
// declares it as a local array): each element becomes a [[user(cullN)]]
// varying, the name SPIRV-Cross gives a fragment stage's gl_CullDistance
// inputs, and the fragment stage culls (fragmentWithCullDistances).
void addCullDistances(std::string& msl) {
    const size_t fn = msl.find("vertex main0_out main0(");
    const size_t structAt = msl.find("struct main0_out\n{\n");
    if (fn == std::string::npos || structAt == std::string::npos || structAt > fn) return;
    // "spvUnsafeArray<float, N> gl_CullDistance" or "float gl_CullDistance[N]".
    int count = 0;
    if (const size_t at = msl.find("> gl_CullDistance", fn); at != std::string::npos) {
        const size_t comma = msl.rfind(", ", at);
        if (comma != std::string::npos) count = std::atoi(msl.c_str() + comma + 2);
    } else if (const size_t native = msl.find("float gl_CullDistance[", fn); native != std::string::npos) {
        count = std::atoi(msl.c_str() + native + 22);
    }
    // gl_PerVertex declares the array whether or not the shader writes it.
    size_t uses = 0;
    for (size_t at = msl.find("gl_CullDistance", fn); at != std::string::npos; at = msl.find("gl_CullDistance", at + 1)) ++uses;
    if (count <= 0 || uses < 2) return;
    // A second varying per distance is 0 at vertices where it is negative and
    // 1e30 elsewhere. Interpolated, it is exactly 0 only where every vertex
    // of the primitive has it negative (or at a fragment on an edge or
    // vertex of such vertices).
    std::string members, assign;
    for (int i = 0; i < count; ++i) {
        const std::string k = std::to_string(i);
        members += "    float wmgl_cull" + k + " [[user(cull" + k + ")]];\n";
        members += "    float wmgl_cull_keep" + k + " [[user(wmgl_cull" + k + ")]];\n";
        assign += "out.wmgl_cull" + k + " = gl_CullDistance[" + k + "]; ";
        assign += "out.wmgl_cull_keep" + k + " = gl_CullDistance[" + k + "] < 0.0 ? 0.0 : 1e30; ";
    }
    std::string main = msl.substr(fn);
    for (size_t at = main.find("return out;"); at != std::string::npos; at = main.find("return out;", at + assign.size() + 11))
        main.insert(at, assign);
    std::string head = msl.substr(0, fn);
    head.insert(structAt + std::string("struct main0_out\n{\n").size(), members);
    msl = head + main;
}

// Every vertex function writes [[point_size]] (Metal leaves it undefined
// otherwise). GL 3.3: the size is the shader's gl_PointSize when
// PROGRAM_POINT_SIZE is enabled, else glPointSize; both come from the driver
// uniforms in buffer kSlotDriverUniforms (float pointSize, uint programPointSize, uint clipFlags, uint clipEnables).
void addPointSize(std::string& msl) {
    const size_t fn = msl.find("vertex main0_out main0(");
    const size_t structAt = msl.find("struct main0_out\n{\n");
    if (fn == std::string::npos || structAt == std::string::npos || structAt > fn) return;
    const bool writes = msl.find("[[point_size]]") != std::string::npos;
    const std::string driver =
        "struct wmgl_DriverUniforms\n{\n    float pointSize;\n    uint programPointSize;\n    uint clipFlags;\n    uint clipEnables;\n};\n\n";
    std::string patched = msl.substr(0, structAt) + driver + msl.substr(structAt, fn - structAt);
    if (!writes) {
        const size_t body = patched.find("struct main0_out\n{\n") + std::string("struct main0_out\n{\n").size();
        patched.insert(body, "    float gl_PointSize [[point_size]];\n");
    }
    std::string main = msl.substr(fn);
    const size_t open = main.find('(');
    const bool noParameters = main[open + 1] == ')';
    main.insert(open + 1, "constant wmgl_DriverUniforms& wmgl_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                              (noParameters ? "" : ", "));
    std::string assign = writes ? "out.gl_PointSize = wmgl_driver.programPointSize != 0u ? out.gl_PointSize : wmgl_driver.pointSize; "
                                : "out.gl_PointSize = wmgl_driver.pointSize; ";
    // Clip distances of disabled planes (GL_CLIP_DISTANCEi) do not clip.
    const std::string clipDecl = "float gl_ClipDistance [[clip_distance]] [";
    if (const size_t clipAt = patched.find(clipDecl); clipAt != std::string::npos) {
        const int planes = std::atoi(patched.c_str() + clipAt + clipDecl.size());
        assign += "for (uint wmgl_i = 0u; wmgl_i < " + std::to_string(planes) +
                  "u; ++wmgl_i) if ((wmgl_driver.clipEnables & (1u << wmgl_i)) == 0u) out.gl_ClipDistance[wmgl_i] = 0.0; ";
    }
    for (size_t at = main.find("return out;"); at != std::string::npos; at = main.find("return out;", at + assign.size() + 11))
        main.insert(at, assign);
    // GL_ARB_clip_control: clipFlags bit 0 is ZERO_TO_ONE depth (no depth
    // remap), bit 1 the UPPER_LEFT origin (no Y flip).
    const std::pair<std::string, std::string> clip[] = {
        {"out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;",
         "if ((wmgl_driver.clipFlags & 1u) == 0u) out.gl_Position.z = (out.gl_Position.z + out.gl_Position.w) * 0.5;"},
        {"out.gl_Position.y = -(out.gl_Position.y);",
         "if ((wmgl_driver.clipFlags & 2u) == 0u) out.gl_Position.y = -(out.gl_Position.y);"},
    };
    for (const auto& [from, to] : clip)
        for (size_t at = main.find(from); at != std::string::npos; at = main.find(from, at + to.size())) main.replace(at, from.size(), to);
    msl = patched + main;
}

// Fragment stage: GL's gl_PointCoord origin is POINT_SPRITE_COORD_ORIGIN
// (upper left by default); Metal's point_coord origin is the top of Metal's
// framebuffer, which is GL's bottom row (GL row 0 = Metal row 0). The driver
// passes a flip flag as word 0 of buffer kSlotDriverUniforms (word 1 is the
// sample mask, fragmentWithSampleMask).
bool patchPointCoord(std::string& msl) {
    const std::string param = "float2 gl_PointCoord [[point_coord]]";
    const size_t at = msl.find(param);
    const size_t fn = msl.find("fragment ");
    if (at == std::string::npos || fn == std::string::npos || at < fn) return false;
    msl.replace(at, param.size(), "float2 wmgl_point_coord [[point_coord]], constant uint* wmgl_fs_driver [[buffer(" +
                                      std::to_string(kSlotDriverUniforms) + ")]]");
    const size_t body = msl.find('{', at);
    msl.insert(body + 1, "\n    float2 gl_PointCoord = wmgl_fs_driver[0] != 0u ? float2(wmgl_point_coord.x, 1.0 - wmgl_point_coord.y) "
                         ": wmgl_point_coord;");
    return true;
}

}  // namespace

std::string fragmentWithLogicOp(const std::string& msl, unsigned op, const LogicOpTarget targets[8]) {
    const std::string signature = "fragment main0_out main0(", member = "struct main0_out\n{\n";
    const size_t structAt = msl.find(member);
    const size_t fnAt = msl.find(signature);
    if (structAt == std::string::npos || fnAt == std::string::npos || structAt > fnAt) return std::string();
    // The colour outputs: "    <type> <name> [[color(N)]];" (dual-source outputs carry ", index(1)").
    struct Output { std::string type, name; int location; };
    std::vector<Output> outputs;
    {
        const size_t end = msl.find("};", structAt);
        std::istringstream lines(msl.substr(structAt, end - structAt));
        std::string line;
        while (std::getline(lines, line)) {
            const size_t attr = line.find(" [[color(");
            if (attr == std::string::npos || line.find("index(") != std::string::npos) continue;
            std::istringstream words(line.substr(0, attr));
            Output o;
            words >> o.type >> o.name;
            o.location = std::atoi(line.c_str() + attr + 9);
            if (!o.name.empty() && o.location >= 0 && o.location < 8) outputs.push_back(o);
        }
    }
    static const char* const kOps[16] = {
        "uint4(0u)", "s & d", "s & ~d", "s", "~s & d", "d", "s ^ d", "s | d",
        "~(s | d)", "~(s ^ d)", "~d", "s | ~d", "~s", "~s | d", "~(s & d)", "uint4(0xffffffffu)"};
    std::string helpers = "static inline uint4 wmgl_logic_op(uint4 s, uint4 d) { return " + std::string(kOps[op & 15]) +
                          "; }\n"
                          "static inline float4 wmgl_logic_unorm(float4 s, float4 d, float4 m) {\n"
                          "    uint4 r = wmgl_logic_op(uint4(rint(saturate(s) * m)), uint4(rint(saturate(d) * m)));\n"
                          "    return float4(r & uint4(m)) / max(m, float4(1.0));\n}\n"
                          "static inline uint4 wmgl_logic_uint(uint4 s, uint4 d, uint4 mask) { return wmgl_logic_op(s, d) & mask; }\n"
                          "static inline int4 wmgl_logic_int(int4 s, int4 d, uint4 shift) {\n"
                          "    return as_type<int4>(wmgl_logic_op(as_type<uint4>(s), as_type<uint4>(d)) << shift) >> int4(shift);\n}\n\n";
    std::string params, assign;
    for (const Output& o : outputs) {
        const LogicOpTarget& t = targets[o.location];
        if (!t.kind) continue;
        const char base = o.type.rfind("float", 0) == 0 ? 'n' : o.type.rfind("uint", 0) == 0 ? 'u' : o.type.rfind("int", 0) == 0 ? 'i' : 0;
        if (base != t.kind) continue;  // an output of the wrong kind for its attachment is undefined in GL
        const int n = std::isdigit(static_cast<unsigned char>(o.type.back())) ? o.type.back() - '0' : 1;
        const std::string vec = base == 'n' ? "float4" : base == 'u' ? "uint4" : "int4";
        const std::string scalar = base == 'n' ? "0.0" : base == 'u' ? "0u" : "0";
        const std::string dst = "wmgl_dst" + std::to_string(o.location);
        params += vec + " " + dst + " [[color(" + std::to_string(o.location) + ")]], ";
        std::string source = "out." + o.name;
        if (n < 4) {
            source = vec + "(" + source;
            for (int c = n; c < 4; ++c) source += ", " + scalar;
            source += ")";
        }
        std::string extra;
        for (int c = 0; c < 4; ++c) {
            const unsigned bits = t.bits[c];
            if (c) extra += ", ";
            if (base == 'n') extra += std::to_string(bits ? (1u << bits) - 1 : 0) + ".0";
            else if (base == 'u') extra += std::to_string(bits >= 32 ? 0xffffffffu : (1u << bits) - 1) + "u";
            else extra += std::to_string(bits >= 32 ? 0u : 32u - bits) + "u";
        }
        const std::string fn = base == 'n' ? "wmgl_logic_unorm" : base == 'u' ? "wmgl_logic_uint" : "wmgl_logic_int";
        std::string value = fn + "(" + source + ", " + dst + ", " + vec + "(" + extra + "))";
        static const char* const kSwizzle[5] = {"", ".x", ".xy", ".xyz", ""};
        if (n < 4) value = "(" + value + ")" + kSwizzle[n];
        assign += "out." + o.name + " = " + value + "; ";
    }
    if (assign.empty()) return msl;
    std::string out = msl;
    out.insert(fnAt, helpers);
    const size_t fn = out.find(signature);
    const size_t open = fn + signature.size() - 1;
    if (out[open + 1] == ')') params.resize(params.size() - 2);
    out.insert(open + 1, params);
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

namespace {

// A fragment function without outputs (depth/stencil only): returns a struct
// holding just the sample mask.
std::string voidFragmentWithSampleMask(const std::string& msl) {
    const std::string signature = "fragment void main0(";
    const size_t fn = msl.find(signature);
    if (fn == std::string::npos) return std::string();
    const size_t bodyOpen = msl.find('{', fn);
    if (bodyOpen == std::string::npos) return std::string();
    size_t bodyClose = std::string::npos;
    int depth = 0;
    for (size_t i = bodyOpen; i < msl.size(); ++i) {
        if (msl[i] == '{') ++depth;
        else if (msl[i] == '}' && --depth == 0) {
            bodyClose = i;
            break;
        }
    }
    if (bodyClose == std::string::npos) return std::string();
    const std::string ret = "return wmgl_mask_out{wmgl_fs_driver[1]};";
    std::string body = msl.substr(bodyOpen, bodyClose - bodyOpen);
    for (size_t at = body.find("return;"); at != std::string::npos; at = body.find("return;", at + ret.size()))
        body.replace(at, 7, ret);
    std::string params = msl.substr(fn + signature.size(), bodyOpen - fn - signature.size());
    const bool hasDriver = params.find("wmgl_fs_driver [[buffer(") != std::string::npos;
    std::string head = "struct wmgl_mask_out\n{\n    uint wmgl_sample_mask [[sample_mask]];\n};\n\n"
                       "fragment wmgl_mask_out main0(";
    if (!hasDriver)
        head += "constant uint* wmgl_fs_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                (params.compare(0, 1, ")") == 0 ? "" : ", ");
    return msl.substr(0, fn) + head + params + body + "    " + ret + "\n" + msl.substr(bodyClose);
}

}  // namespace

namespace {

// The fragment entry point's parameter with `attribute` (" [[sample_mask]]"):
// its name, and where the function body starts (after its '{'). Empty if
// there is none.
std::string fragmentParameter(const std::string& msl, const char* attribute, size_t& body) {
    for (const char* signature : {"fragment main0_out main0(", "fragment void main0("}) {
        const size_t fn = msl.find(signature);
        if (fn == std::string::npos) continue;
        // The parameter list's closing parenthesis ([[buffer(N)]] has its own).
        size_t close = fn + std::strlen(signature);
        for (int depth = 1; close < msl.size() && depth; ++close) depth += msl[close] == '(' ? 1 : msl[close] == ')' ? -1 : 0;
        const std::string parameters = msl.substr(fn, close - fn);
        const size_t at = parameters.find(attribute);
        if (at == std::string::npos) return std::string();
        body = msl.find('{', close) + 1;
        // The word before the attribute.
        const size_t end = parameters.find_last_not_of(' ', at - 1) + 1;
        const size_t begin = parameters.rfind(' ', end - 1) + 1;
        return parameters.substr(begin, end - begin);
    }
    return std::string();
}

}  // namespace

std::string fragmentSampleMaskInPerSample(const std::string& msl) {
    size_t body = 0;
    const std::string mask = fragmentParameter(msl, " [[sample_mask]]", body);
    const std::string sample = fragmentParameter(msl, " [[sample_id]]", body);
    if (mask.empty() || sample.empty()) return msl;
    std::string out = msl;
    out.insert(body, "\n    " + mask + " &= (1u << " + sample + ");");
    return out;
}

std::string fragmentAtSampleRate(const std::string& msl) {
    std::string out = msl;
    // Interpolated (floating-point, not flat) inputs move to the sample.
    const size_t start = out.find("struct main0_in\n{");
    const size_t end = start == std::string::npos ? std::string::npos : out.find("};", start);
    if (start != std::string::npos && end != std::string::npos) {
        std::string body = out.substr(start, end - start), moved;
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            const size_t first = line.find_first_not_of(' ');
            const bool floating = first != std::string::npos &&
                                  (line.compare(first, 5, "float") == 0 || line.compare(first, 4, "half") == 0);
            const size_t user = line.find("[[user(");
            if (floating && user != std::string::npos && line.find(", flat]]") == std::string::npos) {
                const size_t close = line.find(")", user);
                const size_t attrEnd = line.find("]]", user);
                if (close != std::string::npos && attrEnd != std::string::npos) {
                    const bool noPerspective = line.find("no_perspective", close) < attrEnd;
                    line = line.substr(0, close + 1) + (noPerspective ? ", sample_no_perspective" : ", sample_perspective") +
                           line.substr(attrEnd);
                }
            }
            moved += line + "\n";
        }
        out.replace(start, end - start, moved);
    }
    if (out.find("[[sample_id]]") != std::string::npos) return out;
    for (const char* signature : {"fragment main0_out main0(", "fragment void main0("}) {
        const size_t fn = out.find(signature);
        if (fn == std::string::npos) continue;
        const size_t open = fn + std::strlen(signature) - 1;
        const bool noParameters = out[open + 1] == ')';
        out.insert(open + 1, std::string("uint wmgl_sample_rate [[sample_id]]") + (noParameters ? "" : ", "));
        return fragmentSampleMaskInPerSample(out);
    }
    return std::string();
}

std::string fragmentIgnoringSampleMask(const std::string& msl) {
    const size_t structAt = msl.find("struct main0_out\n{");
    const size_t structEnd = structAt == std::string::npos ? std::string::npos : msl.find("};", structAt);
    const size_t attribute = msl.find(" [[sample_mask]]", structAt);
    if (structAt == std::string::npos || attribute == std::string::npos || attribute > structEnd) return std::string();
    const size_t nameStart = msl.rfind(' ', attribute - 1) + 1;
    const std::string assign = "out." + msl.substr(nameStart, attribute - nameStart) + " = 0xffffffffu; ";
    std::string out = msl;
    const size_t fn = out.find("fragment main0_out main0(");
    if (fn == std::string::npos) return std::string();
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

std::string fragmentWithCullDistances(const std::string& msl, int count) {
    if (count <= 0 || count > 8) return msl;
    std::string out = msl;
    size_t fn = out.find("fragment main0_out main0(");
    if (fn == std::string::npos) fn = out.find("fragment void main0(");
    if (fn == std::string::npos) return std::string();
    const std::string inStruct = "struct main0_in\n{\n";
    size_t structAt = out.find(inStruct);
    if (structAt == std::string::npos || structAt > fn) {
        const size_t open = out.find('(', fn);
        const bool noParameters = out[open + 1] == ')';
        out.insert(open + 1, std::string("main0_in in [[stage_in]]") + (noParameters ? "" : ", "));
        // Before the function's line, which may start with [[early_fragment_tests]].
        const size_t line = out.rfind('\n', fn) + 1;
        out.insert(line, inStruct + "};\n\n");
        structAt = line;
    }
    std::string members, test = "\n    if (";
    for (int k = 0; k < count; ++k) {
        const std::string n = std::to_string(k);
        members += "    float wmgl_cull_keep" + n + " [[user(wmgl_cull" + n + "), center_no_perspective]];\n";
        test += std::string(k ? " || " : "") + "in.wmgl_cull_keep" + n + " == 0.0";
    }
    out.insert(out.find("};", structAt), members);
    fn = out.find("fragment ", structAt);
    out.insert(out.find('{', out.find(')', fn)) + 1, test + ")\n        discard_fragment();");
    return out;
}

std::string fragmentCountingInvocations(const std::string& msl) {
    std::string out = msl;
    size_t fn = out.find("fragment main0_out main0(");
    if (fn == std::string::npos) fn = out.find("fragment void main0(");
    if (fn == std::string::npos) return std::string();
    const size_t open = out.find('(', fn);
    const bool noParameters = out[open + 1] == ')';
    out.insert(open + 1, "device atomic_uint* wmgl_fs_invocations [[buffer(" + std::to_string(kSlotFragmentInvocations) + ")]]" +
                             (noParameters ? "" : ", "));
    out.insert(out.find('{', out.find(')', fn)) + 1,
               "\n    atomic_fetch_add_explicit(wmgl_fs_invocations, 1u, memory_order_relaxed);");
    return out;
}

std::string fragmentWithSampleMask(const std::string& msl) {
    const std::string signature = "fragment main0_out main0(", member = "struct main0_out\n{\n";
    const size_t structAt = msl.find(member);
    // The shader's own mask output: a [[sample_mask]] member of main0_out (a
    // [[sample_mask]] parameter is gl_SampleMaskIn).
    const size_t structEnd = structAt == std::string::npos ? std::string::npos : msl.find("};", structAt);
    size_t written = structAt == std::string::npos ? std::string::npos : msl.find(" [[sample_mask]]", structAt);
    if (written != std::string::npos && written > structEnd) written = std::string::npos;
    if (structAt == std::string::npos || msl.find(signature) == std::string::npos || structAt > msl.find(signature))
        return voidFragmentWithSampleMask(msl);
    std::string out = msl;
    // A shader writing gl_SampleMask: the mask applies on top of it.
    std::string maskMember = "wmgl_sample_mask";
    if (written != std::string::npos) {
        const size_t nameStart = msl.rfind(' ', written - 1) + 1;
        maskMember = msl.substr(nameStart, written - nameStart);
    } else {
        out.insert(structAt + member.size(), "    uint wmgl_sample_mask [[sample_mask]];\n");
    }
    const size_t fn = out.find(signature);
    if (out.find("wmgl_fs_driver [[buffer(") == std::string::npos) {
        const size_t open = fn + signature.size() - 1;
        const bool noParameters = out[open + 1] == ')';
        out.insert(open + 1, "constant uint* wmgl_fs_driver [[buffer(" + std::to_string(kSlotDriverUniforms) + ")]]" +
                                 (noParameters ? "" : ", "));
    }
    const std::string assign = written != std::string::npos ? "out." + maskMember + " &= wmgl_fs_driver[1]; "
                                                            : "out.wmgl_sample_mask = wmgl_fs_driver[1]; ";
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    // gl_SampleMaskIn holds no sample the mask removes.
    size_t body = 0;
    const std::string maskIn = fragmentParameter(out, " [[sample_mask]]", body);
    if (!maskIn.empty()) out.insert(body, "\n    " + maskIn + " &= wmgl_fs_driver[1];");
    return out;
}

namespace {

// Fragment stage: whether any input is flat (explicitly, or as an integer,
// which Metal never interpolates); flat inputs take the provoking vertex.
bool hasFlatInputs(const std::string& msl) {
    const size_t start = msl.find("struct main0_in\n{");
    if (start == std::string::npos) return false;
    const size_t end = msl.find("};", start);
    std::istringstream lines(msl.substr(start, end - start));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        if (line.find(", flat]]") != std::string::npos || line.compare(first, 3, "int") == 0 ||
            line.compare(first, 4, "uint") == 0 || line.compare(first, 5, "short") == 0 || line.compare(first, 6, "ushort") == 0)
            return true;
    }
    return false;
}

// Vertex stage: glslang maps gl_InstanceID to Metal's [[instance_id]], which
// counts from the draw's base instance; GL's gl_InstanceID does not
// (GL_ARB_base_instance).
void instanceIdFromZero(std::string& msl) {
    const std::string param = "uint gl_InstanceIndex [[instance_id]]";
    const size_t at = msl.find(param);
    if (at == std::string::npos) return;
    msl.replace(at, param.size(), "uint wmgl_instance_index [[instance_id]], uint wmgl_base_instance [[base_instance]]");
    const size_t body = msl.find('{', at);
    msl.insert(body + 1, "\n    uint gl_InstanceIndex = wmgl_instance_index - wmgl_base_instance;");
}

// Separable programs (GL_ARB_separate_shader_objects) are linked alone, so
// their inter-stage varyings cannot be numbered against the other stages.
// Varyings without an explicit location get a location derived from their
// name (the block name for blocks), so separately linked stages match by
// name as GL requires. Metal matches [[user(locnN)]] names, so the large
// numbers are fine; 64 locations per name leave room for arrays. They are
// only ever set in SPIR-V: GLSL locations stop at 4095 (see
// stripHashedLocations).
constexpr uint32_t kHashedLocationBase = 1024;

uint32_t nameLocation(const std::string& name) {
    uint32_t h = 2166136261u;
    for (char c : name) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return kHashedLocationBase + (h % 1000000u) * 64u;
}

// Removes hashed varying locations from GLSL (SPIRV-Cross output of a
// separable stage): "layout(location = 5389312) out vec4 c;" becomes
// "out vec4 c;" and "layout(location = 5389312, component = 1)" keeps the
// other qualifiers. The stage is then hashed again by name.
std::string stripHashedLocations(const std::string& glsl) {
    std::string out;
    size_t i = 0;
    for (size_t at = glsl.find("layout("); at != std::string::npos; at = glsl.find("layout(", i)) {
        const size_t close = glsl.find(')', at);
        if (close == std::string::npos) break;
        std::vector<std::string> kept;
        bool stripped = false;
        std::istringstream parts(glsl.substr(at + 7, close - at - 7));
        std::string part;
        while (std::getline(parts, part, ',')) {
            const size_t first = part.find_first_not_of(' ');
            const size_t eq = part.find('=');
            if (first != std::string::npos && part.compare(first, 8, "location") == 0 && eq != std::string::npos &&
                std::strtoul(part.c_str() + eq + 1, nullptr, 10) >= kHashedLocationBase) {
                stripped = true;
                continue;
            }
            kept.push_back(part.substr(first == std::string::npos ? 0 : first));
        }
        out.append(glsl, i, at - i);
        i = close + 1;
        if (!stripped) {
            out.append(glsl, at, close + 1 - at);
            continue;
        }
        if (kept.empty()) {
            if (i < glsl.size() && glsl[i] == ' ') ++i;
            continue;
        }
        out += "layout(";
        for (size_t k = 0; k < kept.size(); ++k) out += (k ? ", " : "") + kept[k];
        out += ")";
    }
    out.append(glsl, i, std::string::npos);
    return out;
}

// Buffer blocks a shader declares with the shared or std140 layout, which
// are active even when unused: glslang compiles for SPIR-V, which drops
// shared and packed, so the packing is read from the source. A block takes
// the buffer default ("layout(std430) buffer;" sets it; shared otherwise).
// imageAtomicExchange on an r32f image: Metal has texture atomics on 32-bit
// integer textures only. The image becomes an r32ui one (its binding then
// goes through an R32Uint view or shadow, imageView) and its loads, stores
// and exchanges carry the float's bits. Images used any other way than
// those and imageSize are left alone.
// GL_EXT_fragment_shading_rate's built-ins with the 1x1 rate, the only one
// (api/es.mm): gl_ShadingRateEXT reads 0 (1x1), gl_PrimitiveShadingRateEXT
// writes go to a variable nothing reads, and the rate flags are their values.
// Metal has no shading rate inputs or outputs to give SPIRV-Cross.
std::string shadingRateBuiltinsAs1x1(const std::string& glsl) {
    if (glsl.find("ShadingRate") == std::string::npos) return glsl;
    std::string out = glsl;
    auto replaceAll = [&](const std::string& word, const std::string& with) {
        const std::regex r("\\b" + word + "\\b");
        out = std::regex_replace(out, r, with);
    };
    out = std::regex_replace(out, std::regex(R"(#\s*extension\s+GL_EXT_fragment_shading_rate\s*:\s*\w+[^\n]*)"), "");
    const bool primitive = std::regex_search(out, std::regex(R"(\bgl_PrimitiveShadingRateEXT\b)"));
    replaceAll("gl_ShadingRateEXT", "0");
    replaceAll("gl_PrimitiveShadingRateEXT", "wmgl_primitive_shading_rate");
    replaceAll("gl_ShadingRateFlag2VerticalPixelsEXT", "1");
    replaceAll("gl_ShadingRateFlag4VerticalPixelsEXT", "2");
    replaceAll("gl_ShadingRateFlag2HorizontalPixelsEXT", "4");
    replaceAll("gl_ShadingRateFlag4HorizontalPixelsEXT", "8");
    if (primitive) {
        // After the leading directives (#version, #extension).
        size_t at = 0;
        while (at < out.size()) {
            const size_t first = out.find_first_not_of(" \t\r\n", at);
            if (first == std::string::npos || out[first] != '#') break;
            const size_t end = out.find('\n', first);
            at = end == std::string::npos ? out.size() : end + 1;
        }
        out.insert(at, "highp int wmgl_primitive_shading_rate;\n");
    }
    return out;
}

// SPIRV-Cross fences a read_write texture after its writes but not after its
// atomics: a thread that adds to a texel atomically and then reads it may
// not see its own add on Apple GPUs (GL's coherent images and program order
// require it). Each statement with a texture atomic gets the texture's fence.
// On the M6 (macOS 27.3) the fence is not enough: read() after an atomic and
// a fence still returns the texel from before the atomic (81900 of 81920
// texels in a Metal kernel doing only that), where atomic_load() returns the
// atomic's result. So a texture with atomics is read with atomic_load(),
// which takes the same coordinates: GL has atomics on 32-bit integer images
// alone, which atomic_load reads as read() does.
std::string fenceTextureAtomics(const std::string& msl) {
    if (msl.find(".atomic_") == std::string::npos) return msl;
    static const std::regex kAtomic(R"(\b(\w+)(\[[^\]]*\])?\.atomic_(fetch_\w+|exchange|compare_exchange_weak|store|load)\()");
    std::string out;
    std::set<std::string> atomicTextures;
    size_t at = 0;
    for (auto it = std::sregex_iterator(msl.begin(), msl.end(), kAtomic); it != std::sregex_iterator(); ++it) {
        atomicTextures.insert((*it)[1].str());
        const size_t end = msl.find(";\n", static_cast<size_t>(it->position(0)));
        if (end == std::string::npos || end < at) continue;
        out.append(msl, at, end + 1 - at);
        out += " " + (*it)[1].str() + (*it)[2].str() + ".fence();";
        at = end + 1;
    }
    out.append(msl, at, std::string::npos);
    for (const std::string& name : atomicTextures) {
        const std::regex read("\\b(" + name + R"((?:\[[^\]]*\])?)\.read\()");
        out = std::regex_replace(out, read, "$1.atomic_load(");
    }
    return out;
}

std::string floatImageAtomicsAsUint(const std::string& glsl) {
    if (glsl.find("imageAtomicExchange") == std::string::npos) return glsl;
    static const std::regex kDecl(
        R"(layout\s*\(([^)]*\br32f\b[^)]*)\)((?:\s*\w+)*?)\s+(image(?:2D|3D|Cube|2DArray|CubeArray|Buffer))\s+(\w+)\s*;)");
    std::string out = glsl;
    std::vector<std::string> names;
    for (auto it = std::sregex_iterator(glsl.begin(), glsl.end(), kDecl); it != std::sregex_iterator(); ++it)
        names.push_back((*it)[4]);
    // The arguments of the call whose '(' is at `open`, and the index past its ')'.
    auto arguments = [](const std::string& text, size_t open, std::vector<std::string>& args) {
        args.clear();
        int depth = 0;
        size_t start = open + 1;
        for (size_t i = open; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') {
                if (--depth == 0) {
                    args.push_back(text.substr(start, i - start));
                    return i + 1;
                }
            } else if (c == ',' && depth == 1) {
                args.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        return std::string::npos;
    };
    for (const std::string& name : names) {
        const std::regex use("\\b" + name + "\\b");
        const std::regex call("\\b(imageAtomicExchange|imageLoad|imageStore|imageSize)\\s*\\(\\s*" + name + "\\b");
        size_t uses = 0, calls = 0;
        for (auto it = std::sregex_iterator(out.begin(), out.end(), use); it != std::sregex_iterator(); ++it) ++uses;
        for (auto it = std::sregex_iterator(out.begin(), out.end(), call); it != std::sregex_iterator(); ++it) ++calls;
        bool exchanged = false;
        for (auto it = std::sregex_iterator(out.begin(), out.end(), call); it != std::sregex_iterator(); ++it)
            exchanged |= (*it)[1] == "imageAtomicExchange";
        if (!exchanged || uses != calls + 1) continue;  // the declaration, then only these calls
        std::string rewritten;
        size_t at = 0;
        std::smatch m;
        std::string rest = out;
        while (std::regex_search(rest, m, call)) {
            const size_t begin = at + static_cast<size_t>(m.position(0));
            const size_t open = out.find('(', begin);
            std::vector<std::string> args;
            const size_t end = arguments(out, open, args);
            if (end == std::string::npos) return glsl;
            rewritten += out.substr(at, begin - at);
            const std::string fn = m[1];
            if (fn == "imageAtomicExchange" && args.size() == 3)
                rewritten += "uintBitsToFloat(imageAtomicExchange(" + args[0] + "," + args[1] + ", floatBitsToUint(" + args[2] + ")))";
            else if (fn == "imageLoad" && args.size() == 2)
                rewritten += "vec4(uintBitsToFloat(imageLoad(" + args[0] + "," + args[1] + ").x), 0.0, 0.0, 1.0)";
            else if (fn == "imageStore" && args.size() == 3)
                rewritten += "imageStore(" + args[0] + "," + args[1] + ", uvec4(floatBitsToUint((" + args[2] + ").x)))";
            else
                rewritten += out.substr(begin, end - begin);
            at = end;
            rest = out.substr(at);
        }
        rewritten += out.substr(at);
        out = rewritten;
        // The declaration: r32ui, uimage, highp unless a precision is given.
        const std::regex decl(
            "layout\\s*\\(([^)]*)\\br32f\\b([^)]*)\\)((?:\\s*\\w+)*?)\\s+image(2D|3D|Cube|2DArray|CubeArray|Buffer)\\s+" + name +
            "\\s*;");
        std::smatch d;
        if (!std::regex_search(out, d, decl)) return glsl;
        const std::string qualifiers = d[3];
        const bool precision = std::regex_search(qualifiers, std::regex("\\b(lowp|mediump|highp)\\b"));
        out.replace(static_cast<size_t>(d.position(0)), static_cast<size_t>(d.length(0)),
                    "layout(" + std::string(d[1]) + "r32ui" + std::string(d[2]) + ")" + qualifiers + (precision ? "" : " highp") +
                        " uimage" + std::string(d[4]) + " " + name + ";");
    }
    return out;
}

std::set<std::string> sharedBufferBlocks(const std::string& source) {
    std::set<std::string> out;
    static const std::regex kDecl(R"(((?:layout\s*\(([^)]*)\)\s*)?)((?:\w+\s+)*?)buffer\s*(\w*)\s*(\{|;))");
    std::string defaultPacking = "shared";
    auto packingOf = [](const std::string& qualifiers, const std::string& fallback) {
        std::string packing = fallback;
        static const std::regex kPacking(R"(\b(shared|packed|std140|std430)\b)");
        for (auto it = std::sregex_iterator(qualifiers.begin(), qualifiers.end(), kPacking); it != std::sregex_iterator(); ++it)
            packing = (*it)[1];
        return packing;
    };
    for (auto it = std::sregex_iterator(source.begin(), source.end(), kDecl); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        if (m[5] == ";" && m[4].length() == 0) {
            defaultPacking = packingOf(m[2], defaultPacking);  // "layout(std430) buffer;"
            continue;
        }
        if (m[5] != "{" || m[4].length() == 0) continue;
        const std::string packing = packingOf(m[2], defaultPacking);
        if (packing == "shared" || packing == "std140") out.insert(m[4]);
    }
    return out;
}

void hashVaryingLocations(std::vector<uint32_t>& spirv, Stage stage, const std::set<std::string>& explicitNames) {
    if (spirv.size() < 5) return;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> blockTypes;                       // structs decorated Block
    std::map<uint32_t, uint32_t> pointee, arrayElement;  // pointer -> type, array -> element
    std::map<uint32_t, uint32_t> storage;                // variable -> storage class
    std::map<uint32_t, uint32_t> varType;                // variable -> pointer type
    std::set<uint32_t> builtins;
    std::vector<size_t> locationWords;                   // OpDecorate Location: index of the target word
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!count) break;
        switch (op) {
        case 5:  // OpName
            names[spirv[i + 1]] = reinterpret_cast<const char*>(&spirv[i + 2]);
            break;
        case 32: pointee[spirv[i + 1]] = spirv[i + 3]; break;  // OpTypePointer
        case 28: arrayElement[spirv[i + 1]] = spirv[i + 2]; break;  // OpTypeArray
        case 29: arrayElement[spirv[i + 1]] = spirv[i + 2]; break;  // OpTypeRuntimeArray
        case 59:  // OpVariable
            varType[spirv[i + 2]] = spirv[i + 1];
            storage[spirv[i + 2]] = spirv[i + 3];
            break;
        case 71:  // OpDecorate
            if (spirv[i + 2] == 11) builtins.insert(spirv[i + 1]);
            if (spirv[i + 2] == 2) blockTypes.insert(spirv[i + 1]);
            if (spirv[i + 2] == 30) locationWords.push_back(i);
            break;
        default: break;
        }
        i += count;
    }
    for (size_t at : locationWords) {
        const uint32_t var = spirv[at + 1];
        auto s = storage.find(var);
        if (s == storage.end() || builtins.count(var)) continue;
        const bool input = s->second == 1, output = s->second == 3;
        if (!(input && stage != Stage::Vertex) && !(output && stage != Stage::Fragment)) continue;
        // The variable's name, or its block's name (GL matches blocks by
        // block name, not instance name).
        uint32_t type = pointee[varType[var]];
        while (arrayElement.count(type)) type = arrayElement[type];
        std::string name = names[var];
        if (blockTypes.count(type) || name.empty() || name.compare(0, 3, "gl_") == 0) name = names[type];
        name = unescapeName(name);
        if (name.empty() || name.compare(0, 3, "gl_") == 0 || explicitNames.count(name)) continue;
        spirv[at + 3] = nameLocation(name);
    }
}

// Strips glslang's trailing "N compilation errors" noise but keeps messages.
std::string cleanLog(const char* info, const char* debug) {
    std::string log = info ? info : "";
    if (debug && *debug) log += debug;
    return log;
}

struct ParsedShader {
    std::unique_ptr<glslang::TShader> shader;
    std::string source;    // codegen source (after legacy upgrade)
    std::string epilogue;  // transform feedback capture code (second string)
};

// Inserts `text` after the #version line and the #extension lines that
// follow it (with comments, blank lines and the conditionals around them,
// as in "#ifdef X / #extension ... / #endif"), then restores the original
// line numbering.
std::string insertAfterDirectives(const std::string& source, const std::string& text, int glslVersion) {
    size_t at = 0, insertAt = 0;
    int line = 1, insertLine = 1, depth = 0;
    bool sawVersion = false, inComment = false;
    while (at < source.size()) {
        size_t end = source.find('\n', at);
        if (end == std::string::npos) end = source.size();
        const std::string l = source.substr(at, end - at);
        const size_t first = l.find_first_not_of(" \t\r");
        bool skip = first == std::string::npos;
        if (!skip && inComment) skip = true;
        if (!skip && l.compare(first, 2, "//") == 0) skip = true;
        if (!skip && l.compare(first, 2, "/*") == 0) skip = inComment = true;
        if (inComment && l.find("*/") != std::string::npos) {
            // A comment that ends on this line, with nothing after it.
            const size_t close = l.find("*/");
            inComment = false;
            if (l.find_first_not_of(" \t\r", close + 2) != std::string::npos) break;
        }
        if (!skip) {
            if (l[first] != '#') break;
            const size_t word = l.find_first_not_of(" \t", first + 1);
            auto is = [&](const char* directive) {
                const size_t n = std::strlen(directive);
                return word != std::string::npos && l.compare(word, n, directive) == 0 &&
                       (word + n == l.size() || !std::isalnum(static_cast<unsigned char>(l[word + n])));
            };
            if (is("version")) sawVersion = true;
            else if (!sawVersion) break;
            else if (is("if") || is("ifdef") || is("ifndef")) ++depth;
            else if (is("endif")) --depth;
            else if (!is("extension") && !is("else") && !is("elif")) break;
        }
        at = end < source.size() ? end + 1 : end;
        ++line;
        // Only outside conditionals, after the #version line.
        if (sawVersion && depth == 0 && !inComment) {
            insertAt = at;
            insertLine = line;
        }
    }
    if (!sawVersion) {
        insertAt = 0;
        insertLine = 1;
    }
    // GLSL 3.30+: "#line N" numbers the next line N; earlier versions N + 1.
    const int lineValue = glslVersion >= 330 ? insertLine : insertLine - 1;
    return source.substr(0, insertAt) + text + "#line " + std::to_string(lineValue) + "\n" + source.substr(insertAt);
}

std::string raiseComputeVersion(Stage stage, const std::string& source);
std::string relaxBitcastConstants(const std::string& source);

// Whether a shader object defines main() (one of several objects per stage
// may only hold helper functions). Comments and directives are skipped.
bool definesMain(const std::string& source) {
    const size_t n = source.size();
    // Skips whitespace and comments from j; returns the next position.
    auto skip = [&](size_t j, bool* newline) {
        while (j < n) {
            if (source[j] == '\n' && newline) *newline = true;
            if (std::isspace(static_cast<unsigned char>(source[j]))) {
                ++j;
            } else if (source.compare(j, 2, "//") == 0) {
                const size_t end = source.find('\n', j);
                j = end == std::string::npos ? n : end;
            } else if (source.compare(j, 2, "/*") == 0) {
                const size_t end = source.find("*/", j + 2);
                j = end == std::string::npos ? n : end + 2;
            } else {
                break;
            }
        }
        return j;
    };
    size_t i = 0;
    int depth = 0;
    bool lineStart = true;
    while ((i = skip(i, &lineStart)) < n) {
        const char c = source[i];
        if (c == '#' && lineStart) {
            while (i < n && source[i] != '\n') i += source[i] == '\\' && i + 1 < n ? 2 : 1;
            continue;
        }
        lineStart = false;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = i;
            while (end < n && (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_')) ++end;
            const bool isMain = depth == 0 && end - i == 4 && source.compare(i, 4, "main") == 0;
            i = end;
            if (!isMain) continue;
            size_t j = skip(i, nullptr);
            if (j >= n || source[j] != '(') continue;
            j = source.find(')', j);
            if (j == std::string::npos) return false;
            j = skip(j + 1, nullptr);
            if (j < n && source[j] == '{') return true;
            continue;
        }
        depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        ++i;
    }
    return false;
}

// The GLSL version capture code is written for: GLSL ES shaders have
// storage buffers (as GLSL 4.30's) from GLSL ES 3.10, which parseForCodegen
// raises GLSL ES 3.00 to.
int captureVersion(const std::string& source) {
    const GlslVersion v = parseGlslVersion(source);
    return v.es ? 430 : v.number;
}

// Compute limits glslang does not check: MAX_COMPUTE_WORK_GROUP_INVOCATIONS
// and MAX_COMPUTE_SHARED_MEMORY_SIZE (shared variables at 4 bytes per
// scalar component, booleans included; no padding counted).
std::string checkComputeLimits(glslang::TShader& shader) {
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    if (!intermediate) return {};
    const int64_t invocations = static_cast<int64_t>(intermediate->getLocalSize(0)) * intermediate->getLocalSize(1) *
                                intermediate->getLocalSize(2);
    if (invocations > limits::kMaxComputeWorkGroupInvocations)
        return "ERROR: local work group size exceeds GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS\n";
    std::function<int64_t(const glslang::TType&)> bytes = [&](const glslang::TType& t) -> int64_t {
        int64_t n = 1;
        if (t.isArray() && t.isSizedArray()) n = t.getArraySizes()->getCumulativeSize();
        if (t.isStruct()) {
            int64_t sum = 0;
            for (const glslang::TTypeLoc& m : *t.getStruct()) sum += bytes(*m.type);
            return n * sum;
        }
        const int64_t scalar = t.getBasicType() == glslang::EbtDouble || t.getBasicType() == glslang::EbtInt64 ||
                                       t.getBasicType() == glslang::EbtUint64
                                   ? 8
                                   : 4;
        const int64_t components = t.isMatrix() ? t.getMatrixCols() * t.getMatrixRows() : std::max(1, t.getVectorSize());
        return n * components * scalar;
    };
    int64_t shared = 0;
    glslang::TIntermAggregate* root = intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (root)
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence())
                if (glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                    symbol && symbol->getQualifier().storage == glslang::EvqShared)
                    shared += bytes(symbol->getType());
        }
    if (shared > limits::kMaxComputeSharedMemorySize)
        return "ERROR: shared variables exceed GL_MAX_COMPUTE_SHARED_MEMORY_SIZE\n";
    return {};
}

bool parseForCodegen(Stage stage, const std::string& original, ParsedShader& out, std::string& log,
                     const CaptureCode* capture = nullptr, const UpgradeOptions& upgrade = {}) {
    const EShLanguage lang = language(stage);
    const SubroutineLowering subroutines = lowerSubroutines(stage, spliceLineContinuations(original));
    if (!subroutines.ok) {
        log += subroutines.log;
        return false;
    }
    UpgradeResult upgraded =
        upgradeLegacyGlsl(stage, lowerBuiltinUniforms(stage, raiseComputeVersion(stage, relaxBitcastConstants(subroutines.source))),
                          upgrade);
    if (!upgraded.ok) {
        log += upgraded.log;
        return false;
    }
    out.source = escapeVulkanKeywords(upgraded.source);
    if (capture) {
        // Buffer blocks need a profile (GLSL 1.50+). Upgraded 1.40 code has no legacy built-ins left, so 1.50 core takes it.
        // GLSL ES has them from 3.10, a superset of 3.00.
        const GlslVersion parsedVersion = parseGlslVersion(out.source);
        int version = parsedVersion.number;
        if (parsedVersion.es) {
            if (version < 310) {
                const size_t at = out.source.find("#version");
                const size_t end = out.source.find('\n', at);
                out.source.replace(at, end == std::string::npos ? std::string::npos : end - at, "#version 310 es");
            }
            version = 430;
        } else if (version < 150) {
            const size_t at = out.source.find("#version");
            if (at != std::string::npos) {
                const size_t end = out.source.find('\n', at);
                out.source.replace(at, end == std::string::npos ? std::string::npos : end - at, "#version 150 core");
                version = 150;
            }
        }
        out.source = insertAfterDirectives(out.source, capture->declarations, version);
        out.epilogue = capture->epilogue;
    }
    out.shader = std::make_unique<glslang::TShader>(lang);
    const char* texts[2] = {out.source.c_str(), out.epilogue.c_str()};
    out.shader->setStrings(texts, capture ? 2 : 1);
    configure(*out.shader, lang);
    // Validity was decided by compile()'s GL-rules parse; codegen tolerates
    // what desktop drivers accept (e.g. 'defined' produced by macro expansion).
    if (!out.shader->parse(resources(), 140, ENoProfile, false, false,
                           static_cast<EShMessages>(kMessages | EShMsgRelaxedErrors))) {
        log += unescapeName(cleanLog(out.shader->getInfoLog(), out.shader->getInfoDebugLog()));
        return false;
    }
    if (stage == Stage::Compute) {
        const std::string limitErrors = checkComputeLimits(*out.shader);
        if (!limitErrors.empty()) {
            log += limitErrors;
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------ GL types

GLenum glTypeOf(const spirv_cross::SPIRType& type) {
    using BT = spirv_cross::SPIRType::BaseType;
    const uint32_t n = type.vecsize, c = type.columns;
    if (type.basetype == BT::SampledImage || type.basetype == BT::Image || type.basetype == BT::Sampler) {
        const auto& image = type.image;
        const bool array = image.arrayed, ms = image.ms, shadow = image.depth;
        const BT sampled = BT::Float;  // refined below by caller via image.type
        (void)sampled;
        switch (image.dim) {
        case spv::Dim1D: return shadow ? (array ? GL_SAMPLER_1D_ARRAY_SHADOW : GL_SAMPLER_1D_SHADOW) : (array ? GL_SAMPLER_1D_ARRAY : GL_SAMPLER_1D);
        case spv::Dim2D:
            if (ms) return array ? GL_SAMPLER_2D_MULTISAMPLE_ARRAY : GL_SAMPLER_2D_MULTISAMPLE;
            return shadow ? (array ? GL_SAMPLER_2D_ARRAY_SHADOW : GL_SAMPLER_2D_SHADOW) : (array ? GL_SAMPLER_2D_ARRAY : GL_SAMPLER_2D);
        case spv::Dim3D: return GL_SAMPLER_3D;
        case spv::DimCube: return shadow ? (array ? GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW : GL_SAMPLER_CUBE_SHADOW) : (array ? GL_SAMPLER_CUBE_MAP_ARRAY : GL_SAMPLER_CUBE);
        case spv::DimRect: return shadow ? GL_SAMPLER_2D_RECT_SHADOW : GL_SAMPLER_2D_RECT;
        case spv::DimBuffer: return GL_SAMPLER_BUFFER;
        default: return GL_SAMPLER_2D;
        }
    }
    auto pick = [&](GLenum s, GLenum v2, GLenum v3, GLenum v4) { return n == 1 ? s : n == 2 ? v2 : n == 3 ? v3 : v4; };
    switch (type.basetype) {
    case BT::Float:
        if (c == 1) return pick(GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4);
        if (c == 2) return n == 2 ? GL_FLOAT_MAT2 : n == 3 ? GL_FLOAT_MAT2x3 : GL_FLOAT_MAT2x4;
        if (c == 3) return n == 2 ? GL_FLOAT_MAT3x2 : n == 3 ? GL_FLOAT_MAT3 : GL_FLOAT_MAT3x4;
        return n == 2 ? GL_FLOAT_MAT4x2 : n == 3 ? GL_FLOAT_MAT4x3 : GL_FLOAT_MAT4;
    case BT::Double:
        if (c == 1) return pick(GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4);
        if (c == 2) return n == 2 ? GL_DOUBLE_MAT2 : n == 3 ? GL_DOUBLE_MAT2x3 : GL_DOUBLE_MAT2x4;
        if (c == 3) return n == 2 ? GL_DOUBLE_MAT3x2 : n == 3 ? GL_DOUBLE_MAT3 : GL_DOUBLE_MAT3x4;
        return n == 2 ? GL_DOUBLE_MAT4x2 : n == 3 ? GL_DOUBLE_MAT4x3 : GL_DOUBLE_MAT4;
    case BT::Int: return pick(GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4);
    case BT::UInt: return pick(GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4);
    case BT::Boolean: return pick(GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4);
    default: return 0;
    }
}

GLenum samplerTypeWithComponent(GLenum base, spirv_cross::SPIRType::BaseType component) {
    using BT = spirv_cross::SPIRType::BaseType;
    if (component == BT::Int) {
        switch (base) {
        case GL_SAMPLER_1D: return GL_INT_SAMPLER_1D;
        case GL_SAMPLER_2D: return GL_INT_SAMPLER_2D;
        case GL_SAMPLER_3D: return GL_INT_SAMPLER_3D;
        case GL_SAMPLER_CUBE: return GL_INT_SAMPLER_CUBE;
        case GL_SAMPLER_1D_ARRAY: return GL_INT_SAMPLER_1D_ARRAY;
        case GL_SAMPLER_2D_ARRAY: return GL_INT_SAMPLER_2D_ARRAY;
        case GL_SAMPLER_2D_RECT: return GL_INT_SAMPLER_2D_RECT;
        case GL_SAMPLER_BUFFER: return GL_INT_SAMPLER_BUFFER;
        case GL_SAMPLER_2D_MULTISAMPLE: return GL_INT_SAMPLER_2D_MULTISAMPLE;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: return GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
        case GL_SAMPLER_CUBE_MAP_ARRAY: return GL_INT_SAMPLER_CUBE_MAP_ARRAY;
        default: return base;
        }
    }
    if (component == BT::UInt) {
        switch (base) {
        case GL_SAMPLER_1D: return GL_UNSIGNED_INT_SAMPLER_1D;
        case GL_SAMPLER_2D: return GL_UNSIGNED_INT_SAMPLER_2D;
        case GL_SAMPLER_3D: return GL_UNSIGNED_INT_SAMPLER_3D;
        case GL_SAMPLER_CUBE: return GL_UNSIGNED_INT_SAMPLER_CUBE;
        case GL_SAMPLER_1D_ARRAY: return GL_UNSIGNED_INT_SAMPLER_1D_ARRAY;
        case GL_SAMPLER_2D_ARRAY: return GL_UNSIGNED_INT_SAMPLER_2D_ARRAY;
        case GL_SAMPLER_2D_RECT: return GL_UNSIGNED_INT_SAMPLER_2D_RECT;
        case GL_SAMPLER_BUFFER: return GL_UNSIGNED_INT_SAMPLER_BUFFER;
        case GL_SAMPLER_2D_MULTISAMPLE: return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE;
        case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: return GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY;
        case GL_SAMPLER_CUBE_MAP_ARRAY: return GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY;
        default: return base;
        }
    }
    return base;
}

// The GL internal format of an image format layout qualifier (0: none).
GLenum imageFormatOf(spv::ImageFormat format) {
    switch (format) {
    case spv::ImageFormatRgba32f: return GL_RGBA32F;
    case spv::ImageFormatRgba16f: return GL_RGBA16F;
    case spv::ImageFormatR32f: return GL_R32F;
    case spv::ImageFormatRgba8: return GL_RGBA8;
    case spv::ImageFormatRgba8Snorm: return GL_RGBA8_SNORM;
    case spv::ImageFormatRg32f: return GL_RG32F;
    case spv::ImageFormatRg16f: return GL_RG16F;
    case spv::ImageFormatR11fG11fB10f: return GL_R11F_G11F_B10F;
    case spv::ImageFormatR16f: return GL_R16F;
    case spv::ImageFormatRgba16: return GL_RGBA16;
    case spv::ImageFormatRgb10A2: return GL_RGB10_A2;
    case spv::ImageFormatRg16: return GL_RG16;
    case spv::ImageFormatRg8: return GL_RG8;
    case spv::ImageFormatR16: return GL_R16;
    case spv::ImageFormatR8: return GL_R8;
    case spv::ImageFormatRgba16Snorm: return GL_RGBA16_SNORM;
    case spv::ImageFormatRg16Snorm: return GL_RG16_SNORM;
    case spv::ImageFormatRg8Snorm: return GL_RG8_SNORM;
    case spv::ImageFormatR16Snorm: return GL_R16_SNORM;
    case spv::ImageFormatR8Snorm: return GL_R8_SNORM;
    case spv::ImageFormatRgba32i: return GL_RGBA32I;
    case spv::ImageFormatRgba16i: return GL_RGBA16I;
    case spv::ImageFormatRgba8i: return GL_RGBA8I;
    case spv::ImageFormatR32i: return GL_R32I;
    case spv::ImageFormatRg32i: return GL_RG32I;
    case spv::ImageFormatRg16i: return GL_RG16I;
    case spv::ImageFormatRg8i: return GL_RG8I;
    case spv::ImageFormatR16i: return GL_R16I;
    case spv::ImageFormatR8i: return GL_R8I;
    case spv::ImageFormatRgba32ui: return GL_RGBA32UI;
    case spv::ImageFormatRgba16ui: return GL_RGBA16UI;
    case spv::ImageFormatRgba8ui: return GL_RGBA8UI;
    case spv::ImageFormatR32ui: return GL_R32UI;
    case spv::ImageFormatRgb10a2ui: return GL_RGB10_A2UI;
    case spv::ImageFormatRg32ui: return GL_RG32UI;
    case spv::ImageFormatRg16ui: return GL_RG16UI;
    case spv::ImageFormatRg8ui: return GL_RG8UI;
    case spv::ImageFormatR16ui: return GL_R16UI;
    case spv::ImageFormatR8ui: return GL_R8UI;
    default: return 0;
    }
}

// The GL type of a storage image (GL 4.2).
GLenum imageTypeOf(const spirv_cross::SPIRType& base, spirv_cross::SPIRType::BaseType component) {
    using BT = spirv_cross::SPIRType::BaseType;
    const auto& image = base.image;
    const int kind = component == BT::Int ? 1 : component == BT::UInt ? 2 : 0;
    static const GLenum kTypes[3][11] = {
        {GL_IMAGE_1D, GL_IMAGE_2D, GL_IMAGE_3D, GL_IMAGE_2D_RECT, GL_IMAGE_CUBE, GL_IMAGE_BUFFER, GL_IMAGE_1D_ARRAY,
         GL_IMAGE_2D_ARRAY, GL_IMAGE_CUBE_MAP_ARRAY, GL_IMAGE_2D_MULTISAMPLE, GL_IMAGE_2D_MULTISAMPLE_ARRAY},
        {GL_INT_IMAGE_1D, GL_INT_IMAGE_2D, GL_INT_IMAGE_3D, GL_INT_IMAGE_2D_RECT, GL_INT_IMAGE_CUBE, GL_INT_IMAGE_BUFFER,
         GL_INT_IMAGE_1D_ARRAY, GL_INT_IMAGE_2D_ARRAY, GL_INT_IMAGE_CUBE_MAP_ARRAY, GL_INT_IMAGE_2D_MULTISAMPLE,
         GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY},
        {GL_UNSIGNED_INT_IMAGE_1D, GL_UNSIGNED_INT_IMAGE_2D, GL_UNSIGNED_INT_IMAGE_3D, GL_UNSIGNED_INT_IMAGE_2D_RECT,
         GL_UNSIGNED_INT_IMAGE_CUBE, GL_UNSIGNED_INT_IMAGE_BUFFER, GL_UNSIGNED_INT_IMAGE_1D_ARRAY,
         GL_UNSIGNED_INT_IMAGE_2D_ARRAY, GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY, GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE,
         GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY}};
    int index = 1;
    switch (image.dim) {
    case spv::Dim1D: index = image.arrayed ? 6 : 0; break;
    case spv::Dim2D: index = image.ms ? (image.arrayed ? 10 : 9) : (image.arrayed ? 7 : 1); break;
    case spv::Dim3D: index = 2; break;
    case spv::DimRect: index = 3; break;
    case spv::DimCube: index = image.arrayed ? 8 : 4; break;
    case spv::DimBuffer: index = 5; break;
    default: break;
    }
    return kTypes[kind][index];
}

int textureTargetForSampler(GLenum type) {
    // Must match TextureTarget in core/state.h.
    switch (type) {
    case GL_SAMPLER_1D: case GL_SAMPLER_1D_SHADOW: case GL_INT_SAMPLER_1D: case GL_UNSIGNED_INT_SAMPLER_1D: return 0;
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D: return 1;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D: return 2;
    case GL_SAMPLER_1D_ARRAY: case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_INT_SAMPLER_1D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_1D_ARRAY: return 3;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: return 4;
    case GL_SAMPLER_2D_RECT: case GL_SAMPLER_2D_RECT_SHADOW: case GL_INT_SAMPLER_2D_RECT: case GL_UNSIGNED_INT_SAMPLER_2D_RECT: return 5;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE: return 6;
    case GL_SAMPLER_BUFFER: case GL_INT_SAMPLER_BUFFER: case GL_UNSIGNED_INT_SAMPLER_BUFFER: return 7;
    case GL_SAMPLER_2D_MULTISAMPLE: case GL_INT_SAMPLER_2D_MULTISAMPLE: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE: return 8;
    case GL_SAMPLER_2D_MULTISAMPLE_ARRAY: case GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY: case GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY: return 9;
    case GL_SAMPLER_CUBE_MAP_ARRAY: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: case GL_INT_SAMPLER_CUBE_MAP_ARRAY: case GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY: return 10;
    default: return -1;
    }
}

bool isShadowSampler(GLenum type) {
    switch (type) {
    case GL_SAMPLER_1D_SHADOW: case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_SAMPLER_CUBE_SHADOW: case GL_SAMPLER_2D_RECT_SHADOW: case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: return true;
    default: return false;
    }
}

// Name with every array subscript removed ("B[1].s[0].x" -> "B.s.x"), used to
// match glslang's reflection names against GL interface names.
std::string arrayBaseName(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '[') {
            while (i < name.size() && name[i] != ']') ++i;
            continue;
        }
        out += name[i];
    }
    return out;
}

GLenum boolType(const glslang::TType& type) {
    switch (type.getVectorSize()) {
    case 2: return GL_BOOL_VEC2;
    case 3: return GL_BOOL_VEC3;
    case 4: return GL_BOOL_VEC4;
    default: return GL_BOOL;
    }
}

// Records the GL bool type of every bool member under `prefix` (names without
// array subscripts, see arrayBaseName). With `self`, `type` itself is named `prefix`.
void collectBoolMembers(const glslang::TType& type, const std::string& prefix, std::map<std::string, GLenum>& out,
                        bool self = false) {
    if (self) {
        if (type.getBasicType() == glslang::EbtBool) out[prefix] = boolType(type);
        else if (type.isStruct()) collectBoolMembers(type, prefix + ".", out);
        return;
    }
    if (!type.isStruct()) return;
    for (const glslang::TTypeLoc& member : *type.getStruct()) {
        const glslang::TType& m = *member.type;
        const std::string name = prefix + unescapeName(m.getFieldName().c_str());
        if (m.isStruct()) collectBoolMembers(m, name + ".", out);
        else if (m.getBasicType() == glslang::EbtBool) out[name] = boolType(m);
    }
}

std::string topLevelName(const std::string& name) {
    const size_t end = name.find_first_of(".[");
    return end == std::string::npos ? name : name.substr(0, end);
}

// The dimensions of an array member, outermost first (0: runtime-sized),
// and the byte stride of each level (SPIR-V decorates only the outermost
// one; nested arrays are tightly packed in std140 and std430).
struct ArrayLevels {
    std::vector<uint32_t> sizes, strides;
};
ArrayLevels arrayLevels(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& parent, uint32_t index,
                        const spirv_cross::SPIRType& type) {
    ArrayLevels levels;
    for (size_t d = type.array.size(); d-- > 0;)
        levels.sizes.push_back(type.array_size_literal[d] ? type.array[d] : 0);
    if (levels.sizes.empty()) return levels;
    uint32_t stride = compiler.type_struct_member_array_stride(parent, index);
    if (stride == 0) {
        // Some arrays come without an ArrayStride value; blocks use std140 rules,
        // where every array element starts on a 16-byte boundary (a matrix
        // element spans one 16-byte vector per column, or per row if row-major).
        const bool rowMajor = compiler.has_member_decoration(parent.self, index, spv::DecorationRowMajor);
        stride = 16u * (type.columns > 1 ? (rowMajor ? type.vecsize : type.columns) : 1u);
        for (size_t d = 1; d < levels.sizes.size(); ++d) stride *= std::max(1u, levels.sizes[d]);
    }
    for (size_t d = 0; d < levels.sizes.size(); ++d) {
        levels.strides.push_back(stride);
        if (d + 1 < levels.sizes.size()) stride /= std::max(1u, levels.sizes[d + 1]);
    }
    return levels;
}

// Calls fn(suffix, offset) for every element of dimensions [0, count) of
// `levels` ("[i][j]"); `firstOnly` keeps only element 0 of the outermost.
template <class F>
void forEachElement(const ArrayLevels& levels, size_t count, bool firstOnly, F&& fn, size_t level = 0,
                    const std::string& suffix = "", uint32_t offset = 0) {
    if (level == count) return fn(suffix, offset);
    const uint32_t n = level == 0 && firstOnly ? 1u : std::max(1u, levels.sizes[level]);
    for (uint32_t e = 0; e < n; ++e)
        forEachElement(levels, count, firstOnly, fn, level + 1, suffix + "[" + std::to_string(e) + "]",
                       offset + e * levels.strides[level]);
}

// Expands a block member (recursively) into GL uniform entries. Arrays of
// aggregates list every element; arrays of arrays list each innermost array.
void expandMember(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& parent, uint32_t index,
                  const std::string& prefix, uint32_t baseOffset, int blockIndex, std::vector<UniformInfo>& out) {
    const uint32_t typeId = parent.member_types[index];
    const spirv_cross::SPIRType& type = compiler.get_type(typeId);
    const std::string memberName = compiler.get_member_name(parent.self, index);
    // A sampler member of a struct uniform is hoisted out of the struct by
    // glslang (it is reported as a sampler uniform); its place holder, named
    // "/*member*/", is not a GL uniform.
    if (memberName.compare(0, 2, "/*") == 0) return;
    const std::string name = prefix + unescapeName(memberName);
    const uint32_t offset = baseOffset + compiler.type_struct_member_offset(parent, index);
    const ArrayLevels levels = arrayLevels(compiler, parent, index, type);
    if (type.basetype == spirv_cross::SPIRType::Struct) {
        const spirv_cross::SPIRType& structType = compiler.get_type(type.self);
        forEachElement(levels, levels.sizes.size(), false, [&](const std::string& suffix, uint32_t at) {
            for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                expandMember(compiler, structType, m, name + suffix + ".", offset + at, blockIndex, out);
        });
        return;
    }
    const size_t outer = levels.sizes.empty() ? 0 : levels.sizes.size() - 1;
    forEachElement(levels, outer, false, [&](const std::string& suffix, uint32_t at) {
        UniformInfo info;
        info.name = levels.sizes.empty() ? name : name + suffix + "[0]";
        info.type = glTypeOf(type);
        info.size = static_cast<GLint>(levels.sizes.empty() ? 1u : std::max(1u, levels.sizes.back()));
        info.blockIndex = blockIndex;
        info.offset = static_cast<GLint>(offset + at);
        info.arrayStride = static_cast<GLint>(levels.strides.empty() ? 0u : levels.strides.back());
        if (type.columns > 1) {
            info.matrixStride = static_cast<GLint>(compiler.type_struct_member_matrix_stride(parent, index));
            info.rowMajor = compiler.has_member_decoration(parent.self, index, spv::DecorationRowMajor);
        }
        out.push_back(info);
    });
}

// Expands a storage block member into GL buffer variables (GL 4.3 section
// 7.3.1.1). Unlike uniforms, a top-level array member is enumerated for its
// first element only; its size and stride are the variables'
// TOP_LEVEL_ARRAY_SIZE/STRIDE (0 size: unsized).
void expandBufferMember(const spirv_cross::Compiler& compiler, const spirv_cross::SPIRType& parent, uint32_t index,
                        const std::string& prefix, uint32_t baseOffset, int blockIndex, bool topLevel, GLint topSize,
                        GLint topStride, std::vector<BufferVariableInfo>& out) {
    const spirv_cross::SPIRType& type = compiler.get_type(parent.member_types[index]);
    const std::string name = prefix + unescapeName(compiler.get_member_name(parent.self, index));
    const uint32_t offset = baseOffset + compiler.type_struct_member_offset(parent, index);
    const ArrayLevels levels = arrayLevels(compiler, parent, index, type);
    // A top-level array is one of aggregates (structs or arrays); a member
    // that is a plain array of a basic type reports size 1, stride 0.
    const bool aggregateArray = !levels.sizes.empty() && (levels.sizes.size() > 1 || type.basetype == spirv_cross::SPIRType::Struct);
    if (topLevel) {
        topSize = aggregateArray ? static_cast<GLint>(levels.sizes[0]) : 1;
        topStride = aggregateArray ? static_cast<GLint>(levels.strides[0]) : 0;
    }
    if (type.basetype == spirv_cross::SPIRType::Struct) {
        const spirv_cross::SPIRType& structType = compiler.get_type(type.self);
        forEachElement(levels, levels.sizes.size(), topLevel, [&](const std::string& suffix, uint32_t at) {
            for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                expandBufferMember(compiler, structType, m, name + suffix + ".", offset + at, blockIndex, false, topSize,
                                   topStride, out);
        });
        return;
    }
    const size_t outer = levels.sizes.empty() ? 0 : levels.sizes.size() - 1;
    forEachElement(levels, outer, topLevel, [&](const std::string& suffix, uint32_t at) {
        BufferVariableInfo v;
        v.name = levels.sizes.empty() ? name : name + suffix + "[0]";
        v.type = glTypeOf(type);
        v.size = levels.sizes.empty() ? 1 : static_cast<GLint>(levels.sizes.back());
        v.offset = static_cast<GLint>(offset + at);
        v.arrayStride = levels.strides.empty() ? 0 : static_cast<GLint>(levels.strides.back());
        if (type.columns > 1) {
            v.matrixStride = static_cast<GLint>(compiler.type_struct_member_matrix_stride(parent, index));
            v.rowMajor = compiler.has_member_decoration(parent.self, index, spv::DecorationRowMajor);
        }
        v.blockIndex = blockIndex;
        v.topLevelArraySize = topSize;
        v.topLevelArrayStride = topStride;
        out.push_back(v);
    });
}

// The members of block variable `id` the stage's code accesses (`all`
// when SPIRV-Cross cannot tell, e.g. a member without an Offset).
std::set<uint32_t> accessedMembers(const spirv_cross::Compiler& compiler, uint32_t id, bool& all) {
    std::set<uint32_t> out;
    all = false;
    try {
        for (const spirv_cross::BufferRange& range : compiler.get_active_buffer_ranges(id)) out.insert(range.index);
    } catch (const spirv_cross::CompilerError&) {
        all = true;
    }
    return out;
}

// The elements of block array variable `var` that the module's code
// reaches: access chains with a constant first index name one element; a
// dynamic index or a load of the whole array reaches them all.
std::vector<bool> accessedElements(const std::vector<uint32_t>& spirv, uint32_t var, uint32_t count) {
    std::vector<bool> out(count, false);
    std::map<uint32_t, uint32_t> constants;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t op = spirv[i] & 0xffff, length = spirv[i] >> 16;
        if (!length || i + length > spirv.size()) break;
        if (op == spv::OpConstant && length >= 4) constants[spirv[i + 2]] = spirv[i + 3];
        const bool chain = op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain || op == spv::OpPtrAccessChain;
        if (chain && length >= 4 && spirv[i + 3] == var) {
            if (length == 4) {
                out.assign(count, true);
            } else if (auto c = constants.find(spirv[i + 4]); c != constants.end()) {
                if (c->second < count) out[c->second] = true;
            } else {
                out.assign(count, true);
            }
        }
        if (op == spv::OpLoad && length >= 4 && spirv[i + 3] == var) out.assign(count, true);
        i += length;
    }
    return out;
}

bool isUserStorageBlock(const std::string& blockName) {
    return atomicCounterBlockBinding(blockName) < 0 && blockName.rfind("wmgl_", 0) != 0;
}

}  // namespace

// ------------------------------------------------------------- helpers

int atomicCounterBlockBinding(const std::string& blockName) {
    // SPIRV-Cross prefixes reserved gl_ names when it writes GLSL (geometry kernels).
    static const std::string kName = "gl_AtomicCounterBlock_", kFixup = "_RESERVED_IDENTIFIER_FIXUP_";
    std::string name = blockName.rfind(kFixup, 0) == 0 ? blockName.substr(kFixup.size()) : blockName;
    if (name.rfind(kName, 0) != 0 || name.size() == kName.size()) return -1;
    const std::string digits = name.substr(kName.size());
    if (digits.find_first_not_of("0123456789") != std::string::npos) return -1;
    return std::atoi(digits.c_str());
}

int glTypeComponents(GLenum type) {
    switch (type) {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: case GL_DOUBLE: return 1;
    case GL_FLOAT_VEC2: case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: case GL_DOUBLE_VEC2: return 2;
    case GL_FLOAT_VEC3: case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: case GL_DOUBLE_VEC3: return 3;
    case GL_FLOAT_VEC4: case GL_INT_VEC4: case GL_UNSIGNED_INT_VEC4: case GL_BOOL_VEC4: case GL_DOUBLE_VEC4: return 4;
    case GL_FLOAT_MAT2: case GL_DOUBLE_MAT2: return 4;
    case GL_FLOAT_MAT3: case GL_DOUBLE_MAT3: return 9;
    case GL_FLOAT_MAT4: case GL_DOUBLE_MAT4: return 16;
    case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT3x2: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT3x2: return 6;
    case GL_FLOAT_MAT2x4: case GL_FLOAT_MAT4x2: case GL_DOUBLE_MAT2x4: case GL_DOUBLE_MAT4x2: return 8;
    case GL_FLOAT_MAT3x4: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT3x4: case GL_DOUBLE_MAT4x3: return 12;
    default: return glTypeIsSampler(type) || glTypeIsImage(type) ? 1 : 0;
    }
}

int conventionalAttribLocation(const std::string& name) {
    static const char* const kNames[kConventionalAttribCount] = {
        "wmgl_Vertex", "wmgl_Normal", "wmgl_Color", "wmgl_SecondaryColor", "wmgl_FogCoord",
        "wmgl_MultiTexCoord0", "wmgl_MultiTexCoord1", "wmgl_MultiTexCoord2", "wmgl_MultiTexCoord3",
        "wmgl_MultiTexCoord4", "wmgl_MultiTexCoord5", "wmgl_MultiTexCoord6", "wmgl_MultiTexCoord7",
        "wmgl_LinePartner", "wmgl_LineSide"};
    if (name.rfind("wmgl_", 0) != 0) return -1;
    for (int i = 0; i < kConventionalAttribCount; ++i)
        if (name == kNames[i]) return kConventionalAttribFirst + i;
    return -1;
}

int glTypeLocations(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: return 4;
    // Vertex inputs of double types take one location per column too (GLSL
    // 4.60 section 4.4.1); dvec3/dvec4 columns count twice against
    // MAX_VERTEX_ATTRIBS (glTypeAttribSlots).
    case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT2x4: return 2;
    case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2: case GL_DOUBLE_MAT3x4: return 3;
    case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2: case GL_DOUBLE_MAT4x3: return 4;
    default: return 1;
    }
}

int glTypeAttribSlots(GLenum type) {
    const int columns = glTypeColumns(type);
    const bool wide = glTypeIsDouble(type) && glTypeComponents(type) / columns > 2;
    return glTypeLocations(type) * (wide ? 2 : 1);
}

int glTypeColumns(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3:
    case GL_DOUBLE_MAT2x4: return 2;
    case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2:
    case GL_DOUBLE_MAT3x4: return 3;
    case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2:
    case GL_DOUBLE_MAT4x3: return 4;
    default: return 1;
    }
}

bool glTypeIsDouble(GLenum type) {
    switch (type) {
    case GL_DOUBLE: case GL_DOUBLE_VEC2: case GL_DOUBLE_VEC3: case GL_DOUBLE_VEC4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT3:
    case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT2x3: case GL_DOUBLE_MAT2x4: case GL_DOUBLE_MAT3x2: case GL_DOUBLE_MAT3x4:
    case GL_DOUBLE_MAT4x2: case GL_DOUBLE_MAT4x3: return true;
    default: return false;
    }
}

int glTypeWords(GLenum type) { return glTypeComponents(type) * (glTypeIsDouble(type) ? 2 : 1); }

bool glTypeIsSampler(GLenum type) { return textureTargetForSampler(type) >= 0; }

bool glTypeIsImage(GLenum type) {
    return (type >= GL_IMAGE_1D && type <= GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY);
}

const char* glTypeName(GLenum type) {
    switch (type) {
    case GL_FLOAT: return "float";
    case GL_FLOAT_VEC2: return "vec2";
    case GL_FLOAT_VEC3: return "vec3";
    case GL_FLOAT_VEC4: return "vec4";
    case GL_INT: return "int";
    case GL_UNSIGNED_INT: return "uint";
    case GL_BOOL: return "bool";
    case GL_FLOAT_MAT4: return "mat4";
    default: return "?";
    }
}

// ------------------------------------------------------------- compile

namespace {

// Extensions glslang takes only in a later GLSL version. GL_ARB_compute_shader
// makes compute shaders available to any GLSL version; glslang only accepts
// them from 4.20, so such sources are compiled as 4.30.
// Global constants initialized from bit casts ("const vec4 c =
// intBitsToFloat(ivec4(...));", which wined3d writes for literal constants):
// GLSL counts built-in calls on constant expressions as constant expressions,
// but glslang does not fold the bit casts and rejects the declaration. The
// const goes, so the value is computed with its exact bits (NaN payloads
// included) when the shader starts.
std::string relaxBitcastConstants(const std::string& source) {
    if (source.find("BitsTo") == std::string::npos) return source;
    std::string out = source;
    const size_t n = out.size();
    int depth = 0;
    bool statementStart = true;
    for (size_t i = 0; i < n;) {
        const char c = out[i];
        if (out.compare(i, 2, "//") == 0) {
            while (i < n && out[i] != '\n') ++i;
            continue;
        }
        if (out.compare(i, 2, "/*") == 0) {
            const size_t end = out.find("*/", i + 2);
            i = end == std::string::npos ? n : end + 2;
            continue;
        }
        if (c == '#' && statementStart) {  // directives end at their line
            while (i < n && out[i] != '\n') ++i;
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}') --depth;
        if (c == ';' || c == '{' || c == '}') {
            statementStart = true;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (depth == 0 && statementStart && out.compare(i, 5, "const") == 0 && i + 5 < n &&
            std::isspace(static_cast<unsigned char>(out[i + 5]))) {
            const size_t end = out.find(';', i);
            const size_t equals = out.find('=', i);
            if (end != std::string::npos && equals != std::string::npos && equals < end &&
                out.substr(equals, end - equals).find("BitsTo") != std::string::npos)
                out.replace(i, 5, "     ");  // same length: line and column numbers stay
        }
        statementStart = false;
        ++i;
    }
    return out;
}

// GL_EXT_gpu_shader4's sampling functions that `source` calls, defined
// over GLSL 1.30's for float, int and unsigned samplers, or as macros for
// the ones with texel offsets (rectangle and
// buffer samplers, and shadow comparisons, which return vec4 under GLSL
// 1.20's rules, are left out).
std::string gpuShader4Functions(const std::string& source) {
    struct Dim {
        const char* name;
        const char* coord;   // texture coordinates
        const char* icoord;  // texel coordinates (nullptr: no texel fetch)
        const char* size;
        const char* deriv;
        const char* proj[2];
        const char* offset;  // nullptr: no offsets
        bool array;
    };
    static const Dim kDims[] = {
        {"1D", "float", "int", "int", "float", {"vec2", "vec4"}, "int", false},
        {"2D", "vec2", "ivec2", "ivec2", "vec2", {"vec3", "vec4"}, "ivec2", false},
        {"3D", "vec3", "ivec3", "ivec3", "vec3", {"vec4", nullptr}, "ivec3", false},
        {"Cube", "vec3", nullptr, "ivec2", "vec3", {nullptr, nullptr}, nullptr, false},
        {"1DArray", "vec2", "ivec2", "ivec2", "float", {nullptr, nullptr}, "int", true},
        {"2DArray", "vec3", "ivec3", "ivec3", "vec2", {nullptr, nullptr}, "ivec2", true},
    };
    auto identStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::set<std::string> used;
    for (size_t i = 0; i < source.size();) {
        if (!identStart(source[i])) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < source.size() && ident(source[end])) ++end;
        used.insert(source.substr(i, end - i));
        i = end;
    }
    std::string out;
    // One function, for each sampler type: "R" is its return type ("G": the
    // sampler's vec4), "S" the sampler, "A" the parameters after it, "C" the
    // call's arguments after the sampler.
    auto define = [&](const std::string& name, const std::string& ret, const std::string& params, const std::string& call,
                      const std::string& builtin, const char* dim) {
        if (!used.count(name)) return;
        // Texel offsets must be constant expressions, which parameters are
        // not: the offset functions are GLSL 1.30's under another name.
        if (builtin.find("Offset") != std::string::npos) {
            const std::string alias = "#define " + name + " " + builtin + "\n";
            if (out.find(alias) == std::string::npos) out += alias;
            return;
        }
        for (const char* prefix : {"", "i", "u"}) {
            const std::string vec4 = std::string(prefix) + "vec4";
            out += (ret == "G" ? vec4 : ret) + " " + name + "(" + prefix + "sampler" + dim + " wmgl_s" + params + ") { return " +
                   builtin + "(wmgl_s" + call + "); }\n";
        }
    };
    for (const Dim& d : kDims) {
        const std::string D = d.name, coord = d.coord, deriv = d.deriv;
        if (d.icoord) {
            define("texelFetch" + D, "G", std::string(", ") + d.icoord + " wmgl_p, int wmgl_l", ", wmgl_p, wmgl_l", "texelFetch", d.name);
            define("texelFetch" + D + "Offset", "G", std::string(", ") + d.icoord + " wmgl_p, int wmgl_l, " + d.offset + " wmgl_o",
                   ", wmgl_p, wmgl_l, wmgl_o", "texelFetchOffset", d.name);
        }
        define("textureSize" + D, d.size, ", int wmgl_l", ", wmgl_l", "textureSize", d.name);
        define("texture" + D + "Grad", "G", ", " + coord + " wmgl_p, " + deriv + " wmgl_x, " + deriv + " wmgl_y",
               ", wmgl_p, wmgl_x, wmgl_y", "textureGrad", d.name);
        if (d.array) {
            define("texture" + D, "G", ", " + coord + " wmgl_p", ", wmgl_p", "texture", d.name);
            define("texture" + D, "G", ", " + coord + " wmgl_p, float wmgl_b", ", wmgl_p, wmgl_b", "texture", d.name);
            define("texture" + D + "Lod", "G", ", " + coord + " wmgl_p, float wmgl_l", ", wmgl_p, wmgl_l", "textureLod", d.name);
            continue;
        }
        if (!d.offset) continue;
        const std::string off = std::string(", ") + d.offset + " wmgl_o";
        define("texture" + D + "Offset", "G", ", " + coord + " wmgl_p" + off, ", wmgl_p, wmgl_o", "textureOffset", d.name);
        define("texture" + D + "Offset", "G", ", " + coord + " wmgl_p" + off + ", float wmgl_b", ", wmgl_p, wmgl_o, wmgl_b",
               "textureOffset", d.name);
        define("texture" + D + "LodOffset", "G", ", " + coord + " wmgl_p, float wmgl_l" + off, ", wmgl_p, wmgl_l, wmgl_o",
               "textureLodOffset", d.name);
        define("texture" + D + "GradOffset", "G", ", " + coord + " wmgl_p, " + deriv + " wmgl_x, " + deriv + " wmgl_y" + off,
               ", wmgl_p, wmgl_x, wmgl_y, wmgl_o", "textureGradOffset", d.name);
        for (const char* proj : d.proj) {
            if (!proj) continue;
            const std::string P = proj;
            define("texture" + D + "ProjOffset", "G", ", " + P + " wmgl_p" + off, ", wmgl_p, wmgl_o", "textureProjOffset", d.name);
            define("texture" + D + "ProjOffset", "G", ", " + P + " wmgl_p" + off + ", float wmgl_b", ", wmgl_p, wmgl_o, wmgl_b",
                   "textureProjOffset", d.name);
            define("texture" + D + "ProjLodOffset", "G", ", " + P + " wmgl_p, float wmgl_l" + off, ", wmgl_p, wmgl_l, wmgl_o",
                   "textureProjLodOffset", d.name);
            define("texture" + D + "ProjGrad", "G", ", " + P + " wmgl_p, " + deriv + " wmgl_x, " + deriv + " wmgl_y",
                   ", wmgl_p, wmgl_x, wmgl_y", "textureProjGrad", d.name);
            define("texture" + D + "ProjGradOffset", "G",
                   ", " + P + " wmgl_p, " + deriv + " wmgl_x, " + deriv + " wmgl_y" + off,
                   ", wmgl_p, wmgl_x, wmgl_y, wmgl_o", "textureProjGradOffset", d.name);
        }
    }
    return out;
}

// GLSL 1.10's texture functions that core GLSL 1.40+ removed, which
// `source` calls without defining them, defined over the core ones (the
// shadow ones return vec4, as before). Drivers accept them in core shaders
// (SpaceEngine's #version 460 core shaders call texture2D, issue #35). Bias
// forms are fragment-shader only.
std::string legacyTextureFunctions(Stage stage, const std::string& source) {
    struct Function {
        const char* name;
        const char* ret;      // "vec4", or "shadow": vec4 of the float result
        const char* sampler;
        const char* coord;
        const char* builtin;  // the core function
        int extra;            // 0: none, 1: bias (fragment) or nothing, 2: lod
    };
    static const Function kFunctions[] = {
        {"texture1D", "vec4", "sampler1D", "float", "texture", 1},
        {"texture1DProj", "vec4", "sampler1D", "vec2", "textureProj", 1},
        {"texture1DProj", "vec4", "sampler1D", "vec4", "textureProj", 1},
        {"texture1DLod", "vec4", "sampler1D", "float", "textureLod", 2},
        {"texture1DProjLod", "vec4", "sampler1D", "vec2", "textureProjLod", 2},
        {"texture1DProjLod", "vec4", "sampler1D", "vec4", "textureProjLod", 2},
        {"texture2D", "vec4", "sampler2D", "vec2", "texture", 1},
        {"texture2DProj", "vec4", "sampler2D", "vec3", "textureProj", 1},
        {"texture2DProj", "vec4", "sampler2D", "vec4", "textureProj", 1},
        {"texture2DLod", "vec4", "sampler2D", "vec2", "textureLod", 2},
        {"texture2DProjLod", "vec4", "sampler2D", "vec3", "textureProjLod", 2},
        {"texture2DProjLod", "vec4", "sampler2D", "vec4", "textureProjLod", 2},
        {"texture3D", "vec4", "sampler3D", "vec3", "texture", 1},
        {"texture3DProj", "vec4", "sampler3D", "vec4", "textureProj", 1},
        {"texture3DLod", "vec4", "sampler3D", "vec3", "textureLod", 2},
        {"texture3DProjLod", "vec4", "sampler3D", "vec4", "textureProjLod", 2},
        {"textureCube", "vec4", "samplerCube", "vec3", "texture", 1},
        {"textureCubeLod", "vec4", "samplerCube", "vec3", "textureLod", 2},
        {"texture2DRect", "vec4", "sampler2DRect", "vec2", "texture", 0},
        {"texture2DRectProj", "vec4", "sampler2DRect", "vec3", "textureProj", 0},
        {"texture2DRectProj", "vec4", "sampler2DRect", "vec4", "textureProj", 0},
        {"shadow1D", "shadow", "sampler1DShadow", "vec3", "texture", 1},
        {"shadow1DProj", "shadow", "sampler1DShadow", "vec4", "textureProj", 1},
        {"shadow1DLod", "shadow", "sampler1DShadow", "vec3", "textureLod", 2},
        {"shadow1DProjLod", "shadow", "sampler1DShadow", "vec4", "textureProjLod", 2},
        {"shadow2D", "shadow", "sampler2DShadow", "vec3", "texture", 1},
        {"shadow2DProj", "shadow", "sampler2DShadow", "vec4", "textureProj", 1},
        {"shadow2DLod", "shadow", "sampler2DShadow", "vec3", "textureLod", 2},
        {"shadow2DProjLod", "shadow", "sampler2DShadow", "vec4", "textureProjLod", 2},
        {"shadow2DRect", "shadow", "sampler2DRectShadow", "vec3", "texture", 0},
        {"shadow2DRectProj", "shadow", "sampler2DRectShadow", "vec4", "textureProj", 0},
    };
    // One scan: the identifiers called, and those defined (after vec4 or float).
    std::set<std::string> called, defined;
    std::string previous;
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (!std::isalpha(static_cast<unsigned char>(c)) && c != '_') {
            if (!std::isspace(static_cast<unsigned char>(c))) previous.clear();
            ++i;
            continue;
        }
        size_t end = i;
        while (end < source.size() && (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_')) ++end;
        std::string word = source.substr(i, end - i);
        size_t next = end;
        while (next < source.size() && std::isspace(static_cast<unsigned char>(source[next]))) ++next;
        if (next < source.size() && source[next] == '(') (previous == "vec4" || previous == "float" ? defined : called).insert(word);
        previous = std::move(word);
        i = end;
    }
    std::string out;
    for (const Function& f : kFunctions) {
        if (!called.count(f.name) || defined.count(f.name)) continue;
        auto define = [&](const std::string& params, const std::string& args) {
            const std::string call = std::string(f.builtin) + "(wmgl_s, wmgl_p" + args + ")";
            out += std::string("vec4 ") + f.name + "(" + f.sampler + " wmgl_s, " + f.coord + " wmgl_p" + params +
                   ") { return " + (std::strcmp(f.ret, "shadow") == 0 ? "vec4(" + call + ")" : call) + "; }\n";
        };
        if (f.extra == 2) {
            define(", float wmgl_l", ", wmgl_l");
            continue;
        }
        define("", "");
        if (f.extra == 1 && stage == Stage::Fragment) define(", float wmgl_b", ", wmgl_b");
    }
    return out;
}

std::string raiseComputeVersion(Stage stage, const std::string& source) {
    // Redeclared built-ins and profile-less GLSL 1.50 with compatibility
    // built-ins, as desktop drivers accept them (issue #25).
    std::string raised = relaxDesktopGlsl(source);
    // GL_ARB_cull_distance (GLSL 4.50 core): glslang has gl_CullDistance in
    // GLSL 4.50 only and no such extension, so the #extension line goes
    // (kPreamble defines the macro) and the shader becomes GLSL 4.50.
    const GlslVersion version = parseGlslVersion(raised);
    bool cull = false, gpuShader4 = false;
    for (size_t at = raised.find("#extension"); at != std::string::npos; at = raised.find("#extension", at + 1)) {
        const size_t end = raised.find('\n', at);
        const std::string line = raised.substr(at, end == std::string::npos ? std::string::npos : end - at);
        std::istringstream words(line.substr(10));
        std::string name;
        words >> name;
        if (!name.empty() && name.back() == ':') name.pop_back();
        if (name == "GL_EXT_gpu_shader4" && !version.es) {
            raised.replace(at, line.size(), "");
            gpuShader4 = version.number < 130;
            continue;
        }
        if (name != "GL_ARB_cull_distance") continue;
        raised.replace(at, line.size(), "");
        cull = true;
    }
    // GL_EXT_gpu_shader4 (GLSL 1.10/1.20): GLSL 1.30's integers, bitwise
    // operators and texel fetches, under the extension's function names.
    // glslang does not know it: the #extension line goes (kPreamble defines
    // the macro), and a shader before GLSL 1.30 becomes GLSL 1.30 (still the
    // compatibility profile) with the functions it calls defined over GLSL
    // 1.30's (functions, not macros: shaders may name variables after GLSL
    // 1.30's built-ins, as OpenMW's textureSize). OpenSceneGraph takes GL 3.0
    // for the extension, and OpenMW requires it in its shaders (issue #42).
    if (gpuShader4) {
        const size_t at = raised.find("#version");
        std::string head = "#version 130\n", body = raised;
        if (at != std::string::npos) {
            const size_t end = raised.find('\n', at);
            head = raised.substr(0, at) + "#version 130";
            body = end == std::string::npos ? std::string() : raised.substr(end);
        } else {
            body = "\n" + raised;
        }
        raised = insertAfterDirectives(head + body, gpuShader4Functions(raised), 130);
    }
    if (!version.es && !version.compatibility && version.number >= 140) {
        const std::string legacy = legacyTextureFunctions(stage, raised);
        if (!legacy.empty()) raised = insertAfterDirectives(raised, legacy, version.number);
    }
    if (cull && version.number < 450 && !version.es) {
        const size_t at = raised.find("#version");
        if (at != std::string::npos) {
            const size_t end = raised.find('\n', at);
            raised.replace(at, end == std::string::npos ? std::string::npos : end - at,
                           version.compatibility ? "#version 450 compatibility" : "#version 450");
        }
    }
    if (stage != Stage::Compute || version.number >= 420 || cull) return raised;
    if (raised.find("GL_ARB_compute_shader") == std::string::npos) return raised;
    const size_t at = raised.find("#version");
    if (at == std::string::npos) return "#version 430\n" + raised;
    const size_t end = raised.find('\n', at);
    raised.replace(at, end == std::string::npos ? std::string::npos : end - at, "#version 430");
    return raised;
}

// Explicit locations of a stage's inputs and outputs must fit the stage's
// interface (GL 4.6 section 11.1.2.1: a compile-time or link-time error):
// GL_MAX_VERTEX_ATTRIBS for vertex inputs, GL_MAX_DRAW_BUFFERS for fragment
// outputs, the GL_MAX_*_COMPONENTS limits / 4 between stages.
std::string checkInterfaceLocations(glslang::TShader& shader, Stage stage) {
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return {};
    namespace L = wmgl::limits;
    const EShLanguage lang = language(stage);
    auto limit = [&](bool input) -> int {
        switch (stage) {
        case Stage::Vertex: return input ? L::kMaxVertexAttribs : L::kMaxVertexOutputComponents / 4;
        case Stage::Geometry: return (input ? L::kMaxGeometryInputComponents : L::kMaxInterfaceComponents) / 4;
        case Stage::Fragment: return input ? L::kMaxInterfaceComponents / 4 : L::kMaxDrawBuffers;
        case Stage::Compute: return 0;
        default: return L::kMaxInterfaceComponents / 4;
        }
    };
    auto size = [&](const glslang::TType& type) {
        if (type.isArray() && type.getQualifier().isArrayedIo(lang)) {
            const glslang::TType element(type, 0);
            return glslang::TIntermediate::computeTypeLocationSize(element, lang);
        }
        return glslang::TIntermediate::computeTypeLocationSize(type, lang);
    };
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol) continue;
            const glslang::TType& type = symbol->getType();
            const glslang::TQualifier& q = type.getQualifier();
            const bool input = q.storage == glslang::EvqVaryingIn, output = q.storage == glslang::EvqVaryingOut;
            if ((!input && !output) || q.builtIn != glslang::EbvNone || q.patch) continue;
            const int max = limit(input);
            if (type.getBasicType() == glslang::EbtBlock) {
                // Members take the block's location onwards, or their own.
                int next = q.hasLocation() ? static_cast<int>(q.layoutLocation) : -1;
                for (const glslang::TTypeLoc& member : *type.getStruct()) {
                    const glslang::TQualifier& mq = member.type->getQualifier();
                    if (mq.builtIn != glslang::EbvNone) continue;
                    if (mq.hasLocation()) next = static_cast<int>(mq.layoutLocation);
                    if (next < 0) continue;
                    next += glslang::TIntermediate::computeTypeLocationSize(*member.type, lang);
                    if (next > max)
                        return "ERROR: block '" + unescapeName(type.getTypeName().c_str()) +
                               "': a member's location is outside the stage's interface\n";
                }
                continue;
            }
            if (!q.hasLocation()) continue;
            if (static_cast<int>(q.layoutLocation) + size(type) > max)
                return "ERROR: '" + unescapeName(symbol->getName().c_str()) + "': location " +
                       std::to_string(q.layoutLocation) + " is outside the stage's interface (" + std::to_string(max) +
                       " locations)\n";
        }
    }
    return {};
}

// layout(binding) on a uniform block must leave every element of the block
// within GL_MAX_UNIFORM_BUFFER_BINDINGS (a compile-time error in GLSL).
// glslang checks opaque bindings itself but has no limit for blocks.
std::string checkBlockBindings(glslang::TShader& shader) {
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return {};
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol) continue;
            const glslang::TType& type = symbol->getType();
            const glslang::TQualifier& q = type.getQualifier();
            if (type.getBasicType() != glslang::EbtBlock || !q.hasBinding()) continue;
            const int count = type.isSizedArray() ? type.getCumulativeArraySize() : 1;
            if (q.storage == glslang::EvqUniform && static_cast<int64_t>(q.layoutBinding) + count > wmgl::limits::kMaxUniformBufferBindings)
                return "ERROR: uniform block '" + unescapeName(type.getTypeName().c_str()) +
                       "': binding is not less than GL_MAX_UNIFORM_BUFFER_BINDINGS\n";
            if (q.storage == glslang::EvqBuffer &&
                static_cast<int64_t>(q.layoutBinding) + count > wmgl::limits::kMaxShaderStorageBufferBindings)
                return "ERROR: shader storage block '" + unescapeName(type.getTypeName().c_str()) +
                       "': binding is not less than GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS\n";
        }
    }
    return {};
}

}  // namespace

namespace {

CompileResult compileUncached(Stage stage, const std::string& original);

}  // namespace

CompileResult compile(Stage stage, const std::string& source) {
    // Results of recent compiles by stage and source (the parse depends on
    // nothing else): shaders compiled again from the same source, as
    // applications and shader caches do, skip the parse.
    static std::mutex mutex;
    static std::list<std::string> order;  // most recently used first
    static std::unordered_map<std::string, std::pair<CompileResult, std::list<std::string>::iterator>> cache;
    constexpr size_t kEntries = 512;
    std::string key(1, static_cast<char>(stage));
    key += source;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            order.splice(order.begin(), order, it->second.second);
            return it->second.first;
        }
    }
    CompileResult result = compileUncached(stage, source);
    std::lock_guard<std::mutex> lock(mutex);
    if (!cache.count(key)) {
        order.push_front(key);
        cache.emplace(std::move(key), std::make_pair(result, order.begin()));
        while (cache.size() > kEntries) {
            cache.erase(order.back());
            order.pop_back();
        }
    }
    return result;
}

namespace {

CompileResult compileUncached(Stage stage, const std::string& original) {
    initGlslang();
    CompileResult result;
    // Subroutines are lowered first: glslang does not implement them.
    const SubroutineLowering subroutines = lowerSubroutines(stage, spliceLineContinuations(original));
    if (!subroutines.ok) {
        result.log = subroutines.log;
        return result;
    }
    const std::string source =
        lowerBuiltinUniforms(stage, raiseComputeVersion(stage, relaxBitcastConstants(subroutines.source)));
    // Status and log come from a plain GL parse, which accepts every GLSL
    // version and profile with GL's own rules.
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setPreamble(kPreamble);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
    shader.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
    shader.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
    result.ok = shader.parse(resources(), 110, ENoProfile, false, false, EShMsgDefault);
    result.log = cleanLog(shader.getInfoLog(), shader.getInfoDebugLog());
    if (!result.ok && result.log.find("cannot use in preprocessor expression when expanded from macros") != std::string::npos) {
        // 'defined' produced by macro expansion is undefined behaviour in GLSL;
        // desktop drivers evaluate it. Accept the shader if that is the only problem.
        glslang::TShader relaxed(lang);
        relaxed.setStrings(&text, 1);
        relaxed.setPreamble(kPreamble);
        relaxed.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
        relaxed.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
        relaxed.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
        if (relaxed.parse(resources(), 110, ENoProfile, false, false, EShMsgRelaxedErrors)) {
            const std::string relaxedLog = cleanLog(relaxed.getInfoLog(), relaxed.getInfoDebugLog());
            if (relaxedLog.find("ERROR") == std::string::npos) {
                result.ok = true;
                result.log = relaxedLog;
            }
        }
    }
    if (result.ok) {
        std::string errors = checkBlockBindings(shader);
        if (errors.empty()) errors = checkInterfaceLocations(shader, stage);
        if (!errors.empty()) {
            result.ok = false;
            result.log += errors;
        }
    }
    if (result.ok) {
        const std::string reserved = checkReservedWords(source);
        if (!reserved.empty()) {
            result.ok = false;
            result.log += reserved;
        }
    }
    if (result.ok) {
        // Make sure the shader also translates; report problems here rather
        // than as a mysterious link failure.
        ParsedShader parsed;
        std::string log;
        if (!parseForCodegen(stage, source, parsed, log)) {
            result.ok = false;
            result.log += log;
        }
    }
    return result;
}

}  // namespace

namespace {

// The #version of a GLSL ES shader: 100 without one, 0 for versions OpenGL
// ES does not have (desktop ones).
int esVersionOf(const std::string& source) {
    const GlslVersion v = parseGlslVersion(source);
    if (!v.declared) return 100;
    if (!v.es) return v.number == 100 ? 100 : 0;
    return v.number == 300 || v.number == 310 || v.number == 320 ? v.number : 0;
}

void configureNoClient(glslang::TShader& shader, EShLanguage lang) {
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
    shader.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
    shader.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
}

// The uniform and storage blocks of a parsed shader: each member's type,
// qualifiers and name, and the block's own layout.
std::map<std::string, std::string> blockSignaturesOf(glslang::TShader& shader) {
    std::map<std::string, std::string> out;
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return out;
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol) continue;
            const glslang::TType& type = symbol->getType();
            const glslang::TQualifier& q = type.getQualifier();
            if (type.getBasicType() != glslang::EbtBlock || (q.storage != glslang::EvqUniform && q.storage != glslang::EvqBuffer))
                continue;
            const std::string name = type.getTypeName().c_str();
            if (name.rfind("gl_", 0) == 0) continue;
            std::string signature = q.storage == glslang::EvqBuffer ? "buffer" : "uniform";
            signature += " packing " + std::to_string(q.layoutPacking) + " binding " +
                         std::to_string(q.hasBinding() ? static_cast<int>(q.layoutBinding) : -1) + " {";
            // Members' precisions may differ between stages (KHR-GLES3
            // uniform_block.common.precision_matching).
            for (const glslang::TTypeLoc& member : *type.getStruct()) {
                signature += std::string(member.type->getCompleteString(true, true, false, true).c_str()) + " " +
                             member.type->getFieldName().c_str();
                // Array sizes, an unsized (runtime) array's as "[]".
                if (const glslang::TArraySizes* arrays = member.type->getArraySizes())
                    for (int d = 0; d < arrays->getNumDims(); ++d)
                        signature += arrays->getDimSize(d) > 0 ? "[" + std::to_string(arrays->getDimSize(d)) + "]" : "[]";
                signature += ";";
            }
            out[(q.storage == glslang::EvqBuffer ? "buffer " : "uniform ") + name] = signature + "}";
        }
    }
    return out;
}

// The default-block uniforms of a parsed shader and their precisions.
std::map<std::string, int> uniformPrecisionsOf(glslang::TShader& shader) {
    std::map<std::string, int> out;
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return out;
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol) continue;
            const glslang::TType& type = symbol->getType();
            if (type.getQualifier().storage != glslang::EvqUniform || type.getBasicType() == glslang::EbtBlock) continue;
            const std::string name = symbol->getName().c_str();
            if (name.rfind("gl_", 0) == 0) continue;
            // Struct uniforms: each member's precision (GLSL ES 3.00 section 4.5.3).
            const std::function<void(const glslang::TType&, const std::string&)> add = [&](const glslang::TType& t,
                                                                                           const std::string& path) {
                if (t.isStruct()) {
                    for (const glslang::TTypeLoc& member : *t.getStruct())
                        add(*member.type, path + "." + member.type->getFieldName().c_str());
                } else {
                    out[path] = static_cast<int>(t.getQualifier().precision);
                }
            };
            add(type, name);
        }
    }
    return out;
}

// Each user stage input's (`input`) or output's interpolation and
// precision, by name ("Block.member" for members of blocks).
std::map<std::string, EsCompileResult::Varying> varyingsOf(glslang::TShader& shader, bool input) {
    std::map<std::string, EsCompileResult::Varying> out;
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return out;
    const glslang::TStorageQualifier storage = input ? glslang::EvqVaryingIn : glslang::EvqVaryingOut;
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol || symbol->getQualifier().storage != storage) continue;
            const glslang::TType& type = symbol->getType();
            const std::string name = symbol->getName().c_str();
            if (type.getBasicType() == glslang::EbtBlock) {
                const std::string block = type.getTypeName().c_str();
                if (block.rfind("gl_", 0) == 0) continue;
                for (const glslang::TTypeLoc& member : *type.getStruct())
                    out[block + "." + member.type->getFieldName().c_str()] = {
                        member.type->getQualifier().flat || type.getQualifier().flat,
                        static_cast<int>(member.type->getQualifier().precision)};
                continue;
            }
            if (name.rfind("gl_", 0) != 0) out[name] = {type.getQualifier().flat, static_cast<int>(type.getQualifier().precision)};
        }
    }
    return out;
}

// A type as program pipeline interfaces compare it: basic type, sizes,
// arrays, precision, flat, and a struct's or block's name and
// members in order. `dropOuterArray`: the per-vertex array of an arrayed stage.
std::string interfaceSignature(const glslang::TType& type, bool dropOuterArray) {
    std::string s;
    const glslang::TQualifier& q = type.getQualifier();
    // Auxiliary qualifiers (centroid, sample) may differ.
    if (q.flat) s += "flat ";
    if (q.patch) s += "patch ";
    if (type.getBasicType() != glslang::EbtBlock && type.getBasicType() != glslang::EbtStruct)
        s += std::string(glslang::GetPrecisionQualifierString(q.precision)) + " ";
    if (type.isStruct()) {
        s += std::string(type.getBasicType() == glslang::EbtBlock ? "block " : "struct ") + type.getTypeName().c_str() + " {";
        for (const glslang::TTypeLoc& member : *type.getStruct())
            s += std::string(member.type->getFieldName().c_str()) + ": " + interfaceSignature(*member.type, false) + "; ";
        s += "}";
    } else {
        s += type.getBasicTypeString().c_str();
        if (type.isMatrix()) s += std::to_string(type.getMatrixCols()) + "x" + std::to_string(type.getMatrixRows());
        else if (type.isVector()) s += std::to_string(type.getVectorSize());
    }
    if (const glslang::TArraySizes* arrays = type.getArraySizes())
        for (int d = dropOuterArray ? 1 : 0; d < arrays->getNumDims(); ++d) s += "[" + std::to_string(arrays->getDimSize(d)) + "]";
    return s;
}

// The pipeline interface of a stage's inputs (`input`) or outputs
// (EsCompileResult::inputInterface).
std::map<std::string, std::string> interfaceOf(glslang::TShader& shader, Stage stage, bool input) {
    std::map<std::string, std::string> out;
    const glslang::TIntermediate* intermediate = shader.getIntermediate();
    glslang::TIntermAggregate* root =
        intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return out;
    const glslang::TStorageQualifier storage = input ? glslang::EvqVaryingIn : glslang::EvqVaryingOut;
    // Per-vertex inputs of arrayed stages, and tessellation control outputs, have the vertex index outermost.
    const bool arrayed = input ? stage == Stage::TessControl || stage == Stage::TessEval || stage == Stage::Geometry
                               : stage == Stage::TessControl;
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol || symbol->getQualifier().storage != storage) continue;
            const glslang::TType& type = symbol->getType();
            const bool block = type.getBasicType() == glslang::EbtBlock;
            const std::string name = block ? type.getTypeName().c_str() : symbol->getName().c_str();
            if (name.rfind("gl_", 0) == 0) continue;
            const bool vertexArray = arrayed && !type.getQualifier().patch && type.isArray();
            std::string key = block ? "block " + name : name;
            std::string signature = interfaceSignature(type, vertexArray);
            if (type.getQualifier().hasLocation()) {
                if (!block) key = "@" + std::to_string(type.getQualifier().layoutLocation);
                signature = "location " + std::to_string(type.getQualifier().layoutLocation) + " " + signature;
            }
            out[key] = signature;
        }
    }
    return out;
}

}  // namespace

namespace {

EsCompileResult compileEsUncached(Stage stage, const std::string& source);

}  // namespace

EsCompileResult compileEs(Stage stage, const std::string& source) {
    // Recent results by stage and source, as compile() keeps.
    static std::mutex mutex;
    static std::list<std::string> order;  // most recently used first
    static std::unordered_map<std::string, std::pair<EsCompileResult, std::list<std::string>::iterator>> cache;
    constexpr size_t kEntries = 512;
    std::string key(1, static_cast<char>(stage));
    key += source;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            order.splice(order.begin(), order, it->second.second);
            return it->second.first;
        }
    }
    EsCompileResult result = compileEsUncached(stage, source);
    std::lock_guard<std::mutex> lock(mutex);
    if (!cache.count(key)) {
        order.push_front(key);
        cache.emplace(std::move(key), std::make_pair(result, order.begin()));
        while (cache.size() > kEntries) {
            cache.erase(order.back());
            order.pop_back();
        }
    }
    return result;
}

namespace {

EsCompileResult compileEsUncached(Stage stage, const std::string& source) {
    initGlslang();
    EsCompileResult result;
    const EShLanguage lang = language(stage);
    const int version = esVersionOf(source);
    if (!version) {
        result.log = "ERROR: 0:1: '#version' : OpenGL ES shaders are GLSL ES 1.00, 3.00 es, 3.10 es or 3.20 es\n";
        return result;
    }
    // Extensions glslang has no GLSL ES form of (EXT_clip_cull_distance,
    // OES_viewport_array): the shader is checked and compiled as GLSL 4.50,
    // which has their built-ins, without its ES extension directives (GLSL
    // reserves GL_ macros: GL_ES and the extensions' are not defined).
    if (version >= 300 && (source.find("GL_EXT_clip_cull_distance") != std::string::npos ||
                           source.find("GL_OES_viewport_array") != std::string::npos)) {
        std::string desktop = source;
        const size_t at = desktop.find("#version");
        const size_t end = desktop.find('\n', at);
        desktop.replace(at, end - at, "#version 450 core");
        for (size_t e = desktop.find("#extension"); e != std::string::npos; e = desktop.find("#extension", e + 1)) {
            const size_t lineEnd = desktop.find('\n', e);
            const std::string line = desktop.substr(e, lineEnd == std::string::npos ? std::string::npos : lineEnd - e);
            if (line.find("GL_EXT_") != std::string::npos || line.find("GL_OES_") != std::string::npos)
                desktop.replace(e, line.size(), "");
        }
        const CompileResult desktopResult = compile(stage, desktop);
        result.ok = desktopResult.ok;
        result.log = desktopResult.log;
        result.desktop = desktop;
        return result;
    }
    const char* text = source.c_str();
    // 'defined' produced by macro expansion is undefined behaviour, which
    // drivers evaluate: as compile() does, a shader whose only
    // errors are that is accepted.
    EShMessages messages = EShMsgDefault;
    for (int attempt = 0; attempt < 2; ++attempt) {
        glslang::TShader shader(lang);
        shader.setStrings(&text, 1);
        configureNoClient(shader, lang);
        result.ok = shader.parse(resources(), 100, EEsProfile, false, false, messages);
        result.log = cleanLog(shader.getInfoLog(), shader.getInfoDebugLog());
        if (result.ok) {
            result.uniformPrecisions = uniformPrecisionsOf(shader);
            result.blockSignatures = blockSignaturesOf(shader);
            result.inputs = varyingsOf(shader, true);
            result.outputs = varyingsOf(shader, false);
            result.inputInterface = interfaceOf(shader, stage, true);
            result.outputInterface = interfaceOf(shader, stage, false);
            // compile()'s checks: block bindings within the limits, interface
            // locations, reserved words.
            std::string errors = checkBlockBindings(shader);
            if (errors.empty()) errors = checkInterfaceLocations(shader, stage);
            if (errors.empty()) errors = checkReservedWords(source);
            if (!errors.empty()) {
                result.ok = false;
                result.log += errors;
            }
        }
        if (result.ok || attempt ||
            result.log.find("cannot use in preprocessor expression when expanded from macros") == std::string::npos)
            break;
        messages = EShMsgRelaxedErrors;
    }
    if (!result.ok) return result;
    // The preprocessed shader, its macros expanded with GLSL ES's values.
    glslang::TShader pre(lang);
    pre.setStrings(&text, 1);
    configureNoClient(pre, lang);
    std::string preprocessed;
    glslang::TShader::ForbidIncluder includer;
    if (!pre.preprocess(resources(), 100, EEsProfile, false, false,
                        static_cast<EShMessages>(EShMsgOnlyPreprocessor | messages), &preprocessed, includer)) {
        result.ok = false;
        result.log += cleanLog(pre.getInfoLog(), pre.getInfoDebugLog());
        return result;
    }
    // GLSL ES 3.00 section 3.4: #extension directives come before any
    // non-preprocessor token (glslang only enforces that for 1.00).
    if (version >= 300) {
        bool tokens = false;
        size_t at = 0;
        while (at < preprocessed.size()) {
            size_t end = preprocessed.find('\n', at);
            if (end == std::string::npos) end = preprocessed.size();
            const size_t first = preprocessed.find_first_not_of(" \t\r", at);
            if (first < end) {
                if (preprocessed[first] != '#') tokens = true;
                else if (tokens && preprocessed.compare(first, 10, "#extension") == 0) {
                    result.ok = false;
                    result.log += "ERROR: '#extension' : must occur before any non-preprocessor tokens\n";
                    return result;
                }
            }
            at = end + 1;
        }
    }
    // GLSL ES 3.20 section 4.1.8: struct members take precision qualifiers
    // only (glslang lets 'precise' through).
    {
        static const std::regex kPreciseMember(R"(\bstruct\b[^{;]*\{[^}]*\bprecise\b)");
        if (std::regex_search(preprocessed, kPreciseMember)) {
            result.ok = false;
            result.log += "ERROR: 'precise' : not allowed on struct members\n";
            return result;
        }
    }
    // GLSL ES 3.10 section 7.1.3: gl_WorkGroupSize needs a fixed local size
    // declared before its first use.
    if (stage == Stage::Compute) {
        static const std::regex kUse(R"(\bgl_WorkGroupSize\b)");
        static const std::regex kLocalSize(R"(layout\s*\([^)]*\blocal_size_[xyz]\s*=[^)]*\)\s*in\s*;)");
        std::smatch use, size;
        if (std::regex_search(preprocessed, use, kUse) &&
            (!std::regex_search(preprocessed, size, kLocalSize) || size.position(0) > use.position(0))) {
            result.ok = false;
            result.log += "ERROR: 'gl_WorkGroupSize' : used before a fixed local group size is declared\n";
            return result;
        }
    }
    result.desktop = esToDesktopGlsl(shadingRateBuiltinsAs1x1(floatImageAtomicsAsUint(preprocessed)), version);
    // Make sure the shader translates (as compile() does).
    ParsedShader parsed;
    std::string log;
    if (!parseForCodegen(stage, result.desktop, parsed, log)) {
        result.ok = false;
        result.log += log;
    }
    return result;
}

}  // namespace

std::string checkEsLink(const std::vector<ShaderSource>& esShaders) {
    // The precisions compileEs recorded (its results are cached: linking
    // parses nothing again).
    std::map<std::string, std::pair<int, Stage>> precisions;
    std::map<std::string, std::pair<std::string, Stage>> blocks;
    std::string errors;
    // Interpolation qualifiers (flat or not) of a stage's inputs match the
    // previous stage's outputs (GLSL ES 3.20 section 4.5; an auxiliary
    // qualifier such as centroid may differ).
    const Stage order[] = {Stage::Vertex, Stage::TessControl, Stage::TessEval, Stage::Geometry, Stage::Fragment};
    std::map<std::string, EsCompileResult::Varying> previousOutputs;
    bool havePrevious = false;
    for (Stage stage : order)
        for (const ShaderSource& s : esShaders) {
            if (s.stage != stage) continue;
            const EsCompileResult compiled = compileEs(s.stage, s.source);
            if (havePrevious)
                for (const auto& [name, in] : compiled.inputs)
                    if (auto out = previousOutputs.find(name); out != previousOutputs.end() && out->second.flat != in.flat)
                        errors += "error: '" + name + "' has different interpolation qualifiers in its stages\n";
            previousOutputs = compiled.outputs;
            havePrevious = true;
            break;
        }
    for (const ShaderSource& s : esShaders) {
        const EsCompileResult compiled = compileEs(s.stage, s.source);
        for (const auto& [name, precision] : compiled.uniformPrecisions) {
            auto [it, added] = precisions.emplace(name, std::make_pair(precision, s.stage));
            if (!added && it->second.second != s.stage && it->second.first != precision)
                errors += "error: uniform '" + name + "' has different precisions in its stages\n";
        }
        // A block of one name is declared alike in every stage (GLSL ES 3.10 section 4.3.9).
        for (const auto& [name, signature] : compiled.blockSignatures) {
            auto [it, added] = blocks.emplace(name, std::make_pair(signature, s.stage));
            if (!added && it->second.second != s.stage && it->second.first != signature)
                errors += "error: " + name + " block is declared differently in its stages\n";
        }
    }
    return errors;
}

// ---------------------------------------------------------------- link

std::string fragmentWithAdvancedBlend(const std::string& msl, unsigned equation) {
    const std::string signature = "fragment main0_out main0(", member = "struct main0_out\n{\n";
    const size_t structAt = msl.find(member);
    const size_t fnAt = msl.find(signature);
    if (structAt == std::string::npos || fnAt == std::string::npos || structAt > fnAt) return std::string();
    // Color output 0: "    float4 <name> [[color(0)]];"
    std::string name;
    {
        const size_t end = msl.find("};", structAt);
        std::istringstream lines(msl.substr(structAt, end - structAt));
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(" [[color(0)]]") == std::string::npos || line.find("float4") == std::string::npos) continue;
            std::istringstream words(line);
            std::string type;
            words >> type >> name;
        }
    }
    if (name.empty()) return msl;
    // The KHR_blend_equation_advanced equations on unpremultiplied colors
    // (section 3.8.5 table X.1 and X.2); the result is premultiplied.
    static const char* const kSeparable[11] = {
        "cs * cd",
        "cs + cd - cs * cd",
        "select(1.0 - 2.0 * (1.0 - cs) * (1.0 - cd), 2.0 * cs * cd, cd <= 0.5)",
        "min(cs, cd)",
        "max(cs, cd)",
        "select(select(min(float3(1.0), cd / max(1.0 - cs, float3(1e-30))), float3(1.0), cs >= 1.0), float3(0.0), cd <= 0.0)",
        "select(select(1.0 - min(float3(1.0), (1.0 - cd) / max(cs, float3(1e-30))), float3(0.0), cs <= 0.0), float3(1.0), cd >= 1.0)",
        "select(1.0 - 2.0 * (1.0 - cs) * (1.0 - cd), 2.0 * cs * cd, cs <= 0.5)",
        "select(select(cd + (2.0 * cs - 1.0) * (sqrt(cd) - cd), cd + (2.0 * cs - 1.0) * cd * ((16.0 * cd - 12.0) * cd + 3.0), "
        "cd <= 0.25), cd - (1.0 - 2.0 * cs) * cd * (1.0 - cd), cs <= 0.5)",
        "abs(cd - cs)",
        "cs + cd - 2.0 * cs * cd",
    };
    static const char* const kNonSeparable[4] = {
        "wmgl_set_lum_sat(cs, cd, cd)",  // HSL_HUE
        "wmgl_set_lum_sat(cd, cs, cd)",  // HSL_SATURATION
        "wmgl_set_lum(cs, cd)",          // HSL_COLOR
        "wmgl_set_lum(cd, cs)",          // HSL_LUMINOSITY
    };
    if (equation > 14) return msl;
    std::string helpers =
        "static inline float wmgl_lum(float3 c) { return dot(c, float3(0.30, 0.59, 0.11)); }\n"
        "static inline float3 wmgl_clip_color(float3 c) {\n"
        "    float l = wmgl_lum(c), n = min(min(c.r, c.g), c.b), x = max(max(c.r, c.g), c.b);\n"
        "    if (n < 0.0) c = l + (c - l) * l / (l - n);\n"
        "    if (x > 1.0) c = l + (c - l) * (1.0 - l) / (x - l);\n"
        "    return c;\n}\n"
        "static inline float3 wmgl_set_lum(float3 cbase, float3 clum) { return wmgl_clip_color(cbase + (wmgl_lum(clum) - wmgl_lum(cbase))); }\n"
        "static inline float3 wmgl_set_lum_sat(float3 cbase, float3 csat, float3 clum) {\n"
        "    float mn = min(min(cbase.r, cbase.g), cbase.b), mx = max(max(cbase.r, cbase.g), cbase.b);\n"
        "    float s = max(max(csat.r, csat.g), csat.b) - min(min(csat.r, csat.g), csat.b);\n"
        "    float3 c = mx > mn ? (cbase - mn) * s / (mx - mn) : float3(0.0);\n"
        "    return wmgl_set_lum(c, clum);\n}\n"
        "static inline float4 wmgl_advanced_blend(float4 s, float4 d) {\n"
        "    s = saturate(s); d = saturate(d);  // the premultiplied colors, clamped before dividing\n"
        "    float3 cs = s.a > 0.0 ? s.rgb / s.a : float3(0.0);\n"
        "    float3 cd = d.a > 0.0 ? d.rgb / d.a : float3(0.0);\n"
        "    float3 f = " + std::string(equation < 11 ? kSeparable[equation] : kNonSeparable[equation - 11]) + ";\n"
        "    float p0 = s.a * d.a, p1 = s.a * (1.0 - d.a), p2 = d.a * (1.0 - s.a);\n"
        "    return float4(f * p0 + cs * p1 + cd * p2, p0 + p1 + p2);\n}\n\n";
    std::string out = msl;
    out.insert(fnAt, helpers);
    const size_t fn = out.find(signature);
    const size_t open = fn + signature.size() - 1;
    std::string param = "float4 wmgl_dst0 [[color(0)]]";
    if (out[open + 1] != ')') param += ", ";
    out.insert(open + 1, param);
    const std::string assign = "out." + name + " = wmgl_advanced_blend(out." + name + ", wmgl_dst0); ";
    for (size_t at = out.find("return out;", fn); at != std::string::npos; at = out.find("return out;", at + assign.size() + 11))
        out.insert(at, assign);
    return out;
}

namespace {

GLenum glTypeOfAst(const glslang::TType& t) {
    const int n = t.getVectorSize();
    auto pick = [&](GLenum s, GLenum v2, GLenum v3, GLenum v4) { return n <= 1 ? s : n == 2 ? v2 : n == 3 ? v3 : v4; };
    if (t.isMatrix()) {
        const int c = t.getMatrixCols(), r = t.getMatrixRows();
        static const GLenum kMat[3][3] = {{GL_FLOAT_MAT2, GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4},
                                          {GL_FLOAT_MAT3x2, GL_FLOAT_MAT3, GL_FLOAT_MAT3x4},
                                          {GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4}};
        static const GLenum kDMat[3][3] = {{GL_DOUBLE_MAT2, GL_DOUBLE_MAT2x3, GL_DOUBLE_MAT2x4},
                                           {GL_DOUBLE_MAT3x2, GL_DOUBLE_MAT3, GL_DOUBLE_MAT3x4},
                                           {GL_DOUBLE_MAT4x2, GL_DOUBLE_MAT4x3, GL_DOUBLE_MAT4}};
        return t.getBasicType() == glslang::EbtDouble ? kDMat[c - 2][r - 2] : kMat[c - 2][r - 2];
    }
    switch (t.getBasicType()) {
    case glslang::EbtDouble: return pick(GL_DOUBLE, GL_DOUBLE_VEC2, GL_DOUBLE_VEC3, GL_DOUBLE_VEC4);
    case glslang::EbtInt: return pick(GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4);
    case glslang::EbtUint: return pick(GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4);
    case glslang::EbtBool: return pick(GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4);
    default: return pick(GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4);
    }
}

// The outputs a stage can capture: its user outputs plus the built-in
// gl_Position, gl_PointSize, gl_ClipDistance and gl_CullDistance.
std::vector<StageOutputVar> captureableOutputs(glslang::TIntermediate* intermediate) {
    std::vector<StageOutputVar> outputs;
    bool clipDistance = false;
    // A struct's members, by "s.member" ("s[k].member" for an array of
    // structs), recursively: `glName` names the struct, `expr` reads it.
    std::function<void(const glslang::TType&, const std::string&, const std::string&)> structMembers =
        [&](const glslang::TType& t, const std::string& glName, const std::string& expr) {
            for (const glslang::TTypeLoc& member : *t.getStruct()) {
                const glslang::TType& m = *member.type;
                const std::string field = unescapeName(m.getFieldName().c_str());
                const std::string n = glName + "." + field, e = expr + "." + m.getFieldName().c_str();
                if (m.isStruct()) {
                    if (m.isSizedArray())
                        for (int k = 0; k < m.getOuterArraySize(); ++k) {
                            glslang::TType element(m, 0);
                            structMembers(element, n + "[" + std::to_string(k) + "]", e + "[" + std::to_string(k) + "]");
                        }
                    else
                        structMembers(m, n, e);
                    continue;
                }
                outputs.push_back({n, glTypeOfAst(m), m.isSizedArray() ? m.getOuterArraySize() : 0, e});
            }
        };
    // A struct-typed variable or member (an array of structs element by element).
    auto structVar = [&](const glslang::TType& t, const std::string& glName, const std::string& expr) {
        if (!t.isSizedArray()) return structMembers(t, glName, expr);
        for (int k = 0; k < t.getOuterArraySize(); ++k) {
            glslang::TType element(t, 0);
            structMembers(element, glName + "[" + std::to_string(k) + "]", expr + "[" + std::to_string(k) + "]");
        }
    };
    glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
    if (root) {
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol || symbol->getQualifier().storage != glslang::EvqVaryingOut) continue;
                const glslang::TType& type = symbol->getType();
                if (type.getBasicType() == glslang::EbtBlock) {
                    const std::string blockName = unescapeName(type.getTypeName().c_str());
                    const bool anonymous = glslang::IsAnonymous(symbol->getName());
                    for (const glslang::TTypeLoc& member : *type.getStruct()) {
                        const glslang::TType& m = *member.type;
                        const std::string field = unescapeName(m.getFieldName().c_str());
                        if (field == "gl_ClipDistance" && m.isSizedArray()) {
                            outputs.push_back({"gl_ClipDistance", GL_FLOAT, m.getOuterArraySize()});
                            clipDistance = true;
                        }
                        if (field == "gl_CullDistance" && m.isSizedArray())
                            outputs.push_back({"gl_CullDistance", GL_FLOAT, m.getOuterArraySize()});
                        if (blockName == "gl_PerVertex" || field.rfind("gl_", 0) == 0) continue;
                        if (m.isStruct()) {
                            // A struct-typed member: its members, "Block.member.field".
                            if (type.isArray()) {
                                if (anonymous || !type.isSizedArray()) continue;
                                for (int k = 0; k < type.getOuterArraySize(); ++k) {
                                    const std::string element = "[" + std::to_string(k) + "].";
                                    structVar(m, blockName + element + field,
                                              std::string(symbol->getName().c_str()) + element + m.getFieldName().c_str());
                                }
                                continue;
                            }
                            const std::string expression = anonymous ? m.getFieldName().c_str()
                                                                     : std::string(symbol->getName().c_str()) + "." +
                                                                           m.getFieldName().c_str();
                            structVar(m, blockName + "." + field, expression);
                            if (anonymous) structVar(m, field, expression);
                            continue;
                        }
                        if (type.isArray()) {
                            // An array of blocks: "Block[k].member" (GL 4.6 section 7.3.1.1).
                            if (anonymous || !type.isSizedArray()) continue;
                            for (int k = 0; k < type.getOuterArraySize(); ++k) {
                                const std::string element = "[" + std::to_string(k) + "].";
                                outputs.push_back({blockName + element + field, glTypeOfAst(m),
                                                   m.isSizedArray() ? m.getOuterArraySize() : 0,
                                                   std::string(symbol->getName().c_str()) + element + m.getFieldName().c_str()});
                            }
                            continue;
                        }
                        // User blocks: members are named "Block.member" (GL 3.3 section 2.15);
                        // members of a block without an instance name also by their own name.
                        const std::string expression = anonymous ? m.getFieldName().c_str()
                                                                 : std::string(symbol->getName().c_str()) + "." + m.getFieldName().c_str();
                        outputs.push_back({blockName + "." + field, glTypeOfAst(m), m.isSizedArray() ? m.getOuterArraySize() : 0,
                                           expression});
                        if (anonymous)
                            outputs.push_back({field, glTypeOfAst(m), m.isSizedArray() ? m.getOuterArraySize() : 0, expression,
                                               blockName + "." + field});
                    }
                    continue;
                }
                const std::string codegenName = symbol->getName().c_str();
                const std::string name = unescapeName(codegenName);
                if (name.rfind("gl_", 0) == 0) continue;
                // Lowered legacy varyings are captured by their built-in names (gl_FrontColor, gl_TexCoord, ...).
                static const char* const kLegacy[] = {"wmgl_FrontColor", "wmgl_BackColor", "wmgl_FrontSecondaryColor",
                                                      "wmgl_BackSecondaryColor", "wmgl_TexCoord", "wmgl_FogFragCoord"};
                const bool legacy = std::find(std::begin(kLegacy), std::end(kLegacy), name) != std::end(kLegacy);
                // A struct (GLSL ES 3.00 allows them as outputs): its members (structMembers).
                if (type.isStruct()) {
                    structVar(type, name, codegenName);
                    continue;
                }
                // The capture code reads the codegen name (escaped keywords keep their prefix there).
                outputs.push_back({legacy ? "gl_" + name.substr(5) : name, glTypeOfAst(type),
                                   type.isSizedArray() ? type.getOuterArraySize() : 0,
                                   legacy || codegenName != name ? codegenName : std::string()});
                if (symbol->getQualifier().hasLocation())
                    outputs.back().location = static_cast<int>(symbol->getQualifier().layoutLocation);
            }
        }
    }
    outputs.push_back({"gl_Position", GL_FLOAT_VEC4, 0});
    outputs.push_back({"gl_PointSize", GL_FLOAT, 0});
    if (!clipDistance) outputs.push_back({"gl_ClipDistance", GL_FLOAT, 8});
    return outputs;
}

// Transform feedback declared in the shader (GL 4.4, enhanced layouts:
// xfb_buffer, xfb_offset, xfb_stride on the last vertex processing stage's
// outputs). It replaces glTransformFeedbackVaryings. Returns false when the
// stage declares none.
bool layoutShaderTransformFeedback(glslang::TIntermediate* last, const std::vector<StageOutputVar>& outputs, LinkResult& result) {
    if (!last || !last->getXfbMode()) return false;
    glslang::TIntermAggregate* root = last->getTreeRoot() ? last->getTreeRoot()->getAsAggregate() : nullptr;
    if (!root) return false;
    std::vector<TransformFeedbackVarying> varyings;
    // A struct is captured member by member from its offset, each member
    // aligned to its own size class (4 bytes, 8 for doubles).
    std::function<void(const std::string&, const glslang::TType&, int, uint32_t&)> addStruct =
        [&](const std::string& name, const glslang::TType& type, int buffer, uint32_t& offset) {
            const int elements = type.isSizedArray() ? type.getOuterArraySize() : 1;
            for (int k = 0; k < elements; ++k) {
                const std::string element = type.isSizedArray() ? name + "[" + std::to_string(k) + "]" : name;
                for (const glslang::TTypeLoc& member : *type.getStruct()) {
                    const glslang::TType& m = *member.type;
                    const std::string path = element + "." + unescapeName(m.getFieldName().c_str());
                    if (m.getBasicType() == glslang::EbtStruct) {
                        addStruct(path, m, buffer, offset);
                        continue;
                    }
                    TransformFeedbackVarying v;
                    v.name = path;
                    v.type = glTypeOfAst(m);
                    v.size = m.isSizedArray() ? m.getOuterArraySize() : 1;
                    v.array = m.isSizedArray();
                    v.components = glTypeWords(v.type) * v.size;
                    const uint32_t align = m.getBasicType() == glslang::EbtDouble ? 8 : 4;
                    offset = (offset + align - 1) / align * align;
                    v.buffer = buffer;
                    v.offset = offset / 4;
                    v.expression = path;
                    v.source = path;
                    offset += static_cast<uint32_t>(v.components) * 4;
                    varyings.push_back(v);
                }
            }
        };
    auto add = [&](const std::string& name, const glslang::TType& type, const glslang::TQualifier& q, int extraBuffers = 0) {
        if (!q.hasXfbOffset()) return;
        if (type.getBasicType() == glslang::EbtStruct) {
            uint32_t offset = q.layoutXfbOffset;
            addStruct(name, type, (q.hasXfbBuffer() ? static_cast<int>(q.layoutXfbBuffer) : 0) + extraBuffers, offset);
            return;
        }
        TransformFeedbackVarying v;
        v.name = name;
        v.type = glTypeOfAst(type);
        v.size = type.isSizedArray() ? type.getOuterArraySize() : 1;
        v.array = type.isSizedArray();
        v.components = glTypeWords(v.type) * v.size;
        v.buffer = (q.hasXfbBuffer() ? static_cast<int>(q.layoutXfbBuffer) : 0) + extraBuffers;
        v.offset = q.layoutXfbOffset / 4;
        v.expression = name;
        v.source = name;
        for (const StageOutputVar& o : outputs)
            if (o.name == name) {
                if (!o.expression.empty()) v.expression = o.expression;
                if (!o.qualified.empty()) v.source = o.qualified;
            }
        varyings.push_back(v);
    };
    for (TIntermNode* node : root->getSequence()) {
        glslang::TIntermAggregate* objects = node->getAsAggregate();
        if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
        for (TIntermNode* object : objects->getSequence()) {
            glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
            if (!symbol || symbol->getQualifier().storage != glslang::EvqVaryingOut) continue;
            const glslang::TType& type = symbol->getType();
            if (type.getBasicType() == glslang::EbtBlock) {
                const std::string blockName = unescapeName(type.getTypeName().c_str());
                const bool anonymous = glslang::IsAnonymous(symbol->getName());
                // An array of N blocks is captured by N consecutive buffers:
                // element k's members at their offsets in buffer xfb_buffer + k
                // (GLSL 4.60 section 4.4.2.1).
                const int elements = type.isSizedArray() && !anonymous ? type.getOuterArraySize() : 0;
                for (int k = 0; k < std::max(1, elements); ++k)
                    for (const glslang::TTypeLoc& member : *type.getStruct()) {
                        const std::string field = unescapeName(member.type->getFieldName().c_str());
                        const std::string element = elements ? "[" + std::to_string(k) + "]" : "";
                        const std::string name = field.rfind("gl_", 0) == 0 || anonymous ? field : blockName + element + "." + field;
                        add(name, *member.type, member.type->getQualifier(), k);
                    }
                continue;
            }
            add(unescapeName(symbol->getName().c_str()), type, symbol->getQualifier());
        }
    }
    if (varyings.empty()) return false;
    std::sort(varyings.begin(), varyings.end(), [](const TransformFeedbackVarying& a, const TransformFeedbackVarying& b) {
        return a.buffer != b.buffer ? a.buffer < b.buffer : a.offset < b.offset;
    });
    int buffers = 0;
    for (const TransformFeedbackVarying& v : varyings) buffers = std::max(buffers, v.buffer + 1);
    for (int b = 0; b < buffers; ++b) {
        const unsigned stride = last->getXfbStride(b);
        uint32_t words = stride != glslang::TQualifier::layoutXfbStrideEnd ? stride / 4 : 0;
        for (const TransformFeedbackVarying& v : varyings)
            if (v.buffer == b) words = std::max(words, v.offset + static_cast<uint32_t>(v.components));
        result.xfbStride[b] = words;
    }
    result.transformFeedback = varyings;
    result.xfbBufferCount = buffers;
    return true;
}

}  // namespace

namespace {

// Uniform initializers (GLSL 1.20+). The codegen parse (Vulkan rules) drops
// them, so they come from a GL-rules parse of each shader: name -> the
// initializer's components, flattened (array elements, matrix columns).
struct UniformInitializer {
    std::vector<double> values;
};

std::map<std::string, UniformInitializer> uniformInitializers(const std::vector<ShaderSource>& shaders) {
    std::map<std::string, UniformInitializer> out;
    for (const ShaderSource& shader : shaders) {
        if (shader.source.find('=') == std::string::npos) continue;
        const SubroutineLowering lowered = lowerSubroutines(shader.stage, spliceLineContinuations(shader.source));
        const std::string source = raiseComputeVersion(shader.stage, lowered.source);
        const EShLanguage lang = language(shader.stage);
        glslang::TShader parsed(lang);
        const char* text = source.c_str();
        parsed.setStrings(&text, 1);
        parsed.setPreamble(kPreamble);
        parsed.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientNone, 0);
        parsed.setEnvClient(glslang::EShClientNone, glslang::EShTargetClientVersion(0));
        parsed.setEnvTarget(glslang::EShTargetNone, glslang::EShTargetLanguageVersion(0));
        if (!parsed.parse(resources(), 110, ENoProfile, false, false, EShMsgRelaxedErrors)) continue;
        glslang::TIntermediate* intermediate = parsed.getIntermediate();
        glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot() ? intermediate->getTreeRoot()->getAsAggregate() : nullptr;
        if (!root) continue;
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol || symbol->getQualifier().storage != glslang::EvqUniform) continue;
                const glslang::TConstUnionArray& values = symbol->getConstArray();
                if (values.empty() || symbol->getType().isStruct() || symbol->getBasicType() == glslang::EbtSampler) continue;
                UniformInitializer init;
                for (int i = 0; i < values.size(); ++i) {
                    const glslang::TConstUnion& v = values[i];
                    switch (v.getType()) {
                    case glslang::EbtInt: init.values.push_back(v.getIConst()); break;
                    case glslang::EbtUint: init.values.push_back(v.getUConst()); break;
                    case glslang::EbtBool: init.values.push_back(v.getBConst() ? 1.0 : 0.0); break;
                    default: init.values.push_back(v.getDConst()); break;
                    }
                }
                out[unescapeName(symbol->getName().c_str())] = init;
            }
        }
    }
    return out;
}

// Writes the initializers into the default block image of `result`.
void applyUniformInitializers(const std::vector<ShaderSource>& shaders, LinkResult& result) {
    result.defaultBlockInit.assign((result.defaultBlockSize + 15) & ~15u, 0);
    const std::map<std::string, UniformInitializer> inits = uniformInitializers(shaders);
    if (inits.empty()) return;
    for (const UniformInfo& u : result.uniforms) {
        if (u.blockIndex >= 0 || u.samplerSlot >= 0 || u.imageSlot >= 0 || u.atomicBufferIndex >= 0 || u.offset < 0) continue;
        auto it = inits.find(arrayBaseName(u.name));
        if (it == inits.end()) continue;
        const std::vector<double>& values = it->second.values;
        const int components = glTypeComponents(u.type);
        int columns = 1;
        switch (u.type) {
        case GL_FLOAT_MAT2: case GL_FLOAT_MAT2x3: case GL_FLOAT_MAT2x4: case GL_DOUBLE_MAT2: case GL_DOUBLE_MAT2x3:
        case GL_DOUBLE_MAT2x4: columns = 2; break;
        case GL_FLOAT_MAT3: case GL_FLOAT_MAT3x2: case GL_FLOAT_MAT3x4: case GL_DOUBLE_MAT3: case GL_DOUBLE_MAT3x2:
        case GL_DOUBLE_MAT3x4: columns = 3; break;
        case GL_FLOAT_MAT4: case GL_FLOAT_MAT4x2: case GL_FLOAT_MAT4x3: case GL_DOUBLE_MAT4: case GL_DOUBLE_MAT4x2:
        case GL_DOUBLE_MAT4x3: columns = 4; break;
        default: break;
        }
        const int rows = components / columns;
        const bool isDouble = glTypeWords(u.type) == 2 * components;
        const char kind = isDouble ? 'd'
                          : (u.type == GL_INT || u.type == GL_INT_VEC2 || u.type == GL_INT_VEC3 || u.type == GL_INT_VEC4) ? 'i'
                          : (u.type == GL_UNSIGNED_INT || u.type == GL_UNSIGNED_INT_VEC2 || u.type == GL_UNSIGNED_INT_VEC3 ||
                             u.type == GL_UNSIGNED_INT_VEC4) ? 'u'
                          : (u.type == GL_BOOL || u.type == GL_BOOL_VEC2 || u.type == GL_BOOL_VEC3 || u.type == GL_BOOL_VEC4) ? 'b'
                                                                                                                        : 'f';
        const size_t scalar = isDouble ? 8 : 4;
        const size_t matrixStride = u.matrixStride ? static_cast<size_t>(u.matrixStride) : 16;
        const size_t stride = u.arrayStride ? static_cast<size_t>(u.arrayStride) : 0;
        for (GLint e = 0; e < u.size; ++e)
            for (int c = 0; c < components; ++c) {
                const size_t index = static_cast<size_t>(e) * static_cast<size_t>(components) + static_cast<size_t>(c);
                if (index >= values.size()) break;
                const size_t at = static_cast<size_t>(u.offset) + static_cast<size_t>(e) * stride +
                                  (columns > 1 ? static_cast<size_t>(c / rows) * matrixStride + static_cast<size_t>(c % rows) * scalar
                                               : static_cast<size_t>(c) * scalar);
                if (at + scalar > result.defaultBlockInit.size()) continue;
                uint8_t* dst = result.defaultBlockInit.data() + at;
                const double v = values[index];
                if (kind == 'd') std::memcpy(dst, &v, 8);
                else if (kind == 'f') { const float f = static_cast<float>(v); std::memcpy(dst, &f, 4); }
                else if (kind == 'i') { const int32_t i = static_cast<int32_t>(v); std::memcpy(dst, &i, 4); }
                else { const uint32_t i = static_cast<uint32_t>(v); std::memcpy(dst, &i, 4); }
            }
    }
}

}  // namespace

// Moves the wmgl_FixedFunction block and its members after the
// application's blocks and uniforms (LinkResult::fixedFunctionBlock).
static void hideFixedFunctionBlock(LinkResult& r) {
    int ff = -1;
    for (size_t b = 0; b < r.blocks.size(); ++b)
        if (r.blocks[b].name == kFixedFunctionBlockName) ff = static_cast<int>(b);
    if (ff < 0) return;
    std::vector<int> blockMap(r.blocks.size()), uniformMap(r.uniforms.size());
    std::vector<UniformBlockInfo> blocks;
    for (size_t b = 0; b < r.blocks.size(); ++b) {
        if (static_cast<int>(b) == ff) continue;
        blockMap[b] = static_cast<int>(blocks.size());
        blocks.push_back(std::move(r.blocks[b]));
    }
    blockMap[static_cast<size_t>(ff)] = static_cast<int>(blocks.size());
    blocks.push_back(std::move(r.blocks[static_cast<size_t>(ff)]));
    std::vector<UniformInfo> uniforms;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t u = 0; u < r.uniforms.size(); ++u) {
            if ((r.uniforms[u].blockIndex == ff) != (pass == 1)) continue;
            uniformMap[u] = static_cast<int>(uniforms.size());
            uniforms.push_back(r.uniforms[u]);
        }
    for (UniformInfo& u : uniforms)
        if (u.blockIndex >= 0) u.blockIndex = blockMap[static_cast<size_t>(u.blockIndex)];
    for (UniformBlockInfo& b : blocks)
        for (int& u : b.activeUniforms) u = uniformMap[static_cast<size_t>(u)];
    for (AtomicCounterBufferInfo& b : r.atomicBuffers)
        for (int& u : b.activeUniforms) u = uniformMap[static_cast<size_t>(u)];
    for (auto& location : r.locations)
        if (location.first >= 0) location.first = uniformMap[static_cast<size_t>(location.first)];
    r.blocks = std::move(blocks);
    r.uniforms = std::move(uniforms);
    r.fixedFunctionBlock = static_cast<int>(r.blocks.size() - 1);
}

// `geometry`/`geometryCapture` (optional) receive the parsed geometry stage
// and the layout of the vertex outputs it reads (see geometry.h).
// What link() needs to build the tessellation emulation: the parsed control
// and evaluation stages, the layout of the vertex outputs they read, and the
// geometry stage as GLSL (linked again after the evaluation stage).
struct TessellationLink {
    TessStageInfo control, evaluation;
    bool hasControl = false;
    LinkResult capture;
    std::string geometryGlsl;
};

static LinkResult linkImplCore(const LinkInput& input, const CaptureCode* capture, GeometryShaderInfo* geometry,
                               LinkResult* geometryCapture, TessellationLink* tessellation);

// Internal programs (emulation kernels and stages) that fail to link are
// WineMetalGL bugs: WINEMETALGL_DUMP_SHADERS=1 prints their sources.
static LinkResult linkImpl(const LinkInput& input, const CaptureCode* capture, GeometryShaderInfo* geometry = nullptr,
                           LinkResult* geometryCapture = nullptr, TessellationLink* tessellation = nullptr) {
    LinkResult result = linkImplCore(input, capture, geometry, geometryCapture, tessellation);
    result.spirv = input.spirv;
    result.spirvUnnamed = input.spirvUnnamed;
    if (!result.ok && input.internal)
        if (const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS"); v && v[0] == '1')
            for (const ShaderSource& shader : input.shaders)
                logRawFormat(LogCategory::Shader, "winemetalgl: internal stage %d failed to link:\n%s\n",
                             static_cast<int>(shader.stage),
                             shader.source.c_str());
    return result;
}

static LinkResult linkImplCore(const LinkInput& input, const CaptureCode* capture, GeometryShaderInfo* geometry,
                               LinkResult* geometryCapture, TessellationLink* tessellation) {
    initGlslang();
    LinkResult result;
    std::vector<ParsedShader> parsed(input.shaders.size());
    bool stagePresent[kStageCount] = {};
    // Legacy built-in varyings the fragment stage reads from the vertex stage.
    UpgradeOptions vertexUpgrade;
    bool hasGeometry = false;
    for (const ShaderSource& shader : input.shaders) hasGeometry |= shader.stage == Stage::Geometry;
    if (!hasGeometry)
        for (const ShaderSource& shader : input.shaders)
            if (shader.stage == Stage::Fragment)
                for (const std::string& v : upgradeLegacyGlsl(Stage::Fragment, relaxDesktopGlsl(shader.source)).builtinVaryingsRead)
                    vertexUpgrade.provideVaryings.insert(v);
    for (size_t i = 0; i < input.shaders.size(); ++i) {
        const bool captureHere = capture && input.shaders[i].stage == Stage::Vertex && definesMain(input.shaders[i].source);
        if (!parseForCodegen(input.shaders[i].stage, input.shaders[i].source, parsed[i], result.log,
                             captureHere ? capture : nullptr,
                             input.shaders[i].stage == Stage::Vertex ? vertexUpgrade : UpgradeOptions()))
            return result;
        stagePresent[static_cast<int>(input.shaders[i].stage)] = true;
        if (input.shaders[i].stage == Stage::Fragment && parsed[i].shader->getIntermediate())
            result.blendSupport |= parsed[i].shader->getIntermediate()->getBlendEquations();
    }
    if (stagePresent[static_cast<int>(Stage::Compute)]) {
        for (int s = 0; s < kStageCount; ++s)
            if (stagePresent[s] && s != static_cast<int>(Stage::Compute)) {
                result.log += "error: a compute shader cannot be linked with other shader stages\n";
                return result;
            }
    }
    if (!stagePresent[static_cast<int>(Stage::Compute)] && !input.separable) {
        if (!stagePresent[static_cast<int>(Stage::Vertex)] && !stagePresent[static_cast<int>(Stage::Fragment)] &&
            !stagePresent[static_cast<int>(Stage::Geometry)]) {
            result.log += "error: no shaders attached\n";
            return result;
        }
        if (stagePresent[static_cast<int>(Stage::Geometry)] && !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: a geometry shader requires a vertex shader\n";
            return result;
        }
        if ((stagePresent[static_cast<int>(Stage::TessControl)] || stagePresent[static_cast<int>(Stage::TessEval)]) &&
            !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: tessellation shaders require a vertex shader\n";
            return result;
        }
    }
    // A control shader without an evaluation shader cannot draw anything
    // (GL_PATCHES needs both; transform feedback cannot capture patches).
    if (stagePresent[static_cast<int>(Stage::TessControl)] && !stagePresent[static_cast<int>(Stage::TessEval)] &&
        !input.separable) {
        result.log += "error: a tessellation control shader requires a tessellation evaluation shader\n";
        return result;
    }

    glslang::TProgram program;
    for (auto& p : parsed) program.addShader(p.shader.get());
    if (!program.link(kMessages)) {
        result.log += unescapeName(cleanLog(program.getInfoLog(), program.getInfoDebugLog()));
        return result;
    }
    if (stagePresent[static_cast<int>(Stage::Compute)]) {
        const glslang::TIntermediate* cs = program.getIntermediate(EShLangCompute);
        if (!cs->isLocalSizeSet()) {
            result.log += "error: compute shader has no local_size layout qualifier\n";
            return result;
        }
        for (int d = 0; d < 3; ++d) result.computeLocalSize[d] = cs->getLocalSize(d);
    }
    const bool tessellated = stagePresent[static_cast<int>(Stage::TessEval)];
    if (tessellated) {
        // The tessellation layout (merged over the stage's shader objects).
        TessellationEmulation& t = result.tess;
        const glslang::TIntermediate* tes = program.getIntermediate(EShLangTessEvaluation);
        t.mode = tes->getInputPrimitive() == glslang::ElgQuads      ? GL_QUADS
                 : tes->getInputPrimitive() == glslang::ElgIsolines ? GL_ISOLINES
                                                                    : GL_TRIANGLES;
        t.spacing = tes->getVertexSpacing() == glslang::EvsFractionalEven  ? GL_FRACTIONAL_EVEN
                    : tes->getVertexSpacing() == glslang::EvsFractionalOdd ? GL_FRACTIONAL_ODD
                                                                           : GL_EQUAL;
        t.order = tes->getVertexOrder() == glslang::EvoCw ? GL_CW : GL_CCW;
        t.pointMode = tes->getPointMode();
        if (const glslang::TIntermediate* tcs = program.getIntermediate(EShLangTessControl)) {
            t.hasControl = true;
            t.verticesOut = tcs->getVertices();
        }
    } else if (stagePresent[static_cast<int>(Stage::TessControl)]) {
        // A separable control-only program: its output vertex count is queryable.
        if (const glslang::TIntermediate* tcs = program.getIntermediate(EShLangTessControl))
            result.tess.verticesOut = tcs->getVertices();
    }
    // Transform feedback varyings are validated against the last vertex
    // processing stage (geometry if present, else tessellation evaluation,
    // else vertex).
    // A separable control-only program captures nothing: its varyings would
    // apply if it ended a pipeline, which the control stage cannot, so desktop
    // GL accepts them without checking (unlike OpenGL ES).
    const bool controlOnly = input.separable && stagePresent[static_cast<int>(Stage::TessControl)] && !tessellated &&
                             !stagePresent[static_cast<int>(Stage::Geometry)] && !stagePresent[static_cast<int>(Stage::Vertex)];
    // Transform feedback declared in the shader (GL 4.4) replaces the API's varyings.
    bool shaderCapture = false;
    if (!capture && !controlOnly) {
        const bool geometry = stagePresent[static_cast<int>(Stage::Geometry)];
        glslang::TIntermediate* last = program.getIntermediate(geometry      ? EShLangGeometry
                                                               : tessellated ? EShLangTessEvaluation
                                                                             : EShLangVertex);
        shaderCapture = last && layoutShaderTransformFeedback(last, captureableOutputs(last), result);
    }
    if (!capture && !shaderCapture && !input.transformFeedbackVaryings.empty() && !controlOnly) {
        const bool geometry = stagePresent[static_cast<int>(Stage::Geometry)];
        if (!geometry && !tessellated && !stagePresent[static_cast<int>(Stage::Vertex)]) {
            result.log += "error: transform feedback varyings need a vertex or geometry shader\n";
            return result;
        }
        glslang::TIntermediate* last = program.getIntermediate(geometry      ? EShLangGeometry
                                                               : tessellated ? EShLangTessEvaluation
                                                                             : EShLangVertex);
        if (!layoutTransformFeedback(input.transformFeedbackVaryings, input.transformFeedbackMode,
                                     captureableOutputs(last), result, result.log))
            return result;
    }
    // Reflect before mapIO: afterwards every input/output has a location and
    // explicit layout(location) can no longer be told apart from assigned ones.
    program.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers | EShReflectionUnwrapIOBlocks |
                            EShReflectionIntermediateIO);  // the program's own first and last stages

    // Live top-level names and blocks, from glslang's liveness-aware reflection.
    std::set<std::string> liveDefault;
    std::set<std::string> liveBlocks;
    std::set<std::string> liveBuffers;  // storage blocks (GL 4.3)
    for (int i = 0; i < program.getNumBufferBlocks(); ++i)
        liveBuffers.insert(unescapeName(topLevelName(program.getBufferBlock(i).name)));
    // OpenGL ES: shared and std140 buffer blocks, unused ones too (GLSL ES
    // 3.10 section 4.4.5).
    if (input.es)
        for (const ShaderSource& sh : input.shaders)
            for (const std::string& name : sharedBufferBlocks(sh.source)) liveBuffers.insert(name);
    int defaultBlockReflectionIndex = -1;
    for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
        const auto& block = program.getUniformBlock(i);
        if (block.name == kDefaultBlockName) defaultBlockReflectionIndex = i;
        else liveBlocks.insert(unescapeName(topLevelName(block.name)));
    }
    // SPIR-V stores bools in blocks as uint; the AST keeps the GL types.
    std::map<std::string, GLenum> boolTypes;
    // Cross-stage uniform interface rules (GLSL 3.30 section 4.3.7): a block
    // is anonymous in every stage or in none, and names that anonymous blocks
    // put in the global scope must not clash with other blocks' members or
    // with default-block uniforms.
    std::map<std::string, bool> blockAnonymous;             // block name -> declared without instance name
    std::map<std::string, std::string> globalUniformOwner;  // global name -> block name ("" = default block)
    std::string interfaceError;
    auto claimGlobal = [&](const std::string& name, const std::string& owner) {
        auto [it, inserted] = globalUniformOwner.emplace(name, owner);
        if (!inserted && it->second != owner && interfaceError.empty())
            interfaceError = "error: uniform '" + name + "' is declared by both " +
                             (it->second.empty() ? std::string("the default uniform block") : "block " + it->second) + " and " +
                             (owner.empty() ? std::string("the default uniform block") : "block " + owner) + "\n";
    };
    // GL treats uniform blocks declared shared or std140 as active even when
    // nothing references them (only packed blocks may be eliminated). The
    // packing qualifier is gone in SPIR-V, so read it from the linker objects.
    // Separable programs: varyings declared with layout(location) keep it
    // (reflection reports only the program's own inputs and outputs).
    std::set<std::string> explicitVaryings;
    std::map<std::string, int> explicitBindings;  // "block:<name>" or opaque uniform name -> binding
    for (int s = 0; s < kStageCount; ++s) {
        if (!stagePresent[s]) continue;
        glslang::TIntermediate* intermediate = program.getIntermediate(language(static_cast<Stage>(s)));
        glslang::TIntermAggregate* root = intermediate && intermediate->getTreeRoot()
                                              ? intermediate->getTreeRoot()->getAsAggregate()
                                              : nullptr;
        if (!root) continue;
        for (TIntermNode* node : root->getSequence()) {
            glslang::TIntermAggregate* objects = node->getAsAggregate();
            if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
            for (TIntermNode* object : objects->getSequence()) {
                glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                if (!symbol) continue;
                const glslang::TType& type = symbol->getType();
                const glslang::TQualifier& q = type.getQualifier();
                const bool varying = (q.storage == glslang::EvqVaryingIn && s != static_cast<int>(Stage::Vertex)) ||
                                     (q.storage == glslang::EvqVaryingOut && s != static_cast<int>(Stage::Fragment));
                if (varying && q.hasLocation())
                    explicitVaryings.insert(unescapeName(type.getBasicType() == glslang::EbtBlock ? type.getTypeName().c_str()
                                                                                                    : symbol->getName().c_str()));
                if (q.storage == glslang::EvqBuffer && type.getBasicType() == glslang::EbtBlock) {
                    const std::string blockName = unescapeName(type.getTypeName().c_str());
                    if (q.hasBinding()) explicitBindings["buffer:" + blockName] = static_cast<int>(q.layoutBinding);
                    if (q.layoutPacking == glslang::ElpShared || q.layoutPacking == glslang::ElpStd140) liveBuffers.insert(blockName);
                    collectBoolMembers(type, glslang::IsAnonymous(symbol->getName()) ? "" : blockName + ".", boolTypes);
                }
                if (q.storage == glslang::EvqUniform && q.hasBinding()) {
                    // layout(binding = N) (GL 4.2): the initial binding of blocks, samplers and images.
                    if (type.getBasicType() == glslang::EbtBlock)
                        explicitBindings["block:" + unescapeName(type.getTypeName().c_str())] = static_cast<int>(q.layoutBinding);
                    else if (type.getBasicType() == glslang::EbtSampler)
                        explicitBindings[unescapeName(symbol->getName().c_str())] = static_cast<int>(q.layoutBinding);
                }
                if (q.storage == glslang::EvqUniform) {
                    if (type.getBasicType() == glslang::EbtBlock) {
                        const bool anonymous = glslang::IsAnonymous(symbol->getName());
                        const std::string blockName = unescapeName(type.getTypeName().c_str());
                        collectBoolMembers(type, anonymous ? "" : blockName + ".", boolTypes);
                        if (blockName != kDefaultBlockName) {
                            auto [it, inserted] = blockAnonymous.emplace(blockName, anonymous);
                            if (!inserted && it->second != anonymous && interfaceError.empty())
                                interfaceError = "error: uniform block '" + blockName +
                                                 "' has an instance name in one stage but not in another\n";
                            if (anonymous)
                                for (const glslang::TTypeLoc& member : *type.getStruct())
                                    claimGlobal(unescapeName(member.type->getFieldName().c_str()), blockName);
                        } else {
                            // Loose uniforms gathered into the default block.
                            for (const glslang::TTypeLoc& member : *type.getStruct())
                                claimGlobal(unescapeName(member.type->getFieldName().c_str()), "");
                        }
                    } else {
                        collectBoolMembers(type, unescapeName(symbol->getName().c_str()), boolTypes, true);
                        claimGlobal(unescapeName(symbol->getName().c_str()), "");
                    }
                }
                if (type.getBasicType() == glslang::EbtBlock && q.storage == glslang::EvqUniform &&
                    (q.layoutPacking == glslang::ElpShared || q.layoutPacking == glslang::ElpStd140) &&
                    type.getTypeName() != kDefaultBlockName)
                    liveBlocks.insert(type.getTypeName().c_str());
            }
        }
    }
    if (!interfaceError.empty()) {
        result.log += interfaceError;
        return result;
    }
    std::set<std::string> liveSamplers;
    for (int i = 0; i < program.getNumUniformVariables(); ++i) {
        const auto& u = program.getUniform(i);
        if (u.index == defaultBlockReflectionIndex && defaultBlockReflectionIndex >= 0) liveDefault.insert(unescapeName(topLevelName(u.name)));
        else if (u.index < 0) liveSamplers.insert(unescapeName(topLevelName(u.name)));
    }

    // Collect attributes/outputs from this pre-mapIO reflection (see below),
    // then assign locations for codegen.
    struct PipeVar {
        std::string name; GLenum type; int size; unsigned location; uint32_t stages; int index; bool array = false;
        bool patch = false;
        int component = 0;
    };
    std::vector<PipeVar> pipeInputs, pipeOutputs;
    for (int i = 0; i < program.getNumPipeInputs(); ++i) {
        const auto& v = program.getPipeInput(i);
        const glslang::TType* type = v.getType();
        pipeInputs.push_back({unescapeName(v.name), static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(),
                              static_cast<uint32_t>(v.stages), 0, type && type->isArray(), type && type->getQualifier().patch,
                              type && type->getQualifier().hasComponent() ? static_cast<int>(type->getQualifier().layoutComponent)
                                                                          : 0});
    }
    for (int i = 0; i < program.getNumPipeOutputs(); ++i) {
        const auto& v = program.getPipeOutput(i);
        const glslang::TType* type = v.getType();
        const int index = type && type->getQualifier().hasIndex() ? static_cast<int>(type->getQualifier().layoutIndex) : 0;
        pipeOutputs.push_back({unescapeName(v.name), static_cast<GLenum>(v.glDefineType), v.size, v.layoutLocation(),
                               static_cast<uint32_t>(v.stages), index, type && type->isArray(), type && type->getQualifier().patch,
                               type && type->getQualifier().hasComponent() ? static_cast<int>(type->getQualifier().layoutComponent)
                                                                           : 0});
    }
    // Members of "patch in" / "patch out" blocks and fields of patch struct
    // variables are per patch too (reflection gives a member its own type,
    // without the qualifier).
    {
        std::set<std::string> patchMembers;
        for (int st = EShLangTessControl; st <= EShLangTessEvaluation; ++st) {
            const glslang::TIntermediate* stage = program.getIntermediate(static_cast<EShLanguage>(st));
            glslang::TIntermAggregate* root = stage && stage->getTreeRoot() ? stage->getTreeRoot()->getAsAggregate() : nullptr;
            for (TIntermNode* node : root ? root->getSequence() : glslang::TIntermSequence()) {
                glslang::TIntermAggregate* objects = node->getAsAggregate();
                if (!objects || objects->getOp() != glslang::EOpLinkerObjects) continue;
                for (TIntermNode* object : objects->getSequence()) {
                    glslang::TIntermSymbol* symbol = object->getAsSymbolNode();
                    if (!symbol || !symbol->getQualifier().patch) continue;
                    // A patch struct variable's fields ("v.a").
                    if (symbol->getBasicType() != glslang::EbtBlock) {
                        patchMembers.insert(unescapeName(symbol->getName().c_str()));
                        continue;
                    }
                    const glslang::TType& type = symbol->getType();
                    const bool anonymous = glslang::IsAnonymous(symbol->getName());
                    for (const glslang::TTypeLoc& member : *type.getStruct()) {
                        const std::string field = unescapeName(member.type->getFieldName().c_str());
                        patchMembers.insert(unescapeName(type.getTypeName().c_str()) + "." + field);
                        if (anonymous) patchMembers.insert(field);
                    }
                }
            }
        }
        auto inPatchBlock = [&](const std::string& name) {
            for (size_t end = name.size(); end != std::string::npos && end > 0;) {
                if (patchMembers.count(name.substr(0, end))) return true;
                end = name.find_last_of(".[", end - 1);
            }
            return false;
        };
        for (std::vector<PipeVar>* vars : {&pipeInputs, &pipeOutputs})
            for (PipeVar& v : *vars) v.patch = v.patch || inPatchBlock(v.name);
    }
    for (const PipeVar& v : pipeInputs)
        if (v.location != kNoLocation && !(v.stages & EShLangVertexMask)) explicitVaryings.insert(v.name);
    for (const PipeVar& v : pipeOutputs)
        if (v.location != kNoLocation && !(v.stages & EShLangFragmentMask)) explicitVaryings.insert(v.name);
    // The GLSL I/O mapper matches varyings between stages by name (GL rules);
    // the default (Vulkan) mapper numbers each stage's interface in
    // declaration order, which swaps varyings declared in different orders.
    std::unique_ptr<glslang::TIoMapper> ioMapper(glslang::GetGlslIoMapper());
    std::unique_ptr<glslang::TIoMapResolver> resolver;
    for (int s = 0; s < kStageCount && !resolver; ++s)
        if (stagePresent[s]) resolver.reset(program.getGlslIoResolver(language(static_cast<Stage>(s))));
    if (!program.mapIO(resolver.get(), ioMapper.get())) {
        result.log += unescapeName(cleanLog(program.getInfoLog(), program.getInfoDebugLog()));
        return result;
    }

    // SPIR-V per stage.
    for (int s = 0; s < kStageCount; ++s) {
        if (!stagePresent[s]) continue;
        glslang::TIntermediate* intermediate = program.getIntermediate(language(static_cast<Stage>(s)));
        if (!intermediate) continue;
        glslang::SpvOptions options;
        options.disableOptimizer = true;
        options.validate = false;
        glslang::GlslangToSpv(*intermediate, result.stages[s].spirv, &options);
        result.stages[s].present = true;
        if (input.separable && !input.internal)
            hashVaryingLocations(result.stages[s].spirv, static_cast<Stage>(s), explicitVaryings);
    }
    // SPIRV-Cross GLSL of a linked stage (the emulations rebuild stages from it).
    auto stageGlsl = [&](Stage stage, std::string& glsl) {
        try {
            spirv_cross::CompilerGLSL glslOut(result.stages[static_cast<int>(stage)].spirv);
            // Renamed keywords (a sampler named `texture`) keep the program's names (unescapeName).
            glslOut.illegal_name_prefix = kEscapePrefix;
            spirv_cross::CompilerGLSL::Options options = glslOut.get_common_options();
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            // Row-major matrices load as in the geometry path (doubles need the plain form).
            options.enable_row_major_load_workaround = false;
            glslOut.set_common_options(options);
            glsl = glslOut.compile();
            return true;
        } catch (const std::exception& error) {
            result.log += std::string("error: shader translation failed: ") + error.what() + "\n";
            return false;
        }
    };
    // Separable programs without a vertex stage link without the emulation
    // (a program pipeline would have to assemble it from several programs).
    if (tessellation && tessellated && stagePresent[static_cast<int>(Stage::Vertex)]) {
        TessellationLink& t = *tessellation;
        std::string glsl;
        t.hasControl = result.stages[static_cast<int>(Stage::TessControl)].present;
        if (t.hasControl) {
            if (!stageGlsl(Stage::TessControl, glsl) || !parseTessellationGlsl(Stage::TessControl, glsl, t.control, result.log))
                return result;
        }
        if (!stageGlsl(Stage::TessEval, glsl) || !parseTessellationGlsl(Stage::TessEval, glsl, t.evaluation, result.log))
            return result;
        if (result.stages[static_cast<int>(Stage::Geometry)].present) {
            if (!stageGlsl(Stage::Geometry, t.geometryGlsl)) return result;
            // The geometry shader layout, for the program queries.
            GeometryShaderInfo info;
            if (!parseGeometryGlsl(t.geometryGlsl, info, result.log)) return result;
            result.geometryInputType = info.inputType;
            result.geometryOutputType = info.outputType;
            result.geometryVerticesOut = info.maxVertices;
        }
        // Inputs the vertex stage does not write (allowed when the control
        // or evaluation stage never reads them) are not captured.
        const std::vector<StageOutputVar> vertexOutputs = captureableOutputs(program.getIntermediate(EShLangVertex));
        std::vector<std::string> varyings;
        for (const std::string& v : tessellationCaptureVaryings(t.hasControl ? t.control : t.evaluation))
            for (const StageOutputVar& o : vertexOutputs)
                if (o.name == v) {
                    varyings.push_back(v);
                    break;
                }
        if (!layoutTransformFeedback(varyings, GL_INTERLEAVED_ATTRIBS, vertexOutputs, t.capture, result.log, UINT32_MAX))
            return result;
    } else if (geometry && result.stages[static_cast<int>(Stage::Geometry)].present) {
        std::string glsl;
        try {
            spirv_cross::CompilerGLSL glslOut(result.stages[static_cast<int>(Stage::Geometry)].spirv);
            // Renamed keywords (a sampler named `texture`) keep the program's names (unescapeName).
            glslOut.illegal_name_prefix = kEscapePrefix;
            spirv_cross::CompilerGLSL::Options options = glslOut.get_common_options();
            options.version = 450;
            options.es = false;
            options.vulkan_semantics = false;
            // The GLSL is compiled again by glslang, not by a GL driver: no
            // row-major load workaround (it has no double-matrix overloads).
            options.enable_row_major_load_workaround = false;
            glslOut.set_common_options(options);
            glsl = glslOut.compile();
        } catch (const std::exception& error) {
            result.log += std::string("error: geometry shader translation failed: ") + error.what() + "\n";
            return result;
        }
        if (!parseGeometryGlsl(glsl, *geometry, result.log)) return result;
        if (!stagePresent[static_cast<int>(Stage::Vertex)] && !input.separable) {
            result.log += "error: a geometry shader requires a vertex shader\n";
            return result;
        }
        // A separable geometry-only program links; its emulation needs the
        // vertex stage and is not built (program pipelines then report it).
        if (stagePresent[static_cast<int>(Stage::Vertex)]) {
            // Inputs the vertex stage does not write (allowed when the
            // geometry shader never reads them) are not captured.
            const std::vector<StageOutputVar> vertexOutputs = captureableOutputs(program.getIntermediate(EShLangVertex));
            matchGeometryInputs(*geometry, vertexOutputs);
            std::vector<std::string> varyings;
            for (const std::string& v : geometryCaptureVaryings(*geometry))
                for (const StageOutputVar& o : vertexOutputs)
                    if (o.name == v) {
                        varyings.push_back(v);
                        break;
                    }
            if (!layoutTransformFeedback(varyings, GL_INTERLEAVED_ATTRIBS, vertexOutputs, *geometryCapture, result.log, UINT32_MAX))
                return result;
        }
        result.geometryInputType = geometry->inputType;
        result.geometryOutputType = geometry->outputType;
        result.geometryVerticesOut = geometry->maxVertices;
        result.geometryInvocations = geometry->invocations;
    }

    // Program-wide interface, built from the first stage that declares each resource.
    // Default uniform block members.
    std::map<std::string, int> blockIndexByName;
    std::map<std::string, int> samplerUniformByName, imageUniformByName;
    // A sampler variable's element within its uniform (struct sampler arrays, above).
    std::map<std::string, int> samplerElement;
    std::map<std::string, int> samplerUniformCount;  // uniforms per sampler variable (arrays of arrays: several)
    std::map<std::string, int> imageUniformCount;    // likewise per image variable
    for (int s = 0; s < kStageCount; ++s) {
        if (!result.stages[s].present) continue;
        spirv_cross::Compiler compiler(result.stages[s].spirv);
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for (const auto& ubo : resources.uniform_buffers) {
            const spirv_cross::SPIRType& type = compiler.get_type(ubo.base_type_id);
            const std::string blockName = unescapeName(compiler.get_name(ubo.base_type_id));
            const uint32_t binding = compiler.get_decoration(ubo.id, spv::DecorationBinding);
            if (binding == kDefaultBlockBinding || blockName == kDefaultBlockName) {
                result.defaultBlockSize = std::max<uint32_t>(result.defaultBlockSize,
                                                             static_cast<uint32_t>(compiler.get_declared_struct_size(type)));
                // The default block is shared by every stage: a uniform is
                // referenced by the stages whose code reads its member.
                bool all;
                const std::set<uint32_t> accessed = accessedMembers(compiler, ubo.id, all);
                for (uint32_t m = 0; m < type.member_types.size(); ++m) {
                    std::vector<UniformInfo> members;
                    expandMember(compiler, type, m, "", 0, -1, members);
                    const uint32_t stageBit = all || accessed.count(m) ? 1u << s : 0u;
                    for (UniformInfo& u : members) {
                        if (!liveDefault.count(topLevelName(u.name))) continue;
                        if (auto b = boolTypes.find(arrayBaseName(u.name)); b != boolTypes.end()) u.type = b->second;
                        auto it = std::find_if(result.uniforms.begin(), result.uniforms.end(),
                                               [&](const UniformInfo& e) { return e.name == u.name && e.blockIndex < 0; });
                        if (it == result.uniforms.end()) {
                            u.stageMask = stageBit;
                            result.uniforms.push_back(u);
                        } else {
                            it->stageMask |= stageBit;
                        }
                    }
                }
                continue;
            }
            if (!liveBlocks.count(blockName)) continue;
            // The variable's type carries the instance-array dimension, not the block struct.
            const spirv_cross::SPIRType& varType = compiler.get_type(ubo.type_id);
            const uint32_t count = varType.array.empty() ? 1 : std::max(1u, varType.array.back());
            for (uint32_t element = 0; element < count; ++element) {
                const std::string name = varType.array.empty() ? blockName : blockName + "[" + std::to_string(element) + "]";
                auto it = blockIndexByName.find(name);
                int index;
                if (it == blockIndexByName.end()) {
                    index = static_cast<int>(result.blocks.size());
                    blockIndexByName[name] = index;
                    UniformBlockInfo block;
                    block.name = name;
                    block.dataSize = static_cast<GLint>(compiler.get_declared_struct_size(compiler.get_type(ubo.base_type_id)));
                    // An array of blocks binds consecutive points from its layout(binding).
                    if (auto b = explicitBindings.find("block:" + blockName); b != explicitBindings.end())
                        block.explicitBinding = b->second + static_cast<GLint>(element);
                    result.blocks.push_back(block);
                    std::vector<UniformInfo> members;
                    const spirv_cross::SPIRType& structType = compiler.get_type(ubo.base_type_id);
                    // Members of a block without an instance name are named without the block prefix.
                    const std::string prefix = compiler.get_name(ubo.id).empty() ? "" : blockName + ".";
                    for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                        expandMember(compiler, structType, m, prefix, 0, index, members);
                    for (UniformInfo& u : members) {
                        if (auto b = boolTypes.find(arrayBaseName(u.name)); b != boolTypes.end()) u.type = b->second;
                        result.blocks[index].activeUniforms.push_back(static_cast<int>(result.uniforms.size()));
                        result.uniforms.push_back(u);
                    }
                } else {
                    index = it->second;
                }
                // Referenced by this stage only if its code reads the block
                // (for an array of blocks, this element).
                bool all;
                if (accessedMembers(compiler, ubo.id, all).empty() && !all) continue;
                if (!varType.array.empty() && !accessedElements(result.stages[s].spirv, ubo.id, count)[element]) continue;
                result.blocks[index].stageMask |= 1u << s;
                for (int u : result.blocks[index].activeUniforms) result.uniforms[u].stageMask |= 1u << s;
            }
        }
        // Struct sampler arrays moved out per element ("s.a[2]", below): how
        // many elements each has in this stage (its highest element + 1).
        std::map<std::string, int> elementCounts;
        for (const auto& image : resources.sampled_images) {
            const std::string n = unescapeName(compiler.get_name(image.id));
            if (!compiler.get_type(image.type_id).array.empty() || n.size() < 4 || n.back() != ']') continue;
            const size_t open = n.rfind('[');
            const std::string digits = n.substr(open + 1, n.size() - open - 2);
            if (digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) continue;
            int& count = elementCounts[n.substr(0, open) + "[0]"];
            count = std::max(count, std::stoi(digits) + 1);
        }
        for (const auto& image : resources.sampled_images) {
            const spirv_cross::SPIRType& type = compiler.get_type(image.type_id);
            const spirv_cross::SPIRType& base = compiler.get_type(image.base_type_id);
            const std::string name = unescapeName(compiler.get_name(image.id));
            if (!liveSamplers.count(topLevelName(name))) continue;
            const spirv_cross::SPIRType& sampled = compiler.get_type(base.image.type);
            const GLenum glType = samplerTypeWithComponent(glTypeOf(base), sampled.basetype);
            // An array of arrays is a uniform per innermost array ("s[i][0]",
            // GL 4.6 section 7.3.1.1), its elements in consecutive slots.
            std::vector<uint32_t> dims(type.array.begin(), type.array.end());
            std::reverse(dims.begin(), dims.end());  // outermost first
            for (uint32_t& d : dims) d = std::max(1u, d);
            const uint32_t inner = dims.empty() ? 1 : dims.back();
            uint32_t outer = 1;
            for (size_t d = 0; d + 1 < dims.size(); ++d) outer *= dims[d];
            auto it = samplerUniformByName.find(name);
            // glslang moves a sampler array out of a struct as a variable
            // per element ("s.a[2]"): the elements make one uniform "s.a[0]"
            // of the array's size up to the last active element, each
            // element at its offset from the first's slot.
            if (it == samplerUniformByName.end() && type.array.empty() && name.size() > 3 && name.back() == ']') {
                const size_t open = name.rfind('[');
                const std::string digits = name.substr(open + 1, name.size() - open - 2);
                const bool number = !digits.empty() && digits.size() < 5 && digits.find_first_not_of("0123456789") == std::string::npos;
                if (number) {
                    const int element = std::stoi(digits);
                    const std::string headName = name.substr(0, open) + "[0]";
                    auto head = samplerUniformByName.find(headName);
                    auto addSlots = [&](int n) {
                        for (int e = 0; e < n; ++e) {
                            SamplerSlotInfo slot;
                            slot.type = glType;
                            slot.textureTarget = textureTargetForSampler(glType);
                            slot.shadow = isShadowSampler(glType);
                            result.samplers.push_back(slot);
                        }
                    };
                    if (head == samplerUniformByName.end()) {
                        // The first element seen: the uniform takes every
                        // element's slot (earlier ones even if unused).
                        const int count = std::max(element + 1, elementCounts[headName]);
                        UniformInfo u;
                        u.name = headName;
                        u.type = glType;
                        u.size = count;
                        u.samplerSlot = static_cast<int>(result.samplers.size());
                        u.stageMask = 1u << s;
                        addSlots(count);
                        samplerUniformByName[headName] = static_cast<int>(result.uniforms.size());
                        samplerUniformCount[headName] = 1;
                        samplerElement[headName] = 0;
                        samplerUniformByName[name] = static_cast<int>(result.uniforms.size());
                        samplerUniformCount[name] = 1;
                        samplerElement[name] = element;
                        result.uniforms.push_back(u);
                        continue;
                    }
                    if (head != samplerUniformByName.end() && samplerUniformCount[head->first] == 1 && element > 0) {
                        UniformInfo& u = result.uniforms[static_cast<size_t>(head->second)];
                        const bool last = u.samplerSlot + u.size == static_cast<int>(result.samplers.size());
                        if (u.type == glType && (element < u.size || last)) {
                            if (element >= u.size) {
                                addSlots(element + 1 - u.size);
                                u.size = element + 1;
                            }
                            u.stageMask |= 1u << s;
                            samplerUniformByName[name] = head->second;
                            samplerUniformCount[name] = 1;
                            samplerElement[name] = element;
                            continue;
                        }
                    }
                }
            }
            if (it == samplerUniformByName.end()) {
                samplerUniformByName[name] = static_cast<int>(result.uniforms.size());
                samplerUniformCount[name] = static_cast<int>(outer);
                for (uint32_t k = 0; k < outer; ++k) {
                    UniformInfo u;
                    u.name = name;
                    for (size_t d = 0, rest = k; d + 1 < dims.size(); ++d) {
                        uint32_t span = 1;
                        for (size_t e = d + 1; e + 1 < dims.size(); ++e) span *= dims[e];
                        u.name += "[" + std::to_string(rest / span) + "]";
                        rest %= span;
                    }
                    if (!type.array.empty()) u.name += "[0]";
                    u.type = glType;
                    u.size = static_cast<GLint>(inner);
                    u.samplerSlot = static_cast<int>(result.samplers.size());
                    u.stageMask = 1u << s;
                    for (uint32_t e = 0; e < inner; ++e) {
                        SamplerSlotInfo slot;
                        slot.type = glType;
                        slot.textureTarget = textureTargetForSampler(glType);
                        slot.shadow = isShadowSampler(glType);
                        result.samplers.push_back(slot);
                    }
                    if (auto b = explicitBindings.find(name); b != explicitBindings.end())
                        u.explicitBinding = b->second + static_cast<int>(k * inner);
                    result.uniforms.push_back(u);
                }
            } else {
                for (int k = 0; k < samplerUniformCount[name]; ++k) result.uniforms[it->second + k].stageMask |= 1u << s;
            }
        }
        // Images (GL 4.2 image load/store): uniforms whose values are image units.
        for (const auto& image : resources.storage_images) {
            const spirv_cross::SPIRType& type = compiler.get_type(image.type_id);
            const spirv_cross::SPIRType& base = compiler.get_type(image.base_type_id);
            const std::string name = unescapeName(compiler.get_name(image.id));
            if (!liveSamplers.count(topLevelName(name))) continue;
            const GLenum glType = imageTypeOf(base, compiler.get_type(base.image.type).basetype);
            auto it = imageUniformByName.find(name);
            if (it != imageUniformByName.end()) {
                for (int k = 0; k < imageUniformCount[name]; ++k) result.uniforms[it->second + k].stageMask |= 1u << s;
                continue;
            }
            // An array of arrays is a uniform per innermost array, as for
            // samplers; its elements take consecutive slots (flattened as the
            // MSL side sees them, flattenArraysOfArrays).
            std::vector<uint32_t> dims(type.array.begin(), type.array.end());
            std::reverse(dims.begin(), dims.end());  // outermost first
            for (uint32_t& d : dims) d = std::max(1u, d);
            const uint32_t inner = dims.empty() ? 1 : dims.back();
            uint32_t outer = 1;
            for (size_t d = 0; d + 1 < dims.size(); ++d) outer *= dims[d];
            imageUniformByName[name] = static_cast<int>(result.uniforms.size());
            imageUniformCount[name] = static_cast<int>(outer);
            for (uint32_t k = 0; k < outer; ++k) {
                UniformInfo u;
                u.name = name;
                for (size_t d = 0, rest = k; d + 1 < dims.size(); ++d) {
                    uint32_t span = 1;
                    for (size_t e = d + 1; e + 1 < dims.size(); ++e) span *= dims[e];
                    u.name += "[" + std::to_string(rest / span) + "]";
                    rest %= span;
                }
                if (!type.array.empty()) u.name += "[0]";
                u.type = glType;
                u.size = static_cast<GLint>(inner);
                u.imageSlot = static_cast<int>(result.images.size());
                u.stageMask = 1u << s;
                if (auto b = explicitBindings.find(name); b != explicitBindings.end())
                    u.explicitBinding = b->second + static_cast<int>(k * inner);
                for (uint32_t e = 0; e < inner; ++e) {
                    ImageSlotInfo slot;
                    slot.type = glType;
                    slot.format = imageFormatOf(base.image.format);
                    result.images.push_back(slot);
                }
                result.uniforms.push_back(u);
            }
        }
        // Atomic counters (GL 4.2): the storage blocks glslang made of them.
        uint32_t stageCounters = 0;
        for (const auto& ssbo : resources.storage_buffers) {
            const int binding = atomicCounterBlockBinding(compiler.get_name(ssbo.base_type_id));
            if (binding < 0) continue;
            // Only the counters the stage's code uses are active in it.
            bool all = false;
            const std::set<uint32_t> accessed = accessedMembers(compiler, ssbo.id, all);
            const spirv_cross::SPIRType& blockType = compiler.get_type(ssbo.base_type_id);
            for (uint32_t m = 0; m < blockType.member_types.size(); ++m) {
                const spirv_cross::SPIRType& memberType = compiler.get_type(blockType.member_types[m]);
                uint32_t elements = 1;
                for (uint32_t d : memberType.array) elements *= std::max(1u, d);
                stageCounters += elements;
            }
            if (accessed.empty() && !all) continue;
            auto buffer = std::find_if(result.atomicBuffers.begin(), result.atomicBuffers.end(),
                                       [&](const AtomicCounterBufferInfo& b) { return b.binding == static_cast<GLuint>(binding); });
            if (buffer == result.atomicBuffers.end()) {
                AtomicCounterBufferInfo info;
                info.binding = static_cast<GLuint>(binding);
                result.atomicBuffers.push_back(info);
                buffer = result.atomicBuffers.end() - 1;
            }
            const int bufferIndex = static_cast<int>(buffer - result.atomicBuffers.begin());
            buffer->stageMask |= 1u << s;
            const spirv_cross::SPIRType& type = compiler.get_type(ssbo.base_type_id);
            for (uint32_t m = 0; m < type.member_types.size(); ++m) {
                const spirv_cross::SPIRType& memberType = compiler.get_type(type.member_types[m]);
                const std::string name = unescapeName(compiler.get_member_name(type.self, m));
                const uint32_t offset = compiler.type_struct_member_offset(type, m);
                // Every element of an array of arrays is a counter; a uniform
                // per innermost array ("c[1][0]", GL 4.6 section 7.3.1.1).
                std::vector<uint32_t> dims(memberType.array.begin(), memberType.array.end());
                std::reverse(dims.begin(), dims.end());  // outermost first
                for (uint32_t& d : dims) d = std::max(1u, d);
                const uint32_t inner = dims.empty() ? 1 : dims.back();
                uint32_t outer = 1;
                for (size_t d = 0; d + 1 < dims.size(); ++d) outer *= dims[d];
                buffer->dataSize = std::max<GLint>(buffer->dataSize, static_cast<GLint>(offset + 4 * outer * inner));
                if (!all && !accessed.count(m)) continue;
                for (uint32_t k = 0; k < outer; ++k) {
                    std::string glName = name;
                    for (size_t d = 0, rest = k; d + 1 < dims.size(); ++d) {
                        uint32_t span = 1;
                        for (size_t e = d + 1; e + 1 < dims.size(); ++e) span *= dims[e];
                        glName += "[" + std::to_string(rest / span) + "]";
                        rest %= span;
                    }
                    if (!dims.empty()) glName += "[0]";
                    auto it = std::find_if(result.uniforms.begin(), result.uniforms.end(),
                                           [&](const UniformInfo& u) { return u.name == glName && u.atomicBufferIndex >= 0; });
                    if (it != result.uniforms.end()) {
                        it->stageMask |= 1u << s;
                        continue;
                    }
                    UniformInfo u;
                    u.name = glName;
                    u.type = GL_UNSIGNED_INT_ATOMIC_COUNTER;
                    u.size = static_cast<GLint>(inner);
                    u.offset = static_cast<GLint>(offset + 4 * k * inner);
                    u.arrayStride = dims.empty() ? 0 : 4;
                    u.atomicBufferIndex = bufferIndex;
                    u.stageMask = 1u << s;
                    buffer->activeUniforms.push_back(static_cast<int>(result.uniforms.size()));
                    result.uniforms.push_back(u);
                }
            }
        }
        // GL_MAX_<stage>_ATOMIC_COUNTERS (KHR-GL44.geometry_shader.linking.more_ACs_in_GS_than_supported).
        if (stageCounters > static_cast<uint32_t>(wmgl::limits::kMaxStageAtomicCounters)) {
            result.log += "error: too many atomic counters in one stage\n";
            return result;
        }
        // Shader storage blocks (GL 4.3).
        for (const auto& ssbo : resources.storage_buffers) {
            const std::string blockName = unescapeName(compiler.get_name(ssbo.base_type_id));
            if (!isUserStorageBlock(blockName) || !liveBuffers.count(blockName)) continue;
            const spirv_cross::SPIRType& varType = compiler.get_type(ssbo.type_id);
            const spirv_cross::SPIRType& structType = compiler.get_type(ssbo.base_type_id);
            const uint32_t count = varType.array.empty() ? 1 : std::max(1u, varType.array.back());
            // Referenced by this stage only if its code accesses the block.
            bool all;
            const uint32_t stageBit = !accessedMembers(compiler, ssbo.id, all).empty() || all ? 1u << s : 0u;
            const std::vector<bool> elements = accessedElements(result.stages[s].spirv, ssbo.id, count);
            int first = -1;
            for (uint32_t element = 0; element < count; ++element) {
                const std::string name = varType.array.empty() ? blockName : blockName + "[" + std::to_string(element) + "]";
                const uint32_t elementBit = varType.array.empty() || elements[element] ? stageBit : 0u;
                auto it = std::find_if(result.storageBlocks.begin(), result.storageBlocks.end(),
                                       [&](const StorageBlockInfo& b) { return b.name == name; });
                if (it != result.storageBlocks.end()) {
                    it->stageMask |= elementBit;
                    if (element == 0) first = static_cast<int>(it - result.storageBlocks.begin());
                    continue;
                }
                StorageBlockInfo block;
                block.name = name;
                block.dataSize = static_cast<GLint>(compiler.get_declared_struct_size_runtime_array(structType, 1));
                if (auto b = explicitBindings.find("buffer:" + blockName); b != explicitBindings.end())
                    block.explicitBinding = b->second + static_cast<GLint>(element);
                block.stageMask = elementBit;
                if (element == 0) first = static_cast<int>(result.storageBlocks.size());
                result.storageBlocks.push_back(block);
            }
            // The block's variables, listed once (for its first element).
            if (std::none_of(result.bufferVariables.begin(), result.bufferVariables.end(),
                             [&](const BufferVariableInfo& v) { return v.blockIndex == first; })) {
                const std::string prefix = compiler.get_name(ssbo.id).empty() ? "" : blockName + ".";
                std::vector<BufferVariableInfo> vars;
                for (uint32_t m = 0; m < structType.member_types.size(); ++m)
                    expandBufferMember(compiler, structType, m, prefix, 0, first, true, 1, 0, vars);
                for (BufferVariableInfo& v : vars) {
                    if (auto b = boolTypes.find(arrayBaseName(v.name)); b != boolTypes.end()) v.type = b->second;
                    result.bufferVariables.push_back(v);
                }
            }
            for (BufferVariableInfo& v : result.bufferVariables)
                if (v.blockIndex == first) v.stageMask |= stageBit;
        }
    }
    for (size_t v = 0; v < result.bufferVariables.size(); ++v) {
        const std::string& owner = result.storageBlocks[static_cast<size_t>(result.bufferVariables[v].blockIndex)].name;
        const std::string base = owner.substr(0, owner.find('['));
        for (StorageBlockInfo& block : result.storageBlocks)
            if (block.name.substr(0, block.name.find('[')) == base) block.activeVariables.push_back(static_cast<int>(v));
    }

    // Shader subroutines (shader/subroutines.h): each stage's functions and
    // subroutine uniforms, whose selections live in hidden default-block
    // uniforms that are not part of the GL interface.
    for (const ShaderSource& shader : input.shaders) {
        const SubroutineLowering lowered = lowerSubroutines(shader.stage, spliceLineContinuations(shader.source));
        SubroutineStageInfo& info = result.subroutines[static_cast<int>(shader.stage)];
        for (const std::string& type : lowered.info.types)
            if (std::find(info.types.begin(), info.types.end(), type) == info.types.end()) info.types.push_back(type);
        for (const auto& f : lowered.info.functions)
            if (std::none_of(info.functions.begin(), info.functions.end(), [&](const auto& g) { return g.name == f.name; }))
                info.functions.push_back(f);
        for (const auto& u : lowered.info.uniforms)
            if (std::none_of(info.uniforms.begin(), info.uniforms.end(), [&](const auto& v) { return v.name == u.name; }))
                info.uniforms.push_back(u);
    }
    {
        std::vector<UniformInfo> kept;
        std::vector<int> map(result.uniforms.size(), -1);
        for (size_t i = 0; i < result.uniforms.size(); ++i) {
            const UniformInfo& u = result.uniforms[i];
            if (u.blockIndex < 0 && u.name.rfind(kBuiltinUniformPrefix, 0) == 0) {
                // Hidden built-in uniforms (lowerBuiltinUniforms).
                const std::string name = u.name.substr(std::string(kBuiltinUniformPrefix).size());
                if (name == "DepthRange.near") result.builtinUniforms.depthRange[0] = static_cast<int>(u.offset);
                if (name == "DepthRange.far") result.builtinUniforms.depthRange[1] = static_cast<int>(u.offset);
                if (name == "DepthRange.diff") result.builtinUniforms.depthRange[2] = static_cast<int>(u.offset);
                if (name == "NumSamples") result.builtinUniforms.numSamples = static_cast<int>(u.offset);
                if (name == "BaseVertex") result.builtinUniforms.baseVertex = static_cast<int>(u.offset);
                if (name == "BaseInstance") result.builtinUniforms.baseInstance = static_cast<int>(u.offset);
                if (name == "DrawID") result.builtinUniforms.drawId = static_cast<int>(u.offset);
                continue;
            }
            if (u.blockIndex < 0 && isSubroutineSelectionUniform(u.name)) {
                for (int st = 0; st < kStageCount; ++st)
                    for (auto& su : result.subroutines[st].uniforms)
                        if (arrayBaseName(u.name) == subroutineSelectionUniform(static_cast<Stage>(st), su.name)) {
                            // An array of arrays reflects one uniform per outer
                            // element: the selections start at the first.
                            if (su.selectionOffset < 0 || static_cast<int>(u.offset) < su.selectionOffset)
                                su.selectionOffset = static_cast<int>(u.offset);
                            su.selectionStride = u.arrayStride ? u.arrayStride : 16;
                        }
                continue;
            }
            map[i] = static_cast<int>(kept.size());
            kept.push_back(u);
        }
        if (kept.size() != result.uniforms.size()) {
            for (UniformBlockInfo& b : result.blocks)
                for (int& u : b.activeUniforms) u = map[static_cast<size_t>(u)];
            for (AtomicCounterBufferInfo& b : result.atomicBuffers)
                for (int& u : b.activeUniforms) u = map[static_cast<size_t>(u)];
            for (auto& [name, index] : samplerUniformByName) index = map[static_cast<size_t>(index)];
            for (auto& [name, index] : imageUniformByName) index = map[static_cast<size_t>(index)];
            result.uniforms = std::move(kept);
        }
    }
    // GL_MAX_COMBINED_IMAGE_UNIFORMS: an image uniform counts once per stage that uses it.
    {
        uint64_t combinedImages = 0;
        for (const UniformInfo& u : result.uniforms)
            if (u.imageSlot >= 0) combinedImages += static_cast<uint64_t>(u.size) * static_cast<uint64_t>(__builtin_popcount(u.stageMask));
        if (combinedImages > static_cast<uint64_t>(wmgl::limits::kMaxCombinedImageUniforms)) {
            result.log += "error: too many image uniforms in the program (GL_MAX_COMBINED_IMAGE_UNIFORMS)\n";
            return result;
        }
    }
    for (SubroutineStageInfo& info : result.subroutines) {
        // Only uniforms the stage uses are active; their locations: explicit
        // ones first, then the lowest free ones.
        info.uniforms.erase(std::remove_if(info.uniforms.begin(), info.uniforms.end(),
                                           [](const SubroutineStageInfo::Uniform& u) { return u.selectionOffset < 0; }),
                            info.uniforms.end());
        std::set<GLint> usedLocations;
        for (const auto& u : info.uniforms)
            for (int e = 0; u.location >= 0 && e < u.elements(); ++e) usedLocations.insert(u.location + e);
        for (auto& u : info.uniforms) {
            if (u.location >= 0) continue;
            GLint location = 0;
            for (;; ++location) {
                bool free = true;
                for (int e = 0; e < u.elements(); ++e) free = free && !usedLocations.count(location + e);
                if (free) break;
            }
            u.location = location;
            for (int e = 0; e < u.elements(); ++e) usedLocations.insert(location + e);
        }
        info.locationCount = usedLocations.empty() ? 0 : *usedLocations.rbegin() + 1;
    }

    // Assign default-block and sampler uniform locations (consecutive per
    // element): layout(location) ones first (GL 4.3; the members of an
    // aggregate follow its location), then the lowest free ones. Explicit
    // locations of uniforms the program does not use stay reserved.
    {
        std::map<std::string, std::pair<int, int>> explicitLocations;  // name -> (location, count)
        for (int s = 0; s < kStageCount; ++s) {
            const glslang::TIntermediate* intermediate =
                stagePresent[s] ? program.getIntermediate(language(static_cast<Stage>(s))) : nullptr;
            if (!intermediate) continue;
            for (const auto& e : intermediate->getExplicitUniformLocations()) {
                const std::string name = unescapeName(e.name);
                if (e.location < 0 || e.location + e.count > limits::kMaxUniformLocations) {
                    result.log += "error: uniform '" + name + "' has a location beyond GL_MAX_UNIFORM_LOCATIONS\n";
                    return result;
                }
                auto [it, inserted] = explicitLocations.emplace(name, std::make_pair(e.location, e.count));
                if (!inserted && it->second.first != e.location) {
                    result.log += "error: uniform '" + name + "' is declared with different locations\n";
                    return result;
                }
            }
        }
        std::vector<std::string> owner;  // per location: the explicit uniform reserving it
        for (const auto& [name, range] : explicitLocations) {
            if (owner.size() < static_cast<size_t>(range.first + range.second)) owner.resize(static_cast<size_t>(range.first + range.second));
            for (int l = range.first; l < range.first + range.second; ++l) {
                if (!owner[static_cast<size_t>(l)].empty()) {
                    result.log += "error: uniforms '" + owner[static_cast<size_t>(l)] + "' and '" + name + "' share location " +
                                  std::to_string(l) + "\n";
                    return result;
                }
                owner[static_cast<size_t>(l)] = name;
            }
        }
        std::vector<std::pair<int, int>>& table = result.locations;
        auto taken = [&](GLint location) {
            return (static_cast<size_t>(location) < table.size() && table[static_cast<size_t>(location)].first >= 0) ||
                   (static_cast<size_t>(location) < owner.size() && !owner[static_cast<size_t>(location)].empty());
        };
        auto place = [&](size_t i, GLint location) {
            UniformInfo& u = result.uniforms[i];
            u.location = location;
            if (table.size() < static_cast<size_t>(location + u.size)) table.resize(static_cast<size_t>(location + u.size), {-1, 0});
            for (GLint e = 0; e < u.size; ++e) table[static_cast<size_t>(location + e)] = {static_cast<int>(i), e};
        };
        std::map<std::string, GLint> nextOfAggregate;
        for (size_t i = 0; i < result.uniforms.size(); ++i) {
            UniformInfo& u = result.uniforms[i];
            if (u.blockIndex >= 0 || u.atomicBufferIndex >= 0) continue;  // atomic counters have no location
            auto it = explicitLocations.find(topLevelName(u.name));
            if (it == explicitLocations.end()) continue;
            GLint& next = nextOfAggregate.emplace(it->first, it->second.first).first->second;
            place(i, next);
            next += u.size;
        }
        for (size_t i = 0; i < result.uniforms.size(); ++i) {
            UniformInfo& u = result.uniforms[i];
            if (u.blockIndex >= 0 || u.atomicBufferIndex >= 0 || explicitLocations.count(topLevelName(u.name))) continue;
            GLint location = 0;
            for (;; ++location) {
                bool free = true;
                for (GLint e = 0; e < u.size && free; ++e) free = !taken(location + e);
                if (free) break;
            }
            if (location + u.size > limits::kMaxUniformLocations) {
                result.log += "error: too many uniform locations (GL_MAX_UNIFORM_LOCATIONS)\n";
                return result;
            }
            place(i, location);
        }
    }

    // Vertex attributes (from reflection: active inputs only).
    std::vector<bool> usedLocations(64, false);
    for (const PipeVar& in : pipeInputs) {
        if (in.name.rfind("gl_", 0) == 0) continue;
        if (!(in.stages & EShLangVertexMask)) continue;
        AttribInfo a;
        a.name = in.name;
        a.type = in.type;
        a.size = std::max(1, in.size);
        a.array = in.array;
        a.locationCount = glTypeLocations(a.type) * a.size;
        a.location = in.location != kNoLocation ? static_cast<GLint>(in.location) : -1;
        a.component = in.component;
        // Lowered legacy attributes (gl_Vertex, ...) read the conventional arrays.
        const int conventional = conventionalAttribLocation(a.name);
        if (conventional >= 0) {
            a.location = conventional;
            a.conventional = true;
        }
        result.attribs.push_back(a);
    }
    // Explicit locations from source were already applied and take
    // precedence over glBindAttribLocation; mark them used. Then
    // glBindAttribLocation, then the lowest free locations.
    std::vector<int> needsLocation;
    for (size_t i = 0; i < result.attribs.size(); ++i) {
        AttribInfo& a = result.attribs[i];
        if (a.conventional) continue;
        if (a.location < 0) {
            auto bound = input.attribBindings.find(a.name);
            if (bound == input.attribBindings.end()) {
                needsLocation.push_back(static_cast<int>(i));
                continue;
            }
            a.location = static_cast<GLint>(bound->second);
        }
        for (int l = 0; l < a.locationCount && a.location + l < 64; ++l) usedLocations[a.location + l] = true;
    }
    for (int i : needsLocation) {
        AttribInfo& a = result.attribs[i];
        int location = 0;
        for (;; ++location) {
            bool free = true;
            for (int l = 0; l < a.locationCount; ++l)
                if (location + l >= 64 || usedLocations[location + l]) free = false;
            if (free || location >= 64) break;
        }
        a.location = location;
        for (int l = 0; l < a.locationCount && location + l < 64; ++l) usedLocations[location + l] = true;
    }
    // Attributes packed by component or aliased share their locations; a
    // location holding a dvec3 or dvec4 column counts twice.
    int slotWeight[64] = {};
    for (const AttribInfo& a : result.attribs) {
        if (!a.conventional && a.location + a.locationCount > 16) {
            result.log += "error: vertex attribute '" + a.name + "' uses a location beyond GL_MAX_VERTEX_ATTRIBS\n";
            return result;
        }
        if (a.conventional || a.location < 0) continue;
        const int perElement = std::max(1, a.locationCount / std::max(1, a.size));
        const int weight = std::max(1, glTypeAttribSlots(a.type) / perElement);
        for (int l = 0; l < a.locationCount && a.location + l < 64; ++l)
            slotWeight[a.location + l] = std::max(slotWeight[a.location + l], weight);
    }
    int attribSlots = 0;
    for (int w : slotWeight) attribSlots += w;
    // dvec3 and dvec4 columns count twice (GL_ARB_vertex_attrib_64bit).
    if (attribSlots > 16) {
        result.log += "error: vertex attributes exceed GL_MAX_VERTEX_ATTRIBS\n";
        return result;
    }

    // Fragment outputs.
    for (const PipeVar& out : pipeOutputs) {
        if (out.name.rfind("gl_", 0) == 0) continue;
        if (!(out.stages & EShLangFragmentMask)) continue;
        FragOutputInfo f;
        f.name = out.name;
        f.type = out.type;
        f.size = std::max(1, out.size);
        const unsigned explicitLocation = out.location;
        auto bound = input.fragDataBindings.find(f.name);
        if (explicitLocation != kNoLocation) {
            f.location = static_cast<GLint>(explicitLocation);
            f.index = out.index;  // layout(index = N) (GL_ARB_blend_func_extended)
        } else if (bound != input.fragDataBindings.end()) {
            f.location = static_cast<GLint>(bound->second.first);
            f.index = static_cast<GLint>(bound->second.second);
        } else {
            f.location = -1;
        }
        result.fragOutputs.push_back(f);
    }
    {
        std::vector<bool> used(16, false);
        for (const FragOutputInfo& f : result.fragOutputs)
            if (f.location >= 0)
                for (int e = 0; e < f.size && f.location + e < 16; ++e) used[f.location + e] = true;
        for (FragOutputInfo& f : result.fragOutputs) {
            if (f.location >= 0) continue;
            int location = 0;
            while (location < 16 && used[location]) ++location;
            f.location = location;
            for (int e = 0; e < f.size && location + e < 16; ++e) used[location + e] = true;
        }
    }

    // The program interface (GL 4.3 GL_PROGRAM_INPUT / GL_PROGRAM_OUTPUT):
    // the first stage's inputs and the last stage's outputs, with the
    // locations assigned above for vertex inputs and fragment outputs.
    auto interfaceVariable = [](const PipeVar& v) {
        InterfaceVariableInfo info;
        // An array, block members' too, is named with "[0]" (GL 4.3 section
        // 7.3.1.1: "TargetInterface.target[0]"); built-in block members keep their names.
        const bool builtinMember = v.name.rfind("gl_", 0) == 0 && v.name.find('.') != std::string::npos;
        info.name = v.array && !builtinMember && v.name.back() != ']' ? v.name + "[0]" : v.name;
        // The Vulkan rules rename these built-ins; GL reports its own names.
        if (info.name == "gl_VertexIndex") info.name = "gl_VertexID";
        if (info.name == "gl_InstanceIndex") info.name = "gl_InstanceID";
        info.type = v.type;
        info.size = std::max(1, v.size);
        info.location = v.location != kNoLocation && v.name.rfind("gl_", 0) != 0 ? static_cast<GLint>(v.location) : -1;
        info.perPatch = v.patch;
        info.component = v.component;
        info.stageMask = v.stages;
        return info;
    };
    // Members of the built-in block that the program never declared or
    // wrote come back without a type; they are not active. Nor are built-ins
    // that only WineMetalGL's own lowering writes (gl_PointSize).
    auto userBuiltin = [&](const PipeVar& v) {
        if (v.name.rfind("gl_", 0) != 0) return true;
        std::string name = v.name.substr(v.name.rfind('.') == std::string::npos ? 0 : v.name.rfind('.') + 1);  // gl_PerVertex.gl_Position
        name = name.substr(0, name.find('['));
        if (name == "gl_VertexIndex") name = "gl_VertexID";
        if (name == "gl_InstanceIndex") name = "gl_InstanceID";
        return std::any_of(input.shaders.begin(), input.shaders.end(),
                           [&](const ShaderSource& sh) { return sh.source.find(name) != std::string::npos; });
    };
    for (const PipeVar& v : pipeInputs) {
        if (!v.type || !userBuiltin(v)) continue;
        InterfaceVariableInfo info = interfaceVariable(v);
        for (const AttribInfo& a : result.attribs)
            if (a.name == v.name) {
                info.location = a.conventional ? -1 : a.location;
                if (a.conventional) info.name = "gl_" + a.name.substr(5);  // wmgl_Vertex -> gl_Vertex
            }
        result.programInputs.push_back(info);
    }
    for (const PipeVar& v : pipeOutputs) {
        if (!v.type || !userBuiltin(v)) continue;
        InterfaceVariableInfo info = interfaceVariable(v);
        for (const FragOutputInfo& f : result.fragOutputs)
            if (f.name == v.name) {
                info.location = f.location;
                info.locationIndex = f.index;
            }
        result.programOutputs.push_back(info);
    }

    // Whether the provoking vertex can change the fragment stage's flat
    // inputs (DrawState::rotateProvoking), when the vertex stage feeds it.
    if (result.stages[static_cast<int>(Stage::Vertex)].present && result.stages[static_cast<int>(Stage::Fragment)].present &&
        !result.stages[static_cast<int>(Stage::Geometry)].present &&
        !result.stages[static_cast<int>(Stage::TessControl)].present &&
        !result.stages[static_cast<int>(Stage::TessEval)].present) {
        const FlatSources flat = flatVaryingSources(
            result.stages[static_cast<int>(Stage::Vertex)].spirv, result.stages[static_cast<int>(Stage::Fragment)].spirv,
            [&](const std::string& name, uint32_t& locations) {
                const std::string glName = unescapeName(name);
                for (const AttribInfo& a : result.attribs) {
                    if (a.name != glName) continue;
                    if (a.location < 0 || a.location + a.locationCount > 32) return false;
                    for (int l = 0; l < a.locationCount; ++l) locations |= 1u << (a.location + l);
                    return true;
                }
                return false;
            });
        result.flatPerVertex = flat.perVertex;
        result.flatAttribs = flat.attribs;
    }

    // Metal translation per stage. The vertex stage is translated twice: the
    // second variant returns void, as Metal requires when rasterization is
    // disabled (GL_RASTERIZER_DISCARD, transform feedback capture passes).
    // The geometry stage has no Metal equivalent; link() emulates it.
    for (int s = 0; s < kStageCount; ++s)
    for (int variant = 0; variant < (s == static_cast<int>(Stage::Vertex) ? 2 : 1); ++variant) {
        StageOutput& stage = result.stages[s];
        if (!stage.present || s == static_cast<int>(Stage::Geometry) || s == static_cast<int>(Stage::TessControl) ||
            s == static_cast<int>(Stage::TessEval))
            continue;
        const bool noRaster = variant == 1;
        try {
            // Doubles become soft-float code (fp64.h); no-op without them.
            std::vector<uint32_t> spirv =
                inferStorageImageAccess(mergeAliasedVertexInputs(
                    flattenArraysOfArrays(cubeStorageImagesAs2DArrays(lowerGatherOffsets(rectImagesAs2D(stage.spirv))))));
            if (!lowerFp64(spirv, result.log)) return result;
            BorderMsl msl(std::move(spirv));
            msl.renameMetalFunctionObjects();
            spirv_cross::CompilerMSL::Options mslOptions;
            mslOptions.disable_rasterization = noRaster;
            mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
            mslOptions.set_msl_version(3, 1);      // texture atomics (macOS 14+; the floor is macOS 15)
            mslOptions.native_texture_atomics = true;
            mslOptions.enable_decoration_binding = false;
            mslOptions.pad_fragment_output_components = true;
            mslOptions.force_native_arrays = false;
            mslOptions.enable_base_index_zero = false;
            mslOptions.texture_buffer_native = true;   // samplerBuffer -> texture_buffer
            mslOptions.texture_1D_as_2D = true;        // GL 1D textures are stored as 2D (mipmaps, rendering)
            msl.set_msl_options(mslOptions);
            spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
            common.vertex.flip_vert_y = true;        // GL row 0 = Metal row 0 (see docs/design/core.md)
            common.vertex.fixup_clipspace = true;    // z: [-w, w] -> [0, w]
            msl.set_common_options(common);
            // Metal has no cull distance output: the shader writes a local
            // gl_CullDistance array, passed on as varyings (addCullDistances).
            if (s == static_cast<int>(Stage::Vertex)) msl.mask_stage_output_by_builtin(spv::BuiltInCullDistance);
            const spv::ExecutionModel model = executionModel(static_cast<Stage>(s));
            const spirv_cross::ShaderResources resources = msl.get_shader_resources();

            // Uniform buffers.
            uint32_t nextBlockSlot = kSlotFirstUniformBlock;
            // glslang can give an unreferenced resource the same binding as a
            // referenced one, and SPIRV-Cross maps slots by (set, binding), so
            // every resource gets a unique binding before slots are assigned.
            uint32_t uniqueBinding = 0;
            for (const auto& ubo : resources.uniform_buffers) {
                const uint32_t binding = msl.get_decoration(ubo.id, spv::DecorationBinding);
                const std::string blockName = unescapeName(msl.get_name(ubo.base_type_id));
                const bool defaultBlock = binding == kDefaultBlockBinding || blockName == kDefaultBlockName;
                if (!defaultBlock) msl.set_decoration(ubo.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(ubo.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(ubo.id, spv::DecorationBinding);
                const spirv_cross::SPIRType& type = msl.get_type(ubo.type_id);
                b.count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                if (defaultBlock) {
                    b.msl_buffer = kSlotDefaultUniforms;
                    stage.usesDefaultUniforms = true;
                } else {
                    b.msl_buffer = nextBlockSlot;
                    for (uint32_t e = 0; e < b.count; ++e) {
                        const std::string name = type.array.empty() ? blockName : blockName + "[" + std::to_string(e) + "]";
                        auto it = blockIndexByName.find(name);
                        if (it != blockIndexByName.end()) result.blocks[it->second].stageSlot[s] = static_cast<int>(nextBlockSlot + e);
                    }
                    nextBlockSlot += b.count;
                    if (nextBlockSlot > kSlotFirstUniformBlock + kMaxStageUniformBlocks) {
                        result.log += "error: too many uniform blocks in one stage\n";
                        return result;
                    }
                }
                msl.add_msl_resource_binding(b);
            }
            // Transform feedback capture and geometry emulation buffers
            // (see transform_feedback.h, geometry.h).
            static const std::map<std::string, uint32_t> kInternalBuffers = {
                {"wmgl_XfbControl", kSlotXfbControl},      {"wmgl_GsInput", kSlotFirstXfbBuffer},
                {"wmgl_GsOutput", kSlotFirstXfbBuffer + 1}, {"wmgl_GsIndices", kSlotFirstXfbBuffer + 2},
                {"wmgl_GsControl", kSlotFirstXfbBuffer + 3}, {"wmgl_GsVertices", kSlotFirstXfbBuffer},
                {"wmgl_TcsInput", kSlotFirstXfbBuffer}, {"wmgl_TcsOutput", kSlotFirstXfbBuffer + 1},
                {"wmgl_TcsControl", kSlotFirstXfbBuffer + 2}, {"wmgl_TesVertices", kSlotTessVertices},
                {"wmgl_TesPatches", kSlotTessPatches}, {"wmgl_TesControl", kSlotTessControl},
            };
            uint32_t nextAtomicSlot = kSlotFirstAtomicCounterBuffer, vertexCounterBuffers = 0;
            stage.storageArgs.clear();
            for (const auto& ssbo : resources.storage_buffers) {
                const std::string blockName = msl.get_name(ssbo.base_type_id);
                if (isUserStorageBlock(unescapeName(blockName))) {
                    // Shader storage block: the stage's argument buffer (kSlotStorageBlocks).
                    const std::string name = unescapeName(blockName);
                    const spirv_cross::SPIRType& varType = msl.get_type(ssbo.type_id);
                    const uint32_t count = varType.array.empty() ? 1 : std::max(1u, varType.array.back());
                    const bool writes = !msl.get_buffer_block_flags(ssbo.id).get(spv::DecorationNonWritable);
                    const uint32_t id = static_cast<uint32_t>(stage.storageArgs.size());
                    for (uint32_t e = 0; e < count; ++e) {
                        const std::string element = varType.array.empty() ? name : name + "[" + std::to_string(e) + "]";
                        auto it = std::find_if(result.storageBlocks.begin(), result.storageBlocks.end(),
                                               [&](const StorageBlockInfo& b) { return b.name == element; });
                        stage.storageArgs.push_back(
                            {it == result.storageBlocks.end() ? -1 : static_cast<int>(it - result.storageBlocks.begin()), writes});
                    }
                    if (std::count_if(stage.storageArgs.begin(), stage.storageArgs.end(),
                                      [](const StageOutput::StorageArg& a) { return a.counterBinding < 0; }) >
                        static_cast<long>(kMaxStageStorageBlocks)) {
                        result.log += "error: too many shader storage blocks in one stage\n";
                        return result;
                    }
                    msl.set_decoration(ssbo.id, spv::DecorationDescriptorSet, 1);
                    msl.set_decoration(ssbo.id, spv::DecorationBinding, uniqueBinding++);
                    spirv_cross::MSLResourceBinding b;
                    b.stage = model;
                    b.desc_set = 1;
                    b.binding = msl.get_decoration(ssbo.id, spv::DecorationBinding);
                    b.count = count;
                    b.msl_buffer = id;   // [[id(n)]]
                    b.msl_texture = id;  // its size's index in spvBufferSizeConstants
                    msl.add_msl_resource_binding(b);
                    continue;
                }
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                msl.set_decoration(ssbo.id, spv::DecorationBinding, uniqueBinding++);
                b.desc_set = msl.get_decoration(ssbo.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(ssbo.id, spv::DecorationBinding);
                const int counterBinding = atomicCounterBlockBinding(blockName);
                if (auto internal = kInternalBuffers.find(blockName); internal != kInternalBuffers.end()) b.msl_buffer = internal->second;
                else if (blockName.rfind("wmgl_XfbData", 0) == 0)
                    b.msl_buffer = kSlotFirstXfbBuffer + static_cast<uint32_t>(std::strtoul(blockName.c_str() + 12, nullptr, 10));
                else if (counterBinding >= 0 && s == static_cast<int>(Stage::Vertex)) {
                    // A vertex function's counter buffers: argument buffer entries (it has no free slots).
                    const uint32_t id = static_cast<uint32_t>(stage.storageArgs.size());
                    if (++vertexCounterBuffers > kMaxVertexAtomicCounterBuffers) {
                        result.log += "error: too many atomic counter buffers in one stage\n";
                        return result;
                    }
                    stage.storageArgs.push_back({-1, true, counterBinding});
                    msl.set_decoration(ssbo.id, spv::DecorationDescriptorSet, 1);
                    b.desc_set = 1;
                    b.count = 1;
                    b.msl_buffer = id;
                    b.msl_texture = id;
                } else if (counterBinding >= 0) {
                    if (s != static_cast<int>(Stage::Fragment) && s != static_cast<int>(Stage::Compute)) {
                        result.log += "error: atomic counters are not supported in this shader stage\n";
                        return result;
                    }
                    const uint32_t slotEnd = kSlotFirstAtomicCounterBuffer + kMaxStageAtomicCounterBuffers;
                    if (nextAtomicSlot >= slotEnd) {
                        result.log += "error: too many atomic counter buffers in one stage\n";
                        return result;
                    }
                    b.msl_buffer = nextAtomicSlot++;
                    for (AtomicCounterBufferInfo& buffer : result.atomicBuffers)
                        if (buffer.binding == static_cast<GLuint>(counterBinding)) buffer.stageSlot[s] = static_cast<int>(b.msl_buffer);
                }
                msl.add_msl_resource_binding(b);
            }
            if (!stage.storageArgs.empty()) {
                // Descriptor set 1 becomes the argument buffer; set 0 keeps its discrete slots.
                mslOptions.argument_buffers = true;
                mslOptions.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
                msl.set_msl_options(mslOptions);
                msl.add_discrete_descriptor_set(0);
                spirv_cross::MSLResourceBinding ab;
                ab.stage = model;
                ab.desc_set = 1;
                ab.binding = spirv_cross::kArgumentBufferBinding;
                ab.msl_buffer = kSlotStorageBlocks;
                msl.add_msl_resource_binding(ab);
                spirv_cross::MSLResourceBinding sizes;
                sizes.stage = model;
                sizes.desc_set = 1;
                sizes.binding = spirv_cross::kBufferSizeBufferBinding;
                sizes.msl_buffer = static_cast<uint32_t>(stage.storageArgs.size());
                msl.add_msl_resource_binding(sizes);
            }
            // Samplers. Only those the stage uses take a texture slot: an
            // unused one is inactive, as in GL, and SPIRV-Cross leaves it out
            // of the function (Iris shader packs declare dozens in shared
            // headers and use a few per program).
            const std::unordered_set<spirv_cross::VariableID> active = msl.get_active_interface_variables();
            uint32_t nextTexture = 0;
            std::vector<std::pair<uint32_t, std::vector<int>>> samplerSlotsById;  // image id -> its slots
            for (const auto& image : resources.sampled_images) {
                if (!active.count(image.id)) continue;
                const std::string name = unescapeName(msl.get_name(image.id));
                const spirv_cross::SPIRType& type = msl.get_type(image.type_id);
                const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                msl.set_decoration(image.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(image.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(image.id, spv::DecorationBinding);
                b.count = count;
                b.msl_texture = nextTexture;
                b.msl_sampler = nextTexture;
                msl.add_msl_resource_binding(b);
                if (type.array.empty()) msl.textureSlots[image.id] = nextTexture;
                auto it = samplerUniformByName.find(name);
                if (it != samplerUniformByName.end()) {
                    const auto element = samplerElement.find(name);
                    const int first = result.uniforms[it->second].samplerSlot + (element != samplerElement.end() ? element->second : 0);
                    for (uint32_t e = 0; e < count; ++e) result.samplers[first + e].stageTexture[s] = static_cast<int>(nextTexture + e);
                    samplerSlotsById.push_back({image.id, {}});
                    for (uint32_t e = 0; e < count; ++e) samplerSlotsById.back().second.push_back(first + static_cast<int>(e));
                }
                nextTexture += count;
                if (nextTexture > kMaxStageSamplers) {
                    result.log += "error: too many samplers in one stage\n";
                    return result;
                }
            }
            // Images: textures kSlotFirstImage..; at most 8 of them read_write
            // (inferStorageImageAccess).
            uint32_t nextImage = kSlotFirstImage, readWriteImages = 0;
            for (const auto& image : resources.storage_images) {
                if (!active.count(image.id)) continue;  // inactive, as samplers
                const std::string name = unescapeName(msl.get_name(image.id));
                const spirv_cross::SPIRType& type = msl.get_type(image.type_id);
                const uint32_t count = type.array.empty() ? 1 : std::max(1u, type.array.back());
                msl.set_decoration(image.id, spv::DecorationBinding, uniqueBinding++);
                spirv_cross::MSLResourceBinding b;
                b.stage = model;
                b.desc_set = msl.get_decoration(image.id, spv::DecorationDescriptorSet);
                b.binding = msl.get_decoration(image.id, spv::DecorationBinding);
                b.count = count;
                b.msl_texture = nextImage;
                msl.add_msl_resource_binding(b);
                if (auto it = imageUniformByName.find(name); it != imageUniformByName.end()) {
                    const size_t first = static_cast<size_t>(result.uniforms[it->second].imageSlot);
                    for (uint32_t e = 0; e < count && first + e < result.images.size(); ++e)
                        result.images[first + e].stageTexture[s] = static_cast<int>(nextImage + e);
                }
                nextImage += count;
                if (!msl.has_decoration(image.id, spv::DecorationNonWritable) &&
                    !msl.has_decoration(image.id, spv::DecorationNonReadable))
                    readWriteImages += count;
                if (nextImage > kSlotFirstImage + kMaxStageImages || readWriteImages > kMaxStageReadWriteImages) {
                    result.log += "error: too many images in one stage\n";
                    return result;
                }
            }
            // Vertex inputs: GL locations.
            if (s == static_cast<int>(Stage::Vertex)) {
                if (noRaster) stage.vertexInputs.clear();  // rebuilt identically below
                // 64-bit inputs come split into one Metal input per 16 bytes
                // ("<name>_wmgl_<column>_<part>", fp64.h): part 0 of column
                // k reads at GL location + k, part 1 at a free Metal index.
                std::set<GLint> usedAttributes;
                for (const AttribInfo& a : result.attribs)
                    for (int l = 0; l < a.locationCount; ++l) usedAttributes.insert(a.location + l);
                GLint nextFree = 30;
                for (const auto& in : resources.stage_inputs) {
                    std::string split = unescapeName(msl.get_name(in.id));
                    const size_t marker = split.rfind("_wmgl_");
                    if (marker == std::string::npos || split.find('_', marker + 6) == std::string::npos) continue;
                    const int column = std::atoi(split.c_str() + marker + 6);
                    const int part = std::atoi(split.c_str() + split.find('_', marker + 6) + 1);
                    const std::string base = split.substr(0, marker);
                    auto it = std::find_if(result.attribs.begin(), result.attribs.end(),
                                           [&](const AttribInfo& a) { return a.name == base; });
                    if (it == result.attribs.end()) continue;
                    const spirv_cross::SPIRType& type = msl.get_type(in.type_id);
                    const GLint attrib = it->location + column;
                    GLint metal = attrib;
                    // Part 1, and a double packed by layout(component) into a location
                    // another 64-bit input already reads, take a free Metal index.
                    const bool shared = part == 0 && std::any_of(stage.vertexInputs.begin(), stage.vertexInputs.end(),
                                                                 [&](const StageOutput::VertexInput& v) { return v.location == attrib; });
                    if (part == 1 || shared) {
                        while (nextFree >= 0 && usedAttributes.count(nextFree)) --nextFree;
                        if (nextFree < 0) {
                            result.log += "error: too many 64-bit vertex attribute components\n";
                            return result;
                        }
                        metal = nextFree--;
                    }
                    msl.set_decoration(in.id, spv::DecorationLocation, static_cast<uint32_t>(metal));
                    StageOutput::VertexInput input{metal, static_cast<int>(type.vecsize), 'd', attrib, part};
                    input.byteOffset = part == 0 ? it->component * 4 : 0;
                    stage.vertexInputs.push_back(input);
                }
                for (const auto& in : resources.stage_inputs) {
                    const std::string name = unescapeName(msl.get_name(in.id));
                    if (name.rfind("_wmgl_") != std::string::npos && name.find('_', name.rfind("_wmgl_") + 6) != std::string::npos)
                        continue;
                    auto it = std::find_if(result.attribs.begin(), result.attribs.end(),
                                           [&](const AttribInfo& a) { return a.name == name; });
                    const spirv_cross::SPIRType& type = msl.get_type(in.type_id);
                    if (it == result.attribs.end()) continue;
                    msl.set_decoration(in.id, spv::DecorationLocation, static_cast<uint32_t>(it->location));
                    using BT = spirv_cross::SPIRType::BaseType;
                    const char kind = type.basetype == BT::Int ? 'i' : type.basetype == BT::UInt ? 'u' : 'f';
                    for (int l = 0; l < it->locationCount; ++l)
                        stage.vertexInputs.push_back({it->location + l, static_cast<int>(type.vecsize), kind});
                }
            }
            if (s == static_cast<int>(Stage::Fragment)) {
                for (const auto& out : resources.stage_outputs) {
                    const std::string name = unescapeName(msl.get_name(out.id));
                    auto it = std::find_if(result.fragOutputs.begin(), result.fragOutputs.end(),
                                           [&](const FragOutputInfo& f) { return f.name == name; });
                    if (it == result.fragOutputs.end()) continue;
                    msl.set_decoration(out.id, spv::DecorationLocation, static_cast<uint32_t>(it->location));
                    if (it->index) msl.set_decoration(out.id, spv::DecorationIndex, static_cast<uint32_t>(it->index));
                    const spirv_cross::SPIRType& type = msl.get_type(out.type_id);
                    using BT = spirv_cross::SPIRType::BaseType;
                    const char kind = type.basetype == BT::Int ? 'i' : type.basetype == BT::UInt ? 'u' : 'f';
                    for (int e = 0; e < it->size; ++e) stage.fragOutputs.push_back({it->location + e, it->index, kind});
                }
            }
            if (noRaster) {
                stage.mslNoRaster = finishFp64Msl(msl.compile());
                addDefaultBorderMacros(stage.mslNoRaster);
                if (const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS"); v && v[0] == '1')
                    logRawFormat(LogCategory::Shader, "winemetalgl: MSL vertex stage without rasterization:\n%s\n",
                                 stage.mslNoRaster.c_str());
                instanceIdFromZero(stage.mslNoRaster);
                continue;
            }
            stage.msl = fenceTextureAtomics(finishFp64Msl(msl.compile()));
            addDefaultBorderMacros(stage.msl);
            // Samplers read through filtering lookups (SamplerSlotInfo::sampled).
            for (const auto& [id, slots] : samplerSlotsById) {
                const std::regex lookup("\\b" + msl.get_name(id) + "(\\[[^\\]]*\\])*\\.(sample|gather)");
                if (std::regex_search(stage.msl, lookup))
                    for (int slot : slots) result.samplers[static_cast<size_t>(slot)].sampled = true;
            }
            if (s == static_cast<int>(Stage::Vertex)) {
                ensureVertexOutput(stage.msl);
                addCullDistances(stage.msl);
                addPointSize(stage.msl);
                instanceIdFromZero(stage.msl);
            }
            if (s == static_cast<int>(Stage::Fragment)) {
                stage.usesPointCoord = patchPointCoord(stage.msl);
                stage.flatInputs = hasFlatInputs(stage.msl);
                stage.msl = fragmentSampleMaskInPerSample(stage.msl);
            }
            // WINEMETALGL_DUMP_SHADERS=1 prints every generated MSL stage (debugging aid).
            static const bool dump = [] {
                const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
                return v && v[0] == '1';
            }();
            if (dump) logRawFormat(LogCategory::Shader, "winemetalgl: MSL stage %d:\n%s\n", s, stage.msl.c_str());
            stage.entryPoint = msl.get_cleansed_entry_point_name("main", model);
            stage.writesPointSize = stage.msl.find("[[point_size]]") != std::string::npos;
        } catch (const std::exception& error) {
            result.log += std::string("error: Metal translation failed: ") + error.what() + "\n";
            return result;
        }
    }

    // Resource limits glslang does not check, in OpenGL ES programs: image
    // uniforms per stage and storage blocks over all stages (OpenGL ES 3.2
    // sections 7.11.1 and 7.8).
    if (input.es) {
        int images[kStageCount] = {}, blocks = 0;
        for (const UniformInfo& u : result.uniforms)
            if (u.imageSlot >= 0)
                for (int s = 0; s < kStageCount; ++s)
                    if (u.stageMask & (1u << s)) images[s] += std::max(1, static_cast<int>(u.size));
        for (int s = 0; s < kStageCount; ++s)
            if (images[s] > limits::kMaxImageUniformsPerStage) {
                result.log += "error: too many image uniforms in one stage\n";
                return result;
            }
        // An array of blocks counts all its elements in a stage that uses any.
        std::map<std::string, std::pair<int, uint32_t>> arrays;  // base name -> (elements, stages)
        for (const StorageBlockInfo& b : result.storageBlocks) {
            auto& a = arrays[b.name.substr(0, b.name.find('['))];
            ++a.first;
            a.second |= b.stageMask;
        }
        for (const auto& [name, a] : arrays)
            for (int s = 0; s < kStageCount; ++s) blocks += ((a.second >> s) & 1) * a.first;
        if (blocks > limits::kMaxCombinedStorageBlocks) {
            result.log += "error: too many shader storage blocks in the program\n";
            return result;
        }
    }
    hideFixedFunctionBlock(result);
    applyUniformInitializers(input.shaders, result);
    result.ok = true;
    return result;
}

void remapStorageArgs(StageOutput& stage, const LinkResult& from, const LinkResult& to) {
    for (StageOutput::StorageArg& arg : stage.storageArgs) {
        if (arg.block < 0 || static_cast<size_t>(arg.block) >= from.storageBlocks.size()) continue;
        const std::string& name = from.storageBlocks[static_cast<size_t>(arg.block)].name;
        auto it = std::find_if(to.storageBlocks.begin(), to.storageBlocks.end(), [&](const StorageBlockInfo& b) { return b.name == name; });
        arg.block = it == to.storageBlocks.end() ? -1 : static_cast<int>(it - to.storageBlocks.begin());
    }
}

void copyStageSlots(const LinkResult& source, int from, LinkResult& result, int to) {
    for (UniformBlockInfo& block : result.blocks)
        for (const UniformBlockInfo& other : source.blocks)
            if (other.name == block.name) block.stageSlot[to] = other.stageSlot[from];
    for (AtomicCounterBufferInfo& buffer : result.atomicBuffers)
        for (const AtomicCounterBufferInfo& other : source.atomicBuffers)
            if (other.binding == buffer.binding) buffer.stageSlot[to] = other.stageSlot[from];
    for (const UniformInfo& u : result.uniforms) {
        if (u.imageSlot < 0) continue;
        for (const UniformInfo& other : source.uniforms)
            if (other.name == u.name && other.imageSlot >= 0)
                for (GLint e = 0; e < std::min(u.size, other.size); ++e)
                    result.images[u.imageSlot + e].stageTexture[to] = source.images[other.imageSlot + e].stageTexture[from];
    }
    for (const UniformInfo& u : result.uniforms) {
        if (u.samplerSlot < 0) continue;
        for (const UniformInfo& other : source.uniforms)
            if (other.name == u.name && other.samplerSlot >= 0)
                for (GLint e = 0; e < std::min(u.size, other.size); ++e)
                    result.samplers[u.samplerSlot + e].stageTexture[to] = source.samplers[other.samplerSlot + e].stageTexture[from];
    }
}

namespace {

// Relinks the program with capture code in its vertex shader and makes that
// the program's vertex stage.
bool useVertexCapture(const LinkInput& input, const LinkResult& layout, LinkResult& result) {
    int version = 140;
    for (const ShaderSource& shader : input.shaders)
        if (shader.stage == Stage::Vertex) version = std::max(version, captureVersion(shader.source));
    const CaptureCode code = vertexCaptureCode(layout, version);
    LinkInput again = input;
    again.transformFeedbackVaryings.clear();
    LinkResult captured = linkImpl(again, &code);
    if (!captured.ok) {
        result.log += "error: internal error building vertex output capture:\n" + captured.log;
        if (const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS"); v && v[0] == '1')
            logRawFormat(LogCategory::Shader, "winemetalgl: capture declarations:\n%s\ncapture epilogue:\n%s\n",
                         code.declarations.c_str(),
                         code.epilogue.c_str());
        return false;
    }
    const int vs = static_cast<int>(Stage::Vertex);
    result.stages[vs] = captured.stages[vs];
    copyStageSlots(captured, vs, result, vs);
    return true;
}

bool buildGeometryEmulation(const LinkInput& input, const GeometryShaderInfo& info, const LinkResult& captureLayout,
                            LinkResult& result) {
    if (!useVertexCapture(input, captureLayout, result)) return false;
    const GeometrySources sources = geometrySources(info, captureLayout);
    GeometryEmulation& g = result.geometry;
    g.inputType = info.inputType;
    g.outputType = info.outputType;
    g.verticesIn = info.verticesIn;
    g.maxVertices = info.maxVertices;
    g.invocations = info.invocations;
    g.inputStride = captureLayout.xfbStride[0];
    g.outputStride = sources.outputStride;
    g.outputs = sources.fields;
    g.streams = info.streams;
    g.streamOffset = sources.streamOffset;
    bool bufferStreamSet[4] = {};
    for (TransformFeedbackVarying& v : result.transformFeedback) {
        if (v.type == GL_NONE) continue;
        const size_t open = v.name.find('[');
        // The kernel's GLSL names anonymous block members "Block.member".
        const std::string base = v.source.empty() ? v.name.substr(0, open) : v.source;
        const int index = open == std::string::npos ? 0 : std::atoi(v.name.c_str() + open + 1);
        v.sourceOffset = TransformFeedbackVarying::kNotEmitted;
        for (const GeometryEmulation::Field& f : g.outputs)
            if (f.name == base) {
                v.sourceOffset = f.offset + static_cast<uint32_t>(index * glTypeWords(f.type));
                v.stream = f.stream;
            }
        // Each buffer captures the vertices of one stream (GL 4.0 section 13.2.2).
        if (bufferStreamSet[v.buffer] && result.xfbStream[v.buffer] != v.stream) {
            result.log += "error: transform feedback buffer " + std::to_string(v.buffer) +
                          " captures varyings of more than one vertex stream\n";
            return false;
        }
        bufferStreamSet[v.buffer] = true;
        result.xfbStream[v.buffer] = v.stream;
    }
    LinkInput kernelInput;
    kernelInput.shaders = {{Stage::Compute, sources.compute}};
    kernelInput.internal = true;
    LinkResult kernel = linkImpl(kernelInput, nullptr);
    if (!kernel.ok) {
        result.log += "error: internal error building the geometry shader kernel:\n" + kernel.log;
        return false;
    }
    const int cs = static_cast<int>(Stage::Compute);
    g.kernel = kernel.stages[cs];
    remapStorageArgs(g.kernel, kernel, result);
    copyStageSlots(kernel, cs, result, cs);
    LinkInput passthroughInput;
    passthroughInput.separable = true;
    // A separable program's geometry outputs have locations hashed from
    // their names: the pass-through declares them without and is hashed
    // the same way.
    passthroughInput.internal = !input.separable;
    passthroughInput.shaders = {{Stage::Vertex, input.separable ? stripHashedLocations(sources.passthrough) : sources.passthrough}};
    LinkResult passthrough = linkImpl(passthroughInput, nullptr);
    if (!passthrough.ok) {
        result.log += "error: internal error building the geometry shader output stage:\n" + passthrough.log;
        return false;
    }
    g.passthrough = passthrough.stages[static_cast<int>(Stage::Vertex)];
    g.active = true;
    static const bool dump = [] {
        const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
        return v && v[0] == '1';
    }();
    if (dump) logRawFormat(LogCategory::Shader, "winemetalgl: geometry kernel GLSL:\n%s\npass-through GLSL:\n%s\n",
                           sources.compute.c_str(),
                           sources.passthrough.c_str());
    return true;
}

bool buildTessellationEmulation(const LinkInput& input, const TessellationLink& t, LinkResult& result) {
    if (!useVertexCapture(input, t.capture, result)) return false;
    TessellationEmulation& e = result.tess;
    e.inputStride = t.capture.xfbStride[0];
    // Whole identifiers only (as usesIdentifier below).
    const auto uses = [](const std::string& source, const std::string& name) {
        for (size_t at = source.find(name); at != std::string::npos; at = source.find(name, at + 1)) {
            const auto part = [&](size_t i) { return std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_'; };
            if ((at == 0 || !part(at - 1)) && (at + name.size() >= source.size() || !part(at + name.size()))) return true;
        }
        return false;
    };
    for (const ShaderSource& shader : input.shaders)
        if (shader.stage == Stage::TessControl || shader.stage == Stage::TessEval || shader.stage == Stage::Fragment ||
            shader.stage == Stage::Geometry)
            e.readsPrimitiveId |= uses(shader.source, "gl_PrimitiveID");
        else if (shader.stage == Stage::Vertex)
            e.readsInstanceId |= uses(shader.source, "gl_InstanceID") || uses(shader.source, "gl_InstanceIndex");
    static const bool dump = [] {
        const char* v = std::getenv("WINEMETALGL_DUMP_SHADERS");
        return v && v[0] == '1';
    }();
    TessControlSources control;
    if (t.hasControl) {
        control = tessControlSources(t.control, t.capture);
        e.vertexWords = control.vertexWords;
        e.patchWords = control.patchWords;
        e.patchesPerGroup = control.patchesPerGroup;
        if (dump) logRawFormat(LogCategory::Shader, "winemetalgl: tessellation control kernel GLSL:\n%s\n",
                               control.kernel.c_str());
        LinkInput kernelInput;
        kernelInput.shaders = {{Stage::Compute, control.kernel}};
        kernelInput.internal = true;
        LinkResult kernel = linkImpl(kernelInput, nullptr);
        if (!kernel.ok) {
            result.log += "error: internal error building the tessellation control kernel:\n" + kernel.log;
            return false;
        }
        const int cs = static_cast<int>(Stage::Compute);
        e.control = kernel.stages[cs];
        remapStorageArgs(e.control, kernel, result);
        copyStageSlots(kernel, cs, result, static_cast<int>(Stage::TessControl));
    }
    std::string log;
    const std::string vertex = tessEvaluationVertexShader(t.evaluation, t.hasControl ? &control : nullptr, &t.capture, log);
    if (vertex.empty()) {
        result.log += log;
        return false;
    }
    if (dump) logRawFormat(LogCategory::Shader, "winemetalgl: tessellation evaluation vertex GLSL:\n%s\n",
                           vertex.c_str());
    LinkInput evaluationInput;
    // A separable program's outputs have locations hashed from their names:
    // the evaluation stage declares them without and is hashed the same way
    // (as the geometry pass-through).
    evaluationInput.separable = input.separable;
    evaluationInput.shaders = {{Stage::Vertex, input.separable ? stripHashedLocations(vertex) : vertex}};
    if (!t.geometryGlsl.empty())
        evaluationInput.shaders.push_back({Stage::Geometry, input.separable ? stripHashedLocations(t.geometryGlsl) : t.geometryGlsl});
    // Varyings by their qualified names: the geometry stage here is GLSL
    // translated by SPIRV-Cross, which names anonymous blocks.
    for (const TransformFeedbackVarying& v : result.transformFeedback) {
        const size_t open = v.name.find('[');
        evaluationInput.transformFeedbackVaryings.push_back(
            v.source.empty() || v.source == v.name.substr(0, open) ? v.name
                                                                   : v.source + (open == std::string::npos ? "" : v.name.substr(open)));
    }
    evaluationInput.transformFeedbackMode = input.transformFeedbackMode;
    evaluationInput.internal = !input.separable;
    auto evaluation = std::make_shared<LinkResult>(link(evaluationInput));
    if (!evaluation->ok) {
        result.log += "error: internal error building the tessellation evaluation stage:\n" + evaluation->log;
        return false;
    }
    e.evaluation = std::move(evaluation);
    e.active = true;
    return true;
}

}  // namespace

namespace {

bool usesIdentifier(const std::string& source, const std::string& name) {
    for (size_t at = source.find(name); at != std::string::npos; at = source.find(name, at + 1)) {
        const bool before = at > 0 && (std::isalnum(static_cast<unsigned char>(source[at - 1])) || source[at - 1] == '_');
        const size_t end = at + name.size();
        const bool after = end < source.size() && (std::isalnum(static_cast<unsigned char>(source[end])) || source[end] == '_');
        if (!before && !after) return true;
    }
    return false;
}

// A geometry shader's gl_PrimitiveID reaches the fragment shader as a flat
// varying: the emulation draws the emitted vertices with a vertex shader, so
// Metal's primitive ID would count the pass-through draw's primitives.
LinkInput withGeometryPrimitiveId(const LinkInput& input) {
    bool geometry = false, fragment = false;
    for (const ShaderSource& shader : input.shaders) {
        if (shader.stage == Stage::Geometry) geometry |= usesIdentifier(shader.source, "gl_PrimitiveID");
        if (shader.stage == Stage::Fragment) fragment |= usesIdentifier(shader.source, "gl_PrimitiveID");
    }
    if (!geometry || !fragment) return input;
    LinkInput out = input;
    for (ShaderSource& shader : out.shaders) {
        if ((shader.stage != Stage::Geometry && shader.stage != Stage::Fragment) || !usesIdentifier(shader.source, "gl_PrimitiveID"))
            continue;
        const std::string declaration = shader.stage == Stage::Geometry ? "flat out int wmgl_gs_primitive_id;\n"
                                                                         : "flat in int wmgl_gs_primitive_id;\n";
        shader.source = insertAfterDirectives(shader.source, declaration + "#define gl_PrimitiveID wmgl_gs_primitive_id\n",
                                              parseGlslVersion(shader.source).number);
    }
    return out;
}

}  // namespace

namespace {

LinkResult linkUncached(const LinkInput& original) {
    const LinkInput input = withGeometryPrimitiveId(original);
    GeometryShaderInfo geometry;
    LinkResult geometryCapture;
    TessellationLink tessellation;
    LinkResult result = linkImpl(input, nullptr, &geometry, &geometryCapture, &tessellation);
    if (!result.ok) return result;
    if (result.stages[static_cast<int>(Stage::TessEval)].present) {
        // The evaluation executable captures transform feedback and runs the
        // geometry shader (if any); the program's vertex stage feeds patches.
        if (result.stages[static_cast<int>(Stage::Vertex)].present && !buildTessellationEmulation(input, tessellation, result))
            result.ok = false;
        return result;
    }
    if (result.stages[static_cast<int>(Stage::TessControl)].present) return result;  // separable control-only program
    if (result.stages[static_cast<int>(Stage::Geometry)].present) {
        if (!result.stages[static_cast<int>(Stage::Vertex)].present) return result;  // separable, geometry only
        // Transform feedback (if any) captures the emitted vertices; the draw
        // path reads them back from the kernel output (DrawState).
        if (!buildGeometryEmulation(input, geometry, geometryCapture, result)) result.ok = false;
        return result;
    }
    if (result.transformFeedback.empty()) return result;
    if (!useVertexCapture(input, result, result)) {
        result.ok = false;
        return result;
    }
    result.xfbVertexCapture = true;
    return result;
}

// Every input a link depends on, as one string: the link is a function of
// it alone (the limits are constants).
std::string linkKey(const LinkInput& input) {
    std::string key;
    auto add = [&](const std::string& text) {
        const uint64_t size = text.size();
        key.append(reinterpret_cast<const char*>(&size), sizeof(size));
        key += text;
    };
    auto addNumber = [&](uint64_t value) { key.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
    addNumber(input.shaders.size());
    for (const ShaderSource& shader : input.shaders) {
        addNumber(static_cast<uint64_t>(shader.stage));
        add(shader.source);
    }
    addNumber(input.attribBindings.size());
    for (const auto& [name, location] : input.attribBindings) {
        add(name);
        addNumber(location);
    }
    addNumber(input.fragDataBindings.size());
    for (const auto& [name, binding] : input.fragDataBindings) {
        add(name);
        addNumber(binding.first);
        addNumber(binding.second);
    }
    addNumber(input.transformFeedbackVaryings.size());
    for (const std::string& varying : input.transformFeedbackVaryings) add(varying);
    addNumber(input.transformFeedbackMode);
    addNumber(uint64_t{input.es});
    addNumber(uint64_t{input.separable} | uint64_t{input.internal} << 1 | uint64_t{input.captureLayout} << 2 |
              uint64_t{input.spirv} << 3 | uint64_t{input.spirvUnnamed} << 4);
    return key;
}

// Results of recent links by their inputs: programs linked again from the
// same shaders and bindings (a pipeline built per draw setup, shader caches
// that relink) skip compiling and translating, as drivers' shader caches do.
class LinkCache {
public:
    std::shared_ptr<const LinkResult> find(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second.position);  // most recently used first
        return it->second.result;
    }
    void add(std::string key, std::shared_ptr<const LinkResult> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.count(key)) return;
        order_.push_front(key);
        entries_.emplace(std::move(key), Entry{std::move(result), order_.begin()});
        while (entries_.size() > kEntries) {
            entries_.erase(order_.back());
            order_.pop_back();
        }
    }

private:
    static constexpr size_t kEntries = 256;
    struct Entry {
        std::shared_ptr<const LinkResult> result;
        std::list<std::string>::iterator position;
    };
    std::mutex mutex_;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace

LinkResult link(const LinkInput& input) {
    static LinkCache cache;
    std::string key = linkKey(input);
    if (std::shared_ptr<const LinkResult> cached = cache.find(key)) return *cached;
    // Linked in an earlier run (link_file_cache.h).
    if (std::shared_ptr<LinkResult> stored = loadLinkResult(key)) {
        LinkResult result = *stored;
        cache.add(std::move(key), std::move(stored));
        return result;
    }
    LinkResult result = linkUncached(input);
    storeLinkResult(key, result);
    cache.add(std::move(key), std::make_shared<const LinkResult>(result));
    return result;
}

}  // namespace wmgl::shader

// ------------------------------------------------------ program pipelines

namespace wmgl::shader {
namespace {

// SPIRV-Cross GLSL of `stage` of a linked program (the emulations rebuild
// stages from it, so they keep that program's default uniform block).
bool linkedStageGlsl(const LinkResult& link, Stage stage, std::string& glsl, std::string& log) {
    try {
        spirv_cross::CompilerGLSL glslOut(link.stages[static_cast<int>(stage)].spirv);
        // Renamed keywords (a sampler named `texture`) keep the program's names (unescapeName).
        glslOut.illegal_name_prefix = kEscapePrefix;
        spirv_cross::CompilerGLSL::Options options = glslOut.get_common_options();
        options.version = 450;
        options.es = false;
        options.vulkan_semantics = false;
        options.enable_row_major_load_workaround = false;
        glslOut.set_common_options(options);
        glsl = glslOut.compile();
        return true;
    } catch (const std::exception& error) {
        log += std::string("error: shader translation failed: ") + error.what() + "\n";
        return false;
    }
}

// The layout of the outputs `varyings` of `input`'s vertex stage, captured
// one record per vertex.
bool vertexCaptureLayout(const LinkInput& input, const std::vector<std::string>& varyings, LinkResult& layout,
                         std::string& log) {
    LinkInput vertexOnly;
    for (const ShaderSource& shader : input.shaders)
        if (shader.stage == Stage::Vertex) vertexOnly.shaders.push_back(shader);
    vertexOnly.attribBindings = input.attribBindings;
    vertexOnly.separable = true;
    vertexOnly.internal = true;
    vertexOnly.captureLayout = true;
    vertexOnly.transformFeedbackVaryings = varyings;
    layout = linkImpl(vertexOnly, nullptr);
    if (!layout.ok) log += "error: a pipeline stage reads outputs the vertex stage does not write:\n" + layout.log;
    return layout.ok;
}

// `input` relinked with capture code writing `layout` from its vertex stage.
bool relinkVertexCapture(const LinkInput& input, const LinkResult& layout, LinkResult& captured, std::string& log) {
    int version = 140;
    for (const ShaderSource& shader : input.shaders)
        if (shader.stage == Stage::Vertex) version = std::max(version, captureVersion(shader.source));
    const CaptureCode code = vertexCaptureCode(layout, version);
    LinkInput again = input;
    again.transformFeedbackVaryings.clear();
    captured = linkImpl(again, &code);
    if (!captured.ok) log += "error: internal error building vertex output capture:\n" + captured.log;
    return captured.ok;
}

// Transform feedback of the program that owns the last stage (`source`),
// captured from the geometry emulation's output records.
void geometryTransformFeedback(const LinkResult& source, LinkResult& out) {
    out.transformFeedback = source.transformFeedback;
    std::copy(std::begin(source.xfbStride), std::end(source.xfbStride), std::begin(out.xfbStride));
    out.xfbBufferCount = source.xfbBufferCount;
    for (TransformFeedbackVarying& v : out.transformFeedback) {
        if (v.type == GL_NONE) continue;
        const size_t open = v.name.find('[');
        const std::string base = v.source.empty() ? v.name.substr(0, open) : v.source;
        const int index = open == std::string::npos ? 0 : std::atoi(v.name.c_str() + open + 1);
        v.sourceOffset = TransformFeedbackVarying::kNotEmitted;
        for (const GeometryEmulation::Field& f : out.geometry.outputs)
            if (f.name == base) v.sourceOffset = f.offset + static_cast<uint32_t>(index * glTypeWords(f.type));
    }
}

// The geometry emulation of `geometryGlsl` fed by `vertexInput`'s vertex
// stage: out.stages[Vertex] (capture variant) and out.geometry; the slots of
// the capture variant and of the kernel go to `vertexSlots` / `kernelSlots`.
bool buildGeometryPieces(const LinkInput& vertexInput, const std::string& geometryGlsl, LinkResult& out, LinkResult& vertexSlots,
                         LinkResult& kernelSlots, std::string& log) {
    GeometryShaderInfo info;
    if (!parseGeometryGlsl(geometryGlsl, info, log)) return false;
    LinkResult layout;
    if (!vertexCaptureLayout(vertexInput, geometryCaptureVaryings(info), layout, log)) return false;
    if (!relinkVertexCapture(vertexInput, layout, vertexSlots, log)) return false;
    const int vs = static_cast<int>(Stage::Vertex), cs = static_cast<int>(Stage::Compute);
    out.stages[vs] = vertexSlots.stages[vs];
    const GeometrySources sources = geometrySources(info, layout);
    GeometryEmulation& g = out.geometry;
    g.inputType = info.inputType;
    g.outputType = info.outputType;
    g.verticesIn = info.verticesIn;
    g.maxVertices = info.maxVertices;
    g.invocations = info.invocations;
    g.inputStride = layout.xfbStride[0];
    g.outputStride = sources.outputStride;
    g.outputs = sources.fields;
    out.geometryInputType = info.inputType;
    out.geometryOutputType = info.outputType;
    out.geometryVerticesOut = info.maxVertices;
    out.geometryInvocations = info.invocations;
    LinkInput kernelInput;
    kernelInput.shaders = {{Stage::Compute, sources.compute}};
    kernelInput.internal = true;
    kernelSlots = linkImpl(kernelInput, nullptr);
    if (!kernelSlots.ok) {
        log += "error: internal error building the geometry shader kernel:\n" + kernelSlots.log;
        return false;
    }
    g.kernel = kernelSlots.stages[cs];
    // The outputs keep their names; separable stages match by name.
    LinkInput passthroughInput;
    passthroughInput.separable = true;
    passthroughInput.shaders = {{Stage::Vertex, stripHashedLocations(sources.passthrough)}};
    LinkResult passthrough = linkImpl(passthroughInput, nullptr);
    if (!passthrough.ok) {
        log += "error: internal error building the geometry shader output stage:\n" + passthrough.log;
        return false;
    }
    g.passthrough = passthrough.stages[vs];
    g.active = true;
    return true;
}

// Transform feedback varyings of a program as link() takes them for a
// stage SPIRV-Cross translated (which names anonymous blocks).
std::vector<std::string> qualifiedVaryings(const LinkResult& link) {
    std::vector<std::string> varyings;
    for (const TransformFeedbackVarying& v : link.transformFeedback) {
        const size_t open = v.name.find('[');
        varyings.push_back(v.source.empty() || v.source == v.name.substr(0, open)
                               ? v.name
                               : v.source + (open == std::string::npos ? "" : v.name.substr(open)));
    }
    return varyings;
}

}  // namespace

PipelineLinkResult linkPipeline(const PipelineStageInput stages[kStageCount]) {
    PipelineLinkResult out;
    const int vs = static_cast<int>(Stage::Vertex), tcs = static_cast<int>(Stage::TessControl),
              tes = static_cast<int>(Stage::TessEval), gs = static_cast<int>(Stage::Geometry), cs = static_cast<int>(Stage::Compute);
    auto bound = [&](int s) { return stages[s].input && stages[s].link && stages[s].link->stages[s].present; };
    if (!bound(vs)) {
        out.log += "error: the pipeline has no vertex shader\n";
        return out;
    }
    auto piece = [&](int owner, LinkResult&& slots, int from, int to, bool evaluation) {
        out.pieces.push_back({owner, from, to, evaluation, std::make_shared<LinkResult>(std::move(slots))});
    };
    std::string& log = out.log;
    LinkResult& l = out.link;
    // The program whose stage is last before rasterization owns transform feedback.
    const int last = bound(gs) ? gs : bound(tes) ? tes : vs;
    const LinkInput& vertexInput = *stages[vs].input;

    if (!bound(tes)) {
        if (!bound(gs)) {
            log += "error: nothing to assemble\n";
            return out;
        }
        std::string geometryGlsl;
        LinkResult vertexSlots, kernelSlots;
        if (!linkedStageGlsl(*stages[gs].link, Stage::Geometry, geometryGlsl, log) ||
            !buildGeometryPieces(vertexInput, geometryGlsl, l, vertexSlots, kernelSlots, log))
            return out;
        geometryTransformFeedback(*stages[gs].link, l);
        piece(vs, std::move(vertexSlots), vs, vs, false);
        piece(gs, std::move(kernelSlots), cs, cs, false);
        out.ok = true;
        return out;
    }

    // Tessellation: control (optional) and evaluation stages from their programs.
    TessStageInfo control, evaluation;
    std::string glsl;
    const bool hasControl = bound(tcs);
    if (hasControl && (!linkedStageGlsl(*stages[tcs].link, Stage::TessControl, glsl, log) ||
                       !parseTessellationGlsl(Stage::TessControl, glsl, control, log)))
        return out;
    if (!linkedStageGlsl(*stages[tes].link, Stage::TessEval, glsl, log) ||
        !parseTessellationGlsl(Stage::TessEval, glsl, evaluation, log))
        return out;
    LinkResult layout;
    if (!vertexCaptureLayout(vertexInput, tessellationCaptureVaryings(hasControl ? control : evaluation), layout, log)) return out;
    LinkResult vertexSlots;
    if (!relinkVertexCapture(vertexInput, layout, vertexSlots, log)) return out;
    l.stages[vs] = vertexSlots.stages[vs];
    piece(vs, std::move(vertexSlots), vs, vs, false);
    TessellationEmulation& t = l.tess;
    const LinkResult& evaluationLink = *stages[tes].link;
    t.mode = evaluationLink.tess.mode;
    t.spacing = evaluationLink.tess.spacing;
    t.order = evaluationLink.tess.order;
    t.pointMode = evaluationLink.tess.pointMode;
    t.hasControl = hasControl;
    t.inputStride = layout.xfbStride[0];
    TessControlSources controlSources;
    if (hasControl) {
        controlSources = tessControlSources(control, layout);
        t.verticesOut = control.verticesOut;
        t.vertexWords = controlSources.vertexWords;
        t.patchWords = controlSources.patchWords;
        LinkInput kernelInput;
        kernelInput.shaders = {{Stage::Compute, controlSources.kernel}};
        kernelInput.internal = true;
        LinkResult kernel = linkImpl(kernelInput, nullptr);
        if (!kernel.ok) {
            log += "error: internal error building the tessellation control kernel:\n" + kernel.log;
            return out;
        }
        t.control = kernel.stages[cs];
        piece(tcs, std::move(kernel), cs, tcs, false);
    }
    const std::string evaluationVertex =
        tessEvaluationVertexShader(evaluation, hasControl ? &controlSources : nullptr, &layout, log);
    if (evaluationVertex.empty()) return out;
    // The evaluation outputs keep their names; separable stages match by name.
    LinkInput evaluationInput;
    evaluationInput.shaders = {{Stage::Vertex, stripHashedLocations(evaluationVertex)}};
    evaluationInput.separable = true;
    auto evaluationResult = std::make_shared<LinkResult>();
    const bool geometryElsewhere = bound(gs) && stages[gs].link != stages[tes].link;
    if (!geometryElsewhere) {
        // The evaluation stage alone, or with the geometry stage of its own program.
        if (bound(gs)) {
            std::string geometryGlsl;
            if (!linkedStageGlsl(*stages[gs].link, Stage::Geometry, geometryGlsl, log)) return out;
            evaluationInput.shaders.push_back({Stage::Geometry, stripHashedLocations(geometryGlsl)});
        }
        evaluationInput.transformFeedbackVaryings = qualifiedVaryings(*stages[last].link);
        evaluationInput.transformFeedbackMode = stages[last].input->transformFeedbackMode;
        *evaluationResult = link(evaluationInput);
        if (!evaluationResult->ok) {
            log += "error: internal error building the tessellation evaluation stage:\n" + evaluationResult->log;
            return out;
        }
        piece(tes, LinkResult(*evaluationResult), vs, vs, true);
        if (bound(gs)) piece(tes, LinkResult(*evaluationResult), cs, cs, true);
    } else {
        // The geometry stage of another program, fed by the evaluation stage.
        std::string geometryGlsl;
        LinkResult evaluationSlots, kernelSlots;
        if (!linkedStageGlsl(*stages[gs].link, Stage::Geometry, geometryGlsl, log) ||
            !buildGeometryPieces(evaluationInput, geometryGlsl, *evaluationResult, evaluationSlots, kernelSlots, log))
            return out;
        evaluationResult->ok = true;
        geometryTransformFeedback(*stages[gs].link, *evaluationResult);
        piece(tes, std::move(evaluationSlots), vs, vs, true);
        piece(gs, std::move(kernelSlots), cs, cs, true);
    }
    t.evaluation = evaluationResult;
    t.active = true;
    // The pipeline's own transform feedback (the API reads it from the
    // executable; capture happens in the evaluation executable).
    l.transformFeedback = evaluationResult->transformFeedback;
    std::copy(std::begin(evaluationResult->xfbStride), std::end(evaluationResult->xfbStride), std::begin(l.xfbStride));
    l.xfbBufferCount = evaluationResult->xfbBufferCount;
    if (bound(gs)) {
        l.geometryInputType = evaluationResult->geometryInputType;
        l.geometryOutputType = evaluationResult->geometryOutputType;
        l.geometryVerticesOut = evaluationResult->geometryVerticesOut;
        l.geometryInvocations = evaluationResult->geometryInvocations;
    }
    out.ok = true;
    return out;
}

}  // namespace wmgl::shader
