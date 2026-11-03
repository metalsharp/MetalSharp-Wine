// Window and pbuffer surfaces: the default framebuffer.
//
// The default framebuffer is an offscreen back buffer in GL row order (row 0
// is the bottom). Swapping draws it, flipped, into the CAMetalLayer's
// drawable. Reads of GL_FRONT/GL_BACK and swap-copy semantics therefore
// never depend on the drawable.
//
// With a swap interval of 1 or more, SwapBuffers waits for a drawable, which
// paces the application at the display rate. With interval 0 it must not
// wait for the window server: frames go through a mailbox. The swap copies
// the back buffer into a free slot on the GPU and returns; a presenter
// thread shows the newest finished slot and drops older ones.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/gl/types.h"
#include "core/util/ref.h"
#include "winemetalgl/driver.h"

namespace wmgl {

class CommandStream;
struct RenderTargetDesc;

// The pixel formats WineMetalGL offers (1-based index = WGL pixel format).
const std::vector<wmgl_pixel_format>& pixelFormats();
unsigned onscreenPixelFormatCount();

class Surface : public RefCounted {
public:
    Surface(CAMetalLayer* layer, int format, int width, int height);
    ~Surface() override;

    bool isWindow() const { return layer_ != nil; }
    int format() const { return format_; }
    const wmgl_pixel_format& pixelFormat() const { return pixelFormats()[format_ - 1]; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint32_t samples() const { return samples_; }

    // Resizes the buffers (contents become undefined, as GL allows).
    // `stream`: the current context's stream when it renders to this
    // surface, so the copy of the old contents is ordered after its work.
    void resize(int width, int height, CommandStream* stream = nullptr);
    // The size the back buffer is shown at, in the window's pixels (0: its
    // own): UpscaleGL's games render smaller than their window, and MetalFX
    // scales the frame up to it (see the Wine module's upscalegl.c).
    void setOutputSize(int width, int height);
    void setSwapInterval(int interval);

    id<MTLTexture> color() const { return color_; }          // what draws render into (MSAA if samples > 1)
    id<MTLTexture> resolvedColor() const { return resolve_ ? resolve_ : color_; }
    id<MTLTexture> depthStencil() const { return depthStencil_; }
    // Non-sRGB view of the colour buffer, for rendering with GL_FRAMEBUFFER_SRGB disabled.
    id<MTLTexture> linearColor();
    bool hasDepth() const { return pixelFormat().depth_bits > 0; }
    bool hasStencil() const { return pixelFormat().stencil_bits > 0; }
    bool srgbCapable() const { return pixelFormat().srgb_capable; }
    // The GL format of the color buffer: RGB565 pixel formats (stored as
    // RGBA8, alpha reading one) for OpenGL ES's rgb565 configurations.
    // `srgb`: whether an sRGB-capable buffer counts as sRGB (OpenGL ES
    // contexts treat it as linear; see Context::windowSrgb).
    GLenum colorFormat(bool srgb) const {
        return srgb && srgbCapable() ? GL_SRGB8_ALPHA8 : pixelFormat().red_bits == 5 ? GL_RGB565 : GL_RGBA8;
    }
    // The GL format of the depth/stencil buffer (32-bit depth is DEPTH32F).
    GLenum depthStencilFormat() const {
        const int depth = pixelFormat().depth_bits;
        if (hasStencil()) return depth == 32 ? GL_DEPTH32F_STENCIL8 : depth ? GL_DEPTH24_STENCIL8 : GL_STENCIL_INDEX8;
        return depth == 16 ? GL_DEPTH_COMPONENT16 : depth == 32 ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    }
    void describe(RenderTargetDesc& target) const;

    // Resolves (if multisampled) and presents the back buffer. The stream's
    // command buffer is committed.
    bool swap(CommandStream& stream);
    // Resolves multisampled colour into the single-sample texture for reads.
    void resolveForRead(CommandStream& stream);
    // The depth/stencil buffer for reads: resolved (sample 0) into a
    // single-sample texture when multisampled.
    id<MTLTexture> resolvedDepthStencil(CommandStream& stream);

private:
    void allocate();
    struct Mailbox;
    bool swapMailbox(CommandStream& stream);
    void stopMailbox();
    // WINEMETALGL_MAX_FRAME_RATE: waits until the next frame's time (see surface.mm).
    void limitFrameRate();
    uint64_t nextFrameTime_ = 0;  // mach_absolute_time units; 0 before the first frame
    void ensurePresentPipeline();
    // Encodes the full-screen copy of `source` into `drawable`.
    void encodePresent(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> source, id<CAMetalDrawable> drawable);

    std::mutex mutex_;
    CAMetalLayer* layer_ = nil;
    int format_ = 1;
    int width_ = 0, height_ = 0;
    uint32_t samples_ = 1;
    int interval_ = 1;
    id<MTLTexture> color_ = nil, resolve_ = nil, depthStencil_ = nil, depthResolve_ = nil, linearColor_ = nil;
    std::mutex pipelineMutex_;
    id<MTLRenderPipelineState> presentPipeline_ = nil;
    id<MTLSamplerState> presentSampler_ = nil;
    // MetalFX spatial upscaling of the back buffer into a drawable `upscale_`
    // times its output size (metalFxFactor; 1: off), through `upscaled_`.
    float upscale_ = 1.0f;
    int outputWidth_ = 0, outputHeight_ = 0;  // setOutputSize
    bool upscaleFailed_ = false;
    id upscaler_ = nil;  // id<MTLFXSpatialScaler>
    id<MTLTexture> upscaled_ = nil;
    void sizeDrawable();
    // `source` scaled to `width` x `height` by MetalFX (itself when the sizes
    // match, or MetalFX cannot: the present pass then stretches it).
    id<MTLTexture> upscale(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> source, NSUInteger width, NSUInteger height);
    std::unique_ptr<Mailbox> mailbox_;
};

}  // namespace wmgl
