// The driver ABI the Wine module uses (include/winemetalgl/driver.h).

#include "winemetalgl/driver.h"

#include <dlfcn.h>
#include <execinfo.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "core/context.h"
#include "core/metal/device.h"
#include "core/proc_table.h"
#include "core/stats.h"
#include "core/surface.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/util/thread_qos.h"
#include "core/util/version.h"

#define WMGL_EXPORT extern "C" __attribute__((visibility("default")))

using namespace wmgl;

namespace {

// WGL_ARB_create_context / _profile / _robustness / ARB_create_context_no_error.
constexpr int kMajorVersion = 0x2091, kMinorVersion = 0x2092, kLayerPlane = 0x2093, kFlags = 0x2094,
              kProfileMask = 0x9126, kResetStrategy = 0x8256, kNoError = 0x31B3, kReleaseBehavior = 0x2097;
constexpr int kDebugBit = 0x1, kForwardCompatibleBit = 0x2, kRobustAccessBit = 0x4;
constexpr int kCoreBit = 0x1, kCompatBit = 0x2;
// WGL_EXT_create_context_es_profile (and es2_profile, the same bit).
constexpr int kEsBit = 0x4;
constexpr int kLoseContextOnReset = 0x8252;

// The highest version this core implements completely.
constexpr int kHonestMajor = 4, kHonestMinor = 6;

void honestMaxVersion(int& major, int& minor) {
    major = kHonestMajor;
    minor = kHonestMinor;
    if (const char* level = std::getenv("WINEMETALGL_MAX_FEATURE_LEVEL")) {
        int a = 0, b = 0;
        if (std::sscanf(level, "%d.%d", &a, &b) == 2 && (a < kHonestMajor || (a == kHonestMajor && b <= kHonestMinor))) {
            major = a;
            minor = b;
        }
    }
}

// The highest OpenGL ES version ES contexts implement. ES contexts run on
// the desktop 4.6 core underneath (Context::isEs). WINEMETALGL_ES_VERSION
// lowers it ("3.0") to test a smaller version.
constexpr int kEsMajor = 3, kEsMinor = 2;

void esMaxVersion(int& major, int& minor) {
    major = kEsMajor;
    minor = kEsMinor;
    if (const char* level = std::getenv("WINEMETALGL_ES_VERSION")) {
        int a = 0, b = 0;
        if (std::sscanf(level, "%d.%d", &a, &b) == 2 && a >= 2 && a * 10 + b <= kEsMajor * 10 + kEsMinor) {
            major = a;
            minor = b;
        }
    }
}

bool validEsVersion(int major, int minor) {
    return (major == 2 && minor == 0) || (major == 3 && minor >= 0 && minor <= 2);
}

// The version contexts get: the honest one, unless the development override
// (devMaxVersion, core/util/version.h) raises it.
void maxVersion(int& major, int& minor) {
    honestMaxVersion(major, minor);
    const int dev = devMaxVersion();
    if (dev > major * 10 + minor) {
        major = dev / 10;
        minor = dev % 10;
    }
}

bool validVersion(int major, int minor) {
    switch (major) {
    case 1: return minor >= 0 && minor <= 5;
    case 2: return minor >= 0 && minor <= 1;
    case 3: return minor >= 0 && minor <= 3;
    case 4: return minor >= 0 && minor <= 6;
    default: return false;
    }
}

Context* asContext(wmgl_context* c) { return reinterpret_cast<Context*>(c); }

// Why a context request is refused, for the log.
wmgl_context* refuseContext(int* error, int code, int major, int minor, int flags, int profile, const char* why) {
    WMGL_WARN(Driver, "context refused: OpenGL %d.%d flags 0x%x profile 0x%x: %s", major, minor, flags, profile, why);
    if (error) *error = code;
    return nullptr;
}

NSUncaughtExceptionHandler* g_previousObjcHandler;

void onObjcException(NSException* exception) {
    NSString* text = [NSString stringWithFormat:@"%@: %@", exception.name, exception.reason];
    crashReport("uncaught Objective-C exception", text.UTF8String);
    if (g_previousObjcHandler) g_previousObjcHandler(exception);
}

#ifndef WMGL_COMMIT
#define WMGL_COMMIT ""
#endif
Surface* asSurface(wmgl_surface* s) { return reinterpret_cast<Surface*>(s); }

}  // namespace

WMGL_EXPORT int wmgl_initialize(unsigned int abi_version) {
    CallPool pool;  // see CallPool (context.h)
    installCrashHandlers();
    // One GL front end per process. The core keeps one current context per
    // thread, so a second front end (another library of the host, such as a
    // Wine display driver's own OpenGL) would make its contexts current on the
    // first one's threads and the application's GL calls would land in them.
    // WineMetalGL's own Wine module (winemetalgl.so), the program's GL, owns
    // the core whenever it loads, also after a display driver's OpenGL
    // initialized it first (it does at startup, enumerating GPUs: Steam).
    // Otherwise the first library to initialize the core owns it; another
    // is refused.
    static std::mutex ownerLock;
    static const void* owner = nullptr;
    static std::string ownerName;
    static bool ownerIsModule = false;
    Dl_info caller;
    const bool known = dladdr(__builtin_return_address(0), &caller) && caller.dli_fbase;
    const char* by = known && caller.dli_fname ? caller.dli_fname : "?";
    const char* byName = std::strrchr(by, '/') ? std::strrchr(by, '/') + 1 : by;
    const bool module = known && std::strstr(byName, "winemetalgl") != nullptr;
    {
        std::lock_guard<std::mutex> lock(ownerLock);
        if (!owner || (module && !ownerIsModule && caller.dli_fbase != owner)) {
            if (owner)
                WMGL_INFO(Driver, "initialized by %s, which takes the core over from %s", by, ownerName.c_str());
            else
                WMGL_INFO(Driver, "initialized by %s", by);
            owner = known ? caller.dli_fbase : reinterpret_cast<const void*>(1);
            ownerName = by;
            ownerIsModule = module;
        } else if (known && caller.dli_fbase != owner) {
            WMGL_WARN(Driver, "refused a second GL front end, %s: %s owns this process's GL", by, ownerName.c_str());
            void* frames[24];
            const int count = backtrace(frames, 24);
            for (int i = 1; i < count; ++i) {
                Dl_info info;
                if (!dladdr(frames[i], &info) || !info.dli_fname) continue;
                const char* image = std::strrchr(info.dli_fname, '/');
                WMGL_INFO(Driver, "  #%d %s+0x%lx %s", i, image ? image + 1 : info.dli_fname,
                          static_cast<unsigned long>(static_cast<char*>(frames[i]) - static_cast<char*>(info.dli_fbase)),
                          info.dli_sname ? info.dli_sname : "");
            }
            return -1;
        }
    }
    static std::once_flag objcOnce;
    std::call_once(objcOnce, [] {
        if (const char* v = std::getenv("WINEMETALGL_CRASH_HANDLER"); v && !std::strcmp(v, "0")) return;
        g_previousObjcHandler = NSGetUncaughtExceptionHandler();
        NSSetUncaughtExceptionHandler(onObjcException);
    });
    if (abi_version != WMGL_DRIVER_ABI_VERSION) {
        logBanner(WMGL_VERSION, WMGL_COMMIT, nullptr);
        WMGL_ERR(Driver, "driver ABI mismatch: caller %u, core %u", abi_version, WMGL_DRIVER_ABI_VERSION);
        return -1;
    }
    const bool device = Device::get().mtl() != nil;
    logBanner(WMGL_VERSION, WMGL_COMMIT, device ? Device::get().name() : nullptr);
    if (!device) {
        WMGL_ERR(Driver, "no Metal device: the core cannot start");
        return -1;
    }
    WMGL_INFO(Driver, "WineMetalGL core on %s", Device::get().name());
    return 0;
}

WMGL_EXPORT unsigned int wmgl_get_pixel_formats(struct wmgl_pixel_format* formats, unsigned int max, unsigned int* onscreen) {
    const auto& list = pixelFormats();
    for (unsigned i = 0; i < max && i < list.size(); ++i) formats[i] = list[i];
    if (onscreen) *onscreen = onscreenPixelFormatCount();
    return static_cast<unsigned>(list.size());
}

WMGL_EXPORT wmgl_surface* wmgl_surface_create_window(void* layer, int format, int width, int height) {
    CallPool pool;  // see CallPool (context.h)
    auto* surface = new Surface((__bridge CAMetalLayer*)layer, format, width, height);
    surface->retain();
    WMGL_INFO(Driver, "surface %p: window %dx%d, pixel format %d", surface, width, height, format);
    return reinterpret_cast<wmgl_surface*>(surface);
}

WMGL_EXPORT wmgl_surface* wmgl_surface_create_pbuffer(int format, int width, int height) {
    CallPool pool;  // see CallPool (context.h)
    auto* surface = new Surface(nil, format, width, height);
    surface->retain();
    WMGL_INFO(Driver, "surface %p: pbuffer %dx%d, pixel format %d", surface, width, height, format);
    return reinterpret_cast<wmgl_surface*>(surface);
}

WMGL_EXPORT void wmgl_surface_destroy(wmgl_surface* surface) {
    CallPool pool;  // see CallPool (context.h)
    if (!surface) return;
    WMGL_TRACE(Driver, "surface %p released", surface);
    asSurface(surface)->release();
}

WMGL_EXPORT void wmgl_surface_set_output(wmgl_surface* handle, int width, int height) {
    CallPool pool;  // see CallPool (context.h)
    if (Surface* surface = asSurface(handle)) surface->setOutputSize(width, height);
}

WMGL_EXPORT void wmgl_surface_flush(wmgl_surface* handle, unsigned int flags, int width, int height, int interval) {
    CallPool pool;  // see CallPool (context.h)
    Surface* surface = asSurface(handle);
    if (!surface) return;
    if (flags & WMGL_FLUSH_UPDATED) {
        WMGL_TRACE(Driver, "surface %p resized to %dx%d", handle, width, height);
        Context* ctx = currentContext();
        surface->resize(width, height, ctx && ctx->drawSurface() == surface ? &ctx->stream() : nullptr);
    }
    if (flags & WMGL_FLUSH_INTERVAL) surface->setSwapInterval(interval);
    if (flags & WMGL_FLUSH_PRESENT) {
        Context* ctx = currentContext();
        if (ctx && ctx->drawSurface() == surface) surface->swap(ctx->stream());
        stats::endFrame();
        captureFrameBoundary(ctx);
    }
}

WMGL_EXPORT int wmgl_surface_swap(wmgl_surface* handle) {
    CallPool pool;  // see CallPool (context.h)
    Surface* surface = asSurface(handle);
    if (!surface) return 0;
    Context* ctx = currentContext();
    if (ctx && ctx->drawSurface() == surface) {
        const bool presented = surface->swap(ctx->stream());
        stats::endFrame();
        captureFrameBoundary(ctx);
        return presented ? 1 : 0;
    }
    // SwapBuffers on a DC whose surface is not current here: present what it has.
    static Queue* queue = new Queue();
    CommandStream stream(*queue);
    return surface->swap(stream) ? 1 : 0;
}

WMGL_EXPORT wmgl_context* wmgl_context_create(int format, wmgl_context* share, const int* attribs, int* error) {
    CallPool pool;  // see CallPool (context.h)
    if (error) *error = WMGL_ERROR_NONE;
    ContextConfig config;
    int requestedMajor = 1, requestedMinor = 0, flags = 0, profile = kCoreBit;
    bool profileGiven = false;
    for (const int* a = attribs; a && a[0]; a += 2) {
        switch (a[0]) {
        case kMajorVersion: requestedMajor = a[1]; break;
        case kMinorVersion: requestedMinor = a[1]; break;
        case kLayerPlane: break;
        case kFlags: flags = a[1]; break;
        case kProfileMask: profile = a[1]; profileGiven = true; break;
        case kResetStrategy: config.resetNotification = a[1] == kLoseContextOnReset ? GL_LOSE_CONTEXT_ON_RESET : GL_NO_RESET_NOTIFICATION; break;
        case kNoError: config.noError = a[1] != 0; break;
        case kReleaseBehavior: break;
        default:
            WMGL_WARN(Driver, "unknown context attribute 0x%x", a[0]);
            if (error) *error = WMGL_ERROR_INVALID_ATTRIB;
            return nullptr;
        }
    }
    if (profileGiven && profile == kEsBit) {
        // WGL_EXT_create_context_es_profile: any OpenGL ES version this core
        // implements, answered with the highest (backwards compatible) one.
        // OpenGL ES 1.x is not implemented.
        int esMajor, esMinor;
        esMaxVersion(esMajor, esMinor);
        if (!validEsVersion(requestedMajor, requestedMinor) || (flags & ~(kDebugBit | kRobustAccessBit)) ||
            requestedMajor * 10 + requestedMinor > esMajor * 10 + esMinor)
            return refuseContext(error, WMGL_ERROR_INVALID_VERSION, requestedMajor, requestedMinor, flags, profile,
                                 "not an OpenGL ES version or flags this core implements");
        int maxMajor, maxMinor;
        honestMaxVersion(maxMajor, maxMinor);
        config.es = true;
        config.esMajor = esMajor;
        config.esMinor = esMinor;
        config.coreProfile = true;
        config.debug = flags & kDebugBit;
        config.robustAccess = flags & kRobustAccessBit;
        config.major = maxMajor;
        config.minor = maxMinor;
        RefPtr<ShareGroup> group = share ? asContext(share)->shareRef() : makeRef<ShareGroup>();
        auto* ctx = new Context(group, config, format);
        WMGL_INFO(Driver, "context %p: OpenGL ES %d.%d (requested %d.%d, flags 0x%x, pixel format %d, shares with %p)",
                  ctx,
                  esMajor, esMinor, requestedMajor, requestedMinor, flags, format, share);
        return reinterpret_cast<wmgl_context*>(ctx);
    }
    if (!validVersion(requestedMajor, requestedMinor))
        return refuseContext(error, WMGL_ERROR_INVALID_VERSION, requestedMajor, requestedMinor, flags, profile,
                             "not an OpenGL version");
    // WGL_ARB_create_context(_robustness): unknown flags, and the
    // forward-compatible flag before 3.0, are ERROR_INVALID_VERSION_ARB.
    if ((flags & ~(kDebugBit | kForwardCompatibleBit | kRobustAccessBit)) ||
        ((flags & kForwardCompatibleBit) && requestedMajor < 3))
        return refuseContext(error, WMGL_ERROR_INVALID_VERSION, requestedMajor, requestedMinor, flags, profile,
                             "unknown flags, or forward compatible before 3.0");
    // The profile mask applies from 3.2 (it is ignored before): exactly one of
    // the core and compatibility bits.
    const bool profiles = requestedMajor > 3 || (requestedMajor == 3 && requestedMinor >= 2);
    if (profiles && profileGiven && profile != kCoreBit && profile != kCompatBit)
        return refuseContext(error, WMGL_ERROR_INVALID_PROFILE, requestedMajor, requestedMinor, flags, profile,
                             "not exactly one of the core and compatibility profiles");
    int maxMajor, maxMinor;
    maxVersion(maxMajor, maxMinor);
    if (requestedMajor > maxMajor || (requestedMajor == maxMajor && requestedMinor > maxMinor)) {
        char why[64];
        std::snprintf(why, sizeof(why), "above the highest version, %d.%d", maxMajor, maxMinor);
        return refuseContext(error, WMGL_ERROR_INVALID_VERSION, requestedMajor, requestedMinor, flags, profile, why);
    }
    // Profiles exist from 3.2; earlier requests (and legacy wglCreateContext) get compatibility.
    // A forward-compatible 3.0 or 3.1 context drops the deprecated features:
    // the core profile is its backwards-compatible successor.
    config.coreProfile = profiles ? profile == kCoreBit : (flags & kForwardCompatibleBit) && requestedMajor >= 3;
    config.forwardCompatible = flags & kForwardCompatibleBit;
    config.debug = flags & kDebugBit;
    config.robustAccess = flags & kRobustAccessBit;
    config.legacyRequest = requestedMajor < 3;
    // GL may return any later version that is backwards compatible with the request.
    config.major = maxMajor;
    config.minor = maxMinor;
    RefPtr<ShareGroup> group = share ? asContext(share)->shareRef() : makeRef<ShareGroup>();
    auto* ctx = new Context(group, config, format);
    WMGL_INFO(Driver, "context %p: OpenGL %d.%d %s (requested %d.%d, flags 0x%x, pixel format %d, shares with %p)", ctx,
              config.major, config.minor, config.coreProfile ? "core" : "compatibility", requestedMajor, requestedMinor, flags,
              format, share);
    return reinterpret_cast<wmgl_context*>(ctx);
}

WMGL_EXPORT void wmgl_context_destroy(wmgl_context* handle) {
    CallPool pool;  // see CallPool (context.h)
    Context* ctx = asContext(handle);
    if (!ctx) return;
    WMGL_INFO(Driver, "context %p destroyed", ctx);
    if (currentContext() == ctx) setCurrentContext(nullptr);
    delete ctx;
}

WMGL_EXPORT int wmgl_make_current(wmgl_context* handle, wmgl_surface* draw, wmgl_surface* read) {
    CallPool pool;  // see CallPool (context.h)
    if (handle) raiseThreadQos("render");  // see thread_qos.h
    Context* previous = currentContext();
    Context* ctx = asContext(handle);
    WMGL_TRACE(Driver, "make current: context %p, draw %p, read %p (was %p)", ctx, draw, read, previous);
    // Switching contexts flushes the previous one (WGL requires an implicit glFlush).
    if (previous && previous != ctx) {
        previous->stream().flush();
        if (previous->current) {
            previous->current = false;
            --previous->share().currentContexts;
        }
    }
    if (ctx && !ctx->current) {
        ctx->current = true;
        ++ctx->share().currentContexts;
    }
    setCurrentContext(ctx);
    if (ctx) ctx->bindSurfaces(asSurface(draw), asSurface(read ? read : draw));
    return 1;
}

WMGL_EXPORT void* wmgl_get_proc_address(const char* name) { return lookupProc(name); }

namespace {
thread_local void* t_batchPool = nullptr;
}  // namespace

// A batch of GL calls (the Wine module's): one autorelease pool for them all
// instead of one per call (CallPool).
WMGL_EXPORT void wmgl_batch_begin(void) {
    if (CallPool::depth++ == 0) t_batchPool = objc_autoreleasePoolPush();
}

WMGL_EXPORT void wmgl_batch_end(void) {
    if (CallPool::depth > 0 && --CallPool::depth == 0) objc_autoreleasePoolPop(t_batchPool);
}

WMGL_EXPORT const char* wmgl_renderer_string(void) {
    // GL_RENDERER's string, WINEMETALGL_RENDERER included.
    return rendererString().c_str();
}

namespace wmgl::api {
extern void* (*g_bufferAllocate)(size_t);
extern void (*g_bufferRelease)(void*, size_t);
}

WMGL_EXPORT void wmgl_set_buffer_memory(void* (*allocate)(size_t), void (*release)(void*, size_t)) {
    wmgl::api::g_bufferAllocate = allocate;
    wmgl::api::g_bufferRelease = release;
}

WMGL_EXPORT void wmgl_stats_enable(int enable) { stats::g_enabled = enable || stats::g_reporting; }

WMGL_EXPORT void wmgl_stats_reset(void) { stats::frame() = stats::Frame(); }

WMGL_EXPORT int wmgl_stats_counter(const char* name, unsigned long long* value) {
    uint64_t v = 0;
    if (!stats::counter(name, v)) return 0;
    if (value) *value = v;
    return 1;
}

WMGL_EXPORT void wmgl_version(int* major, int* minor, int* patch) {
    int v[3] = {0, 0, 0};
    std::sscanf(WMGL_VERSION, "%d.%d.%d", &v[0], &v[1], &v[2]);
    if (major) *major = v[0];
    if (minor) *minor = v[1];
    if (patch) *patch = v[2];
}

WMGL_EXPORT unsigned int wmgl_video_memory_mb(void) {
    return static_cast<unsigned>(Device::get().caps().workingSetBytes >> 20);
}

WMGL_EXPORT void wmgl_max_versions(int* coreMajor, int* coreMinor, int* compatMajor, int* compatMinor) {
    // WGL_WINE_query_renderer reports what the core implements, never the
    // development override.
    int major, minor;
    honestMaxVersion(major, minor);
    if (coreMajor) *coreMajor = major;
    if (coreMinor) *coreMinor = minor;
    if (compatMajor) *compatMajor = major;
    if (compatMinor) *compatMinor = minor;
}

WMGL_EXPORT void wmgl_max_es_version(int* major, int* minor) {
    int a, b;
    esMaxVersion(a, b);
    if (major) *major = a;
    if (minor) *minor = b;
}

// Logging for the Wine module (core/util/log.h). Optional: cores before 3.3.0
// do not export them.

namespace {
LogCategory moduleCategory(const char* name) {
    LogCategory category = LogCategory::Wine;
    logCategoryFromName(name, category);
    return category;
}
}  // namespace

WMGL_EXPORT void wmgl_log_set_host(const char* program, const char* host_version) { logSetHost(program, host_version); }

WMGL_EXPORT int wmgl_log_level(const char* category) {
    const LogCategory c = moduleCategory(category);
    for (int level = static_cast<int>(LogLevel::Trace); level >= 0; --level)
        if (logCaptured(static_cast<LogLevel>(level), c)) return level;
    return -1;
}

WMGL_EXPORT void wmgl_log_write(int level, const char* category, const char* text) {
    if (level < 0) return;
    logText(static_cast<LogLevel>(level > 3 ? 3 : level), moduleCategory(category), text);
}
