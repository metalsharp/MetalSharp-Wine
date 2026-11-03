// Game extensions on 3.3 contexts (W23): ARB_copy_image,
// ARB_internalformat_query, KHR_robustness, ARB_invalidate_subdata.
#include "gpu.h"

#define WMGL_EXT_FUNCS(X)                                                                                    \
    X(glCopyImageSubData) X(glReadnPixels) X(glGetnUniformfv) X(glInvalidateFramebuffer) \
    X(glInvalidateBufferData) X(glGetGraphicsResetStatus) X(glGetProgramBinary) X(glProgramBinary)

#define WMGL_EXT_DECLARE(name) static PFN_##name name;
WMGL_EXT_FUNCS(WMGL_EXT_DECLARE)
#undef WMGL_EXT_DECLARE

namespace {

void loadExtensions() {
#define WMGL_EXT_LOAD(name) name = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_EXT_FUNCS(WMGL_EXT_LOAD)
#undef WMGL_EXT_LOAD
}

bool advertised(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
        if (!std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))), name)) return true;
    return false;
}

GLuint texture2D(GLenum internal, GLenum format, GLenum type, int w, int h, const void* data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);  // complete: glCopyImageSubData requires it
    return t;
}

}  // namespace

GPU_TEST(gpu_copy_image) {
    loadExtensions();
    CHECK(advertised("GL_ARB_copy_image"));
    uint8_t pixels[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) {
        pixels[i * 4] = static_cast<uint8_t>(i * 16);
        pixels[i * 4 + 1] = 1;
        pixels[i * 4 + 2] = 2;
        pixels[i * 4 + 3] = 255;
    }
    GLuint src = texture2D(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4, pixels);
    GLuint dst = texture2D(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4, nullptr);
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 1, 1, 0, dst, GL_TEXTURE_2D, 0, 0, 0, 0, 2, 2, 1);
    CHECK_GL();
    uint8_t out[4 * 4 * 4] = {};
    glBindTexture(GL_TEXTURE_2D, dst);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    CHECK_EQ(out[0], pixels[(1 * 4 + 1) * 4]);   // dst (0,0) <- src (1,1)
    CHECK_EQ(out[4], pixels[(1 * 4 + 2) * 4]);   // dst (1,0) <- src (2,1)
    CHECK_EQ(out[16], pixels[(2 * 4 + 1) * 4]);  // dst (0,1) <- src (1,2)
    // A size-compatible format (RGBA8 -> R32UI) copies the bytes.
    GLuint raw = texture2D(GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 4, 4, nullptr);
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0, raw, GL_TEXTURE_2D, 0, 0, 0, 0, 4, 4, 1);
    CHECK_GL();
    uint32_t words[16] = {};
    glBindTexture(GL_TEXTURE_2D, raw);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, words);
    uint32_t expect;
    std::memcpy(&expect, pixels + 4 * 5, 4);
    CHECK_EQ(words[5], expect);
    // Incompatible sizes are rejected.
    GLuint wide = texture2D(GL_RGBA16F, GL_RGBA, GL_FLOAT, 4, 4, nullptr);
    glCopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0, wide, GL_TEXTURE_2D, 0, 0, 0, 0, 4, 4, 1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLuint textures[4] = {src, dst, raw, wide};
    glDeleteTextures(4, textures);
    CHECK_GL();
}

GPU_TEST(gpu_internalformat_query) {
    loadExtensions();
    CHECK(advertised("GL_ARB_internalformat_query"));
    GLint count = 0;
    glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &count);
    CHECK(count >= 1);
    GLint samples[8] = {};
    glGetInternalformativ(GL_RENDERBUFFER, GL_RGBA8, GL_SAMPLES, 8, samples);
    GLint max = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    CHECK_EQ(samples[0], max);  // highest first
    // GL 4.3 (GL_ARB_internalformat_query2): a single-sample target has no sample counts.
    glGetInternalformativ(GL_TEXTURE_2D, GL_RGBA8, GL_NUM_SAMPLE_COUNTS, 1, &count);
    CHECK_EQ(count, 0);
    CHECK_GL();
}

GPU_TEST(gpu_robustness_bounded_queries) {
    loadExtensions();
    CHECK(advertised("GL_KHR_robustness"));
    CHECK_EQ(glGetGraphicsResetStatus(), static_cast<GLenum>(GL_NO_ERROR));
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t px[8] = {};
    glReadnPixels(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4, px);  // needs 8 bytes
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    CHECK_EQ(px[0], 0);
    glReadnPixels(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 8, px);
    CHECK_GL();
    CHECK_EQ(px[0], 255);
    GLuint p = gpu::program("#version 330 core\nuniform vec4 u; void main() { gl_Position = u; }\n",
                            "#version 330 core\nout vec4 o; void main() { o = vec4(1.0); }\n");
    const GLint u = glGetUniformLocation(p, "u");
    glUseProgram(p);
    glUniform4f(u, 1, 2, 3, 4);
    float v[4] = {};
    glGetnUniformfv(p, u, 8, v);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glGetnUniformfv(p, u, 16, v);
    CHECK(v[3] == 4);
    glUseProgram(0);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_invalidate_subdata) {
    loadExtensions();
    CHECK(advertised("GL_ARB_invalidate_subdata"));
    const GLenum color = GL_COLOR;
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &color);
    CHECK_GL();
    const GLenum bad = GL_COLOR_ATTACHMENT0;  // not valid for the default framebuffer
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &bad);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glInvalidateBufferData(12345);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    CHECK_GL();
}

GPU_TEST(gpu_program_binary_roundtrip) {
    loadExtensions();
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    CHECK_EQ(formats, 1);
    GLuint p = gpu::program(R"(#version 330 core
const vec2 v[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
void main() { gl_Position = vec4(v[gl_VertexID], 0.0, 1.0); })",
                            "#version 330 core\nuniform vec4 tint; out vec4 o; void main() { o = tint; }\n");
    GLint length = 0;
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &length);
    CHECK(length > 0);
    std::vector<uint8_t> blob(static_cast<size_t>(length));
    GLsizei written = 0;
    GLenum format = 0;
    glGetProgramBinary(p, length, &written, &format, blob.data());
    CHECK_EQ(written, length);
    GLuint restored = glCreateProgram();
    glProgramBinary(restored, format, blob.data(), written);
    GLint linked = 0;
    glGetProgramiv(restored, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(restored);
    glUniform4f(glGetUniformLocation(restored, "tint"), 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    // A foreign binary leaves the program unlinked without an error.
    const uint8_t junk[16] = {1, 2, 3};
    glProgramBinary(restored, format, junk, sizeof(junk));
    glGetProgramiv(restored, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_FALSE);
    glUseProgram(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteProgram(restored);
    CHECK_GL();
}

TEST(gpu_legacy_extension_string) {
    // Issue #75: Jedi Academy printed GL_EXTENSIONS through a 4096-byte buffer
    // and overran it. A context asked for as OpenGL 1.x keeps the string under
    // 3900 bytes, oldest extensions first; glGetStringi still lists them all,
    // and contexts asked for as 3.x or later get the whole string.
    std::string legacy, last;
    GLint legacyCount = 0;
    {
        gpu::Fixture fixture(64, 64, false, 10);
        CHECK(fixture.ok);
        if (!fixture.ok) return;
        legacy = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        glGetIntegerv(GL_NUM_EXTENSIONS, &legacyCount);
        last = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(legacyCount - 1)));
    }
    CHECK(legacy.size() <= 3900);
    CHECK(legacy.find("GL_ARB_multitexture") != std::string::npos);
    CHECK(legacy.back() != ' ');
    gpu::Fixture fixture(64, 64, false, 46);
    CHECK(fixture.ok);
    if (!fixture.ok) return;
    const std::string full = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    CHECK_EQ(count, legacyCount);
    CHECK(full.size() > legacy.size());
    CHECK(full.find(last) != std::string::npos);
    CHECK(legacy.find(last) == std::string::npos);
    CHECK_EQ(full.compare(0, legacy.size(), legacy), 0);
}
