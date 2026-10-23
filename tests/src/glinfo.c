/* OpenGL (WGL) smoke test: legacy and core contexts on a window, renderer strings, clear and
 * swap 30 frames, read a pixel back. Run with WineMetalGL (opengl32=native) or Wine's own. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
typedef HGLRC (WINAPI *create_attribs_t)( HDC, HGLRC, const int * );

int main(void)
{
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8 };
    create_attribs_t create_attribs;
    HWND hwnd = CreateWindowA( "static", "glinfo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 240, 0, 0, 0, 0 );
    HDC hdc = GetDC( hwnd );
    HGLRC ctx, core;
    unsigned char pixel[4] = { 0 };
    int format = ChoosePixelFormat( hdc, &pfd ), i;
    char path[MAX_PATH] = "";

    if (!format || !SetPixelFormat( hdc, format, &pfd )) { printf( "pixel format failed: %lu\n", GetLastError() ); return 1; }
    if (!(ctx = wglCreateContext( hdc )) || !wglMakeCurrent( hdc, ctx )) { printf( "context failed: %lu\n", GetLastError() ); return 1; }
    GetModuleFileNameA( GetModuleHandleA( "opengl32.dll" ), path, sizeof(path) );
    printf( "opengl32: %s\n", path );
    printf( "legacy: %s | %s | %s\n", glGetString( GL_VENDOR ), glGetString( GL_RENDERER ), glGetString( GL_VERSION ) );

    create_attribs = (create_attribs_t)wglGetProcAddress( "wglCreateContextAttribsARB" );
    if (create_attribs)
    {
        static const int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 1,
                                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
        if ((core = create_attribs( hdc, NULL, attribs )) && wglMakeCurrent( hdc, core ))
        {
            printf( "core 4.1: %s\n", glGetString( GL_VERSION ) );
            wglMakeCurrent( hdc, ctx );
            wglDeleteContext( core );
        }
        else printf( "core 4.1 context: not available (%lu)\n", GetLastError() );
    }

    for (i = 0; i < 30; i++)
    {
        glClearColor( 0.2f, 0.6f, i % 2 ? 0.2f : 0.9f, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        if (i == 29) glReadPixels( 10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
        SwapBuffers( hdc );
    }
    printf( "30 frames swapped, glGetError %#x, pixel %u,%u,%u (expect 51,153,51)\n", glGetError(), pixel[0], pixel[1], pixel[2] );
    return glGetError() != GL_NO_ERROR;
}
