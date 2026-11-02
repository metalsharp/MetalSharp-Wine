// The MetalFX spatial upscaling setting of window back buffers, taken the way
// DXMT takes its own, so a Wine's MetalFX setting applies to OpenGL too.
#pragma once

#include <string>

namespace wmgl {

struct MetalFxSettings {
    const char* winemetalgl = nullptr;  // WINEMETALGL_METALFX: 0 (off) or a factor
    const char* swapchain = nullptr;    // DXMT_METALFX_SPATIAL_SWAPCHAIN: 0 or 1
    const char* config = nullptr;       // DXMT_CONFIG: "key=value;key=value"
    std::string configFile;             // DXMT_CONFIG_FILE, else <wine>/etc/dxmt.conf
    double wine = 0;                    // MetalSharp Wine's factor (ntdll_get_metalfx_scale); 0: another Wine
};

// The value of d3d11.metalSpatialUpscaleFactor in DXMT configuration text:
// "key = value" lines (# comments) or "key=value;key=value", else `fallback`.
float dxmtUpscaleFactor(const std::string& text, float fallback);

// The upscale factor, 1 when off: WINEMETALGL_METALFX when set; else MetalSharp
// Wine's; else off when DXMT_METALFX_SPATIAL_SWAPCHAIN=0; else the configured factor (DXMT_CONFIG
// over the file; 2, DXMT's default, when only the swapchain switch is on).
// Clamped to [1, 4].
float metalFxFactor(const MetalFxSettings& settings);

}  // namespace wmgl
