/* Vulkan through Wine (winevulkan -> MoltenVK): instance, physical devices, a logical device. */
#define _WIN32_WINNT 0x0A00
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

int main(void)
{
    HMODULE vk = LoadLibraryA( "vulkan-1.dll" );
    PFN_vkGetInstanceProcAddr gipa;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkinfo", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstance inst;
    VkPhysicalDevice dev[4];
    uint32_t n = 4, i, qf = 0;
    VkResult r;

    if (!vk) { printf( "vkinfo: vulkan-1.dll not found\n" ); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress( vk, "vkGetInstanceProcAddr" );
    info.pApplicationInfo = &app;
    r = ((PFN_vkCreateInstance)gipa( NULL, "vkCreateInstance" ))( &info, NULL, &inst );
    if (r) { printf( "vkinfo: vkCreateInstance %d\n", r ); return 1; }
    ((PFN_vkEnumeratePhysicalDevices)gipa( inst, "vkEnumeratePhysicalDevices" ))( inst, &n, dev );
    for (i = 0; i < n; i++)
    {
        VkPhysicalDeviceProperties p;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q };
        VkDevice device;
        ((PFN_vkGetPhysicalDeviceProperties)gipa( inst, "vkGetPhysicalDeviceProperties" ))( dev[i], &p );
        r = ((PFN_vkCreateDevice)gipa( inst, "vkCreateDevice" ))( dev[i], &dci, NULL, &device );
        printf( "vkinfo: %u-bit: GPU %u: %s, API %u.%u.%u, vkCreateDevice=%d\n", (unsigned)sizeof(void *) * 8, i,
                p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), r );
        if (!r) ((PFN_vkDestroyDevice)gipa( inst, "vkDestroyDevice" ))( device, NULL );
    }
    ((PFN_vkDestroyInstance)gipa( inst, "vkDestroyInstance" ))( inst, NULL );
    return n ? 0 : 1;
}
