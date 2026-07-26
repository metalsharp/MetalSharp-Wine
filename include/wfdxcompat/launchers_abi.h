#ifndef WFDXCOMPAT_LAUNCHERS_ABI_H
#define WFDXCOMPAT_LAUNCHERS_ABI_H

#define WFDX_LAUNCHERS_ABI_VERSION 1

#define WFDX_LAUNCHER_FLAG_INTERPOSE       0x00000001u
#define WFDX_LAUNCHER_FLAG_CONTEXT_STATE   0x00000002u
#define WFDX_LAUNCHER_FLAG_GDI_SURFACE     0x00000004u

struct wfdx_launcher_init
{
    UINT size;
    UINT abi_version;
    UINT flags;
    UINT reserved;
    void *create_device;
    void *create_device_and_swapchain;
};

struct wfdx_launcher_counters
{
    UINT size;
    UINT abi_version;
    UINT flags;
    UINT reserved;
    LONG init_calls;
    LONG wrapper_calls;
    LONG backend_calls;
    LONG backend_success;
    LONG overlay_attempts;
    LONG overlay_success;
    LONG overlay_failure;
};

#endif
