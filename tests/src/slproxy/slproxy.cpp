// slproxy: a stand-in sl.interposer.dll that records the Streamline calls a game makes to decide what it offers
// (slInit, slIsFeatureSupported, slIsFeatureLoaded, slGetFeatureFunction, DLSS's optimal settings and options) with
// their results, to C:\sllogs\slproxy.log, and passes every call on to the real interposer, renamed
// slinterposerreal.dll in the same folder (a forwarder's module name has no dots) (the other exports are forwarded to it, sl.interposer.def)
#include <windows.h>
#include <cstdio>
#include "include/sl.h"
#include "include/sl_dlss.h"

static HMODULE real;
static FILE *out;

static void log_line(const char *format, ...)
{
    if (!out && !(out = fopen("C:\\sllogs\\slproxy.log", "a"))) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(out, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fflush(out);
}

template <typename F> static F *get(const char *name)
{
    if (!real)
    {
        WCHAR path[MAX_PATH];
        HMODULE self;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&log_line, &self);
        GetModuleFileNameW(self, path, MAX_PATH);
        wcscpy(wcsrchr(path, L'\\') + 1, L"slinterposerreal.dll");
        real = LoadLibraryW(path);
    }
    return real ? (F *)GetProcAddress(real, name) : nullptr;
}

static void *real_optimal, *real_options;

static sl::Result optimal_settings(const sl::DLSSOptions &options, sl::DLSSOptimalSettings &settings)
{
    sl::Result r = ((PFun_slDLSSGetOptimalSettings *)real_optimal)(options, settings);
    log_line("slDLSSGetOptimalSettings(mode %d, %ux%u) = %d: render %ux%u", (int)options.mode, options.outputWidth,
             options.outputHeight, (int)r, settings.optimalRenderWidth, settings.optimalRenderHeight);
    return r;
}

static sl::Result set_options(const sl::ViewportHandle &viewport, const sl::DLSSOptions &options)
{
    sl::Result r = ((PFun_slDLSSSetOptions *)real_options)(viewport, options);
    log_line("slDLSSSetOptions(viewport %u, mode %d, %ux%u) = %d", (uint32_t)viewport, (int)options.mode,
             options.outputWidth, options.outputHeight, (int)r);
    return r;
}

extern "C" {

sl::Result proxy_slInit(const sl::Preferences &pref, uint64_t sdk)
{
    sl::Result r = get<PFun_slInit>("slInit")(pref, sdk);
    log_line("slInit(app %u, %u features, flags %#llx) = %d", pref.applicationId, pref.numFeaturesToLoad,
             (unsigned long long)pref.flags, (int)r);
    for (uint32_t i = 0; i < pref.numFeaturesToLoad; i++) log_line("  feature to load %u", pref.featuresToLoad[i]);
    return r;
}

sl::Result proxy_slIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo &info)
{
    sl::Result r = get<PFun_slIsFeatureSupported>("slIsFeatureSupported")(feature, info);
    unsigned long long luid = 0;
    if (info.deviceLUID && info.deviceLUIDSizeInBytes == 8) memcpy(&luid, info.deviceLUID, 8);
    log_line("slIsFeatureSupported(%u, luid %#llx) = %d", feature, luid, (int)r);
    return r;
}

sl::Result proxy_slIsFeatureLoaded(sl::Feature feature, bool &loaded)
{
    sl::Result r = get<PFun_slIsFeatureLoaded>("slIsFeatureLoaded")(feature, loaded);
    log_line("slIsFeatureLoaded(%u) = %d, loaded %d", feature, (int)r, (int)loaded);
    return r;
}

sl::Result proxy_slSetFeatureLoaded(sl::Feature feature, bool loaded)
{
    sl::Result r = get<PFun_slSetFeatureLoaded>("slSetFeatureLoaded")(feature, loaded);
    log_line("slSetFeatureLoaded(%u, %d) = %d", feature, (int)loaded, (int)r);
    return r;
}

sl::Result proxy_slGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements &req)
{
    sl::Result r = get<PFun_slGetFeatureRequirements>("slGetFeatureRequirements")(feature, req);
    log_line("slGetFeatureRequirements(%u) = %d flags %#x", feature, (int)r, (unsigned)req.flags);
    return r;
}

sl::Result proxy_slSetD3DDevice(void *device)
{
    sl::Result r = get<PFun_slSetD3DDevice>("slSetD3DDevice")(device);
    log_line("slSetD3DDevice = %d", (int)r);
    return r;
}

sl::Result proxy_slGetFeatureFunction(sl::Feature feature, const char *name, void *&function)
{
    sl::Result r = get<PFun_slGetFeatureFunction>("slGetFeatureFunction")(feature, name, function);
    log_line("slGetFeatureFunction(%u, %s) = %d", feature, name, (int)r);
    if (r == sl::Result::eOk && function && !strcmp(name, "slDLSSGetOptimalSettings"))
    {
        real_optimal = function;
        function = (void *)&optimal_settings;
    }
    else if (r == sl::Result::eOk && function && !strcmp(name, "slDLSSSetOptions"))
    {
        real_options = function;
        function = (void *)&set_options;
    }
    return r;
}

}
