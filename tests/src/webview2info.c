/* archs: x86_64 */
/* WebView2 smoke test: create an environment on a Fixed Version runtime, a controller in a window,
 * navigate to a page that runs JavaScript, read document.title back through ExecuteScript.
 *   webview2info.exe <WebView2Loader.dll> <runtime folder> <user data folder>
 * Build with the WebView2 SDK's include directory (WebView2.h). */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include "WebView2.h"

typedef HRESULT (STDAPICALLTYPE *create_env_fn)( PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler * );
static HWND hwnd;
static ICoreWebView2Controller *controller;
static ICoreWebView2 *webview;
static int result = 1;

/* one minimal COM object per handler: refcounting is not needed for these static instances */
static HRESULT STDMETHODCALLTYPE qi( void *this, REFIID iid, void **out ) { *out = this; return S_OK; }
static ULONG STDMETHODCALLTYPE addref( void *this ) { return 1; }
static ULONG STDMETHODCALLTYPE release( void *this ) { return 1; }

static HRESULT STDMETHODCALLTYPE script_done( ICoreWebView2ExecuteScriptCompletedHandler *this, HRESULT hr, LPCWSTR json )
{
    printf( "document.title = %ls (%#lx)\n", json ? json : L"(null)", hr );
    result = !(SUCCEEDED(hr) && json && wcsstr( json, L"WebView2 JS 14999995" ));
    printf( "%s\n", result ? "WEBVIEW2 WRONG" : "WEBVIEW2 OK" );
    SetTimer( hwnd, 1, 3000, NULL );  /* leave the page up briefly, then quit */
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl script_vtbl = { (void *)qi, (void *)addref, (void *)release, script_done };
static ICoreWebView2ExecuteScriptCompletedHandler script_handler = { &script_vtbl };

static HRESULT STDMETHODCALLTYPE nav_done( ICoreWebView2NavigationCompletedEventHandler *this, ICoreWebView2 *sender,
                                           ICoreWebView2NavigationCompletedEventArgs *args )
{
    BOOL ok = FALSE;

    ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess( args, &ok );
    printf( "navigation completed: %s\n", ok ? "success" : "failed" );
    return ICoreWebView2_ExecuteScript( sender, L"document.title", &script_handler );
}
static ICoreWebView2NavigationCompletedEventHandlerVtbl nav_vtbl = { (void *)qi, (void *)addref, (void *)release, nav_done };
static ICoreWebView2NavigationCompletedEventHandler nav_handler = { &nav_vtbl };

static HRESULT STDMETHODCALLTYPE controller_done( ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this,
                                                  HRESULT hr, ICoreWebView2Controller *ctrl )
{
    EventRegistrationToken token;
    RECT rect;

    printf( "controller: %#lx\n", hr );
    if (FAILED(hr)) { PostQuitMessage( 1 ); return hr; }
    controller = ctrl;
    ICoreWebView2Controller_AddRef( controller );
    GetClientRect( hwnd, &rect );
    ICoreWebView2Controller_put_Bounds( controller, rect );
    ICoreWebView2Controller_get_CoreWebView2( controller, &webview );
    ICoreWebView2_add_NavigationCompleted( webview, &nav_handler, &token );
    return ICoreWebView2_NavigateToString( webview,
        L"<body style='margin:0;background:rgb(51,153,51);font:40px sans-serif;color:white'>"
        L"<script>let s=0;for(let i=0;i<5e6;i++)s+=i%7;document.title='WebView2 JS '+s;"
        L"document.write('WebView2 OK '+s)</script></body>" );
}
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl controller_vtbl = { (void *)qi, (void *)addref, (void *)release, controller_done };
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler controller_handler = { &controller_vtbl };

static HRESULT STDMETHODCALLTYPE env_done( ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this,
                                           HRESULT hr, ICoreWebView2Environment *env )
{
    LPWSTR version = NULL;

    printf( "environment: %#lx\n", hr );
    if (FAILED(hr)) { PostQuitMessage( 1 ); return hr; }
    ICoreWebView2Environment_get_BrowserVersionString( env, &version );
    printf( "browser version: %ls\n", version ? version : L"?" );
    return ICoreWebView2Environment_CreateCoreWebView2Controller( env, hwnd, &controller_handler );
}
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_vtbl = { (void *)qi, (void *)addref, (void *)release, env_done };
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler env_handler = { &env_vtbl };

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_TIMER) { PostQuitMessage( result ); return 0; }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int wmain( int argc, WCHAR **argv )
{
    WNDCLASSW cls = { .lpfnWndProc = wndproc, .lpszClassName = L"webview2info", .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1) };
    create_env_fn create_env;
    HMODULE loader;
    HRESULT hr;
    MSG msg;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc < 4) return 2;
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    if (!(loader = LoadLibraryW( argv[1] ))) { printf( "can't load %ls\n", argv[1] ); return 1; }
    create_env = (create_env_fn)GetProcAddress( loader, "CreateCoreWebView2EnvironmentWithOptions" );
    RegisterClassW( &cls );
    hwnd = CreateWindowW( L"webview2info", L"webview2info", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          100, 100, 640, 360, NULL, NULL, NULL, NULL );
    hr = create_env( argv[2], argv[3], NULL, &env_handler );
    printf( "CreateCoreWebView2EnvironmentWithOptions: %#lx\n", hr );
    if (FAILED(hr)) return 1;
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return msg.wParam;
}
