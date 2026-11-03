// An OpenGL rendering context.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/compat/arb_programs.h"
#include "core/gl/types.h"

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* token);
#include "core/metal/command_stream.h"
#include "core/objects.h"
#include "core/share_group.h"
#include "core/state.h"
#include "core/surface.h"
#include "core/util/name_table.h"

namespace wmgl {

struct ContextConfig {
    int major = 1, minor = 0;
    bool coreProfile = false;       // WGL_CONTEXT_CORE_PROFILE_BIT_ARB
    bool forwardCompatible = false;
    bool debug = false;
    bool robustAccess = false;
    bool noError = false;
    // Created as OpenGL 1.x or 2.x (legacy wglCreateContext asks for 1.0): an
    // older program, whose GL_EXTENSIONS string is kept short (initStrings).
    bool legacyRequest = false;
    GLenum resetNotification = GL_NO_RESET_NOTIFICATION;
    // An OpenGL ES context (WGL_CONTEXT_ES_PROFILE_BIT_EXT): ES esMajor.esMinor
    // on the desktop major.minor core profile underneath.
    bool es = false;
    int esMajor = 0, esMinor = 0;
};

class DrawState;
struct ImmediateState;
struct ListCompiler;

class Context {
public:
    Context(RefPtr<ShareGroup> share, const ContextConfig& config, int pixelFormat);
    ~Context();

    // --- configuration
    const ContextConfig& config() const { return config_; }
    bool isCore() const { return config_.coreProfile; }
    bool isCompat() const { return !config_.coreProfile; }
    int versionMajor() const { return config_.major; }
    int versionMinor() const { return config_.minor; }
    int version() const { return config_.major * 10 + config_.minor; }
    bool isEs() const { return config_.es; }
    int esVersion() const { return config_.esMajor * 10 + config_.esMinor; }  // 0 outside ES
    // The core profile's rules without ES's exceptions: drawing needs a
    // vertex array object, and vertex attributes need buffers.
    bool isDesktopCore() const { return config_.coreProfile && !config_.es; }
    // Whether an sRGB-capable window is sRGB. OpenGL ES contexts treat it as
    // linear, as EGL configs (which carry no sRGB) do.
    bool windowSrgb() const { return !config_.es; }

    // --- errors and diagnostics
    void setError(GLenum error);
    GLenum takeError();
    void unimplemented(const char* command);
    void debugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, const std::string& message);

    // --- current surfaces (wglMakeCurrent)
    void bindSurfaces(Surface* draw, Surface* read);
    Surface* drawSurface() const { return drawSurface_.get(); }
    Surface* readSurface() const { return readSurface_.get(); }
    bool everMadeCurrent = false;

    // --- objects
    ShareGroup& share() { return *share_; }
    RefPtr<ShareGroup> shareRef() const { return share_; }
    NameTable<VertexArray> vertexArrays;
    NameTable<Query> queries;
    NameTable<TransformFeedback> transformFeedbacks;
    NameTable<ProgramPipeline> programPipelines;
    RefPtr<VertexArray> defaultVertexArray;
    RefPtr<TransformFeedback> defaultTransformFeedback;
    // Texture object 0 of each target, and the proxy texture of each target.
    std::array<RefPtr<Texture>, kTexTargetCount> defaultTextures;
    std::array<RefPtr<Texture>, kTexTargetCount> proxyTextures;

    // Lookup helpers that apply the share-group lock.
    Buffer* buffer(GLuint name);
    Texture* texture(GLuint name);
    Renderbuffer* renderbuffer(GLuint name);
    Sampler* sampler(GLuint name);

    ContextState state;
    // Recorded but not read yet (see state.h).
    uint64_t dirty = kDirtyAll;
    void markDirty(uint64_t bits) { dirty |= bits; }

    CommandStream& stream() { return stream_; }
    // Textures with GL_GENERATE_MIPMAP levels to regenerate (Texture::mipmapsPending).
    std::vector<RefPtr<Texture>> pendingMipmaps;
    // Current on a thread (ShareGroup::currentContexts counts it).
    bool current = false;
    DrawState& draw() { return *draw_; }

    // Returns the buffer bound to a buffer target, or null for an invalid target.
    RefPtr<Buffer>* bufferBinding(GLenum target);

    // Strings returned by glGetString/glGetStringi (stable storage).
    const std::string& versionString() const { return versionString_; }
    const std::string& glslVersionString() const { return glslVersionString_; }
    const std::vector<std::string>& extensions() const { return extensions_; }
    const std::string& extensionString() const { return extensionString_; }
    // A desktop extension whose features this context has (ES contexts
    // have the desktop 4.6 core's, under ES names).
    bool hasExtension(const char* name) const;
    // An extension glGetString(i) lists.
    bool advertises(const char* name) const;
    // Compute shaders: GL 4.3 or GL_ARB_compute_shader.
    bool computeShaders() const { return version() >= 43 || hasExtension("GL_ARB_compute_shader"); }
    // Tessellation shaders: GL 4.0 or GL_ARB_tessellation_shader.
    bool tessellationShaders() const { return version() >= 40 || hasExtension("GL_ARB_tessellation_shader"); }
    // Image load/store: GL 4.2 or GL_ARB_shader_image_load_store.
    bool imageLoadStore() const { return version() >= 42 || hasExtension("GL_ARB_shader_image_load_store"); }
    // Atomic counters: GL 4.2 or GL_ARB_shader_atomic_counters.
    bool storageBuffers() const { return version() >= 43 || hasExtension("GL_ARB_shader_storage_buffer_object"); }
    bool atomicCounters() const { return version() >= 42 || hasExtension("GL_ARB_shader_atomic_counters"); }
    // Sample shading: GL 4.0 or GL_ARB_sample_shading.
    bool sampleShading() const { return version() >= 40 || hasExtension("GL_ARB_sample_shading"); }
    // GL_ARB_parallel_shader_compile / GL_KHR_parallel_shader_compile.
    bool parallelShaderCompile() const {
        return hasExtension("GL_ARB_parallel_shader_compile") || hasExtension("GL_KHR_parallel_shader_compile");
    }
    // SPIR-V shaders: GL 4.6 or GL_ARB_gl_spirv.
    bool glSpirv() const { return !isEs() && (version() >= 46 || hasExtension("GL_ARB_gl_spirv")); }  // not OpenGL ES
    // Draw counts from PARAMETER_BUFFER: GL 4.6 or GL_ARB_indirect_parameters.
    bool indirectParameters() const { return version() >= 46 || hasExtension("GL_ARB_indirect_parameters"); }

    // glUniformSubroutinesuiv selections per stage (api/subroutine.mm), for
    // the executable they were made with; reset by UseProgram and pipeline
    // changes (resetSubroutineSelections).
    struct SubroutineSelection {
        uint64_t programUid = 0;
        std::vector<GLuint> indices;  // by subroutine uniform location
    };
    std::array<SubroutineSelection, 6> subroutineSelections;

    // Occlusion-query slot of the current pass (see core/query.h).
    struct {
        uint64_t pass = 0, epoch = 0;
        uint64_t samplesBound = 0;  // samples the slot's draws can have counted
        bool valid = false;
    } occlusionSlot;
    uint64_t drawSampleBound = 0;  // samples the current draw can cover (queriesBeforeDraw)
    uint64_t occlusionEpoch = 1;

    // Debug output (KHR_debug / ARB_debug_output, core/api/debug.mm).
    GLDEBUGPROC debugCallback = nullptr;
    const void* debugUserParam = nullptr;
    struct DebugRule {
        GLenum source, type, severity;  // GL_DONT_CARE matches any
        std::vector<GLuint> ids;        // empty: any id
        bool enabled;
    };
    struct DebugGroup {
        GLenum source;
        GLuint id;
        std::string message;
        std::vector<DebugRule> rules;  // applied in order over the default (all but LOW enabled)
    };
    struct DebugLogEntry {
        GLenum source, type, severity;
        GLuint id;
        std::string message;
    };
    std::vector<DebugGroup> debugGroups{DebugGroup{GL_DEBUG_SOURCE_APPLICATION, 0, "", {}}};
    std::deque<DebugLogEntry> debugLog;
    static constexpr size_t kMaxDebugLoggedMessages = 64, kMaxDebugMessageLength = 1024, kMaxDebugGroupDepth = 64;
    static constexpr GLsizei kMaxLabelLength = 256;
    bool debugMessageEnabled(GLenum source, GLenum type, GLuint id, GLenum severity) const;
    // Name of the GL command being executed (set by the generated entry points).
    const char* currentCommand = "";
    // setError logged an error of the current command: its entry point logs
    // the call's arguments (logCallArguments) when it returns.
    bool logArguments = false;

    // Compatibility profile (W22): the packed wmgl_FixedFunction block
    // (compat/fixed_function.h), immediate mode (compat/immediate.h) and
    // display list compilation (compat/display_list.h).
    struct FixedFunctionBlockCache {
        uint64_t version = 0, matrixVersion = 0, generation = 0;
        float pointSize = -1, fade = -1, lineWidth = -1;
        GLfloat viewport[4] = {};
        std::vector<uint8_t> data;
        // The matrices the block's matrix section was computed from.
        bool matricesValid = false;
        Mat4 modelview, projection, modelviewInverse, texture[FixedState::kTextureUnits];
    } fixedFunctionBlockCache;
    // glPushAttrib / glPushClientAttrib (api/fixed_attrib.mm): the mask and
    // a copy of the state it covers.
    struct AttribFrame {
        GLbitfield mask = 0;
        ContextState state;
        RefPtr<VertexArray> vertexArray;
        std::array<VertexAttrib, VertexArray::kAttribSlots> attribs;
    };
    std::vector<AttribFrame> attribStack, clientAttribStack;
    // glBitmap / glDrawPixels textures by content (api/fixed_raster.mm):
    // applications draw the same glyphs and images every frame.
    std::unordered_map<uint64_t, RefPtr<Texture>> pixelRectangleCache;
    std::unique_ptr<ImmediateState> immediate;
    std::unique_ptr<ListCompiler> listCompiler;  // non-null between glNewList and glEndList
    // ARB assembly programs: bindings, environment parameters, load errors.
    compat::ArbContextState arb;
    int listCallDepth = 0;

private:
    void initStrings();
    void initEsStrings();  // es_context.mm

    ContextConfig config_;
    RefPtr<ShareGroup> share_;
    RefPtr<Surface> drawSurface_, readSurface_;
    CommandStream stream_;
    std::unique_ptr<DrawState> draw_;
    GLenum error_ = GL_NO_ERROR;
    std::string versionString_, glslVersionString_, extensionString_;
    std::vector<std::string> extensions_, featureExtensions_;
};

Context* currentContext();
// WINEMETALGL_TRACE=1 logs every GL call (debugging aid).
extern bool g_traceCalls;
void traceCall(const char* command);
void setCurrentContext(Context* context);
void noCurrentContext(const char* command);
// Logs the arguments of a GL call whose error setError logged (`format` and
// the arguments from the generated entry point).
void logCallArguments(Context& ctx, const char* format, ...) __attribute__((format(printf, 2, 3)));
// Frame capture: when ~/Library/Logs/WineMetalGL/capture-frame appears, the
// next frame's GL calls, with their arguments, go to
// ~/Library/Logs/WineMetalGL/<program>-frame-<time>.txt (and the trigger
// goes). For a rendering bug, the calls of a frame that shows it.
extern bool g_captureCalls;
// At each present: starts or ends a capture.
void captureFrameBoundary(Context* ctx);

// An Objective-C autorelease pool for the scope of a GL call. Metal hands
// back some objects autoreleased (encoders, command buffers, pass
// descriptors, and the receivers of `contents`, which ARC retains and
// autoreleases), and the threads GL calls come from have no pool that ever
// drains: without one, every render encoder kept its command buffer, and so
// every texture and buffer storage the frame used, alive (issue #16).
// Inside another pool of this kind (a batch of calls, wmgl_batch_begin) a
// call adds none: the outer one drains when the batch ends.
struct CallPool {
    CallPool() : token(depth++ ? nullptr : objc_autoreleasePoolPush()) {}
    ~CallPool() {
        if (--depth == 0) objc_autoreleasePoolPop(token);
    }
    CallPool(const CallPool&) = delete;
    CallPool& operator=(const CallPool&) = delete;
    void* token;
    static thread_local int depth;  // pools of this kind open on this thread
};

// Scoped share-group lock (every GL command runs under it).
struct ShareLock {
    explicit ShareLock(Context& ctx) : lock(ctx.share().mutex) { ++depth; }
    ~ShareLock() { --depth; }
    std::lock_guard<std::recursive_mutex> lock;
    static thread_local int depth;  // share-group locks this thread holds
};

// Releases the calling thread's share-group lock around a GPU wait that
// touches no shared state (fence and query waits, glFinish), so the other
// contexts of the share group keep running meanwhile.
class ShareUnlock {
public:
    explicit ShareUnlock(Context& ctx) : mutex_(ctx.share().mutex), depth_(ShareLock::depth) {
        for (int i = 0; i < depth_; ++i) mutex_.unlock();
    }
    ~ShareUnlock() {
        for (int i = 0; i < depth_; ++i) mutex_.lock();
    }
    ShareUnlock(const ShareUnlock&) = delete;
    ShareUnlock& operator=(const ShareUnlock&) = delete;

private:
    std::recursive_mutex& mutex_;
    int depth_;
};

// Waits for `serial` (this context's work is flushed first) without the
// share-group lock, so the other contexts of the share group keep running.
// Shared object state may change meanwhile: callers re-validate it after.
// keepBusy: commit the draws recorded since `serial` before waiting, so the
// GPU runs them during the wait (the CPU reads commands a dispatch wrote).
void waitForGpuUnlocked(Context& ctx, uint64_t serial, bool keepBusy = false);
// glFinish-style: flushes, then waits for everything submitted, unlocked.
void finishUnlocked(Context& ctx);

}  // namespace wmgl
