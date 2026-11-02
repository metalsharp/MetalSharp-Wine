#include <string>

#include "core/util/metalfx_config.h"
#include "test.h"

using namespace wmgl;

TEST(metalfx_dxmt_conf_factor) {
    // MetalSharp's dxmt.conf.
    CHECK(dxmtUpscaleFactor("d3d11.metalSpatialUpscaleFactor = 2.0\n", 1.0f) == 2.0f);
    CHECK(dxmtUpscaleFactor("# d3d11.metalSpatialUpscaleFactor = 3\nd3d11.metalSpatialUpscaleFactor=1.5", 1.0f) == 1.5f);
    CHECK(dxmtUpscaleFactor("dxgi.syncInterval = 0\n  d3d11.metalSpatialUpscaleFactor\t= 3\r\n", 1.0f) == 3.0f);
    // DXMT_CONFIG; the last setting wins.
    CHECK(dxmtUpscaleFactor("dxgi.x=1;d3d11.metalSpatialUpscaleFactor=1.25;d3d11.metalSpatialUpscaleFactor=2.5", 1.0f) == 2.5f);
    // Not the setting: commented out, another key, no value.
    CHECK(dxmtUpscaleFactor("#d3d11.metalSpatialUpscaleFactor = 2\n", 1.0f) == 1.0f);
    CHECK(dxmtUpscaleFactor("d3d11.metalSpatialUpscaleFactorMax = 2\n", 1.0f) == 1.0f);
    CHECK(dxmtUpscaleFactor("d3d11.metalSpatialUpscaleFactor =\n", 1.75f) == 1.75f);
    CHECK(dxmtUpscaleFactor("", 1.0f) == 1.0f);
}

TEST(metalfx_factor_precedence) {
    MetalFxSettings s;
    CHECK(metalFxFactor(s) == 1.0f);  // nothing set: off
    s.configFile = "d3d11.metalSpatialUpscaleFactor = 2.0\n";
    CHECK(metalFxFactor(s) == 2.0f);  // a Wine's dxmt.conf
    s.config = "d3d11.metalSpatialUpscaleFactor=1.5";
    CHECK(metalFxFactor(s) == 1.5f);  // DXMT_CONFIG over the file
    s.swapchain = "0";
    CHECK(metalFxFactor(s) == 1.0f);  // DXMT's switch off
    s.winemetalgl = "3";
    CHECK(metalFxFactor(s) == 3.0f);  // WINEMETALGL_METALFX over everything
    s.winemetalgl = "0";
    CHECK(metalFxFactor(s) == 1.0f);
    s.winemetalgl = "9";
    CHECK(metalFxFactor(s) == 4.0f);  // clamped
    s.winemetalgl = "0.5";
    CHECK(metalFxFactor(s) == 1.0f);
}

TEST(metalfx_swapchain_switch_defaults_to_2) {
    MetalFxSettings s;
    s.swapchain = "1";
    CHECK(metalFxFactor(s) == 2.0f);  // DXMT's default factor
    s.config = "d3d11.metalSpatialUpscaleFactor=1.25";
    CHECK(metalFxFactor(s) == 1.25f);
}

TEST(metalfx_metalsharp_wine_decides) {
    MetalFxSettings s;
    s.configFile = "d3d11.metalSpatialUpscaleFactor = 2.0\n";
    s.swapchain = "1";
    s.wine = 1.0;
    CHECK(metalFxFactor(s) == 1.0f);  // MetalSharp Wine without MetalFX for this game: off
    s.wine = 1.5;
    s.swapchain = "0";
    CHECK(metalFxFactor(s) == 1.5f);  // its factor over DXMT's settings
    s.winemetalgl = "2";
    CHECK(metalFxFactor(s) == 2.0f);  // WINEMETALGL_METALFX over it
}
