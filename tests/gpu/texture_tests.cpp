// Texture specification, sampling and read-back.
#include <cmath>
#include <cstring>

#include "gpu.h"

namespace {

const char* kQuadVs = R"(#version 330 core
in vec2 position;
out vec2 uv;
void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); })";

// Draws `fs` over the whole framebuffer and returns the centre pixel.
std::vector<uint8_t> drawAndRead(const char* fs, int x = 32, int y = 32) {
    GLuint p = gpu::program(kQuadVs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    auto pixel = gpu::readRGBA8(x, y, 1, 1);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    return pixel;
}

GLuint texture2D(GLenum internalFormat, int w, int h, GLenum format, GLenum type, const void* data) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0, format, type, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

}  // namespace

GPU_TEST(gpu_texture_rgba8_sample) {
    const uint8_t texels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GLuint tex = texture2D(GL_RGBA8, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    // Texel (0, 0) is the bottom-left quarter.
    const uint8_t red[4] = {255, 0, 0, 255}, white[4] = {255, 255, 255, 255};
    CHECK(gpu::near(drawAndRead(fs, 8, 8).data(), red));
    CHECK(gpu::near(drawAndRead(fs, 56, 56).data(), white));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_formats_roundtrip) {
    struct Case { GLenum internal, format, type; int bytes; };
    const Case cases[] = {
        {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1},       {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2},
        {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, 3},     {GL_RGBA16F, GL_RGBA, GL_FLOAT, 16},
        {GL_R32F, GL_RED, GL_FLOAT, 4},             {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4},
        {GL_R32I, GL_RED_INTEGER, GL_INT, 4},       {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4},
        {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}, {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4},
        {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4},
    };
    for (const Case& c : cases) {
        const int w = 5, h = 3;
        std::vector<uint8_t> in(static_cast<size_t>(w) * h * c.bytes), out(in.size(), 0xcd);
        for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
        if (c.type == GL_FLOAT) {
            auto* f = reinterpret_cast<float*>(in.data());
            for (size_t i = 0; i < in.size() / 4; ++i) f[i] = c.internal == GL_DEPTH_COMPONENT32F ? (i % 7) / 7.0f : i * 0.25f;
        }
        if (c.internal == GL_DEPTH24_STENCIL8) {
            // Depth is 24-bit; keep values exactly representable through the float storage.
            auto* v = reinterpret_cast<uint32_t*>(in.data());
            for (size_t i = 0; i < in.size() / 4; ++i) v[i] = (static_cast<uint32_t>(i * 4096) << 8) | (i & 0xff);
        }
        GLuint tex = texture2D(c.internal, w, h, c.format, c.type, in.data());
        CHECK_GL();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, c.format, c.type, out.data());
        CHECK_GL();
        const bool equal = in == out;
        if (!equal) std::fprintf(stderr, "  round trip mismatch for internal format 0x%x\n", c.internal);
        CHECK(equal);
        glDeleteTextures(1, &tex);
    }
}

GPU_TEST(gpu_texture_rgb_alpha_is_one) {
    const uint8_t texel[3] = {10, 20, 30};
    GLuint tex = texture2D(GL_RGB8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, texel);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {10, 20, 30, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_integer_sampler) {
    const uint32_t texel = 4000000000u;
    GLuint tex = texture2D(GL_R32UI, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &texel);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform usampler2D tex; out vec4 o;
void main() { o = texture(tex, uv).r == 4000000000u ? vec4(0, 1, 0, 1) : vec4(1, 0, 0, 1); })";
    const uint8_t green[4] = {0, 255, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), green));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_swizzle_and_sampler_object) {
    const uint8_t texel[4 * 4] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    GLuint tex = texture2D(GL_RGBA8, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ZERO);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    // The texture alone is incomplete (mipmapped min filter, level 1 missing) but
    // the sampler object overrides the filter.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindSampler(0, sampler);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {0, 255, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glBindSampler(0, 0);
    // Without the sampler the texture is incomplete and samples (0, 0, 0, 1).
    const uint8_t black[4] = {0, 0, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), black));
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_generate_mipmap) {
    std::vector<uint8_t> texels(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        const bool white = (i % 2) == ((i / 4) % 2);
        for (int c = 0; c < 3; ++c) texels[i * 4 + c] = white ? 255 : 0;
        texels[i * 4 + 3] = 255;
    }
    GLuint tex = texture2D(GL_RGBA8, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    GLint w = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &w);
    CHECK_EQ(w, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = textureLod(tex, uv, 2.0); })";
    const auto pixel = drawAndRead(fs);
    CHECK(pixel[0] > 100 && pixel[0] < 155);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_array_and_3d) {
    std::vector<uint8_t> layers(2 * 2 * 3 * 4);
    for (int layer = 0; layer < 3; ++layer)
        for (int i = 0; i < 4; ++i) {
            uint8_t* t = &layers[(layer * 4 + i) * 4];
            t[0] = static_cast<uint8_t>(layer * 100);
            t[1] = 7;
            t[2] = 9;
            t[3] = 255;
        }
    GLuint tex[2];
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex[0]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, layers.data());
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2DArray tex; out vec4 o;
void main() { o = texture(tex, vec3(uv, 2.0)); })";
    auto pixel = drawAndRead(fs);
    CHECK_EQ(static_cast<int>(pixel[0]), 200);
    glBindTexture(GL_TEXTURE_3D, tex[1]);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, layers.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    std::vector<uint8_t> out(layers.size());
    glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    CHECK(out == layers);
    const char* fs3 = R"(#version 330 core
in vec2 uv; uniform sampler3D tex; out vec4 o;
void main() { o = texture(tex, vec3(uv, 0.5)); })";
    pixel = drawAndRead(fs3);
    CHECK_EQ(static_cast<int>(pixel[0]), 100);
    glDeleteTextures(2, tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_buffer) {
    const float values[4] = {0.0f, 0.25f, 0.5f, 1.0f};
    GLuint buffer = 0, tex = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, buffer);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform samplerBuffer tex; out vec4 o;
void main() { o = vec4(texelFetch(tex, 3).r, texelFetch(tex, 1).r, 0, 1); })";
    const uint8_t want[4] = {255, 64, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &buffer);
    CHECK_GL();
}

GPU_TEST(gpu_texture_rgtc_roundtrip) {
    // One BC4 block: endpoints 255 and 0, all indices 0 -> red 255.
    const uint8_t block[8] = {255, 0, 0, 0, 0, 0, 0, 0};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1, 4, 4, 0, sizeof(block), block);
    CHECK_GL();
    GLint compressed = 0, size = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
    CHECK_EQ(compressed, 1);
    CHECK_EQ(size, 8);
    uint8_t out[8] = {};
    glGetCompressedTexImage(GL_TEXTURE_2D, 0, out);
    CHECK(std::memcmp(out, block, 8) == 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tex; out vec4 o;
void main() { o = texture(tex, uv); })";
    const uint8_t want[4] = {255, 0, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_errors) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, -1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindTexture(GL_TEXTURE_3D, tex);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindTexture(GL_TEXTURE_2D, 12345);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, -1);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    // Proxy: too large is not an error, it zeroes the proxy image.
    glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, 1 << 20, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK_GL();
    GLint w = -1;
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    CHECK_EQ(w, 0);
    CHECK(glIsTexture(tex));
    glDeleteTextures(1, &tex);
    CHECK(!glIsTexture(tex));
    CHECK_GL();
}

GPU_TEST(gpu_texture_rectangle_fetch) {
    const uint8_t texels[4] = {32, 64, 96, 128};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_RECTANGLE, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_R8, 2, 2, 0, GL_RED, GL_UNSIGNED_BYTE, texels);
    CHECK_GL();
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2DRect tex; out vec4 o;
void main() { o = vec4(texelFetch(tex, ivec2(1, 0)).r, texture(tex, vec2(0.5, 1.5)).r, 0, 1); })";
    const uint8_t want[4] = {64, 96, 0, 255};
    CHECK(gpu::near(drawAndRead(fs).data(), want));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_texture_multisample_integer_fetch) {
    // Render into a multisampled RGB10_A2UI texture, then fetch a sample with usampler2DMS.
    GLuint ms = 0, fb = 0;
    glGenTextures(1, &ms);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ms);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGB10_A2UI, 8, 8, GL_TRUE);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, ms, 0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    const char* writeFs = R"(#version 330 core
out uvec4 o;
void main() { o = uvec4(4u, 5u, 6u, 1u); })";
    GLuint writer = gpu::program(R"(#version 330 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })", writeFs);
    GLuint vao = gpu::quad();
    glViewport(0, 0, 8, 8);
    glUseProgram(writer);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    const char* readFs = R"(#version 330 core
in vec2 uv; uniform usampler2DMS tex; out vec4 o;
void main() { uvec4 v = texelFetch(tex, ivec2(2, 2), 1); o = vec4(float(v.r) / 8.0, float(v.g) / 8.0, float(v.b) / 8.0, 1.0); })";
    const uint8_t want[4] = {128, 159, 191, 255};
    CHECK(gpu::near(drawAndRead(readFs).data(), want, 2));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(writer);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &ms);
    CHECK_GL();
}

GPU_TEST(gpu_texture_lod_bias_vertex) {
    // TEXTURE_LOD_BIAS applies to explicit-LOD lookups too (GL 3.3 section 3.8.11).
    static const uint8_t colors[9][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255},
                                         {255, 0, 255}, {255, 128, 128}, {128, 255, 128}, {128, 128, 255}};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int level = 0, size = 256; level <= 8; ++level, size >>= 1) {
        std::vector<uint8_t> data(static_cast<size_t>(size) * size * 3);
        for (size_t i = 0; i < data.size(); i += 3) std::memcpy(&data[i], colors[level], 3);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGB, size, size, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    const float bias = 2.519684f;
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &bias);
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; out vec4 tex; uniform sampler2D texture0; uniform float lodbase;
void main() { gl_Position = vec4(position, 0.0, 1.0); tex = textureLod(texture0, position * 0.5 + 0.5, lodbase); })",
                            R"(#version 330 core
in vec4 tex; out vec4 frag; void main() { frag = tex; })");
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, "lodbase"), 0.0f);
    GLuint vao = gpu::quad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const std::vector<uint8_t> px = gpu::readRGBA8(0, 0, 1, 1);
    const uint8_t want[4] = {132, 132, 122, 255};
    CHECK(gpu::near(px.data(), want, 5));
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

// Uploads between draws of one pass: a texture the command buffer has
// sampled gets the upload in order (the draw before it sees the old texels),
// one it has not used may get it ahead (the draw after it sees the new).
GPU_TEST(gpu_texture_upload_between_draws_keeps_order) {
    static const char* vs = R"(#version 330
in vec2 position;
uniform float x;
out vec2 uv;
void main() { uv = vec2(0.5); gl_Position = vec4(position.x * 0.25 + x, position.y, 0.0, 1.0); })";
    static const char* fs = R"(#version 330
in vec2 uv;
uniform sampler2D tex;
out vec4 o;
void main() { o = texture(tex, uv); })";
    GLuint p = gpu::program(vs, fs);
    GLuint vao = gpu::quad();
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    GLuint tex[2];
    glGenTextures(2, tex);
    for (GLuint t : tex) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glFinish();
    glUseProgram(p);
    glBindVertexArray(vao);
    const GLint x = glGetUniformLocation(p, "x");
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // tex[0]: sampled, then uploaded, then sampled again.
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glUniform1f(x, -0.75f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glUniform1f(x, -0.25f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // tex[1]: not used yet in this command buffer: uploaded, then sampled.
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
    glUniform1f(x, 0.25f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int y = viewport[3] / 2, w = viewport[2];
    CHECK(gpu::near(gpu::readRGBA8(w * 1 / 8, y, 1, 1).data(), red));
    CHECK(gpu::near(gpu::readRGBA8(w * 3 / 8, y, 1, 1).data(), green));
    CHECK(gpu::near(gpu::readRGBA8(w * 5 / 8, y, 1, 1).data(), blue));
    glDeleteTextures(2, tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

// glTexSubImage2D from a pixel unpack buffer in the stored layout: a GPU
// copy in command order, after GPU writes to the buffer, with the unpack
// row length and skips applied.
GPU_TEST(gpu_texture_upload_from_unpack_buffer) {
    std::vector<uint32_t> texels(16 * 16);
    for (size_t i = 0; i < texels.size(); ++i) texels[i] = 0xff000000u | static_cast<uint32_t>(i * 2654435761u >> 8);
    GLuint source = 0, pbo = 0, tex = 0;
    glGenBuffers(1, &source);
    glBindBuffer(GL_COPY_READ_BUFFER, source);
    glBufferData(GL_COPY_READ_BUFFER, texels.size() * 4, texels.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, pbo);
    glBufferData(GL_COPY_WRITE_BUFFER, texels.size() * 4, nullptr, GL_STREAM_DRAW);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, texels.size() * 4);  // GPU-written
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 16);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    std::vector<uint32_t> back(8 * 8);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
    bool same = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) same = same && back[static_cast<size_t>(y * 8 + x)] == texels[static_cast<size_t>((y + 2) * 16 + x + 4)];
    CHECK(same);
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &pbo);
    glDeleteBuffers(1, &source);
    CHECK_GL();
}

// Depth comparisons clamp the reference to [0, 1] for fixed-point depth
// formats and not for floating-point ones (GL 4.6 section 8.23.1). Metal
// stores 24-bit depth as Depth32Float and compared it unclamped: OpenMW's
// shadow maps (DEPTH_COMPONENT24) shadowed everything beyond their far plane
// (issue #42). texture, textureProj and textureGather, past both ends.
GPU_TEST(gpu_texture_shadow_reference_clamp) {
    GLuint p = gpu::program(R"(#version 400 core
in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); })",
                            R"(#version 400 core
uniform sampler2DShadow shadow;
uniform float ref;
out vec4 o;
void main() {
    o = vec4(texture(shadow, vec3(0.5, 0.5, ref)), textureProj(shadow, vec4(1.0, 1.0, 2.0 * ref, 2.0)),
             textureGather(shadow, vec2(0.5), ref).x, 1.0);
})");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "shadow"), 0);
    GLuint vao = gpu::quad();
    struct Case {
        float clear;
        GLenum func;
        float ref;
    } cases[] = {{1.0f, GL_LEQUAL, 1.5f}, {0.0f, GL_GEQUAL, -0.5f}};
    for (GLenum format : {GLenum(GL_DEPTH_COMPONENT16), GLenum(GL_DEPTH_COMPONENT24), GLenum(GL_DEPTH24_STENCIL8),
                          GLenum(GL_DEPTH_COMPONENT32F)}) {
        const bool fixedPoint = format != GL_DEPTH_COMPONENT32F;
        GLuint tex = 0, fb = 0;
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, 4, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glGenFramebuffers(1, &fb);
        for (const Case& c : cases) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferTexture2D(GL_FRAMEBUFFER, format == GL_DEPTH24_STENCIL8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D, tex, 0);
            glClearDepth(c.clear);
            glClear(GL_DEPTH_BUFFER_BIT);
            glClearDepth(1.0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, c.func);
            glUniform1f(glGetUniformLocation(p, "ref"), c.ref);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const auto px = gpu::readRGBA8(32, 32, 1, 1);
            // Clamped, the reference equals the stored depth and the comparison passes.
            const uint8_t want = fixedPoint ? 255 : 0;
            for (int k = 0; k < 3; ++k)
                if (px[k] != want) {
                    std::printf("  format 0x%x, reference %g: lookup %d gave %d, want %d\n", format, c.ref, k, px[k], want);
                    CHECK(false);
                }
        }
        CHECK_GL();
        glDeleteFramebuffers(1, &fb);
        glDeleteTextures(1, &tex);
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
}

// Samplers a program declares but never uses are inactive and take no
// texture slot, as in GL: Iris shader packs declare dozens through shared
// headers and use a few per program (Rethinking Voxels' prepare1 failed to
// link with "too many samplers in one stage").
GPU_TEST(gpu_texture_unused_samplers_inactive) {
    std::string fs = "#version 330 core\nin vec2 uv;\nout vec4 o;\n";
    for (int i = 0; i < 24; ++i) fs += "uniform sampler2D t" + std::to_string(i) + ";\n";
    fs += "void main() { o = vec4(texture(t3, uv).r * 0.0, texture(t20, uv).g, 0.0, 1.0); }\n";
    static const char* vs = R"(#version 330 core
in vec2 position;
out vec2 uv;
void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); })";
    GLuint p = gpu::program(vs, fs.c_str());
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    if (!linked) return;
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    GLuint tex[2];
    glGenTextures(2, tex);
    for (int i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, i ? green : red);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t3"), 0);
    glUniform1i(glGetUniformLocation(p, "t20"), 1);
    GLuint vao = gpu::quad();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), green));
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(2, tex);
    glDeleteProgram(p);
    CHECK_GL();
}

// Foundry Mari's #version 330 viewport shaders (issue #88): a sampler array
// indexed by a uniform, and locals named dfdx and dfdy, which are Metal
// functions, next to the dFdx calls that become them.
GPU_TEST(gpu_texture_variable_indexed_sampler_array_330) {
    const uint8_t colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    GLuint tex[3];
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        tex[i] = texture2D(GL_RGBA8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, colors[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    const char* fs = R"(#version 330 core
in vec2 uv; uniform sampler2D tiles[3]; uniform int layer; out vec4 o;
void main() {
    vec3 F = vec3(uv, 0.0);
    vec3 dfdx = dFdx(F);
    vec3 dfdy = dFdy(F);
    o = texture(tiles[layer], uv) + vec4(step(1.0, length(cross(dfdx, dfdy))) * 0.0);
})";
    GLuint p = gpu::program(kQuadVs, fs);
    GLuint vao = gpu::quad();
    glUseProgram(p);
    const GLint units[3] = {0, 1, 2};
    glUniform1iv(glGetUniformLocation(p, "tiles"), 3, units);
    for (int i = 0; i < 3; ++i) {
        glUniform1i(glGetUniformLocation(p, "layer"), i);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        CHECK(gpu::near(gpu::readRGBA8(32, 32, 1, 1).data(), colors[i]));
    }
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    glDeleteTextures(3, tex);
    CHECK_GL();
}
