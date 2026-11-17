/* winemetalgl.so: loads the WineMetalGL core and carries GL calls to it
 * (the call tables are generated: wine_gl_unix.gen.c), with the debug
 * callbacks, which the core calls on the unix side where PE code cannot run:
 * their messages wait in a per-thread queue that the PE side drains after
 * the GL call that produced them (wmgl_after_call). */

#include <dlfcn.h>
#include <libgen.h>

#include "wmgl_unix.h"

#define GL_DEBUG_CALLBACK_FUNCTION 0x8244
#define GL_DEBUG_CALLBACK_USER_PARAM 0x8245

struct wmgl_core wmgl;
static void *core_handle;
/* What the PE side tells about the process (wmgl_unix_init), for the log. */
static char program[3 * 1024], host_version[256];

const char *wmgl_program(void)
{
    return program;
}

void *wmgl_core_symbol( const char *name )
{
    return core_handle ? dlsym( core_handle, name ) : NULL;
}

/* ------------------------------------------------------------ the core */

static BOOL load_core(void)
{
    Dl_info info;
    char path[4096], *dir;
    void *handle;
    unsigned int i;

    wmgl_log_start( NULL, NULL, NULL );
    /* libwinemetalgl.dylib sits next to winemetalgl.so. */
    if (!dladdr( (void *)load_core, &info ) || !info.dli_fname) return FALSE;
    snprintf( path, sizeof(path), "%s", info.dli_fname );
    dir = dirname( path );
    snprintf( path, sizeof(path), "%s/libwinemetalgl.dylib", dir );
    if (!(handle = dlopen( path, RTLD_NOW | RTLD_LOCAL )))
    {
        WMGL_ERR( "cannot load %s: %s (program %s)\n", path, dlerror(), program );
        return FALSE;
    }
    core_handle = handle;
    /* From here the core's log has the messages (before the core starts, so
     * that its log file is named after the program). */
    wmgl_log_start( handle, program, host_version );
    WMGL_INFO( "winemetalgl.so %s loaded the core %s\n", info.dli_fname, path );
#define LOAD(field, name) if (!(*(void **)&wmgl.field = dlsym( handle, name ))) { WMGL_ERR( "%s missing\n", name ); return FALSE; }
    LOAD(initialize, "wmgl_initialize");
    LOAD(get_pixel_formats, "wmgl_get_pixel_formats");
    LOAD(surface_create_window, "wmgl_surface_create_window");
    LOAD(surface_create_pbuffer, "wmgl_surface_create_pbuffer");
    LOAD(surface_destroy, "wmgl_surface_destroy");
    LOAD(surface_flush, "wmgl_surface_flush");
    LOAD(surface_swap, "wmgl_surface_swap");
    LOAD(context_create, "wmgl_context_create");
    LOAD(context_destroy, "wmgl_context_destroy");
    LOAD(make_current, "wmgl_make_current");
    LOAD(get_proc_address, "wmgl_get_proc_address");
    LOAD(renderer_string, "wmgl_renderer_string");
    LOAD(video_memory_mb, "wmgl_video_memory_mb");
    LOAD(max_versions, "wmgl_max_versions");
#undef LOAD
    *(void **)&wmgl.version = dlsym( handle, "wmgl_version" );
    *(void **)&wmgl.surface_set_output = dlsym( handle, "wmgl_surface_set_output" );
    if (!dlsym( handle, "wmgl_log_write" ))
        WMGL_ERR( "this WineMetalGL core is older than the module (it has no log): messages go to stderr\n" );
    if (wmgl.initialize( WMGL_DRIVER_ABI_VERSION ))
    {
        WMGL_ERR( "the WineMetalGL core refused to initialise (driver ABI %u)\n", WMGL_DRIVER_ABI_VERSION );
        return FALSE;
    }
    /* Every command the generated tables call (struct wmgl_core_procs is
     * one pointer per name, in wmgl_core_names order). */
    for (i = 0; i < WMGL_CORE_PROC_COUNT; i++)
        ((void **)&core)[i] = wmgl.get_proc_address( wmgl_core_names[i] );
    return TRUE;
}

static pthread_once_t core_once = PTHREAD_ONCE_INIT;
static BOOL core_loaded;

static void load_core_once(void)
{
    core_loaded = load_core();
}

extern NTSTATUS wmgl_unix_init_formats( void *args );

/* UpscaleGL's preset for the PE side (upscalegl.c): the PE side's environment
 * was copied before the core added the settings file's (settings.h). */
static void copy_upscale( struct wmgl_init_params *params )
{
    const char *upscale = getenv( "WINEMETALGL_UPSCALE" );
    snprintf( params->upscale, sizeof(params->upscale), "%s", upscale ? upscale : "" );
}

/* The first call of the PE side (DllMain): loads the core, then reports the
 * pixel formats (unix_wgl.c). */
NTSTATUS wmgl_unix_init( void *args )
{
    struct wmgl_init_params *params = args;
    const char *client_program = (const char *)(ULONG_PTR)params->program;
    const char *client_host = (const char *)(ULONG_PTR)params->host_version;

    /* The first call wins: one process, one program. */
    if (client_program && !program[0]) snprintf( program, sizeof(program), "%s", client_program );
    if (client_host && !host_version[0]) snprintf( host_version, sizeof(host_version), "%s", client_host );
    pthread_once( &core_once, load_core_once );
    if (!core_loaded)
    {
        memset( params, 0, sizeof(*params) );
        params->log_level = wmgl_pe_log_level();
        copy_upscale( params );
        return STATUS_SUCCESS;
    }
    if (params->game_settings)
    {
        /* Before anything reads its settings, UpscaleGL's preset included. */
        void (*apply_game_settings)( const char * );
        if ((*(void **)&apply_game_settings = wmgl_core_symbol( "wmgl_apply_game_settings" )))
            apply_game_settings( (const char *)(ULONG_PTR)params->game_settings );
    }
    params->log_level = wmgl_pe_log_level();
    copy_upscale( params );
    return wmgl_unix_init_formats( args );
}

/* ------------------------------------------------------- debug messages */

#define MAX_QUEUED 64

struct queued_message
{
    UINT64 callback, user;
    UINT source, type, id, severity;
    INT length;
    BOOL amd;
    char *text;
};

static __thread struct queued_message queue[MAX_QUEUED];
static __thread unsigned int queued, delivered;

NTSTATUS wmgl_after_call( void )
{
    return queued > delivered ? WMGL_STATUS_DEBUG_MESSAGES : STATUS_SUCCESS;
}

/* ------------------------------------------------------- batched calls */

extern const unixlib_entry_t __wine_unix_call_funcs[];

/* Runs a batch's records through `table`, in order; debug messages they
 * queue are delivered after the batch. */
static NTSTATUS run_batch( const struct wmgl_batch_params *params, const unixlib_entry_t *table )
{
    static void (*batch_begin)(void), (*batch_end)(void);
    static BOOL looked_up;
    const char *at = (const char *)(ULONG_PTR)params->data, *end = at + params->size;
    struct wmgl_batch_record record;

    /* The core's calls share one autorelease pool (older cores: one each). */
    if (!looked_up)
    {
        *(void **)&batch_begin = wmgl_core_symbol( "wmgl_batch_begin" );
        *(void **)&batch_end = wmgl_core_symbol( "wmgl_batch_end" );
        looked_up = TRUE;
    }
    if (batch_begin && batch_end) batch_begin();

    while ((size_t)(end - at) >= sizeof(record))
    {
        memcpy( &record, at, sizeof(record) );
        if (record.size < sizeof(record) || record.size > (size_t)(end - at) || record.code <= unix_wmgl_batch ||
            record.code >= unix_wmgl_call_count)
            break;
        table[record.code]( (void *)(at + sizeof(record)) );
        at += record.size;
    }
    if (batch_begin && batch_end) batch_end();
    return wmgl_after_call();
}

NTSTATUS wmgl_unix_batch( void *args )
{
    return run_batch( args, __wine_unix_call_funcs );
}

#ifdef _WIN64
extern const unixlib_entry_t __wine_unix_call_wow64_funcs[];

NTSTATUS wmgl_unix_batch_wow64( void *args )
{
    return run_batch( args, __wine_unix_call_wow64_funcs );
}
#endif

static void queue_message( BOOL amd, GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                           const GLchar *message )
{
    struct wmgl_debug_target *target = wmgl_current_debug_target();
    struct queued_message *slot;
    UINT64 callback, user;
    size_t len;

    if (!target) return;
    callback = amd ? target->callback_amd : target->callback;
    user = amd ? target->user_amd : target->user;
    if (!callback) return;
    if (queued == delivered) queued = delivered = 0;
    if (queued >= MAX_QUEUED) return;  /* the application gets the first ones */
    slot = &queue[queued];
    len = length >= 0 ? (size_t)length : (message ? strlen( message ) : 0);
    if (!(slot->text = malloc( len + 1 ))) return;
    if (len) memcpy( slot->text, message, len );
    slot->text[len] = 0;
    slot->callback = callback;
    slot->user = user;
    slot->source = source;
    slot->type = type;
    slot->id = id;
    slot->severity = severity;
    slot->length = (INT)len;
    slot->amd = amd;
    queued++;
}

static void debug_callback( GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                            const GLchar *message, const void *user )
{
    queue_message( FALSE, source, type, id, severity, length, message );
}

static void debug_callback_amd( GLuint id, GLenum category, GLenum severity, GLsizei length, const GLchar *message,
                                void *user )
{
    queue_message( TRUE, 0, category, id, severity, length, message );
}

NTSTATUS wmgl_unix_debug_message( void *args )
{
    struct wmgl_debug_message_params *params = args;
    char *buffer = (char *)(ULONG_PTR)params->buffer;
    struct queued_message *message;

    params->got = params->more = FALSE;
    if (delivered >= queued) return STATUS_SUCCESS;
    message = &queue[delivered++];
    params->callback = message->callback;
    params->user = message->user;
    params->source = message->source;
    params->type = message->type;
    params->id = message->id;
    params->severity = message->severity;
    params->length = message->length;
    params->amd = message->amd;
    if (buffer && params->size)
    {
        size_t len = (size_t)message->length < params->size - 1 ? (size_t)message->length : params->size - 1;
        memcpy( buffer, message->text, len );
        buffer[len] = 0;
        if ((size_t)message->length > len) params->length = (INT)len;
    }
    free( message->text );
    message->text = NULL;
    params->got = TRUE;
    params->more = delivered < queued;
    if (!params->more) queued = delivered = 0;
    return STATUS_SUCCESS;
}

/* glDebugMessageCallback*: the core calls ours; the application's waits in
 * the context for the PE side. */
static void set_callback( UINT64 callback, UINT64 user, BOOL amd )
{
    wmgl_set_debug_callback( callback, user, amd );
    if (amd) core.p_glDebugMessageCallbackAMD( callback ? (GLDEBUGPROCAMD)debug_callback_amd : NULL, NULL );
    else core.p_glDebugMessageCallback( callback ? (GLDEBUGPROC)debug_callback : NULL, NULL );
}

NTSTATUS manual_glDebugMessageCallback( void *args )
{
    struct glDebugMessageCallback_params *params = args;
    set_callback( (UINT64)(ULONG_PTR)params->callback, (UINT64)(ULONG_PTR)params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS manual_glDebugMessageCallbackARB( void *args )
{
    struct glDebugMessageCallbackARB_params *params = args;
    set_callback( (UINT64)(ULONG_PTR)params->callback, (UINT64)(ULONG_PTR)params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS manual_glDebugMessageCallbackKHR( void *args )
{
    struct glDebugMessageCallbackKHR_params *params = args;
    set_callback( (UINT64)(ULONG_PTR)params->callback, (UINT64)(ULONG_PTR)params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS manual_glDebugMessageCallbackAMD( void *args )
{
    struct glDebugMessageCallbackAMD_params *params = args;
    set_callback( (UINT64)(ULONG_PTR)params->callback, (UINT64)(ULONG_PTR)params->userParam, TRUE );
    return wmgl_after_call();
}

/* glGetPointerv*: the debug callback and its parameter are the application's. */
static BOOL debug_pointer( GLenum pname, UINT64 *value )
{
    struct wmgl_debug_target *target = wmgl_current_debug_target();
    if (pname != GL_DEBUG_CALLBACK_FUNCTION && pname != GL_DEBUG_CALLBACK_USER_PARAM) return FALSE;
    *value = !target ? 0 : pname == GL_DEBUG_CALLBACK_FUNCTION ? target->callback : target->user;
    return TRUE;
}

static NTSTATUS get_pointer( GLenum pname, void **out, void (*proc)( GLenum, void ** ) )
{
    UINT64 value;
    if (debug_pointer( pname, &value ))
    {
        if (out) *out = (void *)(ULONG_PTR)value;
    }
    else proc( pname, out );
    return wmgl_after_call();
}

NTSTATUS manual_glGetPointerv( void *args )
{
    struct glGetPointerv_params *params = args;
    return get_pointer( params->pname, params->params, core.p_glGetPointerv );
}

NTSTATUS manual_glGetPointervEXT( void *args )
{
    struct glGetPointervEXT_params *params = args;
    return get_pointer( params->pname, params->params, core.p_glGetPointervEXT );
}

NTSTATUS manual_glGetPointervKHR( void *args )
{
    struct glGetPointervKHR_params *params = args;
    return get_pointer( params->pname, params->params, core.p_glGetPointervKHR );
}

#ifdef _WIN64

NTSTATUS wow64_manual_glDebugMessageCallback( void *args )
{
    struct glDebugMessageCallback_params32 *params = args;
    set_callback( params->callback, params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glDebugMessageCallbackARB( void *args )
{
    struct glDebugMessageCallbackARB_params32 *params = args;
    set_callback( params->callback, params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glDebugMessageCallbackKHR( void *args )
{
    struct glDebugMessageCallbackKHR_params32 *params = args;
    set_callback( params->callback, params->userParam, FALSE );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glDebugMessageCallbackAMD( void *args )
{
    struct glDebugMessageCallbackAMD_params32 *params = args;
    set_callback( params->callback, params->userParam, TRUE );
    return wmgl_after_call();
}

/* A 32-bit caller's pointers are below 4 GB: what the core reports (the
 * application's own pointers) fits, and the debug callback is its own. */
static NTSTATUS wow64_get_pointer( GLenum pname, UINT out, void (*proc)( GLenum, void ** ) )
{
    UINT64 value;
    void *ptr = NULL;
    if (!debug_pointer( pname, &value ))
    {
        proc( pname, &ptr );
        value = wmgl_wow64_pointer( ptr );
    }
    if (out) *(UINT *)ULongToPtr( out ) = (UINT)value;
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetPointerv( void *args )
{
    struct glGetPointerv_params32 *params = args;
    return wow64_get_pointer( params->pname, params->params, core.p_glGetPointerv );
}

NTSTATUS wow64_manual_glGetPointervEXT( void *args )
{
    struct glGetPointervEXT_params32 *params = args;
    return wow64_get_pointer( params->pname, params->params, core.p_glGetPointervEXT );
}

NTSTATUS wow64_manual_glGetPointervKHR( void *args )
{
    struct glGetPointervKHR_params32 *params = args;
    return wow64_get_pointer( params->pname, params->params, core.p_glGetPointervKHR );
}

#endif
