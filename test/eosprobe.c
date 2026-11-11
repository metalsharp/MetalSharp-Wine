/* Loads a game's Windows EOS SDK under Wine and starts it, to see what it does on Wine (run with WINEDEBUG
 * tracing): eosprobe.exe <path to eossdk-win64-shipping.dll> [platform]
 * "platform" also creates an EOS platform with made-up ids, and asks it for the anti-cheat client interface. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* EOS 1.x: EOS_Initialize, and EOS_Platform_Create with its first options version */
struct eos_initialize_options
{
    int api_version;
    void *allocate, *reallocate, *release;
    const char *product_name, *product_version;
    void *reserved, *system_initialize_options, *override_thread_affinity;
};

struct eos_platform_options_1
{
    int api_version;
    void *reserved;
    const char *product_id, *sandbox_id;
    struct { int api_version; const char *client_id, *client_secret; } client_credentials;
    int is_server;
    const char *encryption_key, *override_country_code, *override_locale_code;
};

struct eos_log_message { const char *category, *message; int level; };

static void __cdecl log_callback( const struct eos_log_message *msg )
{
    printf( "  [eos %s] %s\n", msg->category, msg->message );
    fflush( stdout );
}

int main( int argc, char **argv )
{
    int (__cdecl *initialize)( const struct eos_initialize_options * );
    const char *(__cdecl *result_to_string)( int );
    const char *(__cdecl *get_version)( void );
    int (__cdecl *set_log_callback)( void (__cdecl *)(const struct eos_log_message *) );
    int (__cdecl *set_log_level)( int, int );
    void *(__cdecl *platform_create)( const struct eos_platform_options_1 * );
    void *(__cdecl *get_anticheat)( void * );
    void (__cdecl *platform_tick)( void * );
    struct eos_initialize_options init = { 4 };
    HMODULE eos;
    int ret;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc < 2)
    {
        printf( "usage: eosprobe <eossdk-win64-shipping.dll> [platform]\n" );
        return 2;
    }
    if (!(eos = LoadLibraryA( argv[1] )))
    {
        printf( "LoadLibrary failed: %lu\n", GetLastError() );
        return 1;
    }
#define GET(var, name) var = (void *)GetProcAddress( eos, name )
    GET( initialize, "EOS_Initialize" );
    GET( result_to_string, "EOS_EResult_ToString" );
    GET( get_version, "EOS_GetVersion" );
    GET( set_log_callback, "EOS_Logging_SetCallback" );
    GET( set_log_level, "EOS_Logging_SetLogLevel" );
    GET( platform_create, "EOS_Platform_Create" );
    GET( get_anticheat, "EOS_Platform_GetAntiCheatClientInterface" );
    GET( platform_tick, "EOS_Platform_Tick" );
    printf( "loaded %s, version %s\n", argv[1], get_version() );

    init.product_name = "eosprobe";
    init.product_version = "1.0";
    ret = initialize( &init );
    printf( "EOS_Initialize: %s\n", result_to_string( ret ) );
    printf( "EOS_Logging_SetCallback: %s\n", result_to_string( set_log_callback( log_callback ) ) );
    set_log_level( 0x7fffffff, 600 );

    if (argc > 2 && !strcmp( argv[2], "platform" ))
    {
        struct eos_platform_options_1 opts = { 1 };
        void *platform, *ac;

        opts.product_id = "00000000000000000000000000000000";
        opts.sandbox_id = "00000000000000000000000000000000";
        opts.client_credentials.api_version = 1;
        platform = platform_create( &opts );
        printf( "EOS_Platform_Create: %p\n", platform );
        if (platform)
        {
            ac = get_anticheat( platform );
            printf( "EOS_Platform_GetAntiCheatClientInterface: %p\n", ac );
            for (int i = 0; i < 20; i++) { platform_tick( platform ); Sleep( 100 ); }
        }
    }
    printf( "done\n" );
    return 0;
}
