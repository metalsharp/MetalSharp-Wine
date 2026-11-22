// glGet* of compatibility-profile state (GL 3.3 compatibility tables 6.4-6.62).

#include <algorithm>

#include "core/compat/compat_api.h"
#include "core/compat/display_list.h"
#include "core/compat/fixed_state.h"
#include "core/compat/immediate.h"
#include "core/context.h"
#include "core/format/formats.h"
#include "gl_api.gen.h"

namespace wmgl::api {
namespace {

struct Out {
    double* v;
    char& kind;
    int n = 0;
    int ints(std::initializer_list<double> list) { return put(list, 'i'); }
    int floats(std::initializer_list<double> list) { return put(list, 'f'); }
    int colors(std::initializer_list<double> list) { return put(list, 'c'); }
    int bools(std::initializer_list<bool> list) {
        kind = 'b';
        for (bool b : list) v[n++] = b ? 1 : 0;
        return n;
    }
    int put(std::initializer_list<double> list, char k) {
        kind = k;
        for (double x : list) v[n++] = x;
        return n;
    }
    int floats(const float* p, int count) {
        kind = 'f';
        for (int i = 0; i < count; ++i) v[n++] = p[i];
        return n;
    }
    int colors(const float* p, int count) {
        floats(p, count);
        kind = 'c';
        return n;
    }
    int matrix(const Mat4& m, bool transpose) {
        const Mat4 t = transpose ? m.transpose() : m;
        return floats(t.m, 16);
    }
};

const FormatInfo* attachmentInfo(const Attachment& a) {
    if (a.type == GL_RENDERBUFFER && a.renderbuffer) return a.renderbuffer->format;
    if (a.type == GL_TEXTURE && a.texture) {
        const int face = a.cubeFace ? static_cast<int>(a.cubeFace - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        if (a.level >= 0 && a.level < Texture::kMaxLevels) return a.texture->image(face, a.level).format;
    }
    return nullptr;
}

// RED_BITS ... STENCIL_BITS of the draw framebuffer.
int framebufferBits(Context& ctx, GLenum pname) {
    if (Framebuffer* fb = ctx.state.drawFramebuffer.get()) {
        const FormatInfo* color = nullptr;
        for (GLenum buffer : fb->drawBuffers)
            if (buffer >= GL_COLOR_ATTACHMENT0 && buffer < GL_COLOR_ATTACHMENT0 + Framebuffer::kMaxColor) {
                color = attachmentInfo(fb->color[buffer - GL_COLOR_ATTACHMENT0]);
                break;
            }
        const FormatInfo* depth = attachmentInfo(fb->depth);
        const FormatInfo* stencil = attachmentInfo(fb->stencil);
        switch (pname) {
        case GL_RED_BITS: return color ? color->redBits : 0;
        case GL_GREEN_BITS: return color ? color->greenBits : 0;
        case GL_BLUE_BITS: return color ? color->blueBits : 0;
        case GL_ALPHA_BITS: return color && !color->storedAlphaIsOne ? color->alphaBits : 0;
        case GL_DEPTH_BITS: return depth ? depth->depthBits : 0;
        case GL_STENCIL_BITS: return stencil ? stencil->stencilBits : 0;
        default: return 0;
        }
    }
    Surface* surface = ctx.drawSurface();
    if (!surface) return 0;
    const wmgl_pixel_format& pf = surface->pixelFormat();
    switch (pname) {
    case GL_RED_BITS: return pf.red_bits;
    case GL_GREEN_BITS: return pf.green_bits;
    case GL_BLUE_BITS: return pf.blue_bits;
    case GL_ALPHA_BITS: return pf.alpha_bits;
    case GL_DEPTH_BITS: return pf.depth_bits;
    case GL_STENCIL_BITS: return pf.stencil_bits;
    default: return 0;
    }
}

int arrayState(Context& ctx, Out& o, int slot, GLenum what) {
    const VertexAttrib& a = ctx.state.vertexArray->attribs[static_cast<size_t>(slot)];
    switch (what) {
    case 0: return o.bools({a.enabled});
    case 1: return o.ints({static_cast<double>(a.size)});
    case 2: return o.ints({static_cast<double>(a.type)});
    case 3: return o.ints({static_cast<double>(a.stride)});
    default: return o.ints({static_cast<double>(a.buffer ? a.buffer->name : 0)});
    }
}

}  // namespace

int getFixedState(Context& ctx, GLenum pname, double* values, char& kind) {
    if (!ctx.isCompat()) {
        // OpenGL ES keeps the framebuffer's bit depths.
        if (ctx.isEs() && (pname == GL_RED_BITS || pname == GL_GREEN_BITS || pname == GL_BLUE_BITS || pname == GL_ALPHA_BITS ||
                           pname == GL_DEPTH_BITS || pname == GL_STENCIL_BITS)) {
            Out o{values, kind};
            return o.ints({static_cast<double>(framebufferBits(ctx, pname))});
        }
        return 0;
    }
    const ContextState& s = ctx.state;
    const FixedState& f = s.fixed;
    Out o{values, kind};
    const GLuint unit = std::min<GLuint>(s.activeTexture, FixedState::kTextureUnits - 1);
    const int clientTex = kConvTexCoord0 + static_cast<int>(f.clientActiveTexture);
    auto attrib = [&](int slot) { return s.genericAttribs[static_cast<size_t>(slot)].value.f; };
    switch (pname) {
    case GL_CURRENT_TEXTURE_COORDS: case GL_TEXTURE_MATRIX: case GL_TRANSPOSE_TEXTURE_MATRIX: case GL_TEXTURE_STACK_DEPTH:
        if (s.activeTexture >= FixedState::kTextureUnits) {
            ctx.setError(GL_INVALID_OPERATION);  // texture coordinate set state past MAX_TEXTURE_COORDS
            return -1;
        }
        break;
    default: break;
    }
    switch (pname) {
    // --- current values
    case GL_CURRENT_COLOR: return o.colors(attrib(kConvColor), 4);
    case GL_CURRENT_SECONDARY_COLOR: return o.colors(attrib(kConvSecondaryColor), 4);
    case GL_CURRENT_NORMAL: return o.floats(attrib(kConvNormal), 3);
    case GL_CURRENT_TEXTURE_COORDS: return o.floats(attrib(kConvTexCoord0 + static_cast<int>(unit)), 4);
    case GL_CURRENT_FOG_COORD: return o.floats(attrib(kConvFogCoord), 1);
    case GL_CURRENT_INDEX: return o.ints({1});
    case GL_EDGE_FLAG: return o.bools({f.edgeFlag});
    case GL_CURRENT_RASTER_POSITION: return o.floats(f.rasterPos.window, 4);
    case GL_CURRENT_RASTER_DISTANCE: return o.floats({f.rasterPos.distance});
    case GL_CURRENT_RASTER_COLOR: case GL_CURRENT_RASTER_SECONDARY_COLOR: return o.colors(f.rasterPos.color, 4);
    case GL_CURRENT_RASTER_TEXTURE_COORDS: return o.floats(f.rasterPos.texCoord, 4);
    case GL_CURRENT_RASTER_POSITION_VALID: return o.bools({f.rasterPos.valid});
    case GL_CURRENT_RASTER_INDEX: return o.ints({1});
    // --- conventional arrays
    case GL_VERTEX_ARRAY_SIZE: return arrayState(ctx, o, kConvVertex, 1);
    case GL_VERTEX_ARRAY_TYPE: return arrayState(ctx, o, kConvVertex, 2);
    case GL_VERTEX_ARRAY_STRIDE: return arrayState(ctx, o, kConvVertex, 3);
    case GL_VERTEX_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, kConvVertex, 4);
    case GL_NORMAL_ARRAY_TYPE: return arrayState(ctx, o, kConvNormal, 2);
    case GL_NORMAL_ARRAY_STRIDE: return arrayState(ctx, o, kConvNormal, 3);
    case GL_NORMAL_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, kConvNormal, 4);
    case GL_COLOR_ARRAY_SIZE: return arrayState(ctx, o, kConvColor, 1);
    case GL_COLOR_ARRAY_TYPE: return arrayState(ctx, o, kConvColor, 2);
    case GL_COLOR_ARRAY_STRIDE: return arrayState(ctx, o, kConvColor, 3);
    case GL_COLOR_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, kConvColor, 4);
    case GL_SECONDARY_COLOR_ARRAY_SIZE: return arrayState(ctx, o, kConvSecondaryColor, 1);
    case GL_SECONDARY_COLOR_ARRAY_TYPE: return arrayState(ctx, o, kConvSecondaryColor, 2);
    case GL_SECONDARY_COLOR_ARRAY_STRIDE: return arrayState(ctx, o, kConvSecondaryColor, 3);
    case GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, kConvSecondaryColor, 4);
    case GL_FOG_COORD_ARRAY_TYPE: return arrayState(ctx, o, kConvFogCoord, 2);
    case GL_FOG_COORD_ARRAY_STRIDE: return arrayState(ctx, o, kConvFogCoord, 3);
    case GL_FOG_COORD_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, kConvFogCoord, 4);
    case GL_TEXTURE_COORD_ARRAY_SIZE: return arrayState(ctx, o, clientTex, 1);
    case GL_TEXTURE_COORD_ARRAY_TYPE: return arrayState(ctx, o, clientTex, 2);
    case GL_TEXTURE_COORD_ARRAY_STRIDE: return arrayState(ctx, o, clientTex, 3);
    case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: return arrayState(ctx, o, clientTex, 4);
    case GL_INDEX_ARRAY_TYPE: return o.ints({GL_FLOAT});
    case GL_INDEX_ARRAY_STRIDE: case GL_EDGE_FLAG_ARRAY_STRIDE: case GL_INDEX_ARRAY_BUFFER_BINDING:
    case GL_EDGE_FLAG_ARRAY_BUFFER_BINDING:
        return o.ints({0});
    case GL_CLIENT_ACTIVE_TEXTURE: return o.ints({static_cast<double>(GL_TEXTURE0 + f.clientActiveTexture)});
    // --- transformation
    case GL_MATRIX_MODE: return o.ints({static_cast<double>(f.matrixMode)});
    case GL_MODELVIEW_MATRIX: return o.matrix(f.modelview.back(), false);
    case GL_PROJECTION_MATRIX: return o.matrix(f.projection.back(), false);
    case GL_TEXTURE_MATRIX: return o.matrix(f.texture[unit].back(), false);
    case GL_TRANSPOSE_MODELVIEW_MATRIX: return o.matrix(f.modelview.back(), true);
    case GL_TRANSPOSE_PROJECTION_MATRIX: return o.matrix(f.projection.back(), true);
    case GL_TRANSPOSE_TEXTURE_MATRIX: return o.matrix(f.texture[unit].back(), true);
    case GL_COLOR_MATRIX: return o.matrix(f.color.back(), false);
    case GL_TRANSPOSE_COLOR_MATRIX: return o.matrix(f.color.back(), true);
    case GL_MODELVIEW_STACK_DEPTH: return o.ints({static_cast<double>(f.modelview.size())});
    case GL_PROJECTION_STACK_DEPTH: return o.ints({static_cast<double>(f.projection.size())});
    case GL_TEXTURE_STACK_DEPTH: return o.ints({static_cast<double>(f.texture[unit].size())});
    case GL_COLOR_MATRIX_STACK_DEPTH: return o.ints({static_cast<double>(f.color.size())});
    // The current matrix mode's matrix (GL_ARB_vertex_program: any mode, MATRIXi_ARB included).
    case GL_CURRENT_MATRIX_ARB: case GL_TRANSPOSE_CURRENT_MATRIX_ARB: case GL_CURRENT_MATRIX_STACK_DEPTH_ARB: {
        if (f.matrixMode == GL_TEXTURE && s.activeTexture >= FixedState::kTextureUnits) {
            ctx.setError(GL_INVALID_OPERATION);
            return -1;
        }
        const std::vector<Mat4>& stack = const_cast<FixedState&>(f).currentStack(s.activeTexture);
        if (pname == GL_CURRENT_MATRIX_STACK_DEPTH_ARB) return o.ints({static_cast<double>(stack.size())});
        return o.matrix(stack.back(), pname == GL_TRANSPOSE_CURRENT_MATRIX_ARB);
    }
    case GL_PROGRAM_ERROR_POSITION_ARB: return o.ints({static_cast<double>(ctx.arb.errorPosition)});
    // --- lighting
    case GL_SHADE_MODEL: return o.ints({static_cast<double>(f.shadeModel)});
    case GL_COLOR_MATERIAL_FACE: return o.ints({static_cast<double>(f.colorMaterialFace)});
    case GL_COLOR_MATERIAL_PARAMETER: return o.ints({static_cast<double>(f.colorMaterialMode)});
    case GL_LIGHT_MODEL_AMBIENT: return o.colors(f.lightModelAmbient, 4);
    case GL_LIGHT_MODEL_LOCAL_VIEWER: return o.bools({f.lightModelLocalViewer});
    case GL_LIGHT_MODEL_TWO_SIDE: return o.bools({f.lightModelTwoSide});
    case GL_LIGHT_MODEL_COLOR_CONTROL: return o.ints({static_cast<double>(f.lightModelColorControl)});
    // --- fog, alpha test, points, lines
    case GL_FOG_COLOR: return o.colors(f.fogColor, 4);
    case GL_FOG_INDEX: return o.floats({0});
    case GL_FOG_DENSITY: return o.floats({f.fogDensity});
    case GL_FOG_START: return o.floats({f.fogStart});
    case GL_FOG_END: return o.floats({f.fogEnd});
    case GL_FOG_MODE: return o.ints({static_cast<double>(f.fogMode)});
    case GL_FOG_COORD_SRC: return o.ints({static_cast<double>(f.fogCoordSource)});
    case GL_ALPHA_TEST_FUNC: return o.ints({static_cast<double>(f.alphaFunc)});
    case GL_ALPHA_TEST_REF: return o.colors({f.alphaRef});
    case GL_POINT_SIZE_MIN: return o.floats({f.pointSizeMin});
    case GL_POINT_SIZE_MAX: return o.floats({f.pointSizeMax});
    case GL_POINT_DISTANCE_ATTENUATION: return o.floats(f.pointDistanceAttenuation, 3);
    case GL_LINE_STIPPLE_PATTERN: return o.ints({static_cast<double>(f.lineStipplePattern)});
    case GL_LINE_STIPPLE_REPEAT: return o.ints({static_cast<double>(f.lineStippleFactor)});
    // --- pixels
    case GL_ZOOM_X: return o.floats({f.pixelZoom[0]});
    case GL_ZOOM_Y: return o.floats({f.pixelZoom[1]});
    case GL_RED_SCALE: case GL_GREEN_SCALE: case GL_BLUE_SCALE: case GL_ALPHA_SCALE: case GL_DEPTH_SCALE:
        return o.floats({1});
    case GL_RED_BIAS: case GL_GREEN_BIAS: case GL_BLUE_BIAS: case GL_ALPHA_BIAS: case GL_DEPTH_BIAS:
        return o.floats({0});
    case GL_INDEX_SHIFT: case GL_INDEX_OFFSET: return o.ints({0});
    case GL_MAP_COLOR: case GL_MAP_STENCIL: return o.bools({false});
    // --- display lists and attribute stacks
    case GL_LIST_BASE: return o.ints({static_cast<double>(f.listBase)});
    case GL_LIST_INDEX: return o.ints({static_cast<double>(ctx.listCompiler ? ctx.listCompiler->name : 0)});
    case GL_LIST_MODE: return o.ints({static_cast<double>(ctx.listCompiler ? ctx.listCompiler->mode : 0)});
    case GL_MAX_LIST_NESTING: return o.ints({64});
    case GL_ATTRIB_STACK_DEPTH: return o.ints({static_cast<double>(attribStackDepth(ctx, false))});
    case GL_CLIENT_ATTRIB_STACK_DEPTH: return o.ints({static_cast<double>(attribStackDepth(ctx, true))});
    case GL_MAX_ATTRIB_STACK_DEPTH: case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH: return o.ints({kMaxAttribStackDepth});
    // --- implementation limits
    case GL_MAX_MODELVIEW_STACK_DEPTH: return o.ints({FixedState::kModelViewDepth});
    case GL_MAX_PROJECTION_STACK_DEPTH: return o.ints({FixedState::kProjectionDepth});
    case GL_MAX_TEXTURE_STACK_DEPTH: return o.ints({FixedState::kTextureDepth});
    case GL_MAX_COLOR_MATRIX_STACK_DEPTH: return o.ints({FixedState::kColorDepth});
    case GL_MAX_PROGRAM_MATRICES_ARB: return o.ints({FixedState::kProgramMatrices});
    case GL_MAX_PROGRAM_MATRIX_STACK_DEPTH_ARB: return o.ints({FixedState::kProgramMatrixDepth});
    case GL_MAX_LIGHTS: return o.ints({FixedState::kLights});
    // OpenGL 1.x's names of the first draw buffer's colour blend factors,
    // which City of Heroes' renderer reads as it starts (WineMetalGL #94)
    case GL_BLEND_SRC: return o.ints({static_cast<double>(s.blend[0].srcRGB)});
    case GL_BLEND_DST: return o.ints({static_cast<double>(s.blend[0].dstRGB)});
    case GL_MAX_CLIP_PLANES: return o.ints({FixedState::kClipPlanes});
    case GL_MAX_TEXTURE_UNITS: case GL_MAX_TEXTURE_COORDS: return o.ints({FixedState::kTextureUnits});
    case GL_MAX_EVAL_ORDER: return o.ints({8});
    case GL_MAX_PIXEL_MAP_TABLE: return o.ints({256});
    case GL_MAX_NAME_STACK_DEPTH: return o.ints({64});
    case GL_NAME_STACK_DEPTH: return o.ints({static_cast<double>(f.nameStack.size())});
    case GL_SELECTION_BUFFER_SIZE: case GL_FEEDBACK_BUFFER_SIZE: return o.ints({0});
    case GL_FEEDBACK_BUFFER_TYPE: return o.ints({GL_2D});
    case GL_RENDER_MODE: return o.ints({static_cast<double>(f.renderMode)});
    // --- framebuffer
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: case GL_DEPTH_BITS: case GL_STENCIL_BITS:
        return o.ints({static_cast<double>(framebufferBits(ctx, pname))});
    case GL_INDEX_BITS: case GL_ACCUM_RED_BITS: case GL_ACCUM_GREEN_BITS: case GL_ACCUM_BLUE_BITS:
    case GL_ACCUM_ALPHA_BITS: case GL_AUX_BUFFERS:
        return o.ints({0});
    case GL_RGBA_MODE: return o.bools({true});
    case GL_INDEX_MODE: return o.bools({false});
    case GL_ACCUM_CLEAR_VALUE: return o.colors(f.accumClear, 4);
    case GL_INDEX_CLEAR_VALUE: case GL_INDEX_WRITEMASK: return o.ints({0});
    // --- hints
    case GL_PERSPECTIVE_CORRECTION_HINT: return o.ints({static_cast<double>(f.hintPerspective)});
    case GL_POINT_SMOOTH_HINT: return o.ints({static_cast<double>(f.hintPointSmooth)});
    case GL_FOG_HINT: return o.ints({static_cast<double>(f.hintFog)});
    case GL_GENERATE_MIPMAP_HINT: return o.ints({static_cast<double>(s.hintGenerateMipmap)});
    default: return 0;
    }
}

}  // namespace wmgl::api
