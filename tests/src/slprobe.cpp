// slprobe: drives a game's Streamline (sl.interposer.dll, given as the argument) as a game does: slInit with
// DLSS, DLSS Ray Reconstruction and Reflex, then for the first DXGI adapter slIsFeatureSupported and
// slGetFeatureRequirements of each; then it makes a D3D12 device through the interposer, asks again, and asks DLSS
// for its optimal settings at 1920x1080, so what Streamline accepts on this system is printed (its log goes to
// C:\sllogs)
#include <windows.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <d3d12.h>
#include "include/sl.h"
#include "include/sl_dlss.h"

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 1;
    HMODULE sl = LoadLibraryW(argv[1]);
    if (!sl) { printf("cannot load %S: %lu\n", argv[1], GetLastError()); return 1; }
    auto init = (PFun_slInit *)GetProcAddress(sl, "slInit");
    auto supported = (PFun_slIsFeatureSupported *)GetProcAddress(sl, "slIsFeatureSupported");
    auto requirements = (PFun_slGetFeatureRequirements *)GetProcAddress(sl, "slGetFeatureRequirements");
    if (!init || !supported || !requirements) { printf("missing exports\n"); return 1; }

    sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureReflex};
    sl::Preferences pref{};
    // with only a second argument the log is left to Streamline's own overrides (registry, SL_LOG_* variables); a
    // second and third are the output size DLSS's optimal settings are asked for (1920x1080 without them)
    if (argc != 3)
    {
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.pathToLogsAndData = L"C:\\sllogs";
    }
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.applicationId = 100883811;  // The Witcher 3's, as its Streamline log shows
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;
    sl::Result r = init(pref, sl::kSDKVersion);
    printf("slInit: %d\n", (int)r);

    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || factory->EnumAdapters1(0, &adapter))
    { printf("no DXGI adapter\n"); return 1; }
    adapter->GetDesc1(&desc);
    printf("adapter %S vendor %04x device %04x\n", desc.Description, desc.VendorId, desc.DeviceId);

    sl::AdapterInfo info{};
    info.deviceLUID = (uint8_t *)&desc.AdapterLuid;
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    for (auto f : features)
    {
        sl::FeatureRequirements req{};
        sl::Result s = supported(f, info);
        sl::Result q = requirements(f, req);
        printf("feature %u: supported %d, requirements %d flags %#x os %u.%u.%u/%u.%u.%u driver %u.%u.%u/%u.%u.%u\n", f,
               (int)s, (int)q, (unsigned)req.flags, req.osVersionDetected.major, req.osVersionDetected.minor,
               req.osVersionDetected.build, req.osVersionRequired.major, req.osVersionRequired.minor,
               req.osVersionRequired.build, req.driverVersionDetected.major, req.driverVersionDetected.minor,
               req.driverVersionDetected.build, req.driverVersionRequired.major, req.driverVersionRequired.minor,
               req.driverVersionRequired.build);
    }

    // a device through the interposer, as the game makes it: Streamline's plugins start up (NGX with the device)
    auto create_device = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(sl, "D3D12CreateDevice");
    auto get_function = (PFun_slGetFeatureFunction *)GetProcAddress(sl, "slGetFeatureFunction");
    ID3D12Device *device = nullptr;
    HRESULT hr = create_device ? create_device(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void **)&device)
                               : E_FAIL;
    printf("D3D12CreateDevice: %#lx\n", hr);
    auto set_device = (PFun_slSetD3DDevice *)GetProcAddress(sl, "slSetD3DDevice");
    if (device && set_device) printf("slSetD3DDevice: %d\n", (int)set_device(device));
    for (auto f : features)
        printf("with a device, feature %u: supported %d\n", f, (int)supported(f, info));
    void *fn = nullptr;
    sl::Result g = get_function ? get_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) : sl::Result::eErrorMissingOrInvalidAPI;
    printf("slGetFeatureFunction(slDLSSGetOptimalSettings): %d %p\n", (int)g, fn);
    if (fn)
    {
        for (auto mode : {sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxPerformance})
        {
            sl::DLSSOptions options{};
            sl::DLSSOptimalSettings settings{};
            options.mode = mode;
            options.outputWidth = argc > 3 ? _wtoi(argv[2]) : 1920;
            options.outputHeight = argc > 3 ? _wtoi(argv[3]) : 1080;
            sl::Result o = ((PFun_slDLSSGetOptimalSettings *)fn)(options, settings);
            printf("optimal settings mode %d: %d render %ux%u (min %ux%u max %ux%u)\n", (int)mode, (int)o,
                   settings.optimalRenderWidth, settings.optimalRenderHeight, settings.renderWidthMin,
                   settings.renderHeightMin, settings.renderWidthMax, settings.renderHeightMax);
        }
    }
    return 0;
}
