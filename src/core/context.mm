#include "core/context.h"

#include "core/metal/pipeline_cache.h"

#include <cstdarg>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/compat/display_list.h"
#include "core/compat/immediate.h"
#include "core/draw.h"
#include "core/metal/device.h"
#include "core/program.h"
#include "core/util/log.h"

#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <string>
#include "core/util/version.h"

namespace wmgl {
namespace {

thread_local Context* t_current = nullptr;

}  // namespace

thread_local int ShareLock::depth = 0;
thread_local int CallPool::depth = 0;

void waitForGpuUnlocked(Context& ctx, uint64_t serial, bool keepBusy) {
    if (!serial) return;
    CommandStream& stream = ctx.stream();
    if (stream.queue().isComplete(serial)) return;
    // The callers read buffers: a pending clear stays pending.
    if (!stream.queue().isCommitted(serial)) {
        stream.flush(true);
    } else if (keepBusy && stream.drawsSinceCommit()) {
        // Draws recorded since the serial was committed run while the CPU
        // waits, instead of after it (the GPU would idle meanwhile). Opt-in
        // (WINEMETALGL_WAIT_FLUSH=1): the commit splits the render pass in
        // progress, which cost more than it saved in the Foundation probe.
        static const bool enabled = [] {
            const char* v = std::getenv("WINEMETALGL_WAIT_FLUSH");
            return v && v[0] == '1';
        }();
        if (enabled) stream.flush(true);
    }
    ShareUnlock unlock(ctx);
    stream.waitFor(serial);
}

void finishUnlocked(Context& ctx) { waitForGpuUnlocked(ctx, ctx.stream().flush()); }

bool g_traceCalls = [] {
    const char* value = std::getenv("WINEMETALGL_TRACE");
    return value && value[0] == '1';
}();

void traceCall(const char* command) {
    std::fprintf(stderr, "winemetalgl:call: %s\n", command);
}

Context* currentContext() { return t_current; }

void setCurrentContext(Context* context) { t_current = context; }

void noCurrentContext(const char* command) {
    logOnce(LogLevel::Warn, LogCategory::Context, command, "%s called without a current context", command);
}

Context::Context(RefPtr<ShareGroup> share, const ContextConfig& config, int pixelFormat)
    : config_(config), share_(std::move(share)), stream_(share_->queue) {
    (void)pixelFormat;
    startPipelineArchive();
    defaultVertexArray = makeRef<VertexArray>(0);
    defaultTransformFeedback = makeRef<TransformFeedback>(0);
    state.vertexArray = defaultVertexArray;
    state.transformFeedback = defaultTransformFeedback;
    for (int i = 0; i < kTexTargetCount; ++i) {
        defaultTextures[i] = makeRef<Texture>(0, textureTargetEnum(i));
        proxyTextures[i] = makeRef<Texture>(0, textureTargetEnum(i));
        for (TextureUnit& unit : state.textureUnits) unit.bound[i] = defaultTextures[i];
    }
    draw_ = std::make_unique<DrawState>(*this);
    state.debugOutput = config_.debug;  // DEBUG_OUTPUT starts enabled in debug contexts
    if (config_.es) {
        // OpenGL ES has no switches for these: shaders' gl_PointSize sizes
        // points, cube maps are seamless and sRGB framebuffers convert.
        state.programPointSize = true;
        state.textureCubeMapSeamless = true;
        state.framebufferSRGB = true;
        // Image units start with format R32UI in OpenGL ES 3.1 (R8 on desktop).
        for (auto& unit : state.imageUnits) unit.format = GL_R32UI;
    }
    initStrings();
}

Context::~Context() {
    stream_.finish();
    if (current) --share_->currentContexts;
    {
        ShareLock lock(*this);
        api::releaseCurrentProgram(*this);
    }
    draw_.reset();
}

void Context::initStrings() {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%d.%d.0 WineMetalGL %s", config_.major, config_.minor,
                  config_.coreProfile ? "Core Profile" : "Compatibility Profile");
    versionString_ = buffer;
    glslVersionString_ = config_.major >= 4                                                ? "4." + std::to_string(config_.minor) + "0"
                         : config_.major == 3 && config_.minor >= 3                       ? "3.30"
                         : config_.major == 3 && config_.minor == 2                       ? "1.50"
                         : config_.major == 3 && config_.minor == 1                       ? "1.40"
                         : config_.major == 3                                             ? "1.30"
                                                                                          : "1.20";
    // Only extensions this core implements completely are advertised.
    extensions_.clear();
    if (!config_.coreProfile) {
        // The compatibility profile (W22), and the extensions GL 1.2-2.1
        // made core that engines of that era still test for.
        extensions_.push_back("GL_ARB_compatibility");
        for (const char* name :
             {"GL_ARB_multitexture", "GL_ARB_transpose_matrix", "GL_ARB_texture_env_add", "GL_EXT_texture_env_add",
              "GL_ARB_texture_env_combine", "GL_EXT_texture_env_combine", "GL_ARB_texture_env_dot3",
              "GL_ARB_texture_env_crossbar", "GL_ARB_texture_cube_map", "GL_EXT_texture_cube_map",
              "GL_ARB_texture_border_clamp", "GL_ARB_texture_compression", "GL_ARB_texture_mirrored_repeat",
              "GL_ARB_depth_texture", "GL_ARB_shadow", "GL_ARB_window_pos", "GL_ARB_point_parameters",
              "GL_ARB_point_sprite", "GL_EXT_secondary_color", "GL_EXT_fog_coord", "GL_EXT_blend_func_separate",
              "GL_EXT_blend_minmax", "GL_EXT_blend_subtract", "GL_EXT_blend_color", "GL_EXT_stencil_wrap",
              "GL_EXT_texture_lod_bias", "GL_EXT_bgra", "GL_EXT_draw_range_elements", "GL_EXT_compiled_vertex_array",
              "GL_EXT_rescale_normal", "GL_EXT_separate_specular_color", "GL_ARB_vertex_buffer_object",
              "GL_ARB_texture_non_power_of_two", "GL_ARB_occlusion_query", "GL_ARB_draw_buffers",
              "GL_ARB_texture_float", "GL_ARB_half_float_pixel", "GL_ARB_pixel_buffer_object",
              "GL_EXT_pixel_buffer_object", "GL_SGIS_generate_mipmap", "GL_ARB_texture_rectangle",
              "GL_NV_texture_rectangle", "GL_NV_blend_square", "GL_EXT_packed_pixels",
              "GL_SGIS_texture_edge_clamp", "GL_ARB_multisample", "GL_EXT_texture3D", "GL_EXT_blend_equation_separate",
              "GL_ARB_shading_language_100", "GL_ARB_shader_objects", "GL_ARB_vertex_shader", "GL_ARB_fragment_shader",
              // Assembly programs, translated to GLSL (compat/arb_programs.h).
              "GL_ARB_vertex_program", "GL_ARB_fragment_program", "GL_ARB_fragment_program_shadow",
              "GL_EXT_gpu_program_parameters",
              // Compressed luminance formats (RGTC blocks). ATI's 3Dc token
              // (GL_COMPRESSED_LUMINANCE_ALPHA_3DC_ATI, HPL3's normal maps) is
              // accepted too; its extension was never registered, so it is
              // not advertised.
              "GL_EXT_texture_compression_latc"})
            extensions_.push_back(name);
    }
    extensions_.push_back("GL_ARB_vertex_type_2_10_10_10_rev");
    extensions_.push_back("GL_ARB_ES2_compatibility");
    extensions_.push_back("GL_ARB_texture_storage");
    extensions_.push_back("GL_EXT_texture_storage");  // the same entry points (EXT aliases)
    // GL_ARB_compute_shader is advertised only with GL 4.3 (below): in older
    // contexts a kernel has no storage buffers to write, and engines that
    // detect it would skip their working fallback.
    extensions_.push_back("GL_ARB_compressed_texture_pixel_storage");
    extensions_.push_back("GL_ARB_texture_storage_multisample");
    extensions_.push_back("GL_EXT_texture_compression_s3tc");
    extensions_.push_back("GL_EXT_texture_filter_anisotropic");
    extensions_.push_back("GL_ARB_texture_filter_anisotropic");
    // Extensions core GL 3.3 subsumes. Engines written against 2.x-era
    // drivers test for them even in 3.3 contexts.
    for (const char* name : {"GL_ARB_framebuffer_object", "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit",
                             "GL_EXT_framebuffer_multisample", "GL_EXT_packed_depth_stencil", "GL_ARB_vertex_array_object",
                             "GL_ARB_map_buffer_range", "GL_ARB_texture_rg", "GL_ARB_texture_compression_rgtc",
                             "GL_ARB_instanced_arrays", "GL_ARB_draw_instanced", "GL_ARB_draw_elements_base_vertex",
                             "GL_ARB_uniform_buffer_object", "GL_ARB_copy_buffer", "GL_ARB_sync", "GL_ARB_timer_query",
                             "GL_EXT_timer_query", "GL_ARB_occlusion_query2", "GL_ARB_sampler_objects",
                             "GL_ARB_texture_swizzle", "GL_ARB_depth_clamp", "GL_ARB_seamless_cube_map",
                             "GL_ARB_provoking_vertex", "GL_ARB_texture_multisample", "GL_ARB_blend_func_extended",
                             "GL_ARB_explicit_attrib_location", "GL_ARB_fragment_coord_conventions",
                             "GL_ARB_shader_bit_encoding", "GL_ARB_texture_rgb10_a2ui",
                             "GL_ARB_half_float_vertex", "GL_ARB_texture_buffer_object", "GL_ARB_framebuffer_sRGB",
                             "GL_EXT_framebuffer_sRGB", "GL_EXT_texture_sRGB", "GL_ARB_map_buffer_alignment"})
        extensions_.push_back(name);
    // Texture format extensions core GL 3.0 subsumes, which format queries
    // (and the CTS internalformat cases) test for in core contexts too.
    // OpenSceneGraph tests GL_ARB_depth_buffer_float by name before using
    // floating-point depth (OpenMW turns off reversed-Z's precision and its
    // post-processing without it, issue #42).
    for (const char* name : {"GL_ARB_depth_texture", "GL_ARB_texture_float", "GL_EXT_texture_shared_exponent",
                             "GL_EXT_texture_integer", "GL_ARB_depth_buffer_float"})
        if (!hasExtension(name)) extensions_.push_back(name);
    extensions_.push_back("GL_ARB_draw_buffers_blend");
    extensions_.push_back("GL_ARB_base_instance");
    extensions_.push_back("GL_ARB_clip_control");
    extensions_.push_back("GL_ARB_get_program_binary");
    extensions_.push_back("GL_ARB_buffer_storage");
    // Client memory as buffer storage: Wine maps 32-bit (WoW64) guests'
    // buffers through it, and limits them to GL 4.3 without it.
    extensions_.push_back("GL_AMD_pinned_memory");
    extensions_.push_back("GL_KHR_debug");
    extensions_.push_back("GL_ARB_debug_output");
    extensions_.push_back("GL_ARB_separate_shader_objects");
    for (const char* name : {"GL_KHR_robustness", "GL_ARB_robustness", "GL_ARB_internalformat_query",
                             "GL_ARB_invalidate_subdata", "GL_ARB_copy_image", "GL_NVX_gpu_memory_info", "GL_ATI_meminfo"})
        extensions_.push_back(name);
    // The extensions GL 4.0 to 4.6 made core, in contexts of the
    // version that includes them (their features are gated on the version).
    const int version = config_.major * 10 + config_.minor;
    if (version >= 40)
        for (const char* name :
             {"GL_ARB_draw_indirect", "GL_ARB_gpu_shader5", "GL_ARB_gpu_shader_fp64", "GL_ARB_sample_shading",
              "GL_ARB_shader_subroutine", "GL_ARB_tessellation_shader", "GL_ARB_texture_buffer_object_rgb32",
              "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather", "GL_ARB_texture_query_lod",
              "GL_ARB_transform_feedback2", "GL_ARB_transform_feedback3"})
            extensions_.push_back(name);
    if (version >= 41)
        for (const char* name : {"GL_ARB_shader_precision", "GL_ARB_vertex_attrib_64bit", "GL_ARB_viewport_array"})
            extensions_.push_back(name);
    if (version >= 42)
        for (const char* name :
             {"GL_ARB_conservative_depth", "GL_ARB_shader_atomic_counters", "GL_ARB_shader_image_load_store",
              "GL_ARB_shading_language_420pack", "GL_ARB_shading_language_packing", "GL_ARB_texture_compression_bptc",
              "GL_ARB_transform_feedback_instanced"})
            extensions_.push_back(name);
    if (version >= 43)
        for (const char* name :
             {"GL_ARB_compute_shader", "GL_ARB_explicit_uniform_location", "GL_ARB_program_interface_query",
              "GL_ARB_shader_storage_buffer_object", "GL_ARB_texture_buffer_range", "GL_ARB_texture_view",
              "GL_ARB_texture_query_levels", "GL_ARB_vertex_attrib_binding", "GL_ARB_arrays_of_arrays",
              "GL_ARB_clear_buffer_object", "GL_ARB_ES3_compatibility", "GL_ARB_fragment_layer_viewport",
              "GL_ARB_framebuffer_no_attachments", "GL_ARB_internalformat_query2", "GL_ARB_multi_draw_indirect",
              "GL_ARB_shader_image_size", "GL_ARB_stencil_texturing",
              // GL 4.5, implemented: the Khronos CTS calls glTextureBarrier in GL 4.3 view tests
              "GL_ARB_texture_barrier"})
            extensions_.push_back(name);
    if (version >= 44)
        for (const char* name :
             {"GL_ARB_clear_texture", "GL_ARB_enhanced_layouts", "GL_ARB_multi_bind", "GL_ARB_query_buffer_object",
              "GL_ARB_texture_mirror_clamp_to_edge", "GL_ARB_texture_stencil8", "GL_ARB_vertex_type_10f_11f_11f_rev"})
            extensions_.push_back(name);
    if (version >= 45)
        for (const char* name :
             {"GL_ARB_conditional_render_inverted", "GL_ARB_cull_distance", "GL_ARB_derivative_control",
              "GL_ARB_direct_state_access", "GL_ARB_ES3_1_compatibility", "GL_ARB_get_texture_sub_image",
              "GL_ARB_shader_texture_image_samples", "GL_KHR_context_flush_control"})
            extensions_.push_back(name);
    if (version >= 46)
        for (const char* name : {"GL_ARB_gl_spirv", "GL_ARB_spirv_extensions", "GL_ARB_indirect_parameters",
                                 "GL_ARB_pipeline_statistics_query", "GL_ARB_shader_atomic_counter_ops",
                                 "GL_ARB_shader_draw_parameters", "GL_ARB_shader_group_vote",
                                 "GL_ARB_transform_feedback_overflow_query", "GL_ARB_parallel_shader_compile",
                                 "GL_KHR_parallel_shader_compile", "GL_KHR_no_error"})
            extensions_.push_back(name);
    // Later extensions the core implements, in 4.x contexts: wined3d reads
    // EXT_shader_integer_mix and polygon_offset_clamp for its feature level.
    if (version >= 40)
        for (const char* name : {"GL_EXT_shader_integer_mix", "GL_ARB_polygon_offset_clamp", "GL_EXT_polygon_offset_clamp"})
            extensions_.push_back(name);
    // gl_Layer and gl_ViewportIndex from vertex and tessellation evaluation
    // shaders, which Metal vertex functions write natively (the geometry
    // emulation's pass-through vertex function already does).
    if (version >= 41) extensions_.push_back("GL_ARB_shader_viewport_layer_array");
    // Sampling sRGB textures without decoding: wined3d renders to sRGB
    // targets (GL_ARB_framebuffer_sRGB) only with it.
    extensions_.push_back("GL_EXT_texture_sRGB_decode");
    if (version >= 40) extensions_.push_back("GL_EXT_texture_shadow_lod");
    // Features gate on the desktop names (hasExtension); ES contexts
    // advertise the OpenGL ES names instead.
    featureExtensions_ = extensions_;
    if (config_.es) initEsStrings();
    // Programs written for OpenGL 1.x and 2.x copy or print GL_EXTENSIONS
    // into fixed buffers: Quake III engine games print it through a 4096-byte
    // one (Jedi Academy overran it with the whole ~5 KB string and crashed).
    // Their contexts get the extensions, oldest first, that fit in 3900 bytes
    // (Wine's opengl32 appends its WGL ones), as drivers' legacy extension
    // string limits do; glGetStringi still lists them all.
    // WINEMETALGL_EXTENSIONS_LENGTH sets the limit (0: none).
    size_t limit = config_.legacyRequest && !config_.es ? 3900 : 0;
    if (const char* length = std::getenv("WINEMETALGL_EXTENSIONS_LENGTH"); length && *length)
        limit = std::strtoul(length, nullptr, 10);
    extensionString_.clear();
    size_t kept = 0;
    for (const std::string& ext : extensions_) {
        const size_t added = (extensionString_.empty() ? 0 : 1) + ext.size();
        if (limit && extensionString_.size() + added > limit) break;
        if (!extensionString_.empty()) extensionString_ += ' ';
        extensionString_ += ext;
        kept++;
    }
    if (kept < extensions_.size())
        WMGL_INFO(Context, "GL_EXTENSIONS: %zu of %zu extensions (%zu bytes) for a program that asked for OpenGL 1.x or 2.x; "
                  "glGetStringi lists them all, WINEMETALGL_EXTENSIONS_LENGTH=0 gives the whole string",
                  kept, extensions_.size(), extensionString_.size());
}

bool Context::hasExtension(const char* name) const {
    return std::find(featureExtensions_.begin(), featureExtensions_.end(), name) != featureExtensions_.end();
}

bool Context::advertises(const char* name) const {
    return std::find(extensions_.begin(), extensions_.end(), name) != extensions_.end();
}

bool g_captureCalls = false;

namespace {
FILE* g_captureFile = nullptr;
}  // namespace

void captureFrameBoundary(Context* ctx) {
    if (g_captureFile) {
        std::fclose(g_captureFile);
        g_captureFile = nullptr;
        g_captureCalls = false;
        WMGL_INFO(General, "frame capture written");
    }
    // A stat every 30 presents: about twice a second.
    static unsigned presents = 0;
    if (++presents % 30) return;
    const char* home = std::getenv("HOME");
    if (!home) return;
    const std::string dir = std::string(home) + "/Library/Logs/WineMetalGL/";
    const std::string trigger = dir + "capture-frame";
    if (::access(trigger.c_str(), F_OK) != 0) return;
    ::unlink(trigger.c_str());
    char path[1024];
    std::snprintf(path, sizeof(path), "%s%s-frame-%lld.txt", dir.c_str(), logProgramName().c_str(),
                  static_cast<long long>(::time(nullptr)));
    if ((g_captureFile = std::fopen(path, "w"))) {
        // The programs, by their uniforms' names (what a draw's glUseProgram
        // names is otherwise a number).
        if (ctx) {
            ShareLock lock(*ctx);
            for (const auto& [id, program] : ctx->share().programs) {
                if (!program || !program->linked) continue;
                std::string names;
                for (const shader::UniformInfo& u : program->linked->link.uniforms) {
                    if (names.size() > 600) break;
                    names += " " + u.name;
                }
                std::fprintf(g_captureFile, "# program %u:%s\n", id, names.c_str());
            }
        }
        g_captureCalls = true;
        WMGL_INFO(General, "capturing a frame's GL calls to %s", path);
    }
}

void logCallArguments(Context& ctx, const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_captureCalls && g_captureFile) std::fprintf(g_captureFile, "%s\n", line);
    if (!ctx.logArguments) return;
    ctx.logArguments = false;
    WMGL_WARN(Context, "  the call: %s", line);
}

void Context::setError(GLenum error) {
    if (error_ == GL_NO_ERROR) error_ = error;
    if (logEnabled(LogLevel::Trace, LogCategory::Context)) WMGL_TRACE(Context, "GL error 0x%04x in %s", error,
                                                                      currentCommand);
    // Each command's error once: an application that ignores errors shows
    // what failed (a refused attachment later drops every draw).
    if (logEnabled(LogLevel::Warn, LogCategory::Context)) {
        char key[96];
        std::snprintf(key, sizeof(key), "error-%s-%04x", currentCommand, error);
        // The generated entry point then logs the call's arguments.
        logArguments = logOnce(LogLevel::Warn, LogCategory::Context, key, "GL error 0x%04x in %s (reported once per command and error)", error,
                               currentCommand) || logArguments;
    }
    if (state.debugOutput) {
        const char* name = error == GL_INVALID_ENUM        ? "GL_INVALID_ENUM"
                           : error == GL_INVALID_VALUE     ? "GL_INVALID_VALUE"
                           : error == GL_INVALID_OPERATION ? "GL_INVALID_OPERATION"
                           : error == GL_OUT_OF_MEMORY     ? "GL_OUT_OF_MEMORY"
                           : error == GL_INVALID_FRAMEBUFFER_OPERATION ? "GL_INVALID_FRAMEBUFFER_OPERATION"
                                                                       : "GL error";
        char message[160];
        std::snprintf(message, sizeof(message), "%s in %s", name, currentCommand);
        debugMessage(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, error, GL_DEBUG_SEVERITY_HIGH, message);
    }
}

GLenum Context::takeError() {
    const GLenum error = error_;
    error_ = GL_NO_ERROR;
    return error;
}

void Context::unimplemented(const char* command) {
    logOnce(LogLevel::Warn, LogCategory::Api, command, "unimplemented: %s", command);
    setError(GL_INVALID_OPERATION);
}

bool Context::debugMessageEnabled(GLenum source, GLenum type, GLuint id, GLenum severity) const {
    bool enabled = severity != GL_DEBUG_SEVERITY_LOW;
    for (const DebugRule& r : debugGroups.back().rules) {
        if ((r.source != GL_DONT_CARE && r.source != source) || (r.type != GL_DONT_CARE && r.type != type) ||
            (r.severity != GL_DONT_CARE && r.severity != severity))
            continue;
        if (!r.ids.empty() && std::find(r.ids.begin(), r.ids.end(), id) == r.ids.end()) continue;
        enabled = r.enabled;
    }
    return enabled;
}

void Context::debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, const std::string& message) {
    if (!state.debugOutput || !debugMessageEnabled(source, type, id, severity)) return;
    const std::string text = message.size() >= kMaxDebugMessageLength ? message.substr(0, kMaxDebugMessageLength - 1) : message;
    if (debugCallback) {
        debugCallback(source, type, id, severity, static_cast<GLsizei>(text.size()), text.c_str(), debugUserParam);
        return;
    }
    if (debugLog.size() < kMaxDebugLoggedMessages) debugLog.push_back({source, type, severity, id, text});
}

void Context::bindSurfaces(Surface* draw, Surface* read) {
    drawSurface_ = draw;
    readSurface_ = read;
    if (!everMadeCurrent && draw) {
        // The first MakeCurrent sets the viewport and scissor to the surface size.
        for (int i = 0; i < ContextState::kMaxViewports; ++i) {
            state.viewports[i][2] = static_cast<GLfloat>(state.scissors[i][2] = draw->width());
            state.viewports[i][3] = static_cast<GLfloat>(state.scissors[i][3] = draw->height());
        }
        everMadeCurrent = true;
    }
    markDirty(kDirtyFramebuffer | kDirtyViewport | kDirtyScissor);
}

Buffer* Context::buffer(GLuint name) {
    ShareLock lock(*this);
    return share().buffers.get(name);
}

Texture* Context::texture(GLuint name) {
    ShareLock lock(*this);
    return share().textures.get(name);
}

Renderbuffer* Context::renderbuffer(GLuint name) {
    ShareLock lock(*this);
    return share().renderbuffers.get(name);
}

Sampler* Context::sampler(GLuint name) {
    ShareLock lock(*this);
    return share().samplers.get(name);
}

RefPtr<Buffer>* Context::bufferBinding(GLenum target) {
    switch (target) {
    case GL_ARRAY_BUFFER: return &state.arrayBuffer;
    case GL_ELEMENT_ARRAY_BUFFER: return &state.vertexArray->elementBuffer;
    case GL_COPY_READ_BUFFER: return &state.copyReadBuffer;
    case GL_COPY_WRITE_BUFFER: return &state.copyWriteBuffer;
    case GL_PIXEL_PACK_BUFFER: return &state.pixelPackBuffer;
    case GL_PIXEL_UNPACK_BUFFER: return &state.pixelUnpackBuffer;
    case GL_TEXTURE_BUFFER: return &state.textureBuffer;
    case GL_UNIFORM_BUFFER: return &state.uniformBuffer;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return &state.transformFeedbackBuffer;
    default: break;
    }
    if (version() >= 40 || hasExtension("GL_ARB_draw_indirect"))
        if (target == GL_DRAW_INDIRECT_BUFFER) return &state.drawIndirectBuffer;
    if (computeShaders() && target == GL_DISPATCH_INDIRECT_BUFFER) return &state.dispatchIndirectBuffer;
    if (storageBuffers() && target == GL_SHADER_STORAGE_BUFFER) return &state.shaderStorageBuffer;
    if (atomicCounters() && target == GL_ATOMIC_COUNTER_BUFFER) return &state.atomicCounterBuffer;
    if (version() >= 44 && target == GL_QUERY_BUFFER) return &state.queryBuffer;
    if (indirectParameters() && target == GL_PARAMETER_BUFFER) return &state.parameterBuffer;
    if (target == GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD) return &state.externalVirtualMemoryBuffer;
    return nullptr;
}

}  // namespace wmgl
