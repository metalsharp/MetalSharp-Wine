// Compatibility profile (W22): fixed-function processing, immediate mode,
// display lists, client arrays, legacy GLSL built-ins and pixel rectangles.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu.h"

#define WMGL_COMPAT_FUNCS(X)                                                                                   \
    X(glBegin) X(glEnd) X(glVertex2f) X(glVertex3f) X(glColor3f) X(glColor4f) X(glColor4ub) X(glNormal3f)       \
    X(glTexCoord2f) X(glMatrixMode) X(glLoadIdentity) X(glOrtho) X(glTranslatef) X(glRotatef) X(glScalef)       \
    X(glPushMatrix) X(glPopMatrix) X(glRectf) X(glLightfv) X(glMaterialfv) X(glLightModelfv) X(glShadeModel)    \
    X(glTexEnvi) X(glTexEnvfv) X(glTexGeni) X(glTexGenfv) X(glFogi) X(glFogf) X(glFogfv) X(glAlphaFunc)          \
    X(glClipPlane) X(glNewList) X(glEndList) X(glCallList) X(glCallLists) X(glGenLists) X(glDeleteLists)         \
    X(glIsList) X(glListBase) X(glVertexPointer) X(glColorPointer) X(glTexCoordPointer)                          \
    X(glEnableClientState) X(glDisableClientState) X(glClientActiveTexture) X(glMultiTexCoord2f)                \
    X(glPushAttrib) X(glPopAttrib) X(glRasterPos2f) X(glWindowPos2i) X(glBitmap) X(glDrawPixels)                \
    X(glPixelZoom) X(glIsEnabled) X(glColorMaterial) X(glInterleavedArrays) X(glArrayElement)                  \
    X(glFrustum) X(glMultMatrixf) X(glLoadMatrixf) X(glLineWidth) X(glPolygonStipple) X(glGetPolygonStipple) X(glRenderMode) X(glInitNames) X(glPushName) X(glLightf) X(glMaterialf) X(glTexGenf) \
    X(glGetMaterialfv) X(glGetMaterialiv) X(glGetLightiv) X(glTexEnvf) X(glNormalPointer) X(glTexParameteriv)

#define WMGL_COMPAT_DECLARE(name) static PFN_##name name;
WMGL_COMPAT_FUNCS(WMGL_COMPAT_DECLARE)
#undef WMGL_COMPAT_DECLARE

namespace {

void loadCompat() {
#define WMGL_COMPAT_LOAD(name) name = reinterpret_cast<PFN_##name>(wmgl_get_proc_address(#name));
    WMGL_COMPAT_FUNCS(WMGL_COMPAT_LOAD)
#undef WMGL_COMPAT_LOAD
}

const uint8_t kRed[4] = {255, 0, 0, 255}, kGreen[4] = {0, 255, 0, 255}, kBlue[4] = {0, 0, 255, 255},
              kBlack[4] = {0, 0, 0, 255}, kWhite[4] = {255, 255, 255, 255};

bool pixel(int x, int y, const uint8_t want[4], int tolerance = 2) {
    const std::vector<uint8_t> got = gpu::readRGBA8(x, y, 1, 1);
    if (gpu::near(got.data(), want, tolerance)) return true;
    std::printf("  pixel (%d,%d) = %d %d %d %d, want %d %d %d %d\n", x, y, got[0], got[1], got[2], got[3], want[0], want[1],
                want[2], want[3]);
    return false;
}

void clear() {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Window coordinates 0..64 in both directions.
void pixelProjection() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

}  // namespace

#define COMPAT_TEST(name)                         \
    static void name##_body();                    \
    TEST(name) {                                  \
        gpu::Fixture fixture_(64, 64, false);     \
        CHECK(fixture_.ok);                       \
        loadCompat();                             \
        if (fixture_.ok) name##_body();           \
    }                                             \
    static void name##_body()

COMPAT_TEST(compat_immediate_mode) {
    clear();
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(32, 32, kRed));
    // Quads, and a colour that changes between vertices (smooth by default: the left half is green).
    glBegin(GL_QUADS);
    glColor3f(0, 1, 0);
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glEnd();
    CHECK(pixel(16, 32, kGreen));
    CHECK(pixel(48, 32, kRed));
    // Flat shading takes the last vertex of each quad.
    glShadeModel(GL_FLAT);
    glBegin(GL_QUADS);
    glColor3f(1, 0, 0);
    glVertex2f(0, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glColor3f(0, 0, 1);
    glVertex2f(0, 1);
    glEnd();
    glShadeModel(GL_SMOOTH);
    CHECK(pixel(40, 10, kBlue));
    CHECK(pixel(60, 60, kBlue));
    // glColor outside Begin/End is the current colour; glGet reports it.
    float current[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, current);
    CHECK(current[0] == 0 && current[1] == 0 && current[2] == 1 && current[3] == 1);
    // Most commands (glGetError too) are invalid between Begin and End.
    glBegin(GL_POINTS);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnd();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    CHECK_GL();
}

COMPAT_TEST(compat_matrices) {
    clear();
    pixelProjection();
    glColor3f(0, 1, 0);
    glPushMatrix();
    glTranslatef(16, 16, 0);
    glRectf(0, 0, 32, 32);
    glPopMatrix();
    CHECK(pixel(32, 32, kGreen));
    CHECK(pixel(8, 8, kBlack));
    CHECK(pixel(56, 56, kBlack));
    float mv[16] = {};
    glTranslatef(1, 2, 3);
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    CHECK(mv[12] == 1 && mv[13] == 2 && mv[14] == 3 && mv[0] == 1 && mv[15] == 1);
    GLint depth = 0;
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depth);
    CHECK_EQ(depth, 1);
    glPopMatrix();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_STACK_UNDERFLOW));
    // A rotated quad: rotate 90 degrees about z around the centre.
    clear();
    glLoadIdentity();
    glTranslatef(32, 32, 0);
    glRotatef(90, 0, 0, 1);
    glColor3f(1, 0, 0);
    glRectf(0, -4, 30, 4);  // after rotation: a vertical bar above the centre
    CHECK(pixel(32, 50, kRed));
    CHECK(pixel(50, 32, kBlack));
    CHECK_GL();
}

COMPAT_TEST(compat_lighting) {
    clear();
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    const float position[4] = {0, 0, 1, 0}, white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 1}, none[4] = {0, 0, 0, 1};
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, none);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, none);
    glBegin(GL_QUADS);
    glNormal3f(0, 0, 1);  // facing the light: full diffuse
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glNormal3f(0, 0, -1);  // facing away: black
    glVertex2f(0, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glVertex2f(0, 1);
    glEnd();
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlack));
    // Colour material tracks glColor for the diffuse term.
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    glColor3f(0, 0, 1);
    glBegin(GL_TRIANGLES);
    glNormal3f(0, 0, 1);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(32, 32, kBlue));
    // The tracked parameter takes the current colour, and keeps it after
    // COLOR_MATERIAL is disabled (GL 3.3 compatibility section 2.13.3).
    GLfloat diffuse[4] = {};
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, diffuse);
    CHECK(diffuse[0] == 0 && diffuse[1] == 0 && diffuse[2] == 1 && diffuse[3] == 1);
    glDisable(GL_COLOR_MATERIAL);
    // A light at 45 degrees gives cos(45) of the diffuse colour (blue, from the colour material).
    const float slanted[4] = {1, 0, 1, 0};
    glLightfv(GL_LIGHT0, GL_POSITION, slanted);
    glBegin(GL_TRIANGLES);
    glNormal3f(0, 0, 1);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    const uint8_t dim[4] = {0, 0, 181, 255};
    CHECK(pixel(32, 32, dim, 3));
    // glMaterial of a tracked parameter while COLOR_MATERIAL is on is overridden by the current colour.
    glEnable(GL_COLOR_MATERIAL);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
    glGetMaterialfv(GL_BACK, GL_DIFFUSE, diffuse);
    CHECK(diffuse[0] == 0 && diffuse[2] == 1);
    glDisable(GL_COLOR_MATERIAL);
    CHECK(glIsEnabled(GL_LIGHTING) == GL_TRUE);
    glDisable(GL_LIGHTING);
    CHECK_GL();
}

COMPAT_TEST(compat_texture_environment) {
    // A 2x2 texture: red, green / blue, white with alpha 128 in the last texel.
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    auto quad = [] {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, -1);
        glTexCoord2f(1, 1); glVertex2f(1, 1);
        glTexCoord2f(0, 1); glVertex2f(-1, 1);
        glEnd();
    };
    clear();
    glColor4f(1, 1, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    quad();
    CHECK(pixel(16, 16, kRed));
    CHECK(pixel(48, 16, kGreen));
    CHECK(pixel(16, 48, kBlue));
    // MODULATE with a half-intensity colour.
    glColor4f(0.5f, 0.5f, 0.5f, 1);
    quad();
    const uint8_t halfRed[4] = {128, 0, 0, 255};
    CHECK(pixel(16, 16, halfRed, 3));
    // REPLACE ignores the colour.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad();
    CHECK(pixel(48, 16, kGreen));
    // DECAL blends by texture alpha: white at alpha 0.5 over red primary.
    glColor4f(1, 0, 0, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    quad();
    const uint8_t pink[4] = {255, 128, 128, 255};
    CHECK(pixel(48, 48, pink, 3));
    // ADD clamps.
    glColor4f(0, 0, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    quad();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(pixel(16, 16, magenta));
    // BLEND: primary * (1 - Cs) + env colour * Cs.
    const float env[4] = {0, 1, 0, 1};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    glColor4f(0, 0, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
    quad();
    // Red texel: (0,0,1)*(0,1,1) + (0,1,0)*(1,0,0) = blue; green texel: (0,0,1)*(1,0,1) + (0,1,0)*(0,1,0) = cyan.
    const uint8_t cyan[4] = {0, 255, 255, 255};
    CHECK(pixel(16, 16, kBlue));
    CHECK(pixel(48, 16, cyan));
    // COMBINE: SUBTRACT texture - primary.
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
    glColor4f(0.5f, 0, 0, 1);
    quad();
    CHECK(pixel(16, 16, halfRed, 3));
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

COMPAT_TEST(compat_texgen_and_multitexture) {
    const uint8_t stripes[8] = {255, 0, 0, 255, 0, 0, 255, 255};  // 2x1: red | blue
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, stripes);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    // OBJECT_LINEAR s = (x + 1) / 2 over the screen.
    const float plane[4] = {0.5f, 0, 0, 0.5f};
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGenfv(GL_S, GL_OBJECT_PLANE, plane);
    glEnable(GL_TEXTURE_GEN_S);
    clear();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(3, -1);
    glVertex2f(-1, 3);
    glEnd();
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlue));
    glDisable(GL_TEXTURE_GEN_S);
    // Unit 1 modulates unit 0: both bound to the stripes, unit 1 coordinates flipped.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glActiveTexture(GL_TEXTURE0);
    clear();
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(-1, 1);
    glEnd();
    const uint8_t magenta[4] = {255, 0, 255, 255};
    CHECK(pixel(16, 32, magenta));
    CHECK(pixel(48, 32, magenta));
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

COMPAT_TEST(compat_fog_and_alpha_test) {
    clear();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, 0, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0);
    glFogf(GL_FOG_END, 2);
    const float fogColor[4] = {0, 0, 0, 1};
    glFogfv(GL_FOG_COLOR, fogColor);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);  // eye z = -1: fog factor (2 - 1) / 2
    glVertex3f(-1, -1, -1);
    glVertex3f(3, -1, -1);
    glVertex3f(-1, 3, -1);
    glEnd();
    const uint8_t gray[4] = {128, 128, 128, 255};
    CHECK(pixel(32, 32, gray, 3));
    glDisable(GL_FOG);
    // Alpha test: fragments with alpha 0.4 fail GREATER 0.5.
    clear();
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1, 0, 0, 0.4f);
    glRectf(-1, -1, 0, 1);
    glColor4f(0, 1, 0, 0.6f);
    glRectf(0, -1, 1, 1);
    CHECK(pixel(16, 32, kBlack));
    const uint8_t greenish[4] = {0, 255, 0, 153};
    CHECK(pixel(48, 32, greenish, 3));
    GLint func = 0;
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &func);
    CHECK_EQ(func, GL_GREATER);
    glDisable(GL_ALPHA_TEST);
    CHECK_GL();
}

COMPAT_TEST(compat_clip_plane) {
    clear();
    const double rightHalf[4] = {1, 0, 0, 0};  // keep x >= 0
    glClipPlane(GL_CLIP_PLANE0, rightHalf);
    glEnable(GL_CLIP_PLANE0);
    glColor3f(0, 0, 1);
    glRectf(-1, -1, 1, 1);
    CHECK(pixel(16, 32, kBlack));
    CHECK(pixel(48, 32, kBlue));
    glDisable(GL_CLIP_PLANE0);
    CHECK_GL();
}

COMPAT_TEST(compat_display_lists) {
    const GLuint base = glGenLists(3);
    CHECK(base != 0);
    CHECK(glIsList(base) == GL_TRUE);
    // List 0: a red full-screen quad, compiled only.
    glNewList(base, GL_COMPILE);
    glColor3f(1, 0, 0);
    glRectf(-1, -1, 1, 1);
    glEndList();
    clear();
    CHECK(pixel(32, 32, kBlack));  // GL_COMPILE does not draw
    glCallList(base);
    CHECK(pixel(32, 32, kRed));
    // List 1: matrix state plus a green right half using list 0 nested... then blue left.
    glNewList(base + 1, GL_COMPILE_AND_EXECUTE);
    glPushMatrix();
    glTranslatef(1, 0, 0);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex2f(-1, -1);
    glVertex2f(0, -1);
    glVertex2f(0, 1);
    glVertex2f(-1, 1);
    glEnd();
    glPopMatrix();
    glEndList();
    CHECK(pixel(48, 32, kGreen));  // executed while compiling
    CHECK(pixel(16, 32, kRed));
    clear();
    glCallList(base + 1);
    CHECK(pixel(48, 32, kGreen));
    CHECK(pixel(16, 32, kBlack));
    float color[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, color);
    CHECK(color[1] == 1 && color[0] == 0);  // the list's glColor became current
    // Vertex arrays are read when the list is compiled.
    float verts[8] = {-1, -1, 0, -1, 0, 1, -1, 1};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glNewList(base + 2, GL_COMPILE);
    glColor3f(0, 0, 1);
    glDrawArrays(GL_QUADS, 0, 4);
    glEndList();
    for (float& v : verts) v = 5;  // the list keeps the old positions
    clear();
    glCallList(base + 2);
    CHECK(pixel(16, 32, kBlue));
    glDisableClientState(GL_VERTEX_ARRAY);
    // glCallLists with a base.
    clear();
    const GLubyte names[2] = {0, 2};
    glListBase(base);
    glCallLists(2, GL_UNSIGNED_BYTE, names);
    CHECK(pixel(16, 32, kBlue));
    CHECK(pixel(48, 32, kRed));
    glListBase(0);
    glDeleteLists(base, 3);
    CHECK(glIsList(base) == GL_FALSE);
    CHECK_GL();
}

COMPAT_TEST(compat_client_arrays) {
    clear();
    pixelProjection();
    struct Vertex {
        float x, y;
        uint8_t r, g, b, a;
    };
    const Vertex quad[4] = {{0, 0, 255, 0, 0, 255}, {32, 0, 255, 0, 0, 255}, {32, 64, 255, 0, 0, 255}, {0, 64, 255, 0, 0, 255}};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, sizeof(Vertex), &quad[0].x);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), &quad[0].r);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(pixel(16, 32, kRed));
    CHECK(pixel(48, 32, kBlack));
    // Indexed, from a buffer object, with a fixed colour (colour array off).
    glDisableClientState(GL_COLOR_ARRAY);
    const float right[8] = {32, 0, 64, 0, 64, 64, 32, 64};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(right), right, GL_STATIC_DRAW);
    glVertexPointer(2, GL_FLOAT, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glColor3f(0, 1, 0);
    const GLushort indices[6] = {0, 1, 2, 0, 2, 3};
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    CHECK(pixel(48, 32, kGreen));
    CHECK(glIsEnabled(GL_VERTEX_ARRAY) == GL_TRUE);
    GLint size = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE, &size);
    CHECK_EQ(size, 2);
    // Immediate mode with glArrayElement.
    clear();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexPointer(2, GL_FLOAT, sizeof(Vertex), &quad[0].x);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; ++i) glArrayElement(i);
    glEnd();
    CHECK(pixel(16, 32, kBlue));
    glDisableClientState(GL_VERTEX_ARRAY);
    glDeleteBuffers(1, &vbo);
    CHECK_GL();
}

COMPAT_TEST(compat_legacy_glsl_builtins) {
    // GLSL 1.20 reading the fixed-function matrices, gl_Color and gl_TexCoord.
    GLuint p = gpu::program(R"(#version 120
void main() {
    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
    gl_FrontColor = gl_Color;
    gl_TexCoord[0] = vec4(gl_Normal, 1.0);
})",
                            R"(#version 120
void main() { gl_FragColor = gl_Color * gl_TexCoord[0]; })");
    clear();
    pixelProjection();
    glUseProgram(p);
    glColor3f(1, 1, 0);
    glNormal3f(0, 1, 1);
    glRectf(0, 0, 32, 64);
    CHECK(pixel(16, 32, kGreen));
    CHECK(pixel(48, 32, kBlack));
    // The alpha test applies to shader output too.
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_LESS, 0.5f);
    glRectf(32, 0, 64, 64);  // alpha 1: discarded
    CHECK(pixel(48, 32, kBlack));
    glDisable(GL_ALPHA_TEST);
    // A user clip plane through gl_ClipVertex.
    GLuint clipped = gpu::program(R"(#version 120
void main() { gl_Position = ftransform(); gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex; gl_FrontColor = gl_Color; })",
                                  R"(#version 120
void main() { gl_FragColor = gl_Color; })");
    glUseProgram(clipped);
    const double plane[4] = {1, 0, 0, -32};  // keep x >= 32
    glClipPlane(GL_CLIP_PLANE0, plane);
    glEnable(GL_CLIP_PLANE0);
    clear();
    glColor3f(1, 0, 0);
    glRectf(0, 0, 64, 64);
    CHECK(pixel(16, 32, kBlack));
    CHECK(pixel(48, 32, kRed));
    glDisable(GL_CLIP_PLANE0);
    // Built-in attributes are reported as gl_* (location -1).
    GLint count = 0;
    glGetProgramiv(p, GL_ACTIVE_ATTRIBUTES, &count);
    CHECK_EQ(count, 3);
    CHECK_EQ(glGetAttribLocation(p, "gl_Vertex"), -1);
    GLint uniforms = -1;
    glGetProgramiv(p, GL_ACTIVE_UNIFORM_BLOCKS, &uniforms);
    CHECK_EQ(uniforms, 0);  // the fixed-function block is internal
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteProgram(clipped);
    CHECK_GL();
}

COMPAT_TEST(compat_push_pop_attrib) {
    glColor3f(1, 0, 0);
    glEnable(GL_LIGHTING);
    glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT);
    glColor3f(0, 1, 0);
    glDisable(GL_LIGHTING);
    glEnable(GL_FOG);
    glPopAttrib();
    float color[4] = {};
    glGetFloatv(GL_CURRENT_COLOR, color);
    CHECK(color[0] == 1 && color[1] == 0);
    CHECK(glIsEnabled(GL_LIGHTING) == GL_TRUE);
    CHECK(glIsEnabled(GL_FOG) == GL_FALSE);
    glDisable(GL_LIGHTING);
    glPopAttrib();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_STACK_UNDERFLOW));
    CHECK_GL();
}

COMPAT_TEST(compat_bitmap_and_draw_pixels) {
    clear();
    pixelProjection();
    // An 8x8 bitmap: the left 4 columns set (bits MSB first).
    GLubyte bits[8];
    for (GLubyte& b : bits) b = 0xF0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glColor3f(0, 1, 0);
    glRasterPos2f(10, 10);
    glBitmap(8, 8, 0, 0, 8, 0, bits);
    CHECK(pixel(11, 12, kGreen));
    CHECK(pixel(16, 12, kBlack));
    glBitmap(8, 8, 0, 0, 8, 0, bits);  // advanced by xmove
    CHECK(pixel(19, 12, kGreen));
    float raster[4] = {};
    glGetFloatv(GL_CURRENT_RASTER_POSITION, raster);
    CHECK(raster[0] == 26 && raster[1] == 10);
    // DrawPixels at a window position, zoomed 2x.
    uint8_t image[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) {
        image[i * 4] = 0;
        image[i * 4 + 1] = 0;
        image[i * 4 + 2] = 255;
        image[i * 4 + 3] = 255;
    }
    glWindowPos2i(40, 40);
    glPixelZoom(2, 2);
    glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, image);
    glPixelZoom(1, 1);
    CHECK(pixel(41, 41, kBlue));
    CHECK(pixel(47, 47, kBlue));
    CHECK(pixel(49, 49, kBlack));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    CHECK_GL();
}

COMPAT_TEST(compat_framebuffer_bits) {
    GLint red = 0, depth = 0;
    glGetIntegerv(GL_RED_BITS, &red);
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    CHECK_EQ(red, 8);
    CHECK(depth >= 16);
    GLint lights = 0, units = 0;
    glGetIntegerv(GL_MAX_LIGHTS, &lights);
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    CHECK_EQ(lights, 8);
    CHECK_EQ(units, 8);
    CHECK_GL();
}

COMPAT_TEST(compat_transform_feedback_legacy_varyings) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char* source = "#version 120\nvoid main() { gl_Position = gl_Vertex; gl_FrontColor = gl_Color; }\n";
    glShaderSource(vs, 1, &source, nullptr);
    glCompileShader(vs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    const char* names[2] = {"gl_Position", "gl_FrontColor"};
    glTransformFeedbackVaryings(p, 2, names, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    glUseProgram(p);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 24 * sizeof(float), nullptr, GL_STATIC_DRAW);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_TRIANGLES);
    glBegin(GL_TRIANGLES);
    glColor4f(1, 0, 0, 1);
    glVertex3f(-1, -1, 0);
    glColor4f(0, 1, 0, 1);
    glVertex3f(3, -1, 0);
    glVertex3f(-1, 3, 0);
    glEnd();
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    float out[24] = {};
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(out), out);
    CHECK(out[0] == -1 && out[1] == -1 && out[3] == 1);
    CHECK(out[4] == 1 && out[5] == 0 && out[7] == 1);  // vertex 0: red
    CHECK(out[8] == 3 && out[12] == 0 && out[13] == 1);  // vertex 1: green
    glUseProgram(0);
    glDeleteBuffers(1, &buffer);
    glDeleteProgram(p);
    glDeleteShader(vs);
    CHECK_GL();
}

COMPAT_TEST(compat_fragment_only_program) {
    // No vertex shader: fixed-function vertex processing feeds the fragment shader.
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* source = "#version 120\nvoid main() { gl_FragColor = gl_Color.bgra; }\n";
    glShaderSource(fs, 1, &source, nullptr);
    glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    CHECK(linked == GL_TRUE);
    clear();
    pixelProjection();
    glUseProgram(p);
    glColor3f(1, 0, 0);
    glRectf(0, 0, 32, 64);
    CHECK(pixel(16, 32, kBlue));
    CHECK(pixel(48, 32, kBlack));
    glUseProgram(0);
    glDeleteProgram(p);
    glDeleteShader(fs);
    CHECK_GL();
}

COMPAT_TEST(compat_wide_lines) {
    clear();
    pixelProjection();
    glLineWidth(9);
    glColor3f(1, 0, 0);
    glBegin(GL_LINES);  // x-major: 9 rows centred on y = 32
    glVertex2f(8, 32);
    glVertex2f(56, 32);
    glEnd();
    // Centres within 4.5 pixels of y = 32 (row 36's centre is on the edge).
    CHECK(pixel(32, 32, kRed));
    CHECK(pixel(32, 28, kRed));
    CHECK(pixel(32, 35, kRed));
    CHECK(pixel(32, 26, kBlack));
    CHECK(pixel(32, 38, kBlack));
    CHECK(pixel(4, 32, kBlack));  // ends are not extended
    clear();
    glColor3f(0, 1, 0);
    glBegin(GL_LINE_STRIP);  // y-major segment: 9 columns centred on x = 16
    glVertex2f(16, 8);
    glVertex2f(16, 56);
    glEnd();
    CHECK(pixel(12, 32, kGreen));
    CHECK(pixel(19, 32, kGreen));
    CHECK(pixel(10, 32, kBlack));
    CHECK(pixel(22, 32, kBlack));
    GLfloat range[2] = {};
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
    CHECK(range[0] == 1 && range[1] >= 9);
    glLineWidth(1);
    CHECK_GL();
}

COMPAT_TEST(compat_polygon_stipple) {
    // Rows alternate between all set and all clear.
    GLubyte mask[128];
    for (int row = 0; row < 32; ++row)
        for (int b = 0; b < 4; ++b) mask[row * 4 + b] = row % 2 ? 0x00 : 0xff;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPolygonStipple(mask);
    glEnable(GL_POLYGON_STIPPLE);
    clear();
    glColor3f(0, 0, 1);
    glRectf(-1, -1, 1, 1);
    CHECK(pixel(10, 32, kBlue));
    CHECK(pixel(10, 33, kBlack));
    glDisable(GL_POLYGON_STIPPLE);
    GLubyte back[128] = {};
    glGetPolygonStipple(back);
    CHECK(back[0] == 0xff && back[4] == 0x00);
    // GL_POLYGON_STIPPLE_BIT saves the pattern.
    GLubyte ones[128];
    std::memset(ones, 0xff, sizeof(ones));
    glPushAttrib(GL_POLYGON_STIPPLE_BIT);
    glPolygonStipple(ones);
    glPopAttrib();
    std::memset(back, 0, sizeof(back));
    glGetPolygonStipple(back);
    CHECK(back[0] == 0xff && back[4] == 0x00);
    // Pack state: a row length of 64 pixels gives 8-byte rows.
    GLubyte wide[256] = {};
    glPixelStorei(GL_PACK_ROW_LENGTH, 64);
    glGetPolygonStipple(wide);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    CHECK(wide[0] == 0xff && wide[4] == 0x00 && wide[8] == 0x00 && wide[16] == 0xff);
    CHECK_GL();
    // Pixel buffer objects: the pattern is written to / read from the
    // buffer, and ranges past its end are GL_INVALID_OPERATION.
    GLuint buffers[2] = {};
    glGenBuffers(2, buffers);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffers[0]);
    glBufferData(GL_PIXEL_PACK_BUFFER, 128, nullptr, GL_STREAM_READ);
    glGetPolygonStipple(nullptr);
    CHECK_GL();
    GLubyte packed[128] = {};
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, 128, packed);
    CHECK(packed[0] == 0xff && packed[4] == 0x00 && packed[124] == 0x00);
    glGetPolygonStipple(reinterpret_cast<GLubyte*>(64));
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffers[1]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 64, ones, GL_STATIC_DRAW);
    glPolygonStipple(nullptr);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glWindowPos2i(0, 0);
    glBitmap(32, 32, 0, 0, 0, 0, nullptr);  // 128 bytes from a 64-byte buffer
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glBitmap(32, 16, 0, 0, 0, 0, nullptr);  // exactly 64 bytes
    CHECK_GL();
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(2, buffers);
}

COMPAT_TEST(compat_texture_unit_limits) {
    // Units past MAX_TEXTURE_COORDS (8) have no texture matrix: matrix
    // commands on them are GL_INVALID_OPERATION and leave unit 7 alone.
    glActiveTexture(GL_TEXTURE7);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(2, 2, 2);
    glActiveTexture(GL_TEXTURE9);
    glLoadIdentity();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glPushMatrix();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLfloat m[16] = {};
    glGetFloatv(GL_TEXTURE_MATRIX, m);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glIsEnabled(GL_TEXTURE_2D);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glMatrixMode(GL_MODELVIEW);
    glMatrixMode(GL_TEXTURE);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    glActiveTexture(GL_TEXTURE7);
    glMatrixMode(GL_TEXTURE);
    glGetFloatv(GL_TEXTURE_MATRIX, m);
    CHECK(m[0] == 2 && m[5] == 2);
    glLoadIdentity();
    glActiveTexture(GL_TEXTURE0);
    // The colour matrix belongs to the imaging subset, which is not
    // advertised, but is accepted as drivers that implement it do (the Steam
    // overlay selects it unchecked: compat_steam_overlay_state_round_trip).
    glMatrixMode(GL_COLOR);
    CHECK_GL();
    GLint mode = 0;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    CHECK_EQ(mode, GL_COLOR);
    glMatrixMode(GL_MODELVIEW);
    CHECK_GL();
}

COMPAT_TEST(compat_point_size_range) {
    // The derived point size is clamped to POINT_SIZE_MIN/MAX even without
    // distance attenuation (GL 3.3 compatibility section 3.4).
    pixelProjection();
    clear();
    glPointParameteri(GL_POINT_SIZE_MAX, 4);
    glPointSize(10);
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex2f(32, 32);
    glEnd();
    CHECK(pixel(32, 32, kWhite));
    CHECK(pixel(35, 32, kBlack));
    glPointParameteri(GL_POINT_SIZE_MAX, 255);
    clear();
    glBegin(GL_POINTS);
    glVertex2f(32, 32);
    glEnd();
    CHECK(pixel(35, 32, kWhite));
    glPointSize(1);
    CHECK_GL();
}

COMPAT_TEST(compat_texture_unit_lod_bias) {
    // Level 0 red, level 1 green; drawn 1:1, a unit bias of +1 selects level 1.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    std::vector<uint8_t> level0(64 * 64 * 4), level1(32 * 32 * 4);
    for (size_t i = 0; i < level0.size(); i += 4) std::memcpy(&level0[i], kRed, 4);
    for (size_t i = 0; i < level1.size(); i += 4) std::memcpy(&level1[i], kGreen, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, level0.data());
    glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, level1.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    auto quad = [] {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-1, -1);
        glTexCoord2f(1, 0); glVertex2f(1, -1);
        glTexCoord2f(1, 1); glVertex2f(1, 1);
        glTexCoord2f(0, 1); glVertex2f(-1, 1);
        glEnd();
    };
    clear();
    quad();
    CHECK(pixel(32, 32, kRed));
    glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 1.0f);
    clear();
    quad();
    CHECK(pixel(32, 32, kGreen));
    glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 0.0f);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

COMPAT_TEST(compat_integer_colour_queries) {
    // Integer queries of light and material colours map [-1, 1] to the full
    // integer range (GL 3.3 compatibility section 6.1.2).
    const GLfloat diffuse[4] = {0.5f, 0.25f, 1, -1};
    glLightfv(GL_LIGHT1, GL_DIFFUSE, diffuse);
    GLint v[4] = {};
    glGetLightiv(GL_LIGHT1, GL_DIFFUSE, v);
    CHECK(std::llabs(v[0] - 1073741824LL) <= 2 && std::llabs(v[1] - 536870912LL) <= 2);
    CHECK(v[2] == 2147483647 && v[3] == -2147483647);
    const GLfloat specular[4] = {0.8f, 0, 0, 1};
    glMaterialfv(GL_FRONT, GL_SPECULAR, specular);
    glGetMaterialiv(GL_FRONT, GL_SPECULAR, v);
    CHECK(std::llabs(v[0] - std::llround(static_cast<double>(0.8f) * 2147483647.0)) <= 2 && v[3] == 2147483647);
    glMaterialf(GL_FRONT, GL_SHININESS, 12.6f);
    glGetMaterialiv(GL_FRONT, GL_SHININESS, v);
    CHECK_EQ(v[0], 13);
    CHECK_GL();
}

COMPAT_TEST(compat_render_mode) {
    clear();
    GLuint names[16];
    (void)names;
    glRenderMode(GL_SELECT);
    GLint mode = 0;
    glGetIntegerv(GL_RENDER_MODE, &mode);
    CHECK_EQ(mode, GL_SELECT);
    glInitNames();
    glPushName(7);
    GLint depth = 0;
    glGetIntegerv(GL_NAME_STACK_DEPTH, &depth);
    CHECK_EQ(depth, 1);
    glColor3f(1, 0, 0);
    glRectf(-1, -1, 1, 1);  // selection mode draws nothing
    CHECK_EQ(glRenderMode(GL_RENDER), 0);
    CHECK(pixel(32, 32, kBlack));
    glRectf(-1, -1, 1, 1);
    CHECK(pixel(32, 32, kRed));
    CHECK_GL();
}

COMPAT_TEST(compat_memory_info) {
    GLint dedicated = 0, available = 0, ati[4] = {};
    glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated);
    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available);
    glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, ati);
    CHECK(dedicated > 0 && available > 0 && available <= dedicated);
    CHECK(ati[0] > 0);
    CHECK_GL();
}

COMPAT_TEST(compat_builtin_uniform_layout) {
    // Built-in uniforms across the wmgl_FixedFunction block read back exactly.
    GLuint p = gpu::program("#version 120\nvoid main() { gl_Position = gl_Vertex; }\n",
                            R"(#version 120
uniform int which;
void main() {
    if (which == 0)
        gl_FragColor = vec4(gl_TextureMatrix[1][0][0], gl_LightSource[2].spotCutoff / 180.0,
                            gl_FrontMaterial.shininess / 128.0, gl_Fog.density);
    else if (which == 1)
        gl_FragColor = vec4(gl_ClipPlane[3].y, gl_Point.size / 16.0, gl_TextureEnvColor[2].b, gl_EyePlaneT[1].w);
    else
        gl_FragColor = vec4(gl_LightModel.ambient.g, gl_TextureMatrixInverse[1][0][0] / 4.0,
                            gl_ModelViewMatrixInverse[3][0] / -8.0, gl_NormalMatrix[2][2]);
})");
    glUseProgram(p);
    const GLint which = glGetUniformLocation(p, "which");
    glActiveTexture(GL_TEXTURE1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(0.5f, 1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightf(GL_LIGHT2, GL_SPOT_CUTOFF, 90.0f);
    glMaterialf(GL_FRONT, GL_SHININESS, 64.0f);
    glFogf(GL_FOG_DENSITY, 0.25f);
    const double plane[4] = {0, 0.75, 0, 0};
    glClipPlane(GL_CLIP_PLANE3, plane);
    glPointSize(8.0f);
    glActiveTexture(GL_TEXTURE2);
    const float env[4] = {0, 0, 0.5f, 1};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    glActiveTexture(GL_TEXTURE1);
    const float eyeT[4] = {0, 0, 0, 0.25f};
    glTexGenfv(GL_T, GL_EYE_PLANE, eyeT);
    const float ambient[4] = {0, 0.75f, 0, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glActiveTexture(GL_TEXTURE0);
    auto readback = [&](int w) {
        glUniform1i(which, w);
        glRectf(-1, -1, 1, 1);
        return gpu::readRGBA8(32, 32, 1, 1);
    };
    const uint8_t want0[4] = {128, 128, 128, 64};
    CHECK(gpu::near(readback(0).data(), want0, 2));
    const uint8_t want1[4] = {191, 128, 128, 64};
    CHECK(gpu::near(readback(1).data(), want1, 2));
    // The inverse of scale(0.5) is 2 (/4 = 0.5); a modelview translation shows in the inverse.
    glTranslatef(4.0f, 0, 0);
    const uint8_t want2[4] = {191, 128, 128, 255};
    CHECK(gpu::near(readback(2).data(), want2, 2));
    glLoadIdentity();
    glUseProgram(0);
    glDeleteProgram(p);
    CHECK_GL();
}

COMPAT_TEST(compat_display_list_split_primitives) {
    pixelProjection();
    const GLuint base = glGenLists(4);
    // Vertices compiled outside glBegin belong to the primitive the list is called in.
    glNewList(base, GL_COMPILE);
    glColor3f(0, 1, 0);
    glVertex2f(0, 0);
    glVertex2f(64, 0);
    glVertex2f(64, 64);
    glVertex2f(0, 64);
    glEndList();
    clear();
    glBegin(GL_QUADS);
    glCallList(base);
    glEnd();
    CHECK(pixel(32, 32, kGreen));
    // A list ending inside its glBegin: the primitive continues after the call.
    glNewList(base + 1, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0);
    glVertex2f(0, 0);
    glVertex2f(128, 0);
    glEndList();
    clear();
    glCallList(base + 1);
    glVertex2f(0, 128);
    glEnd();
    CHECK(pixel(32, 32, kRed));
    CHECK_GL();
    // A list called inside a compiled glBegin runs between the vertices
    // around it: with flat shading the quad takes its last vertex's colour.
    glNewList(base + 2, GL_COMPILE);
    glColor3f(0, 0, 1);
    glEndList();
    glNewList(base + 3, GL_COMPILE);
    glBegin(GL_QUADS);
    glColor3f(1, 0, 0);
    glVertex2f(0, 0);
    glVertex2f(64, 0);
    glVertex2f(64, 64);
    glCallList(base + 2);
    glVertex2f(0, 64);
    glEnd();
    glEndList();
    glShadeModel(GL_FLAT);
    clear();
    glCallList(base + 3);
    glShadeModel(GL_SMOOTH);
    CHECK(pixel(32, 32, kBlue));
    glDeleteLists(base, 4);
    CHECK_GL();
}

COMPAT_TEST(compat_array_element_generic_zero) {
    // Generic array 0 is the vertex when enabled; the vertex array is not
    // transferred as well.
    const float generic[] = {-1, -1, 3, -1, -1, 3};
    const float conventional[] = {5, 5, 5, 5, 5, 5};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, conventional);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, generic);
    glEnableVertexAttribArray(0);
    clear();
    glColor3f(1, 0, 0);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; ++i) glArrayElement(i);
    glEnd();
    CHECK(pixel(32, 32, kRed));
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_GL();
}

// In a compatibility context, vertex array 0 of the DSA commands is the
// default vertex array (ARB_direct_state_access).
TEST(compat_dsa_default_vertex_array) {
    gpu::Fixture fixture_(64, 64, false, 46);
    CHECK(fixture_.ok);
    if (!fixture_.ok) return;
    glBindVertexArray(0);
    glEnableVertexArrayAttrib(0, 3);
    CHECK_GL();
    GLint enabled = 0;
    glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
    CHECK_EQ(enabled, 1);
    glDisableVertexArrayAttrib(0, 3);
    CHECK_GL();
}

// The Steam overlay's GL renderer (GameOverlayRenderer64.dll), drawing over a
// compatibility context: it saves, sets and restores the projection,
// texture, colour (GL_COLOR, unchecked for ARB_imaging) and modelview
// matrices, and disables GL_VERTEX_PROGRAM_ARB and GL_FRAGMENT_PROGRAM_ARB
// without checking for them (issue #16). None of it may raise an error or
// leave the game's matrices changed.
COMPAT_TEST(compat_steam_overlay_state_round_trip) {
    auto matrix = [](GLenum pname, float out[16]) { glGetFloatv(pname, out); };
    auto same = [](const float* a, const float* b) {
        for (int i = 0; i < 16; ++i)
            if (a[i] != b[i]) return false;
        return true;
    };
    // The game's state.
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glTranslatef(0.25f, 0.5f, 0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(1, 2, 3);
    float gameProjection[16], gameTexture[16], gameModelview[16];
    matrix(GL_PROJECTION_MATRIX, gameProjection);
    matrix(GL_TEXTURE_MATRIX, gameTexture);
    matrix(GL_MODELVIEW_MATRIX, gameModelview);

    // The overlay's frame: save and set each matrix in turn.
    float savedProjection[16], savedColor[16], savedModelview[16], overlayProjection[16];
    glMatrixMode(GL_PROJECTION);
    matrix(GL_PROJECTION_MATRIX, savedProjection);
    glLoadIdentity();
    glScalef(1, -1, 1);
    glOrtho(0, 64, 0, 64, -1, 1);
    glTranslatef(0.375f, 0.375f, 0);
    matrix(GL_PROJECTION_MATRIX, overlayProjection);
    glMatrixMode(GL_COLOR);
    matrix(GL_COLOR_MATRIX, savedColor);
    glLoadIdentity();
    glTranslatef(0, 0, 0);
    glMatrixMode(GL_MODELVIEW);
    matrix(GL_MODELVIEW_MATRIX, savedModelview);
    glLoadIdentity();
    glDisable(GL_COLOR_SUM);
    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    CHECK_GL();
    // The colour matrix commands left the overlay's projection alone.
    float now[16];
    matrix(GL_PROJECTION_MATRIX, now);
    CHECK(same(now, overlayProjection));
    // ... and restore them.
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(savedProjection);
    glMatrixMode(GL_TEXTURE);
    glLoadMatrixf(gameTexture);
    glMatrixMode(GL_COLOR);
    glLoadMatrixf(savedColor);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(savedModelview);
    CHECK_GL();
    matrix(GL_PROJECTION_MATRIX, now);
    CHECK(same(now, gameProjection));
    matrix(GL_TEXTURE_MATRIX, now);
    CHECK(same(now, gameTexture));
    matrix(GL_MODELVIEW_MATRIX, now);
    CHECK(same(now, gameModelview));

    // ARB assembly program modes: off after the overlay's unchecked disables,
    // and they round-trip (arb_program_tests.cpp draws with them).
    CHECK_EQ(glIsEnabled(GL_VERTEX_PROGRAM_ARB), static_cast<GLboolean>(GL_FALSE));
    CHECK_EQ(glIsEnabled(GL_FRAGMENT_PROGRAM_ARB), static_cast<GLboolean>(GL_FALSE));
    CHECK_GL();
    glEnable(GL_VERTEX_PROGRAM_ARB);
    CHECK_EQ(glIsEnabled(GL_VERTEX_PROGRAM_ARB), static_cast<GLboolean>(GL_TRUE));
    glDisable(GL_VERTEX_PROGRAM_ARB);
    CHECK_GL();

    // The colour matrix stack: loaded values read back, two deep.
    glMatrixMode(GL_COLOR);
    glLoadIdentity();
    glScalef(0.5f, 0.25f, 2);
    matrix(GL_COLOR_MATRIX, now);
    CHECK(now[0] == 0.5f && now[5] == 0.25f && now[10] == 2 && now[15] == 1);
    matrix(GL_TRANSPOSE_COLOR_MATRIX, now);
    CHECK(now[0] == 0.5f && now[5] == 0.25f);
    GLint depth = 0, maxDepth = 0;
    glGetIntegerv(GL_MAX_COLOR_MATRIX_STACK_DEPTH, &maxDepth);
    CHECK_EQ(maxDepth, 2);
    glPushMatrix();
    glGetIntegerv(GL_COLOR_MATRIX_STACK_DEPTH, &depth);
    CHECK_EQ(depth, 2);
    glPushMatrix();
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_STACK_OVERFLOW));
    glLoadIdentity();
    glPopMatrix();
    matrix(GL_COLOR_MATRIX, now);
    CHECK(now[0] == 0.5f);
    glMatrixMode(GL_MODELVIEW);
    CHECK_GL();
}

// Compressed luminance formats (EXT_texture_compression_latc, ATI's 3Dc):
// RGTC blocks sampled as luminance (alpha): SOMA's normal maps are 3Dc, read
// from green and alpha (issue #16).
COMPAT_TEST(compat_latc_and_3dc_textures) {
    // BC4 blocks with all indices 0: every texel is the first endpoint.
    const uint8_t l200[8] = {200, 0, 0, 0, 0, 0, 0, 0}, a60[8] = {60, 0, 0, 0, 0, 0, 0, 0};
    uint8_t la[16];
    std::memcpy(la, l200, 8);
    std::memcpy(la + 8, a60, 8);  // luminance first, then alpha
    GLuint p = gpu::program(R"(#version 120
void main() { gl_Position = ftransform(); })",
                            R"(#version 120
uniform sampler2D tex;
void main() { gl_FragColor = texture2D(tex, vec2(0.5)); })");
    glUseProgram(p);
    pixelProjection();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    struct Case { GLenum format; const uint8_t* data; GLsizei size; uint8_t want[4]; GLenum readFormat; uint8_t read[2]; };
    const Case cases[] = {
        {GL_COMPRESSED_LUMINANCE_ALPHA_3DC_ATI, la, 16, {200, 200, 200, 60}, GL_LUMINANCE_ALPHA, {200, 60}},
        {GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT, la, 16, {200, 200, 200, 60}, GL_LUMINANCE_ALPHA, {200, 60}},
        {GL_COMPRESSED_LUMINANCE_LATC1_EXT, l200, 8, {200, 200, 200, 255}, GL_LUMINANCE, {200, 0}},
    };
    for (const Case& c : cases) {
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, c.format, 4, 4, 0, c.size, c.data);
        CHECK_GL();
        GLint compressed = 0, internal = 0, luminance = 0, alpha = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_LUMINANCE_SIZE, &luminance);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &alpha);
        CHECK_EQ(compressed, 1);
        CHECK_EQ(internal, static_cast<GLint>(c.format));
        CHECK(luminance > 0);
        CHECK_EQ(alpha > 0, c.size == 16);
        clear();
        glRectf(0, 0, 64, 64);
        CHECK(pixel(32, 32, c.want));
        // Decoded on readback to luminance (and alpha).
        uint8_t read[4 * 4 * 2] = {};
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, c.readFormat, GL_UNSIGNED_BYTE, read);
        CHECK_EQ(read[0], c.read[0]);
        if (c.readFormat == GL_LUMINANCE_ALPHA) CHECK_EQ(read[1], c.read[1]);
        uint8_t blocks[16] = {};
        glGetCompressedTexImage(GL_TEXTURE_2D, 0, blocks);
        CHECK(std::memcmp(blocks, c.data, static_cast<size_t>(c.size)) == 0);
    }
    // Uncompressed luminance-alpha pixels are encoded into the blocks: alpha
    // goes to the second block.
    uint8_t pixels[4 * 4 * 2];
    for (int i = 0; i < 16; ++i) {
        pixels[i * 2] = 100;
        pixels[i * 2 + 1] = 30;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT, 4, 4, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, pixels);
    CHECK_GL();
    clear();
    glRectf(0, 0, 64, 64);
    const uint8_t encoded[4] = {100, 100, 100, 30};
    CHECK(pixel(32, 32, encoded));
    // LATC is advertised in the compatibility profile (3Dc's extension was
    // never registered).
    bool latc = false;
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint i = 0; i < extensions; ++i) {
        const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        latc = latc || std::strcmp(name, "GL_EXT_texture_compression_latc") == 0;
    }
    CHECK(latc);
    glDeleteTextures(1, &tex);
    glUseProgram(0);
    glDeleteProgram(p);
    CHECK_GL();
}

// Luminance formats are not part of the core profile.
GPU_TEST_VERSION(gpu_texture_latc_is_compatibility_only, 46) {
    const uint8_t block[16] = {200, 0, 0, 0, 0, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0, 0};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_LUMINANCE_ALPHA_3DC_ATI, 4, 4, 0, sizeof(block), block);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT, 4, 4, 0, sizeof(block), block);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

// GL_DEPTH_TEXTURE_MODE (compatibility profile): depth samples as luminance
// by default, or as intensity, alpha or red. OpenSceneGraph sets it on
// OpenMW's shadow maps and depth copies (issue #42).
COMPAT_TEST(compat_depth_texture_mode) {
    GLuint p = gpu::program(R"(#version 120
void main() { gl_Position = ftransform(); })",
                            R"(#version 120
uniform sampler2D tex;
void main() { gl_FragColor = texture2D(tex, vec2(0.5)); })");
    glUseProgram(p);
    pixelProjection();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    float depth[16];
    for (float& d : depth) d = 0.5f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    GLint mode = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, &mode);
    CHECK_EQ(mode, GL_LUMINANCE);
    struct Case { GLenum mode; uint8_t want[4]; };
    const Case cases[] = {
        {GL_LUMINANCE, {128, 128, 128, 255}}, {GL_INTENSITY, {128, 128, 128, 128}},
        {GL_ALPHA, {0, 0, 0, 128}},           {GL_RED, {128, 0, 0, 255}},
    };
    for (const Case& c : cases) {
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, static_cast<GLint>(c.mode));
        CHECK_GL();
        glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, &mode);
        CHECK_EQ(mode, static_cast<GLint>(c.mode));
        clear();
        glRectf(0, 0, 64, 64);
        CHECK(pixel(32, 32, c.want));
    }
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_RGBA);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glDeleteTextures(1, &tex);
    glUseProgram(0);
    glDeleteProgram(p);
    CHECK_GL();
}

// Not in the core profile.
GPU_TEST_VERSION(gpu_depth_texture_mode_is_compatibility_only, 46) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_ENUM));
    glDeleteTextures(1, &tex);
    CHECK_GL();
}

// GL_GENERATE_MIPMAP (compatibility profile): uploads to the base level
// regenerate the smaller levels (OpenSceneGraph sets it, issue #42).
COMPAT_TEST(compat_generate_mipmap_parameter) {
    GLuint p = gpu::program(R"(#version 120
void main() { gl_Position = ftransform(); })",
                            R"(#version 120
uniform sampler2D tex;
void main() { gl_FragColor = texture2D(tex, vec2(0.5)); })");
    glUseProgram(p);
    pixelProjection();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 2.0f);  // samples the 1x1 level
    GLint generate = -1;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, &generate);
    CHECK_EQ(generate, GL_FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, &generate);
    CHECK_EQ(generate, GL_TRUE);
    std::vector<uint8_t> red(4 * 4 * 4), green(4 * 4 * 4);
    for (size_t i = 0; i < red.size(); i += 4) {
        std::memcpy(&red[i], kRed, 4);
        std::memcpy(&green[i], kGreen, 4);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    GLint width = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &width);
    CHECK_EQ(width, 1);
    clear();
    glRectf(0, 0, 64, 64);
    CHECK(pixel(32, 32, kRed));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, green.data());
    clear();
    glRectf(0, 0, 64, 64);
    CHECK(pixel(32, 32, kGreen));
    // The regeneration is deferred to the next command that reads the levels:
    // several uploads, then a read of a smaller level without a draw.
    for (int i = 0; i < 8; ++i) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, (i & 1 ? green : red).data());
    uint8_t level2[4] = {};
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, level2);
    CHECK(std::memcmp(level2, kGreen, 4) == 0);
    // A write to a smaller level after an upload to the base level wins.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glTexSubImage2D(GL_TEXTURE_2D, 2, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, kGreen);
    clear();
    glRectf(0, 0, 64, 64);
    CHECK(pixel(32, 32, kGreen));
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 2);
    CHECK_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_VALUE));
    glDeleteTextures(1, &tex);
    glUseProgram(0);
    glDeleteProgram(p);
    CHECK_GL();
}

// A shader keeps its own __VERSION__ through the legacy upgrade (GLSL 1.10
// and 1.20 compile as 1.40): osgText's fragment shader, with no #version,
// reads glyph coverage from .a below GLSL 1.30 and from .r above, and its
// GL_ALPHA glyph atlas has coverage in alpha only. Under the upgrade's
// __VERSION__ it read .r, always 0, and OpenMW's statistics text (F3) was
// invisible (issue #42).
COMPAT_TEST(compat_legacy_shader_keeps_version) {
    GLuint p = gpu::program("varying vec2 uv;\nvoid main() { uv = gl_Vertex.xy * 0.5 + 0.5; gl_Position = gl_Vertex; }\n",
                            "#if __VERSION__>=130\n    #define ALPHA r\n#else\n    #define ALPHA a\n#endif\n"
                            "#define VERSION_SEEN __VERSION__\n"
                            "uniform sampler2D glyphTexture;\nvarying vec2 uv;\n"
                            "void main() { gl_FragColor = vec4(texture2D(glyphTexture, uv).ALPHA, VERSION_SEEN == 110 ? 1.0 : 0.0, 0.0, 1.0); }\n");
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const uint8_t coverage = 255;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 1, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, &coverage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    static const float triangle[] = {-1, -1, 0, -1, 3, 0, 3, -1, 0};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, triangle);
    clear();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_GL();
    const uint8_t yellow[4] = {255, 255, 0, 255};  // coverage read from alpha, __VERSION__ 110
    CHECK(pixel(32, 32, yellow));
    glUseProgram(0);
    glDeleteTextures(1, &tex);
    glDeleteProgram(p);
}

// A matrix command alone updates the matrix built-ins and gl_NormalScale (the
// block keeps its other values: OpenSceneGraph loads a matrix before each
// drawable, issue #42).
COMPAT_TEST(compat_matrix_only_changes) {
    GLuint p = gpu::program("#version 120\nvarying vec2 v;\n"
                            "void main() { v = vec2(gl_NormalScale * 0.25, gl_ModelViewMatrix[0][0] * 0.25);\n"
                            "  gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n",
                            "#version 120\nvarying vec2 v;\nvoid main() { gl_FragColor = vec4(v, 0.0, 1.0); }\n");
    glUseProgram(p);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    clear();
    static const float left[] = {-1, -1, 0, -1, 1, 0, 0, -1, 0, 0, 1, 0};
    static const float right[] = {0, -1, 0, 0, 1, 0, 1, -1, 0, 1, 1, 0};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, left);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glScalef(2, 2, 2);  // gl_NormalScale 2, gl_ModelViewMatrix[0][0] 2
    glVertexPointer(3, GL_FLOAT, 0, right);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    CHECK_GL();
    const uint8_t quarter[4] = {64, 64, 0, 255}, half[4] = {128, 128, 0, 255};
    CHECK(pixel(16, 32, quarter));
    CHECK(pixel(48, 32, half));
    glLoadIdentity();
    glUseProgram(0);
    glDeleteProgram(p);
}

// glCallList cost, shaped like OpenMW's (OpenSceneGraph's display lists, issue
// #42): lists compiled from client vertex, normal and texture coordinate
// arrays and a glDrawElements with client indices, each called after a
// glLoadMatrixf. Runs only with WMGL_BENCH set; prints CPU microseconds per call.
COMPAT_TEST(compat_bench_call_list) {
    if (!std::getenv("WMGL_BENCH")) return;
    GLuint p = gpu::program("#version 120\nvarying vec2 uv;\nvoid main() { uv = gl_MultiTexCoord0.xy; gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n",
                            "#version 120\nvarying vec2 uv;\nvoid main() { gl_FragColor = vec4(uv, 0.0, 1.0); }\n");
    glUseProgram(p);
    constexpr int kLists = 200, kVerts = 400, kIndices = 1200, kCallsPerFrame = 1000;
    std::vector<float> pos(kVerts * 3), nrm(kVerts * 3, 0.5f), uv(kVerts * 2);
    for (int i = 0; i < kVerts; ++i) {
        pos[i * 3] = (i % 20) * 0.005f; pos[i * 3 + 1] = (i / 20) * 0.005f; pos[i * 3 + 2] = 0;
        uv[i * 2] = pos[i * 3]; uv[i * 2 + 1] = pos[i * 3 + 1];
    }
    std::vector<GLushort> idx(kIndices);
    for (int i = 0; i < kIndices; ++i) idx[i] = static_cast<GLushort>((i * 7) % kVerts);
    const GLuint base = glGenLists(kLists);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    for (int l = 0; l < kLists; ++l) {
        glNewList(base + l, GL_COMPILE);
        glVertexPointer(3, GL_FLOAT, 0, pos.data());
        glNormalPointer(GL_FLOAT, 0, nrm.data());
        glTexCoordPointer(2, GL_FLOAT, 0, uv.data());
        glDrawElements(GL_TRIANGLES, kIndices, GL_UNSIGNED_SHORT, idx.data());
        glEndList();
    }
    glMatrixMode(GL_MODELVIEW);
    const int frames = std::atoi(std::getenv("WMGL_BENCH")) > 0 ? std::atoi(std::getenv("WMGL_BENCH")) : 100;
    double total = 0;
    for (int f = -10; f < frames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int c = 0; c < kCallsPerFrame; ++c) {
            const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, (c % 32) * 0.05f - 0.8f, (c / 32 % 32) * 0.05f - 0.8f, 0, 1};
            glLoadMatrixf(m);
            glCallList(base + c % kLists);
        }
        glFinish();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (f >= 0) total += us;
    }
    std::printf("BENCH call_list calls=%d us_per_call=%.2f\n", kCallsPerFrame, total / frames / kCallsPerFrame);
    glDeleteLists(base, kLists);
    CHECK_GL();
}

// Generic attribute 0 aliases the conventional vertex array both ways: a
// shader reading a generic attribute at location 0 takes glVertexPointer's
// array when generic array 0 is disabled. OpenMW's post-processing draws its
// full-screen triangle that way, and every pass drew nothing (issue #42).
COMPAT_TEST(compat_generic_attribute_zero_from_vertex_array) {
    GLuint p = gpu::program("#version 330 compatibility\nlayout(location = 0) in vec2 vertex;\n"
                            "void main() { gl_Position = vec4(vertex, 0.0, 1.0); }\n",
                            "#version 330 compatibility\nout vec4 o;\nvoid main() { o = vec4(1.0, 0.0, 0.0, 1.0); }\n");
    static const float triangle[] = {-1, -1, 0, -1, 3, 0, 3, -1, 0};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 0, nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableVertexAttribArray(0);
    clear();
    glUseProgram(p);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    CHECK_GL();
    CHECK(pixel(32, 32, kRed));
    CHECK(pixel(2, 2, kRed));
    glDisableClientState(GL_VERTEX_ARRAY);
    glUseProgram(0);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(p);
}

// Bink's video planes in a D3D9 game through WineD3D (MetalSharp-Wine issue
// #58: Call of Duty 4's videos solid pink, every plane sampling 1.0). On a
// compatibility context WineD3D makes D3DFMT_L8 a GL_R8 texture swizzled
// (R, R, R, 1) and D3DFMT_A8 a GL_ALPHA8 one; it sets the swizzle when the
// texture is first bound, uploads with glTexSubImage2D (from a PBO for
// dynamic textures), samples through a sampler object and reads the planes
// in GLSL 1.20. Each way, the planes read back their texels.
COMPAT_TEST(compat_wined3d_luminance_and_alpha_planes) {
    const char* vs = "#version 120\nattribute vec2 position; varying vec2 uv;\n"
                     "void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
    // L8 planes are read as .x, A8 planes as .w, as WineD3D's ps_2_0 code does.
    const char* fsL = "#version 120\nuniform sampler2D s; varying vec2 uv;\n"
                      "void main() { vec4 t = texture2D(s, uv); gl_FragColor = vec4(t.x, t.y, t.z, t.w * 0.5); }\n";
    const char* fsA = "#version 120\nuniform sampler2D s; varying vec2 uv;\n"
                      "void main() { vec4 t = texture2D(s, uv); gl_FragColor = vec4(t.w, t.x, t.y, 1.0); }\n";
    GLuint pL = gpu::program(vs, fsL), pA = gpu::program(vs, fsA);
    GLuint vao = gpu::quad();
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const GLint lSwizzle[4] = {GL_RED, GL_RED, GL_RED, GL_ONE};
    std::vector<uint8_t> texels(16 * 16, 0x40);
    for (int storage = 0; storage < 2; ++storage)
        for (int pbo = 0; pbo < 2; ++pbo)
            for (int alpha = 0; alpha < 2; ++alpha) {
                GLuint tex = 0, buffer = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                const GLenum internal = alpha ? GL_ALPHA8 : GL_R8, format = alpha ? GL_ALPHA : GL_RED;
                if (storage) glTexStorage2D(GL_TEXTURE_2D, 1, internal, 16, 16);
                else glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), 16, 16, 0, format, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                if (!alpha) glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, lSwizzle);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                if (pbo) {
                    glGenBuffers(1, &buffer);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
                    glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(texels.size()), texels.data(), GL_STREAM_DRAW);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, format, GL_UNSIGNED_BYTE, nullptr);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, format, GL_UNSIGNED_BYTE, texels.data());
                }
                glBindSampler(0, sampler);
                const GLuint p = alpha ? pA : pL;
                glUseProgram(p);
                glUniform1i(glGetUniformLocation(p, "s"), 0);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                // L8: (0x40, 0x40, 0x40, 1 * 0.5); A8: (0x40, 0, 0, 1).
                const uint8_t want[4] = {0x40, uint8_t(alpha ? 0 : 0x40), uint8_t(alpha ? 0 : 0x40), uint8_t(alpha ? 255 : 128)};
                const bool ok = pixel(32, 32, want, 3);
                CHECK(ok);
                if (!ok) {
                    const std::vector<uint8_t> got = gpu::readRGBA8(32, 32, 1, 1);
                    std::fprintf(stderr, "  %s storage=%d pbo=%d: got %d %d %d %d\n", alpha ? "A8" : "L8", storage, pbo, got[0],
                                 got[1], got[2], got[3]);
                }
                glBindSampler(0, 0);
                glDeleteTextures(1, &tex);
                if (buffer) glDeleteBuffers(1, &buffer);
            }
    glDeleteSamplers(1, &sampler);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(pL);
    glDeleteProgram(pA);
    CHECK_GL();
}

// OpenGL 1.x's GL_BLEND_SRC and GL_BLEND_DST are the first draw buffer's
// colour blend factors in a compatibility context: City of Heroes' renderer
// reads them as it starts, and got GL_INVALID_ENUM (WineMetalGL #94).
COMPAT_TEST(gpu_compat_blend_src_dst) {
    GLint src = -1, dst = -1;
    glGetIntegerv(GL_BLEND_SRC, &src);
    glGetIntegerv(GL_BLEND_DST, &dst);
    CHECK_EQ(src, GL_ONE);
    CHECK_EQ(dst, GL_ZERO);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glGetIntegerv(GL_BLEND_SRC, &src);
    glGetIntegerv(GL_BLEND_DST, &dst);
    CHECK_EQ(src, GL_SRC_ALPHA);
    CHECK_EQ(dst, GL_ONE_MINUS_SRC_ALPHA);
    CHECK_GL();
}
