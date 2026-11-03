#include <algorithm>
#include <cstdio>
#include <set>

#include "core/shader/compiler.h"
#include "core/shader/glsl_upgrade.h"
#include "core/shader/link_file_cache.h"
#include "test.h"

using namespace wmgl::shader;

namespace {

const UniformInfo* findUniform(const LinkResult& r, const std::string& name) {
    for (const auto& u : r.uniforms)
        if (u.name == name) return &u;
    return nullptr;
}

const AttribInfo* findAttrib(const LinkResult& r, const std::string& name) {
    for (const auto& a : r.attribs)
        if (a.name == name) return &a;
    return nullptr;
}

const char* kFs = "#version 330 core\nuniform vec4 tint; out vec4 color; void main() { color = tint; }\n";

}  // namespace

TEST(shader_compile_reports_errors) {
    CHECK(compile(Stage::Vertex, "#version 330 core\nvoid main() { gl_Position = vec4(0); }\n").ok);
    CompileResult bad = compile(Stage::Vertex, "#version 330 core\nvoid main() { gl_Position = undefined_thing; }\n");
    CHECK(!bad.ok);
    CHECK(bad.log.find("undefined_thing") != std::string::npos);
}

TEST(shader_link_basic_interface) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nlayout(location = 3) in vec3 pos; in vec2 uv; uniform mat4 mvp; uniform float unused;\n"
                          "out vec2 v; void main() { v = uv; gl_Position = mvp * vec4(pos, 1.0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nin vec2 v; uniform sampler2D tex; uniform vec4 tint; out vec4 color;\n"
                                           "void main() { color = texture(tex, v) * tint; }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(findUniform(r, "mvp") != nullptr);
    CHECK(findUniform(r, "tint") != nullptr);
    CHECK(findUniform(r, "tex") != nullptr);
    CHECK(findUniform(r, "unused") == nullptr);  // inactive uniforms are not reported
    const AttribInfo* pos = findAttrib(r, "pos");
    const AttribInfo* uv = findAttrib(r, "uv");
    CHECK(pos && pos->location == 3);
    CHECK(uv && uv->location == 0);
    CHECK_EQ(r.samplers.size(), size_t(1));
    CHECK(r.stages[0].msl.find("vertex") != std::string::npos);
    CHECK(r.stages[4].msl.find("fragment") != std::string::npos);
}

TEST(shader_attrib_binding_applies) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 a; in vec4 b; void main() { gl_Position = a + b; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.attribBindings["b"] = 0;
    in.attribBindings["a"] = 5;
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findAttrib(r, "a") && findAttrib(r, "a")->location == 5);
    CHECK(findAttrib(r, "b") && findAttrib(r, "b")->location == 0);
    // The MSL must use the GL locations.
    CHECK(r.stages[0].msl.find("[[attribute(5)]]") != std::string::npos);
}

TEST(shader_struct_array_names) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nstruct S { float a; vec2 b[2]; };\nuniform S s[2]; uniform float f[3];\n"
                          "void main() { gl_Position = vec4(s[1].a, s[0].b[1], f[2]); }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findUniform(r, "s[0].a") != nullptr);
    CHECK(findUniform(r, "s[1].a") != nullptr);
    CHECK(findUniform(r, "s[1].b[0]") != nullptr);
    const UniformInfo* fa = findUniform(r, "f[0]");
    CHECK(fa && fa->size == 3);
    // Locations are consecutive per element and unique.
    std::vector<int> locations;
    for (const auto& u : r.uniforms)
        if (u.blockIndex < 0)
            for (int e = 0; e < u.size; ++e) locations.push_back(u.location + e);
    std::sort(locations.begin(), locations.end());
    CHECK(std::adjacent_find(locations.begin(), locations.end()) == locations.end());
}

TEST(shader_uniform_block_interface) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nlayout(std140) uniform Camera { mat4 view; vec4 light; } cam;\n"
                          "void main() { gl_Position = cam.view * cam.light; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK_EQ(r.blocks.size(), size_t(1));
    if (!r.blocks.empty()) {
        CHECK_EQ(r.blocks[0].name, std::string("Camera"));
        CHECK_EQ(r.blocks[0].dataSize, 80);
        CHECK_EQ(r.blocks[0].stageSlot[0], 2);
    }
    const UniformInfo* light = findUniform(r, "Camera.light");
    CHECK(light && light->offset == 64);
}

TEST(shader_legacy_glsl_110) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 110\nattribute vec4 pos; uniform mat4 m; varying vec2 v;\n"
                          "void main() { v = pos.xy; gl_Position = m * pos; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 110\nvarying vec2 v; uniform sampler2D t;\nvoid main() { gl_FragColor = texture2D(t, v); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(findUniform(r, "m") != nullptr);
    CHECK(findAttrib(r, "pos") != nullptr);
}

TEST(shader_legacy_macros) {
    // Built-ins and legacy functions inside #define bodies are rewritten too.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 120\n#define MVP gl_ModelViewProjectionMatrix\nvarying vec2 v;\n"
                          "void main() { v = gl_MultiTexCoord0.xy; gl_Position = MVP * gl_Vertex; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 120\n#define TEX(s, c) texture2D(s, c)\n#define OUT(x) \\\n    gl_FragColor = x\n"
                          "varying vec2 v; uniform sampler2D t;\nvoid main() { OUT(TEX(t, v)); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}

// GL_EXT_gpu_shader4 in GLSL 1.20, as OpenMW's shaders require it (issue
// #42): integers, bitwise operators and the extension's texel fetches, with
// the compatibility built-ins and the line numbers of the original.
TEST(shader_legacy_gpu_shader4) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 120\n\n#if 1\n    #extension GL_EXT_gpu_shader4: require\n#endif\n"
                          "varying vec2 uv;\n"
                          "void main() { uv = gl_MultiTexCoord0.xy; gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 120\n#extension GL_EXT_gpu_shader4 : enable\n"
                          "uniform sampler2D t; uniform isampler2D it; uniform sampler2DArray ta; varying vec2 uv;\n"
                          "void main() {\n"
                          "#ifdef GL_EXT_gpu_shader4\n"
                          "    vec2 textureSize = textureSize2D(t, 0);\n"  // OpenMW's alpha.glsl names a variable so
                          "    ivec2 size = ivec2(textureSize);\n"
                          "    int bits = (int(uv.x * 255.0) & 0xf0) >> 4;\n"
                          "    gl_FragData[0] = texelFetch2D(t, ivec2(uv * vec2(size)), 0) * float(bits)\n"
                          "        + vec4(texelFetch2DOffset(it, ivec2(0), 0, ivec2(1)))\n"
                          "        + texture2DArray(ta, vec3(uv, 1.0)) + texture2DLodOffset(t, uv, 1.0, ivec2(-1, 1));\n"
                          "#endif\n"
                          "}\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CompileResult bad = compile(Stage::Fragment, "#version 120\n#extension GL_EXT_gpu_shader4 : require\n\nvoid main() { undeclared = 1; }\n");
    CHECK(!bad.ok);
    CHECK(bad.log.find("0:4:") != std::string::npos);
    if (bad.log.find("0:4:") == std::string::npos) std::fprintf(stderr, "%s\n", bad.log.c_str());
}

// GLSL 1.10's texture functions in core GLSL 1.40+, which drivers accept
// (SpaceEngine's #version 460 core atmosphere shaders call texture2D, issue
// #35), with bias in fragment shaders and shadow lookups as vec4. A shader
// that defines one itself keeps its own.
TEST(shader_core_legacy_texture_functions) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 460 core\nlayout(location = 0) in vec2 p; uniform sampler2D h;\n"
                                         "out vec2 uv;\n"
                                         "void main() { uv = p; gl_Position = vec4(p, texture2DLod(h, p, 0.0).r, 1.0); }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 460 core\nin vec2 uv; uniform sampler2D t; uniform samplerCube c;\n"
                          "uniform sampler2DShadow s; out vec4 o;\n"
                          "void main() {\n"
                          "    vec4 textureSize = texture2D(t, uv, 1.0);\n"
                          "    o = textureSize + textureCube(c, vec3(uv, 1.0)) + shadow2D(s, vec3(uv, 0.5)) + texture2DProj(t, vec3(uv, 2.0));\n"
                          "}\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CompileResult own = compile(Stage::Fragment, "#version 460 core\nuniform sampler2D t; out vec4 o;\n"
                                                 "vec4 texture2D(sampler2D s, vec2 p) { return texture(s, p) * 2.0; }\n"
                                                 "void main() { o = texture2D(t, vec2(0.5)); }\n");
    if (!own.ok) std::fprintf(stderr, "%s\n", own.log.c_str());
    CHECK(own.ok);
    // SpaceEngine's header: comments before #version, and an #extension in a
    // conditional; the functions go after both, and line numbers stay.
    CompileResult header = compile(Stage::Fragment, "// Atmosphere\n/* scattering\n   tables */\n#version 460 core\n\n"
                                                    "#ifdef LOG_Z_BUFFER_FS\n#extension GL_ARB_conservative_depth : enable\n#endif\n"
                                                    "#define _FRAGMENT_\nuniform sampler2D t; out vec4 o;\n"
                                                    "void main() { o = texture2D(t, vec2(0.5)) + undeclared; }\n");
    CHECK(!header.ok);
    CHECK(header.log.find("0:11:") != std::string::npos && header.log.find("undeclared") != std::string::npos);
    if (header.log.find("0:11:") == std::string::npos) std::fprintf(stderr, "%s\n", header.log.c_str());
    CompileResult headerOk = compile(Stage::Fragment, "// Atmosphere\n#version 460 core\n#ifdef X\n#extension GL_ARB_conservative_depth : enable\n"
                                                      "#endif\nuniform sampler2D t; out vec4 o;\nvoid main() { o = texture2D(t, vec2(0.5)); }\n");
    if (!headerOk.ok) std::fprintf(stderr, "%s\n", headerOk.log.c_str());
    CHECK(headerOk.ok);
    CompileResult line = compile(Stage::Fragment, "#version 460 core\nuniform sampler2D t; out vec4 o;\n\n"
                                                  "void main() { o = texture2D(t, vec2(0.5)) + undeclared; }\n");
    CHECK(!line.ok);
    CHECK(line.log.find("0:4:") != std::string::npos);
    if (line.log.find("0:4:") == std::string::npos) std::fprintf(stderr, "%s\n", line.log.c_str());
}

TEST(shader_legacy_fragdata) {
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, "#version 120\nvoid main() { gl_FragData[1] = vec4(1.0); }\n");
    CHECK(u.ok);
    CHECK(u.source.find("wmgl_FragData[1]") != std::string::npos);
    CHECK(u.source.find("#version 140") == 0);
    UpgradeResult both = upgradeLegacyGlsl(Stage::Fragment,
                                           "#version 120\nvoid main() { gl_FragColor = vec4(0.0); gl_FragData[0] = vec4(1.0); }\n");
    CHECK(!both.ok);
}

// Doom 3 BFG's generated GLSL (issue #25): GLSL 1.50 without a profile that
// redeclares built-ins and uses the compatibility profile's, as desktop
// drivers accept.
TEST(shader_glsl150_redeclared_compatibility_builtins) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 150\n#define PC\n\nuniform vec4 _va_[4];\n"
                          "in vec4 in_Position;\nin vec2 in_TexCoord;\nin vec4 in_Color;\n\n"
                          "out vec4 gl_Position;\nout vec4 vofi_TexCoord0;\nout vec4 gl_FrontColor;\n\n"
                          "void main() {\n\tgl_Position . x = dot ( in_Position , _va_[0] ) ;\n"
                          "\tgl_Position . y = dot ( in_Position , _va_[1] ) ;\n"
                          "\tgl_Position . z = dot ( in_Position , _va_[2] ) ;\n"
                          "\tgl_Position . w = dot ( in_Position , _va_[3] ) ;\n"
                          "\tvofi_TexCoord0 = vec4 ( in_TexCoord , 0 , 0 ) ;\n"
                          "\tgl_FrontColor = in_Color ;\n}\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 150\n#define PC\n\nuniform sampler2D samp0;\n\n"
                          "in vec4 gl_FragCoord;\nin vec4 vofi_TexCoord0;\nin vec4 gl_Color;\n\n"
                          "out vec4 gl_FragColor;\nout float gl_FragDepth;\n\n"
                          "void main() {\n\tgl_FragColor = texture ( samp0 , vofi_TexCoord0 . xy ) * gl_Color ;\n"
                          "\tgl_FragDepth = gl_FragCoord . z ;\n}\n"});
    for (const ShaderSource& shader : in.shaders) {
        CompileResult c = compile(shader.stage, shader.source);
        if (!c.ok) std::fprintf(stderr, "%s\n", c.log.c_str());
        CHECK(c.ok);
    }
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(findAttrib(r, "in_Position") != nullptr);
    CHECK(findAttrib(r, "in_Color") != nullptr);
    CHECK(findUniform(r, "samp0") != nullptr);

    const std::string relaxed = relaxDesktopGlsl("#version 150\nout vec4 gl_Position;\nout vec4 gl_FrontColor;\n"
                                                 "void main() { gl_FrontColor = vec4(1.0); }\n");
    CHECK(relaxed.find("#version 150 compatibility\n") == 0);
    CHECK(relaxed.find("gl_Position") == std::string::npos);
    CHECK(relaxed.find("out vec4 gl_FrontColor;") != std::string::npos);  // glslang takes this one
    CHECK_EQ(std::count(relaxed.begin(), relaxed.end(), '\n'), 4);        // line numbers stay
    // Without compatibility built-ins the profile stays core; an explicit
    // core profile stays strict.
    CHECK(relaxDesktopGlsl("#version 150\nout vec4 gl_Position;\nvoid main() {}\n").find("#version 150\n") == 0);
    CHECK(relaxDesktopGlsl("#version 150 core\nout vec4 gl_FrontColor;\n").find("#version 150 core\n") == 0);
    // Redeclarations with qualifiers are kept.
    CHECK(relaxDesktopGlsl("#version 150\nlayout(origin_upper_left) in vec4 gl_FragCoord;\n").find("gl_FragCoord") !=
          std::string::npos);
}

TEST(shader_version_parsing) {
    CHECK_EQ(parseGlslVersion("// comment\n/* block */\n#version 330 core\n").number, 330);
    CHECK(!parseGlslVersion("#version 330 core\n").compatibility);
    CHECK(parseGlslVersion("#version 150 compatibility\n").compatibility);
    CHECK(parseGlslVersion("void main() {}").compatibility);
    CHECK_EQ(parseGlslVersion("void main() {}").number, 110);
}

TEST(shader_transform_feedback_vertex_capture) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nin vec4 pos; out vec3 a; flat out ivec2 b; out float c[3];\n"
                          "void main() { a = pos.xyz; b = ivec2(gl_VertexID); c[0] = 1.0; c[1] = 2.0; c[2] = 3.0; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.transformFeedbackVaryings = {"gl_Position", "a", "b", "c[1]"};
    in.transformFeedbackMode = GL_INTERLEAVED_ATTRIBS;
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK_EQ(r.transformFeedback.size(), static_cast<size_t>(4));
    CHECK_EQ(r.xfbStride[0], 4u + 3u + 2u + 1u);
    CHECK(r.xfbVertexCapture);
    if (r.transformFeedback.size() == 4) {
        CHECK_EQ(r.transformFeedback[3].offset, 9u);
        CHECK(r.transformFeedback[3].type == GL_FLOAT && r.transformFeedback[3].size == 1);
    }
    const std::string& msl = r.stages[static_cast<int>(Stage::Vertex)].msl;
    CHECK(msl.find("[[buffer(30)]]") != std::string::npos);
    CHECK(msl.find("[[buffer(16)]]") != std::string::npos);

    in.transformFeedbackMode = GL_SEPARATE_ATTRIBS;
    in.transformFeedbackVaryings = {"a", "c"};  // c is 3 components: allowed separately
    r = link(in);
    CHECK(r.ok && r.xfbBufferCount == 2 && r.xfbStride[0] == 3 && r.xfbStride[1] == 3);
}

TEST(shader_transform_feedback_link_errors) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nout vec4 a; out mat2 m; void main() { a = vec4(1); m = mat2(1); gl_Position = a; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.transformFeedbackVaryings = {"missing"};
    CHECK(!link(in).ok);
    in.transformFeedbackVaryings = {"a", "a"};
    CHECK(!link(in).ok);
    in.transformFeedbackVaryings = {"a[0]"};
    CHECK(!link(in).ok);
    in.transformFeedbackMode = GL_SEPARATE_ATTRIBS;
    in.transformFeedbackVaryings = {"m", "a"};  // m has 4 components: fine; 5 would not be
    CHECK(link(in).ok);
    LinkInput fragmentOnly;
    fragmentOnly.shaders.push_back({Stage::Fragment, kFs});
    fragmentOnly.transformFeedbackVaryings = {"color"};
    CHECK(!link(fragmentOnly).ok);
}

TEST(shader_geometry_emulation) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nin vec4 pos; out VS_OUT { vec4 color; } vs_out; out float w;\n"
                          "void main() { vs_out.color = pos; w = 2.0; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Geometry,
                          "#version 330 core\nlayout(triangles) in; layout(triangle_strip, max_vertices = 4) out;\n"
                          "in VS_OUT { vec4 color; } gs_in[]; in float w[];\nuniform float scale; uniform sampler2D tex;\n"
                          "out vec4 fcol; flat out int prim;\n"
                          "void emit(int i) { fcol = gs_in[i].color * texture(tex, vec2(0.5)) * w[i]; prim = gl_PrimitiveIDIn;\n"
                          "    gl_Position = gl_in[i].gl_Position * scale; EmitVertex(); }\n"
                          "void main() { for (int i = 0; i < 3; ++i) emit(i); EndPrimitive(); }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 330 core\nin vec4 fcol; flat in int prim; uniform vec4 tint; out vec4 color;\n"
                          "void main() { color = fcol * tint + float(prim); }\n"});
    in.transformFeedbackVaryings = {"fcol", "gl_Position"};
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.geometry.active);
    CHECK_EQ(r.geometryInputType, static_cast<GLenum>(GL_TRIANGLES));
    CHECK_EQ(r.geometryOutputType, static_cast<GLenum>(GL_TRIANGLE_STRIP));
    CHECK_EQ(r.geometryVerticesOut, 4);
    CHECK_EQ(r.geometry.inputStride, 4u + 4u + 1u);   // gl_Position, VS_OUT.color, w
    CHECK_EQ(r.geometry.outputStride, 4u + 4u + 1u);  // gl_Position, fcol, prim
    CHECK(r.transformFeedback.size() == 2 && r.xfbStride[0] == 8u);
    CHECK(!r.geometry.kernel.msl.empty() && !r.geometry.passthrough.msl.empty());
    CHECK(r.geometry.kernel.msl.find("kernel void") != std::string::npos);
    const UniformInfo* tex = findUniform(r, "tex");
    CHECK(tex && tex->samplerSlot >= 0 && r.samplers[tex->samplerSlot].stageTexture[static_cast<int>(Stage::Compute)] == 0);
    CHECK(findUniform(r, "scale") != nullptr);
    CHECK(r.stages[static_cast<int>(Stage::Vertex)].mslNoRaster.find("[[buffer(16)]]") != std::string::npos);
}

TEST(shader_fragment_point_coord_and_flat) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; flat out int id; void main() { id = 1; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nflat in int id; out vec4 color;\n"
                                           "void main() { color = vec4(gl_PointCoord, float(id), 1.0); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    const StageOutput& fs = r.stages[static_cast<int>(Stage::Fragment)];
    CHECK(fs.usesPointCoord && fs.flatInputs);
    CHECK(fs.msl.find("wmgl_fs_driver[0]") != std::string::npos);
    in.shaders[1].source = "#version 330 core\nout vec4 color; void main() { color = vec4(1.0); }\n";
    r = link(in);
    CHECK(r.ok && !r.stages[static_cast<int>(Stage::Fragment)].usesPointCoord && !r.stages[static_cast<int>(Stage::Fragment)].flatInputs);
}

TEST(shader_instance_id_excludes_base_instance) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nflat out int id; void main() { id = gl_InstanceID; gl_Position = vec4(0.0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nflat in int id; out vec4 color; void main() { color = vec4(float(id)); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    // GL's gl_InstanceID does not include the base instance (GL_ARB_base_instance).
    const std::string& msl = r.stages[static_cast<int>(Stage::Vertex)].msl;
    CHECK(msl.find("[[base_instance]]") != std::string::npos);
}

TEST(shader_explicit_attrib_location_beats_binding) {
    // layout(location) wins over glBindAttribLocation, and stays reserved.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nlayout(location = 0) in vec4 pos; in vec4 color; out vec4 c;\n"
                                         "void main() { c = color; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nin vec4 c; out vec4 o; void main() { o = c; }\n"});
    in.attribBindings["pos"] = 5;
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findAttrib(r, "pos") && findAttrib(r, "pos")->location == 0);
    CHECK(findAttrib(r, "color") && findAttrib(r, "color")->location == 1);
}

TEST(shader_fragment_output_explicit_index) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; void main() { gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nlayout(location = 0, index = 0) out vec4 o0;\n"
                                           "layout(location = 0, index = 1) out vec4 o1;\n"
                                           "void main() { o0 = vec4(1.0); o1 = vec4(0.5); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    bool sawIndex1 = false;
    for (const FragOutputInfo& f : r.fragOutputs) {
        if (f.name == "o0") CHECK(f.location == 0 && f.index == 0);
        if (f.name == "o1") sawIndex1 = f.location == 0 && f.index == 1;
    }
    CHECK(sawIndex1);
}

TEST(shader_struct_sampler_members) {
    // glslang moves samplers out of struct uniforms; its place holder is not a uniform.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; out vec2 uv; void main() { uv = pos.xy; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nstruct S { sampler2D t; float k; };\nuniform S s;\nin vec2 uv; out vec4 o;\n"
                                           "void main() { o = texture(s.t, uv) * s.k; }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK_EQ(r.uniforms.size(), size_t(2));
    for (const UniformInfo& u : r.uniforms) CHECK(u.name.find("/*") == std::string::npos);
    const UniformInfo* k = findUniform(r, "s.k");
    const UniformInfo* t = findUniform(r, "s.t");
    CHECK(k && k->samplerSlot < 0);
    CHECK(t && t->samplerSlot >= 0);
}

TEST(shader_legacy_declarations_outside_conditionals) {
    // Declarations must not land inside a compiled-out #ifdef block.
    const char* fs = "#version 120\n#ifdef GL_ES\nprecision mediump float;\n#endif\nvarying vec4 vv;\n"
                     "void main() { gl_FragColor = vv; }\n";
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, fs);
    CHECK(u.ok);
    CHECK(u.source.find("out vec4 wmgl_FragColor;") < u.source.find("#ifdef GL_ES"));
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "attribute vec4 pos; varying vec4 vv; void main() { gl_Position = pos; vv = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, fs});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}

TEST(shader_legacy_redeclared_builtin_varyings) {
    // GLSL 1.10 lets shaders size gl_TexCoord; 1.30 adds interpolation qualifiers.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 120\nvarying vec4 gl_TexCoord[2];\n"
                                         "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2D t;\nvarying vec4 gl_TexCoord[2];\n"
                                           "void main() { gl_FragColor = texture2D(t, gl_TexCoord[0].xy); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    LinkInput flat;
    flat.shaders.push_back({Stage::Vertex, "#version 130\nflat out vec4 gl_FrontColor;\n"
                                           "void main() { gl_Position = ftransform(); gl_FrontColor = gl_Color; }\n"});
    flat.shaders.push_back({Stage::Fragment, "#version 130\nflat in vec4 gl_Color;\nvoid main() { gl_FragColor = gl_Color; }\n"});
    r = link(flat);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.ok && r.stages[static_cast<int>(Stage::Fragment)].flatInputs);
}

TEST(shader_legacy_helpers_in_several_objects) {
    // Generated helpers must not be defined once per shader object of a stage.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 120\nvec4 xf() { return ftransform(); }\n"});
    in.shaders.push_back({Stage::Vertex, "#version 120\nvec4 xf();\n"
                                         "void main() { gl_Position = xf() + ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2DShadow sh;\n"
                                           "vec4 light() { return gl_LightSource[0].diffuse * shadow2D(sh, vec3(0.5)); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2DShadow sh; uniform sampler2D t;\nvec4 light();\n"
                                           "void main() { gl_FragColor = light() + shadow2DProj(sh, vec4(texture2D(t, gl_TexCoord[0].xy).xy, "
                                           "0.5, 1.0)).r * vec4(1.0); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, "#version 120\nuniform sampler2DShadow s;\n"
                                                         "void main() { gl_FragColor = shadow2D(s, vec3(f(1.0), 0.5, 0.5)); }\n");
    CHECK(u.source.find("vec4(texture(s, vec3(f(1.0), 0.5, 0.5)))") != std::string::npos);
}

TEST(shader_separable_block_varyings_match_by_block_name) {
    auto locations = [](const std::string& msl) {
        std::set<std::string> found;
        for (size_t at = msl.find("[[user(locn"); at != std::string::npos; at = msl.find("[[user(locn", at + 1))
            found.insert(msl.substr(at, msl.find(')', at) - at));
        return found;
    };
    LinkInput vs;
    vs.separable = true;
    vs.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; out VS_OUT { vec4 c; } vs_out;\n"
                                         "void main() { gl_Position = pos; vs_out.c = pos; }\n"});
    LinkInput fs;
    fs.separable = true;
    fs.shaders.push_back({Stage::Fragment, "#version 330 core\nin VS_OUT { vec4 c; } fs_in; out vec4 o; void main() { o = fs_in.c; }\n"});
    LinkResult v = link(vs), f = link(fs);
    CHECK(v.ok && f.ok);
    CHECK(!locations(f.stages[static_cast<int>(Stage::Fragment)].msl).empty());
    CHECK(locations(v.stages[static_cast<int>(Stage::Vertex)].msl) == locations(f.stages[static_cast<int>(Stage::Fragment)].msl));
    // With a geometry stage, the pass-through stage declares the same hashed locations.
    LinkInput vg = vs;
    vg.shaders.push_back({Stage::Geometry, "#version 330 core\nlayout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\n"
                                           "in VS_OUT { vec4 c; } gs_in[]; out VS_OUT { vec4 c; } gs_out;\n"
                                           "void main() { for (int i = 0; i < 3; ++i) { gs_out.c = gs_in[i].c; "
                                           "gl_Position = gl_in[i].gl_Position; EmitVertex(); } }\n"});
    LinkResult g = link(vg);
    if (!g.ok) std::fprintf(stderr, "%s\n", g.log.c_str());
    CHECK(g.ok && g.geometry.active);
    CHECK(g.ok && locations(g.geometry.passthrough.msl) == locations(f.stages[static_cast<int>(Stage::Fragment)].msl));
}

TEST(shader_glsl420_pack_rules) {
    // invariant inputs outside the vertex stage, in any qualifier order.
    CHECK(compile(Stage::Fragment, "#version 420\nhighp in layout(location = 1) smooth invariant vec4 v;\n"
                                   "out vec4 o; void main() { o = v; }\n").ok);
    CHECK(!compile(Stage::Vertex, "#version 420\ninvariant in vec4 v; void main() { gl_Position = v; }\n").ok);
    // Image bindings name image units; block bindings stay below GL_MAX_UNIFORM_BUFFER_BINDINGS.
    CHECK(compile(Stage::Fragment, "#version 420\nlayout(binding = 15, rgba8) readonly uniform image2D i;\n"
                                   "out vec4 o; void main() { o = imageLoad(i, ivec2(0)); }\n").ok);
    CHECK(!compile(Stage::Fragment, "#version 420\nlayout(binding = 16, rgba8) readonly uniform image2D i;\n"
                                    "out vec4 o; void main() { o = imageLoad(i, ivec2(0)); }\n").ok);
    CHECK(compile(Stage::Fragment, "#version 420\nlayout(std140, binding = 82) uniform B { vec4 a; } b[2];\n"
                                   "out vec4 o; void main() { o = b[1].a; }\n").ok);
    CHECK(!compile(Stage::Fragment, "#version 420\nlayout(std140, binding = 82) uniform B { vec4 a; } b[3];\n"
                                    "out vec4 o; void main() { o = b[2].a; }\n").ok);
}

TEST(shader_fourteen_uniform_blocks_per_stage) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 420\nlayout(std140, binding = 2) uniform G { vec4 a; } g[14];\n"
                                         "void main() { vec4 s = vec4(0); for (int i = 0; i < 14; ++i) s += g[i].a;\n"
                                         "gl_Position = s; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 420\nout vec4 o; void main() { o = vec4(1); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok && r.blocks.size() == 14);
    if (r.blocks.size() == 14) CHECK_EQ(r.blocks[13].stageSlot[0], static_cast<int>(kSlotFirstUniformBlock + 13));
}

TEST(shader_rectangle_texture_outside_fragment_stages) {
    // Translated back to GLSL for the tessellation control kernel, which has no textureLod for rectangles.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 420\nout vec4 v; void main() { v = vec4(1); gl_Position = vec4(0); }\n"});
    in.shaders.push_back({Stage::TessControl, "#version 420\nlayout(vertices = 1) out; in vec4 v[]; out vec4 c[];\n"
                                              "uniform sampler2DRect r;\n"
                                              "void main() { c[gl_InvocationID] = v[gl_InvocationID] + texture(r, vec2(0)) +\n"
                                              "texelFetch(r, ivec2(0)); gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 1.0; }\n"});
    in.shaders.push_back({Stage::TessEval, "#version 420\nlayout(isolines, point_mode) in; in vec4 c[]; out vec4 e;\n"
                                           "void main() { e = c[0]; gl_Position = vec4(0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 420\nin vec4 e; out vec4 o; void main() { o = e; }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}

// ---- GL 4.3 reflection (program interface query)

namespace {

LinkResult link43(const char* vs, const char* fs, Stage first = Stage::Vertex) {
    LinkInput in;
    if (vs) in.shaders.push_back({first, vs});
    if (fs) in.shaders.push_back({Stage::Fragment, fs});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    return r;
}

const char* kPassFs = "#version 430 core\nout vec4 color;\nvoid main() { color = vec4(1.0); }\n";

}  // namespace

// Storage blocks: a non-last member sized by its largest constant index
// moves the members after it (glslang relays the block at link).
TEST(shader_ssbo_implicitly_sized_member_offsets) {
    LinkResult r = link43("#version 430 core\nlayout(std430) buffer B { vec4 a[]; vec4 b[]; } g;\n"
                          "void main() { gl_Position = g.a[1] + g.b[gl_VertexID]; }\n",
                          kPassFs);
    CHECK(r.ok);
    CHECK_EQ(r.bufferVariables.size(), size_t(2));
    if (r.bufferVariables.size() == 2) {
        CHECK_EQ(r.bufferVariables[0].size, 2);
        CHECK_EQ(r.bufferVariables[1].offset, 32);
        CHECK_EQ(r.bufferVariables[1].size, 0);  // unsized
    }
}

// Buffer variables of a top-level array of arrays: the first outer element
// only, each innermost array once (GL 4.3 section 7.3.1.1).
TEST(shader_ssbo_top_level_array_names) {
    LinkResult r = link43(nullptr, "#version 430 core\nbuffer Block { vec4 a[5][4][3]; vec4 d[]; };\nout vec4 color;\n"
                                   "void main() { color = a[0][0][0] + d[0]; }\n");
    CHECK(r.ok);
    CHECK_EQ(r.bufferVariables.size(), size_t(5));
    if (r.bufferVariables.size() == 5) {
        CHECK(r.bufferVariables[0].name == "a[0][0][0]");
        CHECK(r.bufferVariables[3].name == "a[0][3][0]");
        CHECK_EQ(r.bufferVariables[0].size, 3);
        CHECK_EQ(r.bufferVariables[0].topLevelArraySize, 5);
        CHECK(r.bufferVariables[0].topLevelArrayStride > 0);
        CHECK(r.bufferVariables[4].name == "d[0]");
        CHECK_EQ(r.bufferVariables[4].topLevelArraySize, 1);  // a plain array is not a top-level array
    }
}

// Uniforms are referenced by the stages whose code reads them, although the
// default block is shared; arrays of blocks per element.
TEST(shader_uniform_referenced_by_stage) {
    LinkResult r = link43("#version 430 core\nuniform vec4 a;\nvoid main() { gl_Position = a; }\n",
                          "#version 430 core\nuniform vec4 b;\nuniform B { vec4 c; } e[2];\nout vec4 color;\n"
                          "void main() { color = b + e[1].c; }\n");
    CHECK(r.ok);
    const UniformInfo* a = findUniform(r, "a");
    const UniformInfo* b = findUniform(r, "b");
    CHECK(a && a->stageMask == 1u << static_cast<int>(Stage::Vertex));
    CHECK(b && b->stageMask == 1u << static_cast<int>(Stage::Fragment));
    CHECK_EQ(r.blocks.size(), size_t(2));
    if (r.blocks.size() == 2) {
        CHECK_EQ(r.blocks[0].stageMask, 0u);
        CHECK_EQ(r.blocks[1].stageMask, 1u << static_cast<int>(Stage::Fragment));
    }
}

// Explicit uniform locations (GL 4.3): unused ones stay reserved, implicit
// ones take the lowest free locations, overlaps fail to link.
TEST(shader_explicit_uniform_locations) {
    LinkResult r = link43("#version 430 core\nlayout(location = 0) uniform float unused;\nlayout(location = 2) uniform vec4 b;\n"
                          "uniform vec4 c[2];\nuniform vec4 d;\nvoid main() { gl_Position = b + c[1] + d; }\n",
                          kPassFs);
    CHECK(r.ok);
    const UniformInfo* b = findUniform(r, "b");
    const UniformInfo* c = findUniform(r, "c[0]");
    const UniformInfo* d = findUniform(r, "d");
    CHECK(b && b->location == 2);
    CHECK(c && c->location == 3);
    CHECK(d && d->location == 1);
    LinkResult bad = link43("#version 430 core\nlayout(location = 1) uniform vec4 x[2];\nlayout(location = 2) uniform vec4 y;\n"
                            "void main() { gl_Position = x[0] + y; }\n",
                            kPassFs);
    CHECK(!bad.ok);
}

// Program inputs and outputs of a separable vertex-only program: block
// members and built-ins under their GL names.
TEST(shader_program_interface_inputs_outputs) {
    LinkInput in;
    in.separable = true;
    in.shaders.push_back({Stage::Vertex, "#version 430 core\nlayout(location = 0) in vec4 v;\nout Color { float r; vec4 c; } o;\n"
                                         "void main() { gl_Position = v + vec4(float(gl_VertexID)); o.r = 1.0; o.c = v; }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    auto has = [](const std::vector<InterfaceVariableInfo>& list, const char* name) {
        return std::any_of(list.begin(), list.end(), [&](const InterfaceVariableInfo& x) { return x.name == name; });
    };
    CHECK(has(r.programInputs, "v"));
    CHECK(has(r.programInputs, "gl_VertexID"));
    CHECK(has(r.programOutputs, "gl_Position"));
    CHECK(has(r.programOutputs, "Color.r"));
    CHECK(has(r.programOutputs, "Color.c"));
    CHECK(!has(r.programOutputs, "gl_PointSize"));
}

// A vertex input that is an array of matrices links (SPIRV-Cross flattens
// it into one Metal attribute per column).
TEST(shader_vertex_input_matrix_array) {
    LinkResult r = link43("#version 430 core\nin mat2x3 m[2];\nvoid main() { gl_Position = vec4(m[1][1], 1.0); }\n", kPassFs);
    CHECK(r.ok);
    CHECK(r.stages[0].msl.find("m_3 [[attribute(") != std::string::npos);
}

// A user-defined struct type whose name is an MSL keyword ("vertex", as in
// Mewgenics) compiles: the type's own name is renamed, like variable, function
// and member names are (issue #77).
TEST(shader_msl_reserved_struct_type_name) {
    const char* vs =
        "#version 430 core\n"
        "struct vertex { vec4 color; vec2 texcoord; };\n"
        "in vec4 in_color;\n"
        "void main() { vertex v; v.color = in_color; v.texcoord = in_color.xy;\n"
        "              gl_Position = v.color; }\n";
    const LinkResult r = link43(vs, kPassFs);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.stages[0].msl.find("struct vertex\n") == std::string::npos);
}

// GLSL 4.30: an array of arrays with an initializer may leave any dimension
// unsized; the initializer sizes them.
TEST(shader_unsized_arrays_of_arrays_with_initializer) {
    const char* src =
        "#version 430 core\n"
        "uniform int x[2][][2][] = int[2][2][2][2](int[2][2][2](int[2][2](int[2](1, 2), int[2](3, 4)), int[2][2](int[2](5, 6), "
        "int[2](7, 8))), int[2][2][2](int[2][2](int[2](11, 12), int[2](13, 14)), int[2][2](int[2](15, 16), int[2](17, 18))));\n"
        "void main() { gl_Position = vec4(float(x[1][1][1][1])); }\n";
    const CompileResult r = compile(Stage::Vertex, src);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}

// Draw parameters and gl_DepthRange become hidden uniforms declared before
// the first code, never inside a #define line before it.
TEST(shader_builtin_uniforms_after_defines) {
    CHECK(compile(Stage::Vertex, "#version 460 core\n#define CULL 1\n#define SHIFT \\\n  7\n"
                                 "layout(location = 0) in vec2 grid;\nout vec2 v;\n"
                                 "void main() { v = grid + vec2(float(gl_BaseInstance - SHIFT * gl_DrawID));\n"
                                 "#if CULL\n  gl_Position = vec4(gl_DepthRange.near);\n#endif\n}\n").ok);
}

// wined3d's literal constants: a global const initialized from a bit cast.
TEST(shader_bitcast_global_constants) {
    CHECK(compile(Stage::Fragment, "#version 440\n"
                                   "const vec4 lc0 = intBitsToFloat(ivec4(0x3f800000 /* 1.0 */, 0, 0, 0x3f000000));\n"
                                   "const uvec2 lc1 = floatBitsToUint(vec2(1.0, 2.0));\nout vec4 o;\n"
                                   "void main() { o = lc0 + vec4(lc1.xyxy); }\n").ok);
    // A const used where a constant expression is required keeps its const.
    CHECK(compile(Stage::Fragment, "#version 440\nconst int n = 2;\nuniform vec4 v[n];\nout vec4 o;\n"
                                   "void main() { o = v[1]; }\n").ok);
}

// A sampler array inside a struct is one uniform of the array's size
// (dEQP uniform_api.info_query.array_in_struct.sampler2D_samplerCube_both).
TEST(shader_struct_sampler_array_size) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 300 es\nin vec4 p; void main() { gl_Position = p; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 300 es\nprecision mediump float;\n"
                          "struct S { sampler2D m0; samplerCube m1; sampler2D m2[3]; };\n"
                          "uniform S u_var; out vec4 color;\n"
                          "void main() { color = texture(u_var.m0, vec2(0)) + texture(u_var.m1, vec3(0)) +\n"
                          "  texture(u_var.m2[0], vec2(0)) + texture(u_var.m2[1], vec2(0)) + texture(u_var.m2[2], vec2(0)); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    const UniformInfo* m2 = findUniform(r, "u_var.m2[0]");
    CHECK(m2 && m2->size == 3);
}

// OpenGL ES contexts: GLSL ES 1.00 (no #version) compiles and links as the
// desktop GLSL compileEs makes of it, with its extensions' names.
TEST(shader_glsl_es_100) {
    const char* vs = "attribute vec4 a_pos; attribute vec2 a_uv; varying mediump vec2 v_uv;\n"
                     "void main() { v_uv = a_uv; gl_Position = a_pos; gl_PointSize = 1.0; }\n";
    const char* fs = "#extension GL_EXT_shader_texture_lod : enable\n#extension GL_OES_standard_derivatives : enable\n"
                     "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv;\n"
                     "void main() { gl_FragColor = texture2DLodEXT(u_tex, v_uv, 0.0) + vec4(dFdx(v_uv.x));\n"
                     "#if __VERSION__ == 100 && defined(GL_ES)\n gl_FragColor.a = 1.0;\n#endif\n}\n";
    EsCompileResult v = compileEs(Stage::Vertex, vs);
    EsCompileResult f = compileEs(Stage::Fragment, fs);
    CHECK(v.ok);
    CHECK(f.ok);
    if (!f.ok) std::fprintf(stderr, "%s\n", f.log.c_str());
    CHECK(f.desktop.find("gl_FragColor.a = 1.0") != std::string::npos);  // ES macros were expanded
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, v.desktop});
    in.shaders.push_back({Stage::Fragment, f.desktop});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(findUniform(r, "u_tex") != nullptr);
}

// GLSL ES's own rules: no implicit int to float conversion, a default
// float precision in fragment shaders, and no desktop #version.
TEST(shader_glsl_es_rules) {
    CHECK(!compileEs(Stage::Vertex, "void main() { float x = 1; gl_Position = vec4(x); }\n").ok);
    CHECK(!compileEs(Stage::Fragment, "void main() { float x = 1.0; gl_FragColor = vec4(x); }\n").ok);
    CHECK(!compileEs(Stage::Vertex, "#version 330\nvoid main() { gl_Position = vec4(0); }\n").ok);
    CHECK(compileEs(Stage::Vertex, "#version 300 es\nin vec4 p; void main() { gl_Position = p; }\n").ok);
}

// Uniforms in several stages have one precision (GLSL ES 1.00 section 4.5.3).
TEST(shader_glsl_es_uniform_precision) {
    const char* vs = "uniform highp float u; void main() { gl_Position = vec4(u); }\n";
    const char* fsSame = "precision mediump float; uniform highp float u; void main() { gl_FragColor = vec4(u); }\n";
    const char* fsOther = "precision mediump float; uniform mediump float u; void main() { gl_FragColor = vec4(u); }\n";
    CHECK(checkEsLink({{Stage::Vertex, vs}, {Stage::Fragment, fsSame}}).empty());
    CHECK(!checkEsLink({{Stage::Vertex, vs}, {Stage::Fragment, fsOther}}).empty());
}

// KHR_blend_equation_advanced: the fragment shader's blend_support layouts
// are reflected, and the blend variant reads color 0 by framebuffer fetch.
TEST(shader_advanced_blend_support) {
    EsCompileResult v = compileEs(Stage::Vertex, "#version 320 es\nin vec4 p; void main() { gl_Position = p; }\n");
    EsCompileResult f = compileEs(Stage::Fragment, "#version 320 es\nprecision mediump float;\n"
                                                   "layout(blend_support_multiply, blend_support_hsl_hue) out;\n"
                                                   "layout(location = 0) out vec4 o; void main() { o = vec4(1.0); }\n");
    CHECK(v.ok);
    CHECK(f.ok);
    if (!f.ok) std::fprintf(stderr, "%s\n", f.log.c_str());
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, v.desktop});
    in.shaders.push_back({Stage::Fragment, f.desktop});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.blendSupport & 1u);          // MULTIPLY
    CHECK(r.blendSupport & (1u << 11));  // HSL_HUE
    CHECK(!(r.blendSupport & (1u << 1)));
    const std::string msl = fragmentWithAdvancedBlend(r.stages[4].msl, 0);
    CHECK(msl.find("[[color(0)]]") != std::string::npos);
    CHECK(msl.find("wmgl_advanced_blend(out.") != std::string::npos);
}

// EXT_clip_cull_distance (no GLSL ES form in glslang): compiled as GLSL 4.50.
TEST(shader_glsl_es_clip_cull_distance) {
    EsCompileResult f = compileEs(Stage::Fragment,
                                  "#version 310 es\n\n#extension GL_EXT_clip_cull_distance : enable\n\nprecision highp float;\n\n"
                                  "flat in  int out_gs;\nout int out_fs;\n\nvoid main()\n{\n    if (out_gs == gl_MaxCullDistances)\n"
                                  "    {\n        out_fs = gl_MaxCullDistances;\n    }\n    else\n    {\n        out_fs = -1;\n    }\n}\n");
    CHECK(f.ok);
    if (!f.ok) std::fprintf(stderr, "%s\n", f.log.c_str());
    EsCompileResult v = compileEs(Stage::Vertex, "#version 300 es\n#extension GL_EXT_clip_cull_distance : require\n"
                                                 "in vec4 p; void main() { gl_Position = p; gl_ClipDistance[0] = p.x;"
                                                 " gl_CullDistance[0] = p.y; }\n");
    CHECK(v.ok);
    if (!v.ok) std::fprintf(stderr, "%s\n", v.log.c_str());
}

// Struct sampler array elements keep their own slots (each bound to its own
// texture), and unused leading elements still count (u_var.m3[0], size 3).
TEST(shader_struct_sampler_array_elements) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 300 es\nin vec4 p; void main() { gl_Position = p; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 300 es\nprecision mediump float;\n"
                          "struct S { sampler2D m2[2]; samplerCube m3[3]; };\n"
                          "uniform S u_var; out vec4 color;\n"
                          "void main() { color = texture(u_var.m2[0], vec2(0)) + texture(u_var.m2[1], vec2(0)) +\n"
                          "  texture(u_var.m3[2], vec3(0)); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    const UniformInfo* m2 = findUniform(r, "u_var.m2[0]");
    const UniformInfo* m3 = findUniform(r, "u_var.m3[0]");
    CHECK(m2 && m2->size == 2);
    CHECK(m3 && m3->size == 3);
    CHECK(findUniform(r, "u_var.m3[2]") == nullptr);
    if (m2 && m3) {
        const int fs = static_cast<int>(Stage::Fragment);
        const int t0 = r.samplers[m2->samplerSlot].stageTexture[fs], t1 = r.samplers[m2->samplerSlot + 1].stageTexture[fs];
        CHECK(t0 >= 0 && t1 >= 0 && t0 != t1);
        CHECK(r.samplers[m3->samplerSlot + 2].stageTexture[fs] >= 0);
    }
}

// Transform feedback of a struct output's member ("s.member"), and stage
// interfaces matched by location under different names (GLSL ES 3.10).
TEST(shader_es_struct_feedback_and_location_matching) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, compileEs(Stage::Vertex,
                                                   "#version 310 es\nstruct S { highp float a; highp vec2 b; };\n"
                                                   "out S target; layout(location = 3) out highp vec4 shaderOutput;\n"
                                                   "void main() { target.a = 1.0; target.b = vec2(2.0); shaderOutput = vec4(3.0);"
                                                   " gl_Position = vec4(0.0); }\n").desktop});
    in.shaders.push_back({Stage::Fragment, compileEs(Stage::Fragment,
                                                     "#version 310 es\nprecision mediump float;\n"
                                                     "layout(location = 3) in highp vec4 shaderInput; out vec4 o;\n"
                                                     "void main() { o = shaderInput; }\n").desktop});
    in.transformFeedbackVaryings = {"target.a", "target.b"};
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK_EQ(r.transformFeedback.size(), size_t(2));
}

// Struct sampler array elements seen out of order (another sampler between
// them) still make one uniform of the array's size.
TEST(shader_struct_sampler_array_interleaved) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 300 es\nin vec4 p; void main() { gl_Position = p; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 300 es\nprecision mediump float;\n"
                          "struct S { sampler2D m0; samplerCube m3[3]; samplerCube m1; };\n"
                          "uniform S u_var; out vec4 color;\n"
                          "void main() { color = texture(u_var.m3[0], vec3(0)) + texture(u_var.m1, vec3(0)) +\n"
                          "  texture(u_var.m3[2], vec3(0)) + texture(u_var.m0, vec2(0)); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    const UniformInfo* m3 = findUniform(r, "u_var.m3[0]");
    CHECK(m3 && m3->size == 3);
    CHECK(findUniform(r, "u_var.m3[2]") == nullptr);
}

// A struct member of an interface block with an explicit location takes it
// (dEQP-GLES31 program_input.location.interface_blocks.in.named_block.var_struct_explicit_location).
TEST(shader_block_struct_member_location) {
    LinkInput in;
    in.separable = true;
    const EsCompileResult f = compileEs(Stage::Fragment,
                                        "#version 320 es\nprecision highp float;\n"
                                        "struct S { highp vec4 target; highp vec2 more; };\n"
                                        "in TargetInterface { layout(location = 2) S target; layout(location = 5) highp vec4 after; } inst;\n"
                                        "layout(location = 0) out vec4 o;\n"
                                        "void main() { o = inst.target.target + inst.after + inst.target.more.xyxy; }\n");
    CHECK(f.ok);
    if (!f.ok) std::fprintf(stderr, "%s\n", f.log.c_str());
    in.shaders.push_back({Stage::Fragment, f.desktop});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    for (const InterfaceVariableInfo& v : r.programInputs) {
        if (v.name == "TargetInterface.target.target") CHECK_EQ(v.location, 2);
        if (v.name == "TargetInterface.target.more") CHECK_EQ(v.location, 3);
        if (v.name == "TargetInterface.after") CHECK_EQ(v.location, 5);
    }
}

TEST(shader_line_continuation_before_420) {
    // Godot 3's GLES3 canvas shader (Cassette Beasts, issue #31): multi-line
    // macros in "#version 330", which desktop drivers accept.
    const char* fs =
        "#version 330\n"
        "uniform sampler2D shadow_texture;\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "#define SHADOW_TEST(m_ofs)                                     \\\n"
        "\t{                                                          \\\n"
        "\t\thighp float sd = texture(shadow_texture, vec2(m_ofs, uv.y)).r; \\\n"
        "\t\tattenuation += step(0.5, sd);                            \\\n"
        "\t}\n"
        "void main() {\n"
        "    float attenuation = 0.0;\n"
        "    SHADOW_TEST(uv.x);\n"
        "    SHADOW_TEST(uv.x + 0.25);\n"
        "    color = vec4(attenuation);\n"
        "}\n";
    CompileResult r = compile(Stage::Fragment, fs);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330\nin vec4 p; out vec2 uv; void main() { uv = p.xy; gl_Position = p; }\n"});
    in.shaders.push_back({Stage::Fragment, fs});
    LinkResult l = link(in);
    CHECK(l.ok);
    if (!l.ok) std::fprintf(stderr, "%s\n", l.log.c_str());
    // Lines after a continued one keep their numbers in the log.
    CompileResult bad = compile(Stage::Fragment,
                                "#version 330\nout vec4 c;\n#define A(x) \\\n  (x)\nvoid main() {\n  c = undefined_thing;\n}\n");
    CHECK(!bad.ok);
    CHECK(bad.log.find("0:6:") != std::string::npos);
    if (bad.log.find("0:6:") == std::string::npos) std::fprintf(stderr, "%s\n", bad.log.c_str());
    // Without a backslash at the end of the line nothing changes; CRLF continuations join too.
    CHECK_EQ(spliceLineContinuations("a\\b\nc"), std::string("a\\b\nc"));
    CHECK_EQ(spliceLineContinuations("a \\\r\nb\nc"), std::string("a b\n\nc"));
}

// Link results kept on disk read back as they were written: a round trip of
// programs with geometry, tessellation, transform feedback, blocks, images
// and subroutines gives the same bytes and the same translations.
TEST(shader_link_results_round_trip) {
    std::vector<LinkInput> inputs(4);
    inputs[0].shaders.push_back({Stage::Vertex, "#version 330 core\nlayout(location = 3) in vec3 pos; uniform mat4 mvp;\n"
                                               "out vec2 v; void main() { v = pos.xy; gl_Position = mvp * vec4(pos, 1.0); }\n"});
    inputs[0].shaders.push_back({Stage::Fragment, "#version 330 core\nin vec2 v; uniform sampler2D tex;\n"
                                                 "layout(std140) uniform B { vec4 tint; }; out vec4 color;\n"
                                                 "void main() { color = texture(tex, v) * tint; }\n"});
    inputs[1].shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; out float w; void main() { w = 2.0; gl_Position = pos; }\n"});
    inputs[1].shaders.push_back({Stage::Geometry, "#version 330 core\nlayout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\n"
                                                 "in float w[]; out vec4 fcol;\n"
                                                 "void main() { for (int i = 0; i < 3; ++i) { fcol = vec4(w[i]); gl_Position = gl_in[i].gl_Position;"
                                                 " EmitVertex(); } EndPrimitive(); }\n"});
    inputs[1].shaders.push_back({Stage::Fragment, "#version 330 core\nin vec4 fcol; out vec4 color; void main() { color = fcol; }\n"});
    inputs[1].transformFeedbackVaryings = {"fcol", "gl_Position"};
    inputs[2].shaders.push_back({Stage::Vertex, "#version 420\nout vec4 v; void main() { v = vec4(1); gl_Position = vec4(0); }\n"});
    inputs[2].shaders.push_back({Stage::TessControl, "#version 420\nlayout(vertices = 3) out; in vec4 v[]; out vec4 c[];\n"
                                                    "void main() { c[gl_InvocationID] = v[gl_InvocationID];\n"
                                                    "gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 2.0;"
                                                    " gl_TessLevelInner[0] = 2.0; }\n"});
    inputs[2].shaders.push_back({Stage::TessEval, "#version 420\nlayout(triangles) in; in vec4 c[]; out vec4 e;\n"
                                                 "void main() { e = c[0] * gl_TessCoord.x; gl_Position = vec4(gl_TessCoord, 1.0); }\n"});
    inputs[2].shaders.push_back({Stage::Fragment, "#version 420\nin vec4 e; out vec4 o; void main() { o = e; }\n"});
    inputs[3].shaders.push_back({Stage::Vertex, "#version 430\nvoid main() { gl_Position = vec4(0); }\n"});
    inputs[3].shaders.push_back({Stage::Fragment, "#version 430\nlayout(r32ui) uniform uimage2D img;\n"
                                                 "layout(std430, binding = 1) buffer S { uint n[]; };\n"
                                                 "subroutine vec4 shade(); subroutine(shade) vec4 red() { return vec4(1, 0, 0, 1); }\n"
                                                 "subroutine uniform shade pick; out vec4 o;\n"
                                                 "void main() { imageAtomicAdd(img, ivec2(0), 1u); n[0] = 1u; o = pick(); }\n"});
    for (const LinkInput& in : inputs) {
        const LinkResult r = link(in);
        if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
        CHECK(r.ok);
        const std::vector<uint8_t> bytes = serializeLinkResult(r);
        LinkResult back;
        CHECK(deserializeLinkResult(bytes, back));
        CHECK(serializeLinkResult(back) == bytes);
        for (int s = 0; s < kStageCount; ++s) CHECK(back.stages[s].msl == r.stages[s].msl);
        CHECK(back.uniforms.size() == r.uniforms.size() && back.blocks.size() == r.blocks.size());
        CHECK(back.geometry.active == r.geometry.active && back.geometry.kernel.msl == r.geometry.kernel.msl);
        CHECK(back.tess.active == r.tess.active && (r.tess.evaluation == nullptr) == (back.tess.evaluation == nullptr));
        if (r.tess.evaluation && back.tess.evaluation)
            CHECK(serializeLinkResult(*back.tess.evaluation) == serializeLinkResult(*r.tess.evaluation));
        // Truncated data is refused, not misread.
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
        CHECK(!deserializeLinkResult(cut, back));
    }
}
