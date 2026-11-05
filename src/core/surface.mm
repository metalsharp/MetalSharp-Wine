#include "core/surface.h"

#import <MetalFX/MetalFX.h>
#include <dlfcn.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/metal/command_stream.h"
#include "core/metal/device.h"
#include "core/stats.h"
#include "core/util/log.h"
#include "core/util/metalfx_config.h"
#include "core/util/thread_qos.h"

namespace wmgl {

namespace {

std::string readFile(const char* path) {
    std::string text;
    if (FILE* f = std::fopen(path, "rb")) {
        char buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
        std::fclose(f);
    }
    return text;
}

// The MetalFX upscale factor of window back buffers (1: off); see
// metalFxFactor. The Wine installation's etc/dxmt.conf is next to the lib
// directory holding the core (lib/wine/<arch>-unix/libwinemetalgl.dylib).
float windowUpscale() {
    static const float factor = [] {
        MetalFxSettings settings;
        settings.winemetalgl = std::getenv("WINEMETALGL_METALFX");
        settings.swapchain = std::getenv("DXMT_METALFX_SPATIAL_SWAPCHAIN");
        settings.config = std::getenv("DXMT_CONFIG");
        if (const char* file = std::getenv("DXMT_CONFIG_FILE")) {
            settings.configFile = readFile(file);
        } else {
            Dl_info info;
            char resolved[PATH_MAX];
            if (dladdr(reinterpret_cast<const void*>(&readFile), &info) && info.dli_fname &&
                realpath(info.dli_fname, resolved)) {
                std::string path = resolved;
                for (int up = 0; up < 4; ++up) path = path.substr(0, path.find_last_of('/'));
                settings.configFile = readFile((path + "/etc/dxmt.conf").c_str());
            }
        }
        const float f = metalFxFactor(settings);
        if (f <= 1.0f) return 1.0f;
        if (![MTLFXSpatialScalerDescriptor supportsDevice:Device::get().mtl()]) {
            WMGL_WARN(Surface, "MetalFX spatial upscaling (x%.2f) is not supported on this device", f);
            return 1.0f;
        }
        WMGL_INFO(Surface, "MetalFX spatial upscaling of window back buffers: x%.2f", f);
        return f;
    }();
    return factor;
}

}  // namespace

const std::vector<wmgl_pixel_format>& pixelFormats() {
    static const std::vector<wmgl_pixel_format> formats = [] {
        std::vector<wmgl_pixel_format> list;
        struct DepthStencil { int depth, stencil; };
        const DepthStencil depthStencil[] = {{24, 8}, {24, 0}, {32, 8}, {0, 0}, {16, 0}};
        const int samples[] = {0, 2, 4};
        // Window formats first (win32u requires on-screen formats to precede the rest).
        for (int pbuffer = 0; pbuffer < 2; ++pbuffer) {
            for (int srgb = 0; srgb < 2; ++srgb)
                for (int ms : samples)
                    for (const DepthStencil& ds : depthStencil)
                        for (int doubleBuffer = 1; doubleBuffer >= 0; --doubleBuffer) {
                            if (pbuffer && doubleBuffer) continue;
                            wmgl_pixel_format f = {};
                            f.draw_to_window = !pbuffer;
                            f.draw_to_pbuffer = pbuffer;
                            f.double_buffer = doubleBuffer;
                            // RGBA8: 32 with its alpha, as Windows drivers count it (and WGL_COLOR_BITS_ARB 32,
                            // which Minecraft asks for, matches).
                            f.color_bits = 32;
                            f.red_bits = f.green_bits = f.blue_bits = f.alpha_bits = 8;
                            f.red_shift = 16; f.green_shift = 8; f.blue_shift = 0; f.alpha_shift = 24;
                            f.depth_bits = ds.depth;
                            f.stencil_bits = ds.stencil;
                            f.sample_buffers = ms ? 1 : 0;
                            f.samples = ms;
                            f.srgb_capable = srgb;
                            f.max_pbuffer_width = f.max_pbuffer_height = 16384;
                            f.max_pbuffer_pixels = 16384 * 16384;
                            list.push_back(f);
                        }
            // RGB565 without depth, stencil or samples (OpenGL ES's
            // rgb565d0s0ms0 configuration): RGBA8 storage, alpha one.
            for (int doubleBuffer = 1; doubleBuffer >= 0; --doubleBuffer) {
                if (pbuffer && doubleBuffer) continue;
                wmgl_pixel_format f = {};
                f.draw_to_window = !pbuffer;
                f.draw_to_pbuffer = pbuffer;
                f.double_buffer = doubleBuffer;
                f.color_bits = 16;
                f.red_bits = 5;
                f.green_bits = 6;
                f.blue_bits = 5;
                f.red_shift = 11; f.green_shift = 5; f.blue_shift = 0;
                f.max_pbuffer_width = f.max_pbuffer_height = 16384;
                f.max_pbuffer_pixels = 16384 * 16384;
                list.push_back(f);
            }
        }
        return list;
    }();
    return formats;
}

unsigned onscreenPixelFormatCount() {
    static const unsigned count = [] {
        unsigned n = 0;
        for (const auto& f : pixelFormats()) n += f.draw_to_window;
        return n;
    }();
    return count;
}

Surface::Surface(CAMetalLayer* layer, int format, int width, int height)
    : layer_(layer), format_(format), width_(std::max(1, width)), height_(std::max(1, height)) {
    if (format_ < 1 || format_ > static_cast<int>(pixelFormats().size())) format_ = 1;
    samples_ = std::max(1, pixelFormat().samples);
    if (layer_) {
        layer_.device = Device::get().mtl();
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer_.framebufferOnly = YES;
        layer_.maximumDrawableCount = 3;
        layer_.displaySyncEnabled = YES;
    }
    if (layer_) upscale_ = windowUpscale();
    sizeDrawable();
    allocate();
}

// Swap interval 0 (see surface.h).
struct Surface::Mailbox {
    static constexpr int kSlots = 3;
    enum State { Free, Pending, Ready, Presenting };
    std::mutex mutex;
    std::condition_variable changed;
    id<MTLTexture> slots[kSlots] = {};
    State state[kSlots] = {Free, Free, Free};
    uint64_t sequence[kSlots] = {};
    uint64_t nextSequence = 1;
    bool stop = false;
    id<MTLCommandQueue> queue = nil;
    std::thread thread;
};

Surface::~Surface() { stopMailbox(); }

void Surface::stopMailbox() {
    if (!mailbox_) return;
    {
        std::lock_guard<std::mutex> lock(mailbox_->mutex);
        mailbox_->stop = true;
    }
    mailbox_->changed.notify_all();
    if (mailbox_->thread.joinable()) mailbox_->thread.join();
    // Wait for presents still running on the GPU (their handlers touch the mailbox).
    std::unique_lock<std::mutex> lock(mailbox_->mutex);
    mailbox_->changed.wait(lock, [&] {
        for (Mailbox::State st : mailbox_->state)
            if (st == Mailbox::Presenting || st == Mailbox::Pending) return false;
        return true;
    });
    lock.unlock();
    mailbox_.reset();
}

void Surface::allocate() {
    id<MTLDevice> device = Device::get().mtl();
    const wmgl_pixel_format& pf = pixelFormat();
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf.srgb_capable ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm
                                     width:width_
                                    height:height_
                                 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    // Only sRGB views of these (linearColor): no format views, which would
    // turn off lossless compression of the back buffer.
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    if (samples_ > 1) {
        MTLTextureDescriptor* ms = [desc copy];
        ms.textureType = MTLTextureType2DMultisample;
        ms.sampleCount = samples_;
        ms.usage = MTLTextureUsageRenderTarget;
        color_ = [device newTextureWithDescriptor:ms];
        resolve_ = [device newTextureWithDescriptor:desc];
    } else {
        color_ = [device newTextureWithDescriptor:desc];
        resolve_ = nil;
    }
    color_.label = @"default framebuffer";
    depthStencil_ = depthResolve_ = nil;
    if (pf.depth_bits || pf.stencil_bits) {
        MTLPixelFormat dsFormat = pf.stencil_bits ? MTLPixelFormatDepth32Float_Stencil8
                                  : pf.depth_bits == 16 ? MTLPixelFormatDepth16Unorm
                                                        : MTLPixelFormatDepth32Float;
        if (!pf.depth_bits) dsFormat = MTLPixelFormatStencil8;
        MTLTextureDescriptor* ds = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dsFormat
                                                                                       width:width_
                                                                                      height:height_
                                                                                   mipmapped:NO];
        ds.storageMode = MTLStorageModePrivate;
        ds.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        if (samples_ > 1) {
            ds.textureType = MTLTextureType2DMultisample;
            ds.sampleCount = samples_;
        }
        depthStencil_ = [device newTextureWithDescriptor:ds];
        depthStencil_.label = @"default depth/stencil";
    }
}

void Surface::resize(int width, int height, CommandStream* stream) {
    width = std::max(1, width);
    height = std::max(1, height);
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == width_ && height == height_) return;
    id<MTLTexture> oldColor = color_;
    const int oldWidth = width_, oldHeight = height_;
    width_ = width;
    height_ = height;
    sizeDrawable();
    allocate();
    // Keep the overlapping contents: Wine reports a resize right before the
    // swap that presents the frame drawn at the old size.
    if (!oldColor || oldColor.sampleCount != color_.sampleCount) return;
    const MTLSize size = MTLSizeMake(std::min(oldWidth, width_), std::min(oldHeight, height_), 1);
    auto copy = [&](id<MTLBlitCommandEncoder> blit) {
        [blit copyFromTexture:oldColor sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                    toTexture:color_ destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    };
    if (stream) {
        copy(stream->blit());
        return;
    }
    static Queue* queue = new Queue();
    CommandStream own(*queue);
    copy(own.blit());
    own.flush();
}

void Surface::setSwapInterval(int interval) {
    interval_ = interval;
    if (layer_) layer_.displaySyncEnabled = interval != 0;
}

id<MTLTexture> Surface::linearColor() {
    if (color_.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) return color_;
    if (!linearColor_ || linearColor_.parentTexture != color_)
        linearColor_ = [color_ newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    return linearColor_;
}

void Surface::describe(RenderTargetDesc& target) const {
    target = RenderTargetDesc();
    target.color[0].texture = color_;
    if (depthStencil_) {
        if (hasDepth()) target.depth.texture = depthStencil_;
        if (hasStencil()) target.stencil.texture = depthStencil_;
    }
    target.width = width_;
    target.height = height_;
    target.samples = samples_;
    target.colorResolve = resolve_;
}

void Surface::resolveForRead(CommandStream& stream) {
    if (!resolve_) return;
    // The pass drawing the frame resolves as it ends; else (nothing drawn
    // since: a clear, or an earlier read) an empty pass whose store action
    // resolves into resolve_.
    if (stream.resolveOpenPass(color_)) return;
    stream.resolvePendingClear();
    stream.endEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color_;
    pass.colorAttachments[0].resolveTexture = resolve_;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    [[stream.commandBuffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
}

id<MTLTexture> Surface::resolvedDepthStencil(CommandStream& stream) {
    if (!depthStencil_ || samples_ <= 1) return depthStencil_;
    if (!depthResolve_) {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depthStencil_.pixelFormat
                                                                                         width:width_
                                                                                        height:height_
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depthResolve_ = [Device::get().mtl() newTextureWithDescriptor:desc];
        depthResolve_.label = @"default depth/stencil (resolved)";
    }
    stream.resolvePendingClear();
    stream.endEncoder();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (hasDepth()) {
        pass.depthAttachment.texture = depthStencil_;
        pass.depthAttachment.resolveTexture = depthResolve_;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
    }
    if (hasStencil()) {
        pass.stencilAttachment.texture = depthStencil_;
        pass.stencilAttachment.resolveTexture = depthResolve_;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    }
    [[stream.commandBuffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    return depthResolve_;
}

namespace {

// WINEMETALGL_DUMP_FRAMES=<dir> writes every swapped back buffer to
// <dir>/frame-<n>.ppm (debugging aid; GL row 0 is the bottom of the image).
void dumpFrame(CommandStream& stream, id<MTLTexture> color, int width, int height) {
    static const char* dir = std::getenv("WINEMETALGL_DUMP_FRAMES");
    if (!dir || !color || color.sampleCount > 1) return;
    static int frame = 0;
    const NSUInteger rowBytes = static_cast<NSUInteger>(width) * 4;
    id<MTLBuffer> staging = [Device::get().mtl() newBufferWithLength:rowBytes * static_cast<NSUInteger>(height)
                                                             options:MTLResourceStorageModeShared];
    [stream.blit() copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
            destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * static_cast<NSUInteger>(height)];
    stream.finish();
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/frame-%04d.ppm", dir, frame++);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", width, height);
        const uint8_t* p = static_cast<const uint8_t*>(staging.contents);
        for (int y = height - 1; y >= 0; --y)
            for (int x = 0; x < width; ++x) std::fwrite(p + (static_cast<size_t>(y) * width + x) * 4, 1, 3, f);
        std::fclose(f);
    }
}

}  // namespace

void Surface::sizeDrawable() {
    if (!layer_) return;
    const int width = outputWidth_ > 0 ? outputWidth_ : width_, height = outputHeight_ > 0 ? outputHeight_ : height_;
    layer_.drawableSize = CGSizeMake(std::lround(width * upscale_), std::lround(height * upscale_));
}

void Surface::setOutputSize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == outputWidth_ && height == outputHeight_) return;
    WMGL_INFO(Surface, "output %dx%d for a %dx%d back buffer", width, height, width_, height_);
    outputWidth_ = width;
    outputHeight_ = height;
    sizeDrawable();
}

// Called by the thread presenting (the mailbox presenter for interval 0).
id<MTLTexture> Surface::upscale(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> source, NSUInteger outWidth,
                                NSUInteger outHeight) {
    if (upscaleFailed_ || (outWidth == source.width && outHeight == source.height)) return source;
    // MetalFX only scales up.
    if (outWidth < source.width || outHeight < source.height) return source;
    static const bool supported = [MTLFXSpatialScalerDescriptor supportsDevice:Device::get().mtl()];
    if (!supported) {
        WMGL_WARN(Surface, "MetalFX spatial upscaling is not supported on this device; frames are stretched");
        upscaleFailed_ = true;
        return source;
    }
    id<MTLFXSpatialScaler> scaler = upscaler_;
    if (!scaler || scaler.inputWidth != source.width || scaler.inputHeight != source.height ||
        scaler.colorTextureFormat != source.pixelFormat || scaler.outputWidth != outWidth ||
        scaler.outputHeight != outHeight) {
        MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
        desc.inputWidth = source.width;
        desc.inputHeight = source.height;
        desc.outputWidth = outWidth;
        desc.outputHeight = outHeight;
        desc.colorTextureFormat = source.pixelFormat;
        desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        // The back buffer holds display-encoded colour.
        desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        scaler = [desc newSpatialScalerWithDevice:Device::get().mtl()];
        upscaler_ = scaler;
        upscaled_ = nil;
        if (!scaler) {
            // The present pass stretches the back buffer over the drawable.
            WMGL_WARN(Surface, "MetalFX spatial scaler creation failed (%lux%lu, format %lu); presenting without it",
                      static_cast<unsigned long>(source.width), static_cast<unsigned long>(source.height),
                      static_cast<unsigned long>(source.pixelFormat));
            upscaleFailed_ = true;
            return source;
        }
        MTLTextureDescriptor* out = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:outWidth
                                                                                      height:outHeight
                                                                                   mipmapped:NO];
        out.storageMode = MTLStorageModePrivate;
        out.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
        upscaled_ = [Device::get().mtl() newTextureWithDescriptor:out];
        WMGL_INFO(Surface, "MetalFX spatial scaler %lux%lu -> %lux%lu", static_cast<unsigned long>(source.width),
                  static_cast<unsigned long>(source.height), static_cast<unsigned long>(outWidth),
                  static_cast<unsigned long>(outHeight));
    }
    if (!upscaled_) return source;
    scaler.colorTexture = source;
    scaler.outputTexture = upscaled_;
    scaler.inputContentWidth = source.width;
    scaler.inputContentHeight = source.height;
    [scaler encodeToCommandBuffer:commandBuffer];
    return upscaled_;
}

void Surface::ensurePresentPipeline() {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    if (presentPipeline_) return;
    id<MTLLibrary> library = Device::get().utilityLibrary();
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"wmgl_fullscreen_vs"];
    desc.fragmentFunction = [library newFunctionWithName:@"wmgl_copy_fs"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError* error = nil;
    presentPipeline_ = [Device::get().mtl() newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!presentPipeline_) WMGL_ERR(Pipeline, "present pipeline: %s", error.localizedDescription.UTF8String);
    MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
    // Linear: a back buffer MetalFX did not scale is stretched over the
    // drawable (the same pixels when the sizes match).
    s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
    presentSampler_ = [Device::get().mtl() newSamplerStateWithDescriptor:s];
}

void Surface::encodePresent(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> source, id<CAMetalDrawable> drawable) {
    // The back buffer may be sRGB-encoded; present (and upscale) the stored
    // bytes unchanged.
    if (source.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        source = [source newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    // Shown at another size (setOutputSize): in the largest rectangle of the
    // back buffer's shape, centred, black around it (a 4:3 mode on a 16:9
    // screen), as a display's scaler keeps a mode's shape.
    const NSUInteger drawableWidth = drawable.texture.width, drawableHeight = drawable.texture.height;
    NSUInteger width = drawableWidth, height = drawableHeight;
    if (outputWidth_ > 0 && outputHeight_ > 0 && source.width && source.height) {
        const double shape = static_cast<double>(source.width) / source.height;
        const double screen = static_cast<double>(drawableWidth) / drawableHeight;
        if (std::fabs(shape / screen - 1.0) > 0.01) {
            if (shape < screen) width = static_cast<NSUInteger>(std::lround(drawableHeight * shape));
            else height = static_cast<NSUInteger>(std::lround(drawableWidth / shape));
        }
    }
    source = upscale(commandBuffer, source, width, height);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    const bool bars = width != drawableWidth || height != drawableHeight;
    pass.colorAttachments[0].loadAction = bars ? MTLLoadActionClear : MTLLoadActionDontCare;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (bars)
        [encoder setViewport:(MTLViewport){static_cast<double>((drawableWidth - width) / 2),
                                           static_cast<double>((drawableHeight - height) / 2), static_cast<double>(width),
                                           static_cast<double>(height), 0.0, 1.0}];
    [encoder setRenderPipelineState:presentPipeline_];
    // No flip in the shader: the drawable's top row samples the back buffer's
    // last row, which is GL's top row (GL row 0 = Metal row 0 = window bottom).
    const float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    [encoder setVertexBytes:params length:sizeof(params) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentSamplerState:presentSampler_ atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

bool Surface::swap(CommandStream& stream) {
    stats::Timer presentTimer(&stats::Frame::presentNanos);
    resolveForRead(stream);
    dumpFrame(stream, resolvedColor(), width_, height_);
    if (!layer_) {
        stream.flush();
        return true;
    }
    stream.resolvePendingClear();
    stream.endEncoder();
    ensurePresentPipeline();
    if (interval_ == 0) {
        const bool presented = swapMailbox(stream);
        limitFrameRate();
        return presented;
    }
    stopMailbox();
    // The frame's work reaches the GPU before the wait for a drawable (with
    // vsync, nextDrawable blocks until one is free), and the present is a
    // small command buffer of its own after it.
    stream.flush();
    id<CAMetalDrawable> drawable = nil;
    {
        stats::Timer drawableTimer(&stats::Frame::drawableNanos);
        drawable = [layer_ nextDrawable];
    }
    if (!drawable) {
        WMGL_WARN(Surface, "no drawable available");
        stream.flush();
        return false;
    }
    encodePresent(stream.commandBuffer(), resolvedColor(), drawable);
    stream.present(drawable);
    stream.flush();
    limitFrameRate();
    return true;
}

// WINEMETALGL_MAX_FRAME_RATE=<fps> caps how often SwapBuffers returns, as
// DXVK's and DXMT's frame rate limits do: older games tie their physics to
// the frame rate and have no limit of their own but vsync, which runs them at
// 120 fps on a 120 Hz display. The frame's work is on the GPU before the wait,
// and a frame later than one interval starts the schedule again rather than
// letting the next ones hurry to catch up.
void Surface::limitFrameRate() {
    static const double fps = [] {
        const char* value = std::getenv("WINEMETALGL_MAX_FRAME_RATE");
        const double rate = value && *value ? std::strtod(value, nullptr) : 0.0;
        if (rate > 0.0) WMGL_INFO(Surface, "frame rate limited to %g fps (WINEMETALGL_MAX_FRAME_RATE)", rate);
        return rate > 0.0 ? rate : 0.0;
    }();
    if (fps <= 0.0) return;
    static const uint64_t interval = [] {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        return static_cast<uint64_t>(1e9 / fps * timebase.denom / timebase.numer);
    }();
    const uint64_t now = mach_absolute_time();
    if (!nextFrameTime_ || now > nextFrameTime_ + interval)
        nextFrameTime_ = now;
    else if (now < nextFrameTime_)
        mach_wait_until(nextFrameTime_);
    nextFrameTime_ += interval;
}

bool Surface::swapMailbox(CommandStream& stream) {
    id<MTLTexture> source = resolvedColor();
    if (mailbox_ && (mailbox_->slots[0].width != source.width || mailbox_->slots[0].height != source.height ||
                     mailbox_->slots[0].pixelFormat != source.pixelFormat))
        stopMailbox();
    if (!mailbox_) {
        mailbox_ = std::make_unique<Mailbox>();
        Mailbox* box = mailbox_.get();
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                                                         width:source.width
                                                                                        height:source.height
                                                                                     mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        for (int i = 0; i < Mailbox::kSlots; ++i) box->slots[i] = [Device::get().mtl() newTextureWithDescriptor:desc];
        box->queue = [Device::get().mtl() newCommandQueue];
        CAMetalLayer* layer = layer_;
        box->thread = std::thread([this, box, layer] {
            raiseThreadQos("present");  // the render thread waits for its slots
            for (;;) {
                int slot = -1;
                {
                    std::unique_lock<std::mutex> lock(box->mutex);
                    box->changed.wait(lock, [&] {
                        if (box->stop) return true;
                        for (int i = 0; i < Mailbox::kSlots; ++i)
                            if (box->state[i] == Mailbox::Ready) return true;
                        return false;
                    });
                    if (box->stop) return;
                    // The newest finished frame; older finished ones are dropped.
                    for (int i = 0; i < Mailbox::kSlots; ++i)
                        if (box->state[i] == Mailbox::Ready && (slot < 0 || box->sequence[i] > box->sequence[slot])) slot = i;
                    for (int i = 0; i < Mailbox::kSlots; ++i)
                        if (i != slot && box->state[i] == Mailbox::Ready) box->state[i] = Mailbox::Free;
                    box->state[slot] = Mailbox::Presenting;
                }
                box->changed.notify_all();
                @autoreleasepool {
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    id<MTLCommandBuffer> commandBuffer = [box->queue commandBuffer];
                    if (drawable) {
                        encodePresent(commandBuffer, box->slots[slot], drawable);
                        [commandBuffer presentDrawable:drawable];
                    }
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                        // Notify under the lock: once it is released, stopMailbox()
                        // may see no work left and free the mailbox.
                        std::lock_guard<std::mutex> lock(box->mutex);
                        box->state[slot] = Mailbox::Free;
                        box->changed.notify_all();
                    }];
                    [commandBuffer commit];
                }
            }
        });
    }
    Mailbox* box = mailbox_.get();
    int slot = -1;
    {
        // A free slot, else the finished frame the presenter has not taken
        // yet (it is dropped). Only a GPU two frames behind makes this wait.
        stats::Timer drawableTimer(&stats::Frame::drawableNanos);
        std::unique_lock<std::mutex> lock(box->mutex);
        box->changed.wait(lock, [&] {
            for (int i = 0; i < Mailbox::kSlots; ++i)
                if (box->state[i] == Mailbox::Free || box->state[i] == Mailbox::Ready) return true;
            return false;
        });
        for (int i = 0; i < Mailbox::kSlots && slot < 0; ++i)
            if (box->state[i] == Mailbox::Free) slot = i;
        for (int i = 0; i < Mailbox::kSlots && slot < 0; ++i)
            if (box->state[i] == Mailbox::Ready) slot = i;
        box->state[slot] = Mailbox::Pending;
        box->sequence[slot] = box->nextSequence++;
    }
    id<MTLBlitCommandEncoder> blit = stream.blit();
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source.width, source.height, 1) toTexture:box->slots[slot] destinationSlice:0
         destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    stream.endEncoder();
    [stream.commandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
        // Notify under the lock (see the present handler).
        std::lock_guard<std::mutex> lock(box->mutex);
        box->state[slot] = Mailbox::Ready;
        box->changed.notify_all();
    }];
    stream.flush();
    return true;
}

}  // namespace wmgl
