// Regression tests for the /code-review of PR #1 (pending clears, buffer
// texture hazards, mipmaps of immutable textures, vertex input layouts,
// one-sample renderbuffers, lazy fences).
#include "gpu.h"

#define WMGL_CR_FUNCS(X) \
    X(glTexStorage2D) X(glGetRenderbufferParameteriv) X(glDeleteRenderbuffers)

#define WMGL_CR_DECLARE(name) static PFN_##name name##_;
WMGL_CR_FUNCS(WMGL_CR_DECLARE)
#undef WMGL_CR_DECLARE

namespace {

void loadCodeReview() {
#define WMGL_CR_LOAD(name) name##_ = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_CR_FUNCS(WMGL_CR_LOAD)
#undef WMGL_CR_LOAD
}

const uint8_t kRed[4] = {255, 0, 0, 255}, kGreen[4] = {0, 255, 0, 255};

bool pixelIs(int x, int y, const uint8_t want[4]) {
    const std::vector<uint8_t> got = gpu::readRGBA8(x, y, 1, 1);
    if (gpu::near(got.data(), want, 2)) return true;
    std::printf("  pixel (%d,%d) = %d %d %d %d, want %d %d %d %d\n", x, y, got[0], got[1], got[2], got[3], want[0], want[1],
                want[2], want[3]);
    return false;
}

GLuint texture2D(GLenum internal, int w, int h, const void* data) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

}  // namespace

GPU_TEST(gpu_cr_pending_clear_before_later_draws) {
    // A clear of FBO A recorded while the default framebuffer's pass is open
    // runs before a later draw that samples A's texture.
    std::vector<uint8_t> green(8 * 8 * 4);
    for (size_t i = 0; i < green.size(); i += 4) { green[i + 1] = 255; green[i + 3] = 255; }
    GLuint tex = texture2D(GL_RGBA8, 8, 8, green.data());
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    GLuint sample = gpu::program(R"(#version 330 core
in vec2 position; out vec2 uv; void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); })",
                                 R"(#version 330 core
uniform sampler2D t; in vec2 uv; out vec4 o; void main() { o = texture(t, uv); })");
    GLuint vao = gpu::quad();
    glUseProgram(sample);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);  // the default framebuffer's pass is now open with a draw
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 8, 8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 64, 64);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK(pixelIs(32, 32, kRed));
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(sample);
    CHECK_GL();
}

GPU_TEST(gpu_cr_buffer_texture_write_after_draw) {
    // A draw sampling a buffer texture sees the buffer's contents at the draw,
    // not a BufferSubData issued after it.
    GLuint buffer = 0, tex = 0;
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer);
    glBufferData(GL_TEXTURE_BUFFER, sizeof(red), red, GL_DYNAMIC_DRAW);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_BUFFER, tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buffer);
    GLuint p = gpu::program(R"(#version 330 core
in vec2 position; uniform float x0; void main() { gl_Position = vec4(x0 + (position.x * 0.5 + 0.5), position.y, 0.0, 1.0); })",
                            R"(#version 330 core
uniform samplerBuffer t; out vec4 o; void main() { o = texelFetch(t, 0); })");
    GLuint vao = gpu::quad();
    glUseProgram(p);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1f(glGetUniformLocation(p, "x0"), -1.0f);  // left half
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof(green), green);
    glUniform1f(glGetUniformLocation(p, "x0"), 0.0f);  // right half
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK(pixelIs(16, 32, kRed));
    CHECK(pixelIs(48, 32, kGreen));
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_cr_generate_mipmap_immutable_short_chain) {
    loadCodeReview();
    std::vector<uint8_t> red(16 * 16 * 4);
    for (size_t i = 0; i < red.size(); i += 4) { red[i] = 255; red[i + 3] = 255; }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D_(GL_TEXTURE_2D, 3, GL_RGBA8, 16, 16);  // levels 16, 8, 4 of a 5-level chain
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    CHECK_GL();
    uint8_t level2[4 * 4 * 4] = {};
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, level2);
    CHECK(level2[0] == 255 && level2[1] == 0 && level2[3] == 255);
    GLint width = -1;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &width);
    CHECK_EQ(width, 0);  // no level beyond the storage
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_cr_fourteen_vertex_streams) {
    // 14 attributes from 14 buffers (more than Metal's 12 vertex buffer
    // slots the core uses): the draw happens, with every attribute read.
    const char* vs = R"(#version 330 core
layout(location = 0) in vec2 position;
layout(location = 1) in float a1; layout(location = 2) in float a2; layout(location = 3) in float a3;
layout(location = 4) in float a4; layout(location = 5) in float a5; layout(location = 6) in float a6;
layout(location = 7) in float a7; layout(location = 8) in float a8; layout(location = 9) in float a9;
layout(location = 10) in float a10; layout(location = 11) in float a11; layout(location = 12) in float a12;
layout(location = 13) in float a13;
out float sum;
void main() {
    sum = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13;
    gl_Position = vec4(position, 0.0, 1.0);
})";
    GLuint p = gpu::program(vs, R"(#version 330 core
in float sum; out vec4 o; void main() { o = vec4(sum / 13.0, 0.0, 0.0, 1.0); })");
    GLuint vao = 0, buffers[14] = {};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(14, buffers);
    const float tri[] = {-1, -1, 3, -1, -1, 3};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    const float ones[3] = {1, 1, 1};
    for (GLuint a = 1; a < 14; ++a) {
        glBindBuffer(GL_ARRAY_BUFFER, buffers[a]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(ones), ones, GL_STATIC_DRAW);
        glVertexAttribPointer(a, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(a);
    }
    glUseProgram(p);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK(pixelIs(32, 32, kRed));
    glDeleteBuffers(14, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(p);
    CHECK_GL();
}

GPU_TEST(gpu_cr_one_sample_renderbuffer) {
    // samples = 1 gives single-sample storage, reported (0) and compared (0) alike.
    loadCodeReview();
    GLuint rb = 0, fbo = 0;
    GLuint tex = texture2D(GL_RGBA8, 8, 8, nullptr);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, GL_DEPTH_COMPONENT24, 8, 8);
    GLint samples = -1;
    glGetRenderbufferParameteriv_(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
    CHECK_EQ(samples, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers_(1, &rb);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

GPU_TEST(gpu_cr_lazy_fence) {
    // A fence signals once its command buffer is flushed and complete.
    glClear(GL_COLOR_BUFFER_BIT);
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(sync != nullptr);
    CHECK_EQ(glClientWaitSync(sync, 0, 0), static_cast<GLenum>(GL_TIMEOUT_EXPIRED));  // not flushed yet
    const GLenum waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
    CHECK(waited == GL_CONDITION_SATISFIED || waited == GL_ALREADY_SIGNALED);
    GLint status = 0;
    glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &status);
    CHECK_EQ(status, GL_SIGNALED);
    glDeleteSync(sync);
    CHECK_GL();
}

GPU_TEST(gpu_cr_fence_timeout_past_the_clock) {
    // wined3d waits ~0 >> 1 ns for its fences: past the clock's range, which
    // must wait for the fence, not time out at once (Crusader Kings II
    // stalled loading with every wined3d fence wait failing).
    for (const GLuint64 timeout : {~static_cast<GLuint64>(0) >> 1, ~static_cast<GLuint64>(0) - 1}) {
        glClear(GL_COLOR_BUFFER_BIT);
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        CHECK(sync != nullptr);
        const GLenum waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
        CHECK(waited == GL_CONDITION_SATISFIED || waited == GL_ALREADY_SIGNALED);
        glDeleteSync(sync);
    }
    CHECK_GL();
}

GPU_TEST(gpu_cr_fence_on_idle_stream) {
    // With nothing recorded, a fence covers the committed work and signals
    // without a flush (tests/wine/opengl_sync.c polls it with timeout 0).
    glFinish();
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(sync != nullptr);
    GLint status = 0;
    glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &status);
    CHECK_EQ(status, GL_SIGNALED);
    CHECK_EQ(glClientWaitSync(sync, 0, 0), static_cast<GLenum>(GL_ALREADY_SIGNALED));
    glDeleteSync(sync);
    // Work flushed but possibly still running: the fence waits for it.
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const GLenum waited = glClientWaitSync(sync, 0, 2000000000ull);
    CHECK(waited == GL_CONDITION_SATISFIED || waited == GL_ALREADY_SIGNALED);
    glDeleteSync(sync);
    CHECK_GL();
}

GPU_TEST(gpu_cr_buffer_range_checks) {
    GLuint buffers[2] = {};
    glGenBuffers(2, buffers);
    glBindBuffer(GL_COPY_READ_BUFFER, buffers[0]);
    glBufferData(GL_COPY_READ_BUFFER, 256, nullptr, GL_STATIC_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[1]);
    glBufferData(GL_COPY_WRITE_BUFFER, 256, nullptr, GL_STATIC_DRAW);
    // An offset and size whose sum wraps are out of range, not a write.
    const char data[16] = {};
    glBufferSubData(GL_COPY_READ_BUFFER, INT64_MAX - 8, 16, data);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    char out[16];
    glGetBufferSubData(GL_COPY_READ_BUFFER, INT64_MAX - 8, 16, out);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, INT64_MAX - 8, 0, 16);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    // Any non-persistent mapping fails reads and copies, wherever it is
    // (GL 4.6 sections 6.6, 6.7); sub-data updates only where they overlap.
    CHECK(glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, 64, GL_MAP_WRITE_BIT) != nullptr);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 128, 16);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glGetBufferSubData(GL_COPY_WRITE_BUFFER, 128, 16, out);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBufferSubData(GL_COPY_WRITE_BUFFER, 128, 16, data);
    CHECK_GL();
    glUnmapBuffer(GL_COPY_WRITE_BUFFER);
    glDeleteBuffers(2, buffers);
    CHECK_GL();
}
