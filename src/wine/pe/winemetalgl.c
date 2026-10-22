/* winemetalgl.dll: WGL and the unix call plumbing (wmgl_wine.h). The GL
 * functions are generated (wine_gl_pe.gen.c), the pixel format functions
 * adapted from Wine (wgl_formats.c). */

#include "wmgl_pe.h"

int wmgl_log_max;

static HINSTANCE module;
static unixlib_handle_t unixlib;
static NTSTATUS (WINAPI *unix_call_dispatcher)( unixlib_handle_t, unsigned int, void * );
static DWORD tls_index = TLS_OUT_OF_INDEXES;
static char *wgl_extensions;
/* Messages go to winemetalgl.so once it answers (init_pixel_formats). */
static BOOL log_ready;
static char upscale_setting[32];  /* WINEMETALGL_UPSCALE as the unix side found it */

void wmgl_log( int level, const char *format, ... )
{
    static const char *const names[] = {"err", "warn", "info", "trace"};
    struct wmgl_log_params params;
    char text[2048];
    size_t len;
    va_list args;

    if (level > wmgl_log_max || level < 0 || level > 3) return;
    va_start( args, format );
    vsnprintf( text, sizeof(text), format, args );
    va_end( args );
    if (log_ready)
    {
        params.level = level;
        params.text = (UINT64)(ULONG_PTR)text;
        /* Not wmgl_unix_call: batched GL calls keep their batch. */
        unix_call_dispatcher( unixlib, unix_wmgl_log, &params );
        return;
    }
    len = strlen( text );
    fprintf( stderr, "winemetalgl:%s: %s%s", names[level], text, len && text[len - 1] == '\n' ? "" : "\n" );
}

/* GL calls batched on this thread (wmgl_batch), run by one unix call. */
#define BATCH_BYTES 16384
struct batch
{
    UINT used;
    BYTE data[BATCH_BYTES] __attribute__((aligned(16)));
};

/* The thread's current context and DCs (wglGetCurrent*), and its batch. */
struct thread_state
{
    HGLRC context;
    HDC draw, read;
    HGLRC extensions_context;  /* the context `extensions` lists the GL extensions of */
    char *extensions;
    struct batch *batch;
};

static struct thread_state *thread_state( BOOL create )
{
    struct thread_state *state = TlsGetValue( tls_index );
    if (!state && create && (state = calloc( 1, sizeof(*state) ))) TlsSetValue( tls_index, state );
    return state;
}

static void deliver_debug_messages(void)
{
    char message[4096];
    struct wmgl_debug_message_params params;

    do
    {
        memset( &params, 0, sizeof(params) );
        params.buffer = (UINT64)(ULONG_PTR)message;
        params.size = sizeof(message);
        if (unix_call_dispatcher( unixlib, unix_wmgl_debug_message, &params ) || !params.got) return;
        if (!params.callback) continue;
        if (params.amd)
        {
            GLDEBUGPROCAMD callback = (GLDEBUGPROCAMD)(ULONG_PTR)params.callback;
            callback( params.id, params.type, params.severity, params.length, message, (void *)(ULONG_PTR)params.user );
        }
        else
        {
            GLDEBUGPROC callback = (GLDEBUGPROC)(ULONG_PTR)params.callback;
            callback( params.source, params.type, params.id, params.severity, params.length, message,
                      (const void *)(ULONG_PTR)params.user );
        }
    } while (params.more);
}

/* Batching is off with WINEMETALGL_BATCH=0, and while the application has
 * a debug callback and GL_DEBUG_OUTPUT_SYNCHRONOUS enabled: its messages
 * then arrive during the call that caused them. Without synchronous output
 * they may come later (GL 4.6 section 20.8), after the batch. */
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
static BOOL batching = TRUE, batching_allowed = TRUE, debug_callback, debug_synchronous;

static void note_call( unsigned int code, const void *args )
{
    if (code == unix_glDebugMessageCallback || code == unix_glDebugMessageCallbackARB ||
        code == unix_glDebugMessageCallbackKHR || code == unix_glDebugMessageCallbackAMD)
        debug_callback = *(void *const *)args != NULL;  /* the callback is the first parameter */
    else if ((code == unix_glEnable || code == unix_glDisable) &&
             ((const struct glEnable_params *)args)->cap == GL_DEBUG_OUTPUT_SYNCHRONOUS)
        debug_synchronous = code == unix_glEnable;
    else
        return;
    batching = batching_allowed && !(debug_callback && debug_synchronous);
}

/* Runs the thread's batched calls (one unix call); FALSE: delivering
 * debug messages is left to the caller. */
static void flush_batch( struct thread_state *state, BOOL deliver )
{
    struct wmgl_batch_params params;

    if (!state || !state->batch || !state->batch->used) return;
    params.data = (UINT64)(ULONG_PTR)state->batch->data;
    params.size = state->batch->used;
    state->batch->used = 0;
    if (unix_call_dispatcher( unixlib, unix_wmgl_batch, &params ) == WMGL_STATUS_DEBUG_MESSAGES && deliver)
        deliver_debug_messages();
}

BOOL wmgl_batch( unsigned int code, const void *args, size_t size, const void *data, UINT64 data_size,
                 size_t pointer_offset )
{
    const size_t params_size = (size + 7) & ~(size_t)7;
    size_t record_size;
    struct thread_state *state;
    struct wmgl_batch_record record;
    BYTE *at;

    note_call( code, args );
    /* Large or missing data (the core reports the error) is passed as is. */
    if (!batching || data_size > BATCH_BYTES || (data_size && !data)) return FALSE;
    record_size = sizeof(struct wmgl_batch_record) + params_size + (((size_t)data_size + 7) & ~(size_t)7);
    if (record_size > BATCH_BYTES || !(state = thread_state( TRUE ))) return FALSE;
    if (!state->batch && !(state->batch = calloc( 1, sizeof(*state->batch) ))) return FALSE;
    if (state->batch->used + record_size > BATCH_BYTES) flush_batch( state, TRUE );
    at = state->batch->data + state->batch->used;
    record.code = code;
    record.size = (UINT)record_size;
    memcpy( at, &record, sizeof(record) );
    memcpy( at + sizeof(record), args, size );
    if (data_size)
    {
        /* The struct points to the copy, which lives until the batch runs. */
        void *copy = at + sizeof(record) + params_size;
        memcpy( copy, data, (size_t)data_size );
        memcpy( at + sizeof(record) + pointer_offset, &copy, sizeof(copy) );
    }
    state->batch->used += (UINT)record_size;
    return TRUE;
}

BOOL wmgl_batch_last( unsigned int code, const void *args, size_t size )
{
    struct thread_state *state = TlsGetValue( tls_index );

    /* Pointers in the arguments stay valid: the batch runs before this returns. */
    if (!batching || !state || !state->batch || !state->batch->used) return FALSE;
    if (!wmgl_batch( code, args, size, NULL, 0, 0 )) return FALSE;
    flush_batch( state, TRUE );
    return TRUE;
}

NTSTATUS wmgl_unix_call( unsigned int code, void *args )
{
    NTSTATUS status;

    /* Batched calls run first, in order. */
    flush_batch( TlsGetValue( tls_index ), TRUE );
    note_call( code, args );
    status = unix_call_dispatcher( unixlib, code, args );
    if (status == WMGL_STATUS_DEBUG_MESSAGES)
    {
        deliver_debug_messages();
        status = 0;
    }
    return status;
}

static BOOL init_unix_calls( HINSTANCE instance )
{
    void **dispatcher = (void **)GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "__wine_unix_call_dispatcher" );
    /* MemoryWineUnixFuncs: the handle of this builtin module's unix library. */
    if (!dispatcher || NtQueryVirtualMemory( GetCurrentProcess(), instance, 1000, &unixlib, sizeof(unixlib), NULL ))
        return FALSE;
    unix_call_dispatcher = *dispatcher;
    return TRUE;
}

#ifdef __x86_64__  /* x64 CONTEXT: x86_64 and ARM64EC; not the native ARM64 half of ARM64X */
/* The application's exceptions that end programs are logged with a backtrace
 * as they are raised, before its own handlers, which often catch them and
 * exit without a word (id Tech 5's "CrashHandler: Aborting"). Invalid handles,
 * stack overflows and corruption always; access violations and illegal
 * instructions, which some programs raise on purpose, with
 * WINEMETALGL_EXCEPTIONS=1. */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
static LONG exception_reports;
static volatile DWORD exception_thread;  /* the thread logging one */
static BOOL log_all_exceptions;

static void describe_address( char *out, size_t size, ULONG_PTR address )
{
    HMODULE mod;
    char path[MAX_PATH];
    const char *name;

    if (GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &mod ) && GetModuleFileNameA( mod, path, sizeof(path) ))
    {
        name = strrchr( path, '\\' ) ? strrchr( path, '\\' ) + 1 : path;
        snprintf( out, size, "%p %s+0x%llx", (void *)address, name, (unsigned long long)(address - (ULONG_PTR)mod) );
    }
    else snprintf( out, size, "%p", (void *)address );
}

static LONG CALLBACK log_exception( EXCEPTION_POINTERS *info )
{
    const EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT context = *info->ContextRecord;
    char where[MAX_PATH + 64];
    unsigned int i;

    switch (rec->ExceptionCode)
    {
    case EXCEPTION_INVALID_HANDLE:
    case EXCEPTION_STACK_OVERFLOW:
    case STATUS_HEAP_CORRUPTION:
    case STATUS_STACK_BUFFER_OVERRUN:
        break;
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        if (log_all_exceptions) break;
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* Not the faults its own probes take (IsBadReadPtr catches them). */
    if (exception_thread == GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement( &exception_reports ) > 16) return EXCEPTION_CONTINUE_SEARCH;
    exception_thread = GetCurrentThreadId();
    describe_address( where, sizeof(where), (ULONG_PTR)rec->ExceptionAddress );
    WMGL_ERR( "exception %08lx (flags %lx) at %s, thread %04lx\n", rec->ExceptionCode, rec->ExceptionFlags, where,
              GetCurrentThreadId() );
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        WMGL_ERR( "  %s %p\n", rec->ExceptionInformation[0] == 8 ? "executing" : rec->ExceptionInformation[0] ? "writing" : "reading",
                  (void *)rec->ExceptionInformation[1] );
    /* Its registers, and what those that point at memory point at: what an
     * overwritten object holds often says who overwrote it. */
    {
        const CONTEXT *c = info->ContextRecord;
        const struct { const char *name; DWORD64 value; } regs[] =
        {
            {"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx}, {"rsi", c->Rsi}, {"rdi", c->Rdi},
            {"rbp", c->Rbp}, {"rsp", c->Rsp}, {"r8", c->R8}, {"r9", c->R9}, {"r10", c->R10}, {"r11", c->R11},
            {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15},
        };
        for (i = 0; i < ARRAY_SIZE(regs); i++)
        {
            const DWORD64 *p = (const DWORD64 *)(ULONG_PTR)(regs[i].value & ~(DWORD64)7);
            if (regs[i].value < 0x10000 || IsBadReadPtr( p - 4, 12 * sizeof(DWORD64) ))
            {
                WMGL_ERR( "  %-3s %016llx\n", regs[i].name, (unsigned long long)regs[i].value );
                continue;
            }
            WMGL_ERR( "  %-3s %016llx -> -20: %016llx %016llx %016llx %016llx | %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n",
                      regs[i].name, (unsigned long long)regs[i].value,
                      (unsigned long long)p[-4], (unsigned long long)p[-3], (unsigned long long)p[-2], (unsigned long long)p[-1],
                      (unsigned long long)p[0], (unsigned long long)p[1], (unsigned long long)p[2], (unsigned long long)p[3],
                      (unsigned long long)p[4], (unsigned long long)p[5], (unsigned long long)p[6], (unsigned long long)p[7] );
        }
    }
    /* The raiser's frames, from its context. */
    for (i = 0; i < 24 && context.Rip; i++)
    {
        DWORD64 base, frame;
        void *data;
        RUNTIME_FUNCTION *function = RtlLookupFunctionEntry( context.Rip, &base, NULL );

        describe_address( where, sizeof(where), context.Rip );
        WMGL_ERR( "  #%u %s\n", i, where );
        if (function)
            RtlVirtualUnwind( UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &data, &frame, NULL );
        else
        {
            /* A leaf: its return address is on top of the stack. */
            if (!context.Rsp || IsBadReadPtr( (void *)context.Rsp, sizeof(DWORD64) )) break;
            context.Rip = *(DWORD64 *)context.Rsp;
            context.Rsp += sizeof(DWORD64);
        }
    }
    exception_thread = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static BOOL init_pixel_formats(void)
{
    struct wmgl_init_params init = {0};
    struct wmgl_pixel_formats_params params = {0};
    struct wmgl_pixel_format *formats;
    const char *(CDECL *wine_version)(void);
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    /* Long paths (a deep Steam library) do not fit MAX_PATH: UTF-8 of up to 1023 characters. */
    static WCHAR path[1024];
    static char program[3 * 1024];
    DWORD len;

    /* For the log: the program, and Wine's build (its version, and whose). */
    const DWORD size = sizeof(path) / sizeof(path[0]);
    if ((len = GetModuleFileNameW( NULL, path, size )) && len < size &&
        WideCharToMultiByte( CP_UTF8, 0, path, -1, program, sizeof(program), NULL, NULL ))
        init.program = (UINT64)(ULONG_PTR)program;
    if ((*(FARPROC *)&wine_version = GetProcAddress( ntdll, "wine_get_build_id" )) ||
        (*(FARPROC *)&wine_version = GetProcAddress( ntdll, "wine_get_version" )))
        init.host_version = (UINT64)(ULONG_PTR)wine_version();
    if (wmgl_unix_call( unix_wmgl_init, &init )) return FALSE;
    wmgl_log_max = init.log_level;
    log_ready = TRUE;
    memcpy( upscale_setting, init.upscale, sizeof(upscale_setting) );
    upscale_setting[sizeof(upscale_setting) - 1] = 0;
    WMGL_INFO( "winemetalgl.dll (%d-bit) in %s\n", (int)sizeof(void *) * 8, program );
#ifdef __x86_64__
    log_all_exceptions = getenv( "WINEMETALGL_EXCEPTIONS" ) && !strcmp( getenv( "WINEMETALGL_EXCEPTIONS" ), "1" );
    AddVectoredExceptionHandler( 1, log_exception );
#endif
    if (!init.ok || !init.format_count)
    {
        WMGL_ERR( "the WineMetalGL core reported no pixel formats\n" );
        return FALSE;
    }
    if (!(formats = calloc( init.format_count, sizeof(*formats) ))) return FALSE;
    params.formats = (UINT64)(ULONG_PTR)formats;
    params.max = init.format_count;
    if (wmgl_unix_call( unix_wmgl_pixel_formats, &params ))
    {
        free( formats );
        return FALSE;
    }
    wmgl_init_pixel_formats( formats, params.count, init.onscreen_count );
    free( formats );
    return TRUE;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        module = instance;
        wmgl_log_max = getenv( "WINEMETALGL_WINE_DEBUG" ) ? 3 : 0;
        if (getenv( "WINEMETALGL_BATCH" ) && !strcmp( getenv( "WINEMETALGL_BATCH" ), "0" )) batching = batching_allowed = FALSE;
        if ((tls_index = TlsAlloc()) == TLS_OUT_OF_INDEXES) return FALSE;
        if (!init_unix_calls( instance ))
        {
            WMGL_ERR( "winemetalgl.so is not loaded (is winemetalgl.dll installed as a builtin?)\n" );
            return FALSE;
        }
        if (!init_pixel_formats())
        {
            WMGL_ERR( "the WineMetalGL core did not start (libwinemetalgl.dylib)\n" );
            return FALSE;
        }
        upscalegl_init( instance, upscale_setting );
        break;
    case DLL_THREAD_DETACH:
    {
        /* A thread's batched calls run before it ends. */
        struct thread_state *state = thread_state( FALSE );
        if (!state) break;
        flush_batch( state, FALSE );
        free( state->batch );
        free( state->extensions );
        free( state );
        TlsSetValue( tls_index, NULL );
        break;
    }
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        if (thread_state( FALSE ))
        {
            flush_batch( thread_state( FALSE ), FALSE );
            free( thread_state( FALSE )->batch );
            free( thread_state( FALSE )->extensions );
        }
        free( thread_state( FALSE ) );
        TlsFree( tls_index );
        free( wgl_extensions );
        break;
    }
    return TRUE;
}

/* Windows that have had a drawable, so their drawables go when they are
 * destroyed: no window message reaches this module. */
static SRWLOCK windows_lock = SRWLOCK_INIT;
static HWND windows[256];
static unsigned int window_count, window_checks;

/* TRUE: the window is new here. */
static BOOL track_window( HWND hwnd )
{
    unsigned int i;
    BOOL added = FALSE;

    AcquireSRWLockExclusive( &windows_lock );
    for (i = 0; i < window_count; i++)
        if (windows[i] == hwnd) break;
    if (i == window_count && window_count < sizeof(windows) / sizeof(windows[0]))
    {
        windows[window_count++] = hwnd;
        added = TRUE;
    }
    ReleaseSRWLockExclusive( &windows_lock );
    return added;
}

static void forget_destroyed_windows(void)
{
    struct wmgl_forget_window_params params;
    HWND destroyed[16];
    unsigned int i, count = 0;

    AcquireSRWLockExclusive( &windows_lock );
    for (i = 0; i < window_count && count < sizeof(destroyed) / sizeof(destroyed[0]);)
    {
        if (IsWindow( windows[i] )) i++;
        else destroyed[count++] = windows[i], windows[i] = windows[--window_count];
    }
    ReleaseSRWLockExclusive( &windows_lock );
    for (i = 0; i < count; i++)
    {
        params.hwnd = (UINT64)(ULONG_PTR)destroyed[i];
        wmgl_unix_call( unix_wmgl_forget_window, &params );
    }
}

static BOOL is_tracked( HWND hwnd )
{
    unsigned int i;
    BOOL found = FALSE;

    AcquireSRWLockShared( &windows_lock );
    for (i = 0; i < window_count && !found; i++) found = windows[i] == hwnd;
    ReleaseSRWLockShared( &windows_lock );
    return found;
}

/* Where a window is (wmgl_window_placement), in pixels no DPI awareness
 * has scaled; `gone`, a child window going away, makes no hole. */
static void hwnd_placement( HWND hwnd, HWND gone, struct wmgl_window_placement *placement )
{
    DPI_AWARENESS_CONTEXT awareness;
    HWND top_level, parent, child;
    RECT client = {0}, around = {0}, clip, shown, rect;
    struct wmgl_rect *hole;

    memset( placement, 0, sizeof(*placement) );
    awareness = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    top_level = GetAncestor( hwnd, GA_ROOT );
    GetClientRect( hwnd, &client );
    GetClientRect( top_level, &around );
    MapWindowPoints( hwnd, top_level, (POINT *)&client, 2 );
    /* A child window shows only within its ancestors' client areas. */
    IntersectRect( &clip, &client, &around );
    for (parent = GetAncestor( hwnd, GA_PARENT ); parent && parent != top_level; parent = GetAncestor( parent, GA_PARENT ))
    {
        GetClientRect( parent, &shown );
        MapWindowPoints( parent, top_level, (POINT *)&shown, 2 );
        IntersectRect( &clip, &clip, &shown );
    }
    placement->hwnd = (UINT64)(ULONG_PTR)hwnd;
    placement->top_level = (UINT64)(ULONG_PTR)top_level;
    placement->x = client.left;
    placement->y = client.top;
    placement->width = client.right - client.left;
    placement->height = client.bottom - client.top;
    upscalegl_render_size( hwnd, placement->width, placement->height, &placement->render_width,
                           &placement->render_height );
    placement->top_level_width = around.right;
    placement->top_level_height = around.bottom;
    placement->screen_width = GetSystemMetrics( SM_CXSCREEN );
    placement->clip_x = clip.left;
    placement->clip_y = clip.top;
    placement->clip_width = clip.right - clip.left;
    placement->clip_height = clip.bottom - clip.top;
    placement->visible = IsWindowVisible( hwnd );
    /* Child windows a window with a pixel format clips out (on Windows, by
     * WS_CLIPCHILDREN, which such a window needs), but whose drawables have
     * layers of their own. */
    for (child = GetWindow( hwnd, GW_CHILD ); child && placement->hole_count < WMGL_MAX_HOLES;
         child = GetWindow( child, GW_HWNDNEXT ))
    {
        if (child == gone || !IsWindowVisible( child ) || is_tracked( child )) continue;
        GetWindowRect( child, &rect );
        MapWindowPoints( HWND_DESKTOP, top_level, (POINT *)&rect, 2 );
        if (!IntersectRect( &rect, &rect, &clip )) continue;
        hole = &placement->holes[placement->hole_count++];
        hole->x = rect.left;
        hole->y = rect.top;
        hole->width = rect.right - rect.left;
        hole->height = rect.bottom - rect.top;
    }
    SetThreadDpiAwarenessContext( awareness );
}

/* A child window of a window with a drawable was created, shown, hidden,
 * moved, given to another parent or destroyed: the drawable's layer makes
 * room for it now, as a program that stops swapping while it plays a movie
 * in it would not have us see before the movie ends. */
static void CALLBACK child_window_event( HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                         DWORD thread, DWORD time )
{
    struct wmgl_place_window_params params;
    HWND parent;

    if (object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd) return;
    if (!(GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD) || !(parent = GetAncestor( hwnd, GA_PARENT ))) return;
    if (!is_tracked( parent )) return;
    hwnd_placement( parent, event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY ? hwnd : NULL, &params.window );
    wmgl_unix_call( unix_wmgl_place_window, &params );
}

static BOOL CALLBACK watch_child_windows( INIT_ONCE *once, void *param, void **context )
{
    /* In context: called in the thread that changes the window, which may
     * not be one that pumps messages. */
    if (!SetWinEventHook( EVENT_OBJECT_CREATE, EVENT_OBJECT_PARENTCHANGE, module, child_window_event,
                          GetCurrentProcessId(), 0, WINEVENT_INCONTEXT ))
        WMGL_WARN( "no hook for child windows (%lu): a window's child windows may be hidden under GL\n",
                  GetLastError() );
    return TRUE;
}

/* Where a DC's window is. */
static void window_placement( HDC hdc, struct wmgl_window_placement *placement )
{
    static INIT_ONCE watching = INIT_ONCE_STATIC_INIT;
    HWND hwnd;

    struct wmgl_place_window_params params;
    HWND parent;

    memset( placement, 0, sizeof(*placement) );
    if (!hdc || !(hwnd = WindowFromDC( hdc ))) return;
    InitOnceExecuteOnce( &watching, watch_child_windows, NULL, NULL );
    /* A child window that gets a drawable of its own (wined3d presenting
     * a movie into a video window) needs no hole in its parent's layer: its
     * own layer is above it, and shows its parent until its first frame,
     * where a hole would show what Wine painted there (white). */
    if (track_window( hwnd ) && (GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD) &&
        (parent = GetAncestor( hwnd, GA_PARENT )) && is_tracked( parent ))
    {
        hwnd_placement( parent, NULL, &params.window );
        wmgl_unix_call( unix_wmgl_place_window, &params );
    }
    hwnd_placement( hwnd, NULL, placement );
}

/* ------------------------------------------------------------ contexts */

static HGLRC create_context( HDC hdc, HGLRC share, const int *attribs )
{
    struct wmgl_create_context_params params =
    {
        .hdc = (UINT64)(ULONG_PTR)hdc,
        .hwnd = (UINT64)(ULONG_PTR)WindowFromDC( hdc ),
        .share = (UINT64)(ULONG_PTR)share,
        .attribs = (UINT64)(ULONG_PTR)attribs,
    };
    if (wmgl_unix_call( unix_wmgl_create_context, &params ) || !params.ret)
    {
        SetLastError( params.error ? params.error : ERROR_INVALID_PARAMETER );
        return 0;
    }
    return (HGLRC)(ULONG_PTR)params.ret;
}

HGLRC WINAPI wglCreateContext( HDC hdc )
{
    return create_context( hdc, 0, NULL );
}

HGLRC WINAPI wglCreateContextAttribsARB( HDC hdc, HGLRC share, const int *attribs )
{
    return create_context( hdc, share, attribs );
}

HGLRC WINAPI wglCreateLayerContext( HDC hdc, int plane )
{
    if (plane == 0) return wglCreateContext( hdc );
    SetLastError( ERROR_INVALID_PARAMETER );
    return 0;
}

static BOOL make_current( HDC draw, HDC read, HGLRC context )
{
    struct thread_state *state = thread_state( TRUE );
    struct wmgl_make_current_params params =
    {
        .draw_hdc = (UINT64)(ULONG_PTR)draw,
        .read_hdc = (UINT64)(ULONG_PTR)read,
        .context = (UINT64)(ULONG_PTR)context,
    };
    if (!state) return FALSE;
    if (context)
    {
        forget_destroyed_windows();
        window_placement( draw, &params.draw );
        window_placement( read, &params.read );
    }
    if (wmgl_unix_call( unix_wmgl_make_current, &params ) || !params.ret)
    {
        SetLastError( params.error ? params.error : ERROR_INVALID_HANDLE );
        return FALSE;
    }
    state->context = context;
    state->draw = context ? draw : 0;
    state->read = context ? read : 0;
    return TRUE;
}

BOOL WINAPI wglMakeCurrent( HDC hdc, HGLRC context )
{
    if (!context)
    {
        struct thread_state *state = thread_state( FALSE );
        if (!state || !state->context) return TRUE;
    }
    return make_current( hdc, hdc, context );
}

BOOL WINAPI wglMakeContextCurrentARB( HDC draw, HDC read, HGLRC context )
{
    return make_current( draw, read, context );
}

BOOL WINAPI wglDeleteContext( HGLRC context )
{
    struct thread_state *state = thread_state( FALSE );
    struct wmgl_delete_context_params params = { .context = (UINT64)(ULONG_PTR)context };

    if (wmgl_unix_call( unix_wmgl_delete_context, &params ) || !params.ret)
    {
        SetLastError( params.error ? params.error : ERROR_INVALID_HANDLE );
        return FALSE;
    }
    /* Deleting this thread's current context releases it. */
    if (state && state->context == context) state->context = 0, state->draw = state->read = 0;
    return TRUE;
}

HGLRC WINAPI wglGetCurrentContext(void)
{
    struct thread_state *state = thread_state( FALSE );
    return state ? state->context : 0;
}

HDC WINAPI wglGetCurrentDC(void)
{
    struct thread_state *state = thread_state( FALSE );
    return state && state->context ? state->draw : 0;
}

HDC WINAPI wglGetCurrentReadDCARB(void)
{
    struct thread_state *state = thread_state( FALSE );
    return state && state->context ? state->read : 0;
}

BOOL WINAPI wglShareLists( HGLRC source, HGLRC dest )
{
    struct wmgl_share_lists_params params = { .source = (UINT64)(ULONG_PTR)source, .dest = (UINT64)(ULONG_PTR)dest };
    if (wmgl_unix_call( unix_wmgl_share_lists, &params )) return FALSE;
    if (!params.ret) SetLastError( ERROR_INVALID_OPERATION );
    return params.ret;
}

BOOL WINAPI wglCopyContext( HGLRC source, HGLRC dest, UINT mask )
{
    struct wmgl_copy_context_params params =
        { .source = (UINT64)(ULONG_PTR)source, .dest = (UINT64)(ULONG_PTR)dest, .mask = mask };
    if (wmgl_unix_call( unix_wmgl_copy_context, &params )) return FALSE;
    return params.ret;
}

/* ------------------------------------------------------- pixel formats */

INT WINAPI wglGetPixelFormat( HDC hdc )
{
    struct wmgl_get_pixel_format_params params =
        { .hdc = (UINT64)(ULONG_PTR)hdc, .hwnd = (UINT64)(ULONG_PTR)WindowFromDC( hdc ) };
    if (wmgl_unix_call( unix_wmgl_get_pixel_format, &params )) return 0;
    if (!params.ret) SetLastError( ERROR_INVALID_PIXEL_FORMAT );
    return params.ret;
}

static BOOL set_pixel_format( HDC hdc, int format, BOOL internal )
{
    struct wmgl_set_pixel_format_params params =
    {
        .hdc = (UINT64)(ULONG_PTR)hdc, .hwnd = (UINT64)(ULONG_PTR)WindowFromDC( hdc ), .format = format,
        .internal = internal,
    };
    if (wmgl_unix_call( unix_wmgl_set_pixel_format, &params )) return FALSE;
    if (!params.ret) SetLastError( ERROR_INVALID_PIXEL_FORMAT );
    return params.ret;
}

BOOL WINAPI wglSetPixelFormat( HDC hdc, int format, const PIXELFORMATDESCRIPTOR *pfd )
{
    return set_pixel_format( hdc, format, FALSE );
}

BOOL WINAPI wglSetPixelFormatWINE( HDC hdc, int format )
{
    return set_pixel_format( hdc, format, TRUE );
}

/* ---------------------------------------------------------------- swaps */

BOOL WINAPI wglSwapBuffers( HDC hdc )
{
    struct wmgl_swap_buffers_params params = { .hdc = (UINT64)(ULONG_PTR)hdc };
    if (!(InterlockedIncrement( (LONG *)&window_checks ) % 64)) forget_destroyed_windows();
    window_placement( hdc, &params.window );
    if (wmgl_unix_call( unix_wmgl_swap_buffers, &params )) return FALSE;
    return params.ret;
}

BOOL WINAPI wglSwapLayerBuffers( HDC hdc, UINT planes )
{
    if (planes & WGL_SWAP_MAIN_PLANE) return wglSwapBuffers( hdc );
    return TRUE;
}

BOOL WINAPI wglSwapIntervalEXT( int interval )
{
    struct wmgl_swap_interval_params params = { .interval = interval };
    if (interval < 0)
    {
        SetLastError( ERROR_INVALID_DATA );
        return FALSE;
    }
    if (wmgl_unix_call( unix_wmgl_swap_interval, &params )) return FALSE;
    return params.ret;
}

int WINAPI wglGetSwapIntervalEXT(void)
{
    struct wmgl_swap_interval_params params = {0};
    if (wmgl_unix_call( unix_wmgl_get_swap_interval, &params )) return 0;
    return params.interval;
}

/* -------------------------------------------------------------- pbuffers */

HPBUFFERARB WINAPI wglCreatePbufferARB( HDC hdc, int format, int width, int height, const int *attribs )
{
    struct wmgl_create_pbuffer_params params =
    {
        .hdc = (UINT64)(ULONG_PTR)hdc, .hwnd = (UINT64)(ULONG_PTR)WindowFromDC( hdc ),
        .format = format, .width = width, .height = height,
        .attribs = (UINT64)(ULONG_PTR)attribs,
    };
    if (wmgl_unix_call( unix_wmgl_create_pbuffer, &params ) || !params.ret)
    {
        SetLastError( params.error ? params.error : ERROR_INVALID_DATA );
        return 0;
    }
    return (HPBUFFERARB)(ULONG_PTR)params.ret;
}

static BOOL pbuffer_call( unsigned int code, struct wmgl_pbuffer_params *params )
{
    if (wmgl_unix_call( code, params )) return FALSE;
    if (!params->ret) SetLastError( params->error ? params->error : ERROR_INVALID_HANDLE );
    return params->ret;
}

BOOL WINAPI wglDestroyPbufferARB( HPBUFFERARB pbuffer )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer };
    return pbuffer_call( unix_wmgl_destroy_pbuffer, &params );
}

/* A pbuffer's DC is a memory DC the unix side knows as the pbuffer's. */
HDC WINAPI wglGetPbufferDCARB( HPBUFFERARB pbuffer )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer };
    HDC hdc = CreateCompatibleDC( 0 );

    if (!hdc) return 0;
    params.hdc = (UINT64)(ULONG_PTR)hdc;
    if (!pbuffer_call( unix_wmgl_get_pbuffer_dc, &params ) || params.hdc != (UINT64)(ULONG_PTR)hdc)
    {
        /* Failed, or the pbuffer has its DC already. */
        DeleteDC( hdc );
        return params.ret ? (HDC)(ULONG_PTR)params.hdc : 0;
    }
    return hdc;
}

int WINAPI wglReleasePbufferDCARB( HPBUFFERARB pbuffer, HDC hdc )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer, .hdc = (UINT64)(ULONG_PTR)hdc };
    if (!pbuffer_call( unix_wmgl_release_pbuffer_dc, &params )) return 0;
    DeleteDC( hdc );
    return 1;
}

BOOL WINAPI wglQueryPbufferARB( HPBUFFERARB pbuffer, int attribute, int *value )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer, .attribute = attribute };
    if (!pbuffer_call( unix_wmgl_query_pbuffer, &params )) return FALSE;
    *value = params.value;
    return TRUE;
}

BOOL WINAPI wglBindTexImageARB( HPBUFFERARB pbuffer, int buffer )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer, .attribute = buffer };
    return pbuffer_call( unix_wmgl_bind_tex_image, &params );
}

BOOL WINAPI wglReleaseTexImageARB( HPBUFFERARB pbuffer, int buffer )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer, .attribute = buffer };
    return pbuffer_call( unix_wmgl_release_tex_image, &params );
}

BOOL WINAPI wglSetPbufferAttribARB( HPBUFFERARB pbuffer, const int *attribs )
{
    struct wmgl_pbuffer_params params = { .pbuffer = (UINT64)(ULONG_PTR)pbuffer, .attribs = (UINT64)(ULONG_PTR)attribs };
    return pbuffer_call( unix_wmgl_set_pbuffer_attrib, &params );
}

/* ---------------------------------------------------- renderer queries */

static BOOL query_renderer_integer( HDC hdc, GLint renderer, GLenum attribute, GLuint *value )
{
    struct wmgl_query_renderer_params params =
        { .hdc = (UINT64)(ULONG_PTR)hdc, .renderer = renderer, .attribute = attribute, .value = (UINT64)(ULONG_PTR)value };
    if (wmgl_unix_call( unix_wmgl_query_renderer_integer, &params )) return FALSE;
    return params.ret;
}

static const GLchar *query_renderer_string( HDC hdc, GLint renderer, GLenum attribute )
{
    static char strings[2][256];  /* device, vendor: constant for the process */
    char buffer[256];
    struct wmgl_query_renderer_params params =
    {
        .hdc = (UINT64)(ULONG_PTR)hdc, .renderer = renderer, .attribute = attribute,
        .buffer = (UINT64)(ULONG_PTR)buffer, .size = sizeof(buffer),
    };
    int slot = attribute == WGL_RENDERER_VENDOR_ID_WINE ? 1 : 0;
    if (wmgl_unix_call( unix_wmgl_query_renderer_string, &params ) || !params.ret) return NULL;
    lstrcpynA( strings[slot], buffer, sizeof(strings[slot]) );
    return strings[slot];
}

BOOL WINAPI wglQueryCurrentRendererIntegerWINE( GLenum attribute, GLuint *value )
{
    return query_renderer_integer( 0, 0, attribute, value );
}

const GLchar *WINAPI wglQueryCurrentRendererStringWINE( GLenum attribute )
{
    return query_renderer_string( 0, 0, attribute );
}

BOOL WINAPI wglQueryRendererIntegerWINE( HDC dc, GLint renderer, GLenum attribute, GLuint *value )
{
    return query_renderer_integer( dc, renderer, attribute, value );
}

const GLchar *WINAPI wglQueryRendererStringWINE( HDC dc, GLint renderer, GLenum attribute )
{
    return query_renderer_string( dc, renderer, attribute );
}

/* ------------------------------------------------------ extension strings */

static const char *extensions_string(void)
{
    struct wmgl_extensions_string_params params = {0};
    char *string;

    if (wgl_extensions) return wgl_extensions;
    if (wmgl_unix_call( unix_wmgl_extensions_string, &params ) || !params.needed) return "";
    if (!(string = calloc( 1, params.needed ))) return "";
    params.buffer = (UINT64)(ULONG_PTR)string;
    params.size = params.needed;
    if (wmgl_unix_call( unix_wmgl_extensions_string, &params ))
    {
        free( string );
        return "";
    }
    if (InterlockedCompareExchangePointer( (void **)&wgl_extensions, string, NULL )) free( string );
    return wgl_extensions;
}

const char *WINAPI wglGetExtensionsStringARB( HDC hdc )
{
    return extensions_string();
}

const char *WINAPI wglGetExtensionsStringEXT(void)
{
    return extensions_string();
}

/* --------------------------------------------------------- proc addresses */

static const struct
{
    const char *name;
    PROC proc;
    const char *extension;
} wgl_procs[] =
{
    {"wglBindTexImageARB", (PROC)wglBindTexImageARB, "WGL_ARB_render_texture"},
    {"wglChoosePixelFormatARB", (PROC)wglChoosePixelFormatARB, "WGL_ARB_pixel_format"},
    {"wglCreateContextAttribsARB", (PROC)wglCreateContextAttribsARB, "WGL_ARB_create_context"},
    {"wglCreatePbufferARB", (PROC)wglCreatePbufferARB, "WGL_ARB_pbuffer"},
    {"wglDestroyPbufferARB", (PROC)wglDestroyPbufferARB, "WGL_ARB_pbuffer"},
    {"wglGetCurrentReadDCARB", (PROC)wglGetCurrentReadDCARB, "WGL_ARB_make_current_read"},
    {"wglGetExtensionsStringARB", (PROC)wglGetExtensionsStringARB, "WGL_ARB_extensions_string"},
    {"wglGetExtensionsStringEXT", (PROC)wglGetExtensionsStringEXT, "WGL_EXT_extensions_string"},
    {"wglGetPbufferDCARB", (PROC)wglGetPbufferDCARB, "WGL_ARB_pbuffer"},
    {"wglGetPixelFormatAttribfvARB", (PROC)wglGetPixelFormatAttribfvARB, "WGL_ARB_pixel_format"},
    {"wglGetPixelFormatAttribivARB", (PROC)wglGetPixelFormatAttribivARB, "WGL_ARB_pixel_format"},
    {"wglGetSwapIntervalEXT", (PROC)wglGetSwapIntervalEXT, "WGL_EXT_swap_control"},
    {"wglMakeContextCurrentARB", (PROC)wglMakeContextCurrentARB, "WGL_ARB_make_current_read"},
    {"wglQueryCurrentRendererIntegerWINE", (PROC)wglQueryCurrentRendererIntegerWINE, "WGL_WINE_query_renderer"},
    {"wglQueryCurrentRendererStringWINE", (PROC)wglQueryCurrentRendererStringWINE, "WGL_WINE_query_renderer"},
    {"wglQueryPbufferARB", (PROC)wglQueryPbufferARB, "WGL_ARB_pbuffer"},
    {"wglQueryRendererIntegerWINE", (PROC)wglQueryRendererIntegerWINE, "WGL_WINE_query_renderer"},
    {"wglQueryRendererStringWINE", (PROC)wglQueryRendererStringWINE, "WGL_WINE_query_renderer"},
    {"wglReleasePbufferDCARB", (PROC)wglReleasePbufferDCARB, "WGL_ARB_pbuffer"},
    {"wglReleaseTexImageARB", (PROC)wglReleaseTexImageARB, "WGL_ARB_render_texture"},
    {"wglSetPbufferAttribARB", (PROC)wglSetPbufferAttribARB, "WGL_ARB_render_texture"},
    {"wglSetPixelFormatWINE", (PROC)wglSetPixelFormatWINE, "WGL_WINE_pixel_format_passthrough"},
    {"wglSwapIntervalEXT", (PROC)wglSwapIntervalEXT, "WGL_EXT_swap_control"},
};

static BOOL has_extension( const char *list, const char *name )
{
    size_t len = strlen( name );
    const char *p = list;
    while ((p = strstr( p, name )))
    {
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || !p[len])) return TRUE;
        p += len;
    }
    return FALSE;
}

/* The current context's GL extensions (one space-separated list per
 * thread, rebuilt when another context becomes current there). */
static BOOL context_has_extension( const char *name )
{
    struct thread_state *state = thread_state( FALSE );
    HGLRC context = wglGetCurrentContext();
    GLint count = 0, i;
    size_t size = 1;
    char *list;

    if (!state || !context) return FALSE;
    if (context != state->extensions_context || !state->extensions)
    {
        glGetIntegerv( GL_NUM_EXTENSIONS, &count );
        for (i = 0; i < count; i++)
        {
            const char *ext = (const char *)glGetStringi( GL_EXTENSIONS, i );
            if (ext) size += strlen( ext ) + 1;
        }
        if (!(list = calloc( 1, size ))) return FALSE;
        for (i = 0; i < count; i++)
        {
            const char *ext = (const char *)glGetStringi( GL_EXTENSIONS, i );
            if (!ext) continue;
            if (*list) strcat( list, " " );
            strcat( list, ext );
        }
        free( state->extensions );
        state->extensions = list;
        state->extensions_context = context;
    }
    return has_extension( state->extensions, name );
}

/* Whether the current context has any of these space-separated extensions. */
static BOOL context_has_any_extension( const char *names )
{
    char name[128];
    const char *end;

    for (; *names; names = *end ? end + 1 : end)
    {
        size_t len;
        if (!(end = strchr( names, ' ' ))) end = names + strlen( names );
        if ((len = end - names) && len < sizeof(name))
        {
            memcpy( name, names, len );
            name[len] = 0;
            if (context_has_extension( name )) return TRUE;
        }
    }
    return FALSE;
}

PROC WINAPI wglGetProcAddress( LPCSTR name )
{
    unsigned int lo = 0, hi = wmgl_proc_count, i;

    if (!name) return NULL;
    for (i = 0; i < sizeof(wgl_procs) / sizeof(wgl_procs[0]); i++)
        if (!strcmp( wgl_procs[i].name, name ))
        {
            if (has_extension( extensions_string(), wgl_procs[i].extension )) return wgl_procs[i].proc;
            WMGL_INFO( "wglGetProcAddress(%s): NULL, %s is not offered\n", name, wgl_procs[i].extension );
            return NULL;
        }
    if (!wglGetCurrentContext())
    {
        WMGL_WARN( "wglGetProcAddress(%s): NULL, no current context\n", name );
        return NULL;
    }
    while (lo < hi)
    {
        unsigned int mid = (lo + hi) / 2;
        int cmp = strcmp( name, wmgl_procs[mid].name );
        if (!cmp)
        {
            const struct wmgl_proc *proc = &wmgl_procs[mid];
            /* Commands of a GL version (every context is the newest) always,
             * extension commands when the context offers the extension. */
            if (proc->source[0] >= '0' && proc->source[0] <= '9') return proc->proc;
            if (context_has_any_extension( proc->source )) return proc->proc;
            WMGL_INFO( "wglGetProcAddress(%s): NULL, not offered (%s)\n", name, proc->source );
            return NULL;
        }
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    WMGL_INFO( "wglGetProcAddress(%s): NULL, not a command WineMetalGL implements\n", name );
    return NULL;
}

PROC WINAPI wglGetDefaultProcAddress( LPCSTR name )
{
    return NULL;
}

/* ----------------------------------------------------------- layer planes */

BOOL WINAPI wglDescribeLayerPlane( HDC hdc, int format, int plane, UINT size, LAYERPLANEDESCRIPTOR *descr )
{
    return FALSE;
}

int WINAPI wglGetLayerPaletteEntries( HDC hdc, int plane, int start, int count, COLORREF *colors )
{
    return 0;
}

int WINAPI wglSetLayerPaletteEntries( HDC hdc, int plane, int start, int count, const COLORREF *colors )
{
    return 0;
}

BOOL WINAPI wglRealizeLayerPalette( HDC hdc, int plane, BOOL realize )
{
    return FALSE;
}

/* glDebugEntry (OPENGL32.@): undocumented, kept for the export list. */
GLint WINAPI glDebugEntry( GLint unknown1, GLint unknown2 )
{
    return 0;
}
