#include "core/util/metalfx_config.h"

#include <algorithm>
#include <cstdlib>

namespace wmgl {

float dxmtUpscaleFactor(const std::string& text, float fallback) {
    static const std::string key = "d3d11.metalSpatialUpscaleFactor";
    float factor = fallback;
    size_t at = 0;
    // The last setting wins.
    while ((at = text.find(key, at)) != std::string::npos) {
        const size_t start = at == 0 ? 0 : text.find_last_of("\n;", at - 1);
        const size_t from = start == std::string::npos ? 0 : (at == 0 ? 0 : start + 1);
        const std::string before = text.substr(from, at - from);
        at += key.size();
        if (before.find_first_not_of(" \t\r") != std::string::npos) continue;  // a comment, or another key
        const size_t equals = text.find_first_not_of(" \t", at);
        if (equals == std::string::npos || text[equals] != '=') continue;
        char* end = nullptr;
        const float value = std::strtof(text.c_str() + equals + 1, &end);
        if (end != text.c_str() + equals + 1) factor = value;
    }
    return factor;
}

float metalFxFactor(const MetalFxSettings& settings) {
    float factor;
    if (settings.winemetalgl) {
        factor = std::strtof(settings.winemetalgl, nullptr);
    } else if (settings.wine > 0) {
        factor = static_cast<float>(settings.wine);
    } else {
        if (settings.swapchain && settings.swapchain[0] == '0') return 1.0f;
        const bool on = settings.swapchain && settings.swapchain[0] != '\0';
        factor = dxmtUpscaleFactor(settings.configFile, on ? 2.0f : 1.0f);
        if (settings.config) factor = dxmtUpscaleFactor(settings.config, factor);
    }
    if (!(factor > 1.0f)) return 1.0f;
    return std::min(factor, 4.0f);
}

}  // namespace wmgl
