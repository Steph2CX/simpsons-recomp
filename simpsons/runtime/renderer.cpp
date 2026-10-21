// Rendu Vulkan (MoltenVK) des commandes du GPU Xenos.
//
// Une seule cible de rendu (couleur RGBA8 + profondeur) recoit tous les dessins ; elle est copiee
// dans la fenetre a chaque swap. Les shaders sont traduits par XenosRecomp et suivent son contrat
// (shader_common.h) : push constants avec trois adresses de tampons (constantes vertex, pixel et
// partagees), tables de descripteurs "bindless" (2D, 3D, cube, samplers), constante de specialisation.
#include "renderer.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#elif defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#include <mach-o/dyld.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#else
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "shader_translator_version.h" // SHADER_TRANSLATOR_VERSION

int PlatformGpuCoreCount(); // mac_window.mm
bool RuntimeLowMemory(); // main.cpp

#define VK_CHECK(call)                                                                  \
    do                                                                                  \
    {                                                                                   \
        VkResult result_ = (call);                                                      \
        if (result_ != VK_SUCCESS)                                                      \
        {                                                                               \
            fprintf(stderr, "[rendu] %s a echoue (%d)\n", #call, int(result_));        \
            return false;                                                               \
        }                                                                               \
    } while (0)

namespace
{
    constexpr uint32_t TARGET_WIDTH = 1280;
    constexpr uint32_t TARGET_HEIGHT = 720;
    constexpr VkFormat COLOR_FORMAT = VK_FORMAT_R8G8B8A8_UNORM; // placeholder textures
    constexpr VkFormat TARGET_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT; // every EDRAM colour surface, and resolves
    constexpr uint32_t SURFACE_SIZE = 2048; // host EDRAM surfaces: SURFACE_SIZE x SURFACE_SIZE samples (x the resolution scale)

    // Internal resolution: every surface and copy is g_resolutionScale times larger than on the console (the game
    // still sees 1280x720). SIMPSONS_RESOLUTION=1 (720p), 2 (1440p), 3 (2160p = 4K), 4 (2880p = 5K). Default chosen in
    // CreateDevice by ChooseResolutionScale.
    uint32_t g_resolutionScale = 1;
    constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT_S8_UINT;
    // Halved where memory is short (Apple TV: about 2 GB for the application, graphics included)
    const VkDeviceSize UPLOAD_SIZE = (RuntimeLowMemory() ? 256ull : 512ull) * 1024 * 1024;
    const VkDeviceSize GEOMETRY_SIZE = (RuntimeLowMemory() ? 128ull : 256ull) * 1024 * 1024;
    constexpr uint32_t DESCRIPTOR_HEAP_SIZE = 4096;
    constexpr uint32_t SHARED_CONSTANTS_SIZE = 528; // 320 + 32 loop constants, vertex index base (448), 8 fetch buffers (456),
                                                    // pixel position scale (520)
    constexpr uint32_t SPEC_CONSTANT_ALPHA_TEST = 1u << 1;

    SDL_Window* g_window = nullptr;
    std::atomic<bool> g_running{ false };

    VkInstance g_instance = VK_NULL_HANDLE;
    VkSurfaceKHR g_surface = VK_NULL_HANDLE;
    VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
    VkDevice g_device = VK_NULL_HANDLE;
    VkQueue g_queue = VK_NULL_HANDLE;
    uint32_t g_queueFamily = 0;

    VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
    VkFormat g_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D g_swapchainExtent{ TARGET_WIDTH, TARGET_HEIGHT };
    std::atomic<bool> g_swapchainOutdated{ false }; // window resized: recreated before the next frame
    std::vector<VkImage> g_swapchainImages;

    VkCommandPool g_commandPool = VK_NULL_HANDLE;
    // Two frames in flight: the next frame is recorded while the Mac's GPU draws the previous one. Each slot has
    // its command buffer, fence, acquire semaphore, half of the upload ring and list of resources to destroy.
    constexpr uint32_t FRAMES_IN_FLIGHT = 2;
    VkCommandBuffer g_commandBuffers[FRAMES_IN_FLIGHT]{};
    VkFence g_frameFences[FRAMES_IN_FLIGHT]{};
    // SIMPSONS_PROFIL_GPU: GPU time of each frame (timestamps at the start and end of its command buffer)
    VkQueryPool g_timestamps = VK_NULL_HANDLE;
    bool g_timestampsWritten[FRAMES_IN_FLIGHT]{};
    double g_timestampPeriod = 1.0;
    double g_gpuNanoseconds = 0;
    // SIMPSONS_TRACE_PASSES=<frame>: GPU time between successive rendering passes of that frame
    VkQueryPool g_passTimestamps = VK_NULL_HANDLE;
    std::vector<std::string> g_passNames;
    bool TracingPasses();
    uint64_t g_gpuFrames = 0;
    VkSemaphore g_imageAvailableSemaphores[FRAMES_IN_FLIGHT]{};
    uint32_t g_frameSlot = 0;
    VkCommandBuffer g_commandBuffer = VK_NULL_HANDLE; // current slot
    VkSemaphore g_imageAvailable = VK_NULL_HANDLE;
    std::vector<VkSemaphore> g_renderFinished;
    VkFence g_frameFence = VK_NULL_HANDLE;

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    // EDRAM surfaces: one host image per (colour or depth, EDRAM base), addressed in samples so that
    // surfaces of different pitch or multisampling at the same base share their contents (a 4x MSAA
    // 640-pixel surface covers the same samples as a 1280-pixel one). Always in GENERAL layout.
    struct RenderTarget
    {
        Image image;
        uint32_t width = 0, height = 0;
        VkFormat format = TARGET_FORMAT;
    };
    std::unordered_map<uint64_t, RenderTarget> g_renderTargets;
    RenderTarget* g_color = nullptr;
    RenderTarget* g_depth = nullptr;
    uint32_t g_lastResolveAddress = UINT32_MAX; // presented at the next swap (front buffer copy)
    float g_sampleScale[2] = { 1.0f, 1.0f }; // samples per pixel horizontally and vertically (MSAA)
    // Part of the surfaces used by the current draws (host pixels): the window scissor of the game. The GPU of the
    // Mac loads and stores only this area at each rendering pass, not the whole SURFACE_SIZE surface.
    uint32_t g_renderWidth = 1, g_renderHeight = 1;

    bool PresentSource(VkImage* image, uint32_t* width, uint32_t* height, VkFormat* format); // resolved texture section

    // Host format of an EDRAM colour surface (RB_COLOR_INFO format). The exact precision matters: some passes
    // pack flags in the low bits of a 2:10:10:10 target and decode them later.
    VkFormat ColorTargetFormat(uint32_t guestFormat)
    {
        switch (guestFormat)
        {
        case 0: // k_8_8_8_8
        case 1: // k_8_8_8_8_GAMMA
            return VK_FORMAT_R8G8B8A8_UNORM;
        case 2: // k_2_10_10_10
        case 10: // k_2_10_10_10_AS_10_10_10_10
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 6: // k_16_16_FLOAT
            return VK_FORMAT_R16G16_SFLOAT;
        case 14: // k_32_FLOAT
            return VK_FORMAT_R32_SFLOAT;
        case 15: // k_32_32_FLOAT
            return VK_FORMAT_R32G32_SFLOAT;
        default: // 2_10_10_10_FLOAT (7e3), 16_16, 16_16_16_16 and their float versions
            return TARGET_FORMAT;
        }
    }

    uint32_t FormatBytes(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
            return 8;
        default:
            return 4;
        }
    }

    // One texel of a colour surface as floats (for captures)
    void ReadColor(VkFormat format, const uint8_t* texel, float* rgba)
    {
        rgba[3] = 1.0f;
        switch (format)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (int c = 0; c < 4; c++)
                rgba[c] = float(reinterpret_cast<const __fp16*>(texel)[c]);
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
            for (int c = 0; c < 4; c++)
                rgba[c] = texel[c] / 255.0f;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        {
            uint32_t v;
            memcpy(&v, texel, 4);
            for (int c = 0; c < 3; c++)
                rgba[c] = ((v >> (c * 10)) & 1023) / 1023.0f;
            rgba[3] = (v >> 30) / 3.0f;
            break;
        }
        case VK_FORMAT_R16G16_SFLOAT:
            rgba[0] = float(reinterpret_cast<const __fp16*>(texel)[0]);
            rgba[1] = float(reinterpret_cast<const __fp16*>(texel)[1]);
            rgba[2] = 0;
            break;
        default: // 32-bit floats
            rgba[0] = reinterpret_cast<const float*>(texel)[0];
            rgba[1] = format == VK_FORMAT_R32G32_SFLOAT ? reinterpret_cast<const float*>(texel)[1] : rgba[0];
            rgba[2] = format == VK_FORMAT_R32G32_SFLOAT ? 0 : rgba[0];
            break;
        }
    }

    // Colour pixels -> binary PPM
    void WritePpm(const char* name, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPixels,
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT)
    {
        FILE* f = fopen(name, "wb");
        if (f == nullptr)
            return;
        fprintf(f, "P6 %u %u 255\n", width, height);
        std::vector<uint8_t> row(size_t(width) * 3);
        for (uint32_t y = 0; y < height; y++)
        {
            uint32_t bytes = FormatBytes(format);
            const uint8_t* source = pixels + size_t(y) * rowPixels * bytes;
            static const bool alpha = getenv("SIMPSONS_CAPTURE_ALPHA") != nullptr; // alpha as grey levels
            for (uint32_t x = 0; x < width; x++)
            {
                float rgba[4];
                ReadColor(format, source + size_t(x) * bytes, rgba);
                for (uint32_t c = 0; c < 3; c++)
                    row[x * 3 + c] = uint8_t(std::clamp(alpha ? rgba[3] : rgba[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    Image g_dummy2D, g_dummy3D, g_dummyCube;
    VkSampler g_dummySampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout g_setLayouts[4]{};
    VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet g_descriptorSets[4]{};
    VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;

    VkBuffer g_uploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory g_uploadMemory = VK_NULL_HANDLE;
    uint8_t* g_uploadMapped = nullptr;
    VkDeviceAddress g_uploadAddress = 0;
    VkDeviceSize g_uploadOffset = 0;
    VkDeviceSize g_uploadLimit = UPLOAD_SIZE / 2; // end of the current slot's half of the upload ring
    uint64_t g_uploadEpoch = 0; // incremented each time the upload ring restarts (offsets of previous uploads invalid)

    // Geometry cache kept across frames: vertex and index arrays drawn again in a later frame with the same content
    // (scenery, characters) are copied once into this buffer instead of the upload ring at every frame. Arrays
    // rewritten every frame (particles, interface) are never seen twice and stay in the ring. When the buffer is
    // full, it is emptied at the start of the next frame, after the GPU finished reading it.
    VkBuffer g_geometryBuffer = VK_NULL_HANDLE;
    VkDeviceMemory g_geometryMemory = VK_NULL_HANDLE;
    uint8_t* g_geometryMapped = nullptr;
    VkDeviceSize g_geometryOffset = 0;
    bool g_geometryFull = false;
    struct GeometryEntry
    {
        VkDeviceSize offset = UINT64_MAX; // in g_geometryBuffer, UINT64_MAX while not kept
        uint64_t seenEpoch = 0;           // upload epoch of the last frame it was drawn in
    };
    std::unordered_map<uint64_t, GeometryEntry> g_geometry;

    struct ShaderEntry
    {
        XenosShader info;
        VkShaderModule module = VK_NULL_HANDLE;
    };
    std::unordered_map<uint64_t, ShaderEntry> g_shaders;
    std::unordered_map<uint64_t, VkPipeline> g_pipelines;

    // Guest textures (key chosen by textures.cpp) and samplers, with their descriptor heap indices
    struct TextureEntry
    {
        Image image;
        uint32_t width = 0, height = 0;
        RendererTextureFormat format = RendererTextureFormat::RGBA8;
        uint32_t levels = 1;
        bool cube = false;
        uint32_t descriptorIndex = 0;
    };
    std::unordered_map<uint64_t, TextureEntry> g_textures;
    std::vector<uint32_t> g_freeTextureIndices;
    uint32_t g_nextTextureIndex = 1; // 0 = placeholder
    std::vector<std::pair<Image, uint32_t>> g_retiredTextures; // destroyed once the frame using them is done
    std::vector<std::pair<VkImageView, uint32_t>> g_retiredViews;
    std::vector<std::pair<Image, uint32_t>> g_retiredTexturesBySlot[2];
    std::vector<std::pair<VkImageView, uint32_t>> g_retiredViewsBySlot[2];
    std::unordered_map<uint32_t, uint32_t> g_samplers; // packed state -> index
    int8_t g_formatSupport[32] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1 }; // -1 unknown, 0 no, 1 yes

    bool g_recording = false;
    bool g_rendering = false;
    double g_pipelineMs = 0; // time spent creating pipelines (SIMPSONS_PROFIL_GPU)
    bool g_targetsInitialized = false;
    uint64_t g_presentedFrames = 0;

    // SIMPSONS_CAPTURE_COPIES=<frame>: every resolve of that frame is saved (recomp/copie_<frame>_<n>_<address>.ppm)
    struct CopyCapture
    {
        VkDeviceSize offset;
        uint32_t address, width, height, rowPixels;
        bool depth = false; // float depth, written stretched between its min and max
        VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    };
    std::vector<CopyCapture> g_copyCaptures;
    uint64_t g_drawsThisFrame = 0;

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags)
    {
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; i++)
        {
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return 0;
    }

    bool CreateImage(Image& out, VkImageType type, VkImageViewType viewType, VkFormat format, VkExtent3D extent,
        uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImageCreateFlags flags = 0,
        VkComponentMapping components = {}, uint32_t levels = 1)
    {
        // Metal aborts the process (instead of failing) on a 2D image above 16384 or a 3D one above 2048
        uint32_t limit = type == VK_IMAGE_TYPE_3D ? 2048 : 16384;
        if (extent.width > limit || extent.height > limit || extent.depth > limit)
            return false;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.flags = flags;
        info.imageType = type;
        info.format = format;
        info.extent = extent;
        info.mipLevels = levels;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(g_device, &info, nullptr, &out.image));

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(g_device, out.image, &requirements);
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(g_device, &alloc, nullptr, &out.memory));
        vkBindImageMemory(g_device, out.image, out.memory, 0);

        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.image;
        view.viewType = viewType;
        view.format = format;
        view.components = components;
        view.subresourceRange = { aspect, 0, levels, 0, layers };
        VK_CHECK(vkCreateImageView(g_device, &view, nullptr, &out.view));
        return true;
    }

    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physicalDevice, g_surface, &capabilities));
        g_swapchainExtent = capabilities.currentExtent;
        if (g_swapchainExtent.width == UINT32_MAX)
            g_swapchainExtent = { TARGET_WIDTH, TARGET_HEIGHT };

        VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = g_surface;
        info.minImageCount = std::max(3u, capabilities.minImageCount);
        info.imageFormat = g_swapchainFormat;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = g_swapchainExtent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = g_swapchain;
        VkSwapchainKHR swapchain;
        VK_CHECK(vkCreateSwapchainKHR(g_device, &info, nullptr, &swapchain));
        if (g_swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = swapchain;

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(g_device, g_swapchain, &count, nullptr);
        g_swapchainImages.resize(count);
        vkGetSwapchainImagesKHR(g_device, g_swapchain, &count, g_swapchainImages.data());

        for (VkSemaphore semaphore : g_renderFinished)
            vkDestroySemaphore(g_device, semaphore, nullptr);
        g_renderFinished.resize(count);
        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& semaphore : g_renderFinished)
            VK_CHECK(vkCreateSemaphore(g_device, &semaphoreInfo, nullptr, &semaphore));
        return true;
    }

    // Mac: 5K on Max/Ultra chips (M2 Max x4, 80 Bites combat: 58.5 images/s, min 53), 4K on Pro chips, 1440p on base
    // chips (MacBook Air: fewer GPU cores, no fan, often 8 GB shared memory). PC with a dedicated card: 5K from 12 GB
    // of video memory, 4K from 8 GB, 1440p below (thresholds not yet measured on a real card). PC without one: 1440p on
    // Windows, 720p on Linux (ThinkPad L13, Iris Xe: 57 images/s in 80 Bites at x1, 29 at x2). Apple TV: 720p.
    uint32_t ChooseResolutionScale(const VkPhysicalDeviceProperties& properties)
    {
        const char* value = getenv("SIMPSONS_RESOLUTION");
#if defined(SIMPSONS_TVOS)
        // Apple TV 4K (A15, 5 GPU cores, ~2 GB for the application)
        (void)properties;
        int defaultScale = 1;
        char detail[64] = "Apple TV";
#elif defined(__APPLE__)
        (void)properties;
        int cores = PlatformGpuCoreCount(), defaultScale = cores >= 30 ? 4 : cores == 0 || cores >= 14 ? 3 : 2;
        char detail[64];
        snprintf(detail, sizeof(detail), "%d coeurs", cores);
#else
        VkDeviceSize videoMemory = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            VkPhysicalDeviceMemoryProperties memory;
            vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &memory);
            for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    videoMemory = std::max(videoMemory, memory.memoryHeaps[i].size);
        }
        uint32_t videoGigabytes = uint32_t((videoMemory + (512ull << 20)) >> 30); // 11.9 GB reported for a 12 GB card
#ifdef _WIN32
        int integratedScale = 2;
#else
        int integratedScale = 1;
#endif
        int defaultScale = videoMemory == 0 ? integratedScale : videoGigabytes >= 12 ? 4 : videoGigabytes >= 8 ? 3 : 2;
        char detail[64];
        if (videoMemory == 0)
            snprintf(detail, sizeof(detail), "puce integree");
        else
            snprintf(detail, sizeof(detail), "carte dediee, %u Go", videoGigabytes);
#endif
        int scale = std::clamp(value && *value ? atoi(value) : defaultScale, 1, 4);
        fprintf(stderr, "[rendu] puce graphique : %s, resolution x%d\n", detail, scale);
        return uint32_t(scale);
    }

    bool CreateDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
        if (count == 0)
            return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(g_instance, &count, devices.data());
        // A discrete GPU first (laptop with Intel + NVIDIA), if it can draw into the window
        g_physicalDevice = devices[0];
        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties candidate;
            vkGetPhysicalDeviceProperties(device, &candidate);
            if (candidate.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || candidate.apiVersion < VK_API_VERSION_1_3)
                continue;
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            bool usable = false;
            for (uint32_t i = 0; i < familyCount && !usable; i++)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_surface, &present);
                usable = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present;
            }
            if (usable)
            {
                g_physicalDevice = device;
                break;
            }
        }

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(g_physicalDevice, &properties);
        fprintf(stderr, "[rendu] GPU : %s (Vulkan %u.%u)\n", properties.deviceName,
            VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
        g_resolutionScale = ChooseResolutionScale(properties);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; i++)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g_physicalDevice, i, g_surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            {
                g_queueFamily = i;
                break;
            }
        }

        // Features required by the shaders generated by XenosRecomp
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.bufferDeviceAddress = VK_TRUE;
        features12.descriptorIndexing = VK_TRUE;
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        // Textures are added to the heaps while a frame using them is being recorded
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        features.features.shaderInt64 = VK_TRUE;
        features.features.independentBlend = VK_TRUE;
        features.pNext = &features12;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = g_queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        // VK_KHR_portability_subset only exists on MoltenVK; native drivers (Windows/Linux) refuse unknown extensions
        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> available(extensionCount);
        vkEnumerateDeviceExtensionProperties(g_physicalDevice, nullptr, &extensionCount, available.data());
        for (const VkExtensionProperties& extension : available)
            if (strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
                extensions.push_back("VK_KHR_portability_subset");
        VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.pNext = &features;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        VK_CHECK(vkCreateDevice(g_physicalDevice, &info, nullptr, &g_device));
        vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = g_queueFamily;
        VK_CHECK(vkCreateCommandPool(g_device, &poolInfo, nullptr, &g_commandPool));
        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = g_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = FRAMES_IN_FLIGHT;
        VK_CHECK(vkAllocateCommandBuffers(g_device, &allocInfo, g_commandBuffers));

        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            VK_CHECK(vkCreateSemaphore(g_device, &semaphoreInfo, nullptr, &g_imageAvailableSemaphores[i]));
            VK_CHECK(vkCreateFence(g_device, &fenceInfo, nullptr, &g_frameFences[i]));
        }
        g_commandBuffer = g_commandBuffers[0];
        g_frameFence = g_frameFences[0];
        g_imageAvailable = g_imageAvailableSemaphores[0];
        if (getenv("SIMPSONS_PROFIL_GPU"))
        {
            VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 2 * FRAMES_IN_FLIGHT;
            vkCreateQueryPool(g_device, &queryInfo, nullptr, &g_timestamps);
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(g_physicalDevice, &properties);
            g_timestampPeriod = properties.limits.timestampPeriod;
        }
        return true;
    }

    bool CreateResources()
    {
        // Render target (all draws go here) and placeholder textures
        if (!CreateImage(g_dummy2D, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, COLOR_FORMAT, { 1, 1, 1 }, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !CreateImage(g_dummy3D, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, COLOR_FORMAT, { 1, 1, 1 }, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !CreateImage(g_dummyCube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, COLOR_FORMAT, { 1, 1, 1 }, 6,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
            return false;

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_dummySampler));

        // Descriptor heaps: set 0 = 2D, 1 = 3D, 2 = cube, 3 = samplers (register spaces of shader_common.h)
        VkDescriptorType types[4] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER };
        for (uint32_t i = 0; i < 4; i++)
        {
            VkDescriptorSetLayoutBinding binding{ 0, types[i], DESCRIPTOR_HEAP_SIZE, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr };
            VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
            VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            flagsInfo.bindingCount = 1;
            flagsInfo.pBindingFlags = &bindingFlags;
            VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutInfo.pNext = &flagsInfo;
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings = &binding;
            VK_CHECK(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr, &g_setLayouts[i]));
        }

        VkDescriptorPoolSize poolSizes[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DESCRIPTOR_HEAP_SIZE * 3 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, DESCRIPTOR_HEAP_SIZE } };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_descriptorPool));
        VkDescriptorSetAllocateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        setInfo.descriptorPool = g_descriptorPool;
        setInfo.descriptorSetCount = 4;
        setInfo.pSetLayouts = g_setLayouts;
        VK_CHECK(vkAllocateDescriptorSets(g_device, &setInfo, g_descriptorSets));

        // Index 0 of every heap: placeholder texture / sampler
        VkDescriptorImageInfo images[4] = {
            { VK_NULL_HANDLE, g_dummy2D.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, g_dummy3D.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { VK_NULL_HANDLE, g_dummyCube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { g_dummySampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED } };
        VkWriteDescriptorSet writes[4];
        for (uint32_t i = 0; i < 4; i++)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet = g_descriptorSets[i];
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = types[i];
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(g_device, 4, writes, 0, nullptr);

        VkPushConstantRange pushRange{ VK_SHADER_STAGE_ALL_GRAPHICS, 0, 24 };
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 4;
        layoutInfo.pSetLayouts = g_setLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VK_CHECK(vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &g_pipelineLayout));

        // Upload ring for constants, vertices and indices (host visible, addressable by shaders)
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = UPLOAD_SIZE;
        bufferInfo.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_uploadBuffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(g_device, g_uploadBuffer, &requirements);
        VkMemoryAllocateFlagsInfo flagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext = &flagsInfo;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(g_device, &alloc, nullptr, &g_uploadMemory));
        vkBindBufferMemory(g_device, g_uploadBuffer, g_uploadMemory, 0);
        VK_CHECK(vkMapMemory(g_device, g_uploadMemory, 0, UPLOAD_SIZE, 0, reinterpret_cast<void**>(&g_uploadMapped)));
        VkBufferDeviceAddressInfo addressInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addressInfo.buffer = g_uploadBuffer;
        g_uploadAddress = vkGetBufferDeviceAddress(g_device, &addressInfo);

        // Geometry cache (vertex and index arrays only)
        VkBufferCreateInfo geometryInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        geometryInfo.size = GEOMETRY_SIZE;
        geometryInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VK_CHECK(vkCreateBuffer(g_device, &geometryInfo, nullptr, &g_geometryBuffer));
        vkGetBufferMemoryRequirements(g_device, g_geometryBuffer, &requirements);
        VkMemoryAllocateInfo geometryAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        geometryAlloc.allocationSize = requirements.size;
        geometryAlloc.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(g_device, &geometryAlloc, nullptr, &g_geometryMemory));
        vkBindBufferMemory(g_device, g_geometryBuffer, g_geometryMemory, 0);
        VK_CHECK(vkMapMemory(g_device, g_geometryMemory, 0, GEOMETRY_SIZE, 0, reinterpret_cast<void**>(&g_geometryMapped)));
        return true;
    }

    // Offset of the array <key> in the geometry cache, copied there when it was already drawn in a previous frame;
    // UINT64_MAX when it must go through the upload ring
    bool TracingPasses()
    {
        static const char* frame = getenv("SIMPSONS_TRACE_PASSES");
        return frame && g_presentedFrames + 1 == uint64_t(atoll(frame));
    }

    VkDeviceSize KeptGeometry(uint64_t key, const void* data, size_t size, size_t alignment)
    {
        // SIMPSONS_SANS_CACHE_GEOMETRIE=1 (tests): everything goes through the upload ring at every frame
        static const bool keepGeometry = getenv("SIMPSONS_SANS_CACHE_GEOMETRIE") == nullptr;
        if (key == 0 || !keepGeometry || g_geometryBuffer == VK_NULL_HANDLE)
            return UINT64_MAX;
        GeometryEntry& entry = g_geometry[key];
        if (entry.offset != UINT64_MAX)
            return entry.offset;
        bool seenBefore = entry.seenEpoch != 0 && entry.seenEpoch != g_uploadEpoch;
        entry.seenEpoch = g_uploadEpoch;
        if (!seenBefore)
            return UINT64_MAX;
        VkDeviceSize offset = (g_geometryOffset + alignment - 1) & ~VkDeviceSize(alignment - 1);
        if (offset + size > GEOMETRY_SIZE)
        {
            g_geometryFull = true;
            return UINT64_MAX;
        }
        memcpy(g_geometryMapped + offset, data, size);
        g_geometryOffset = offset + size;
        entry.offset = offset;
        return offset;
    }

    // Copies data into the upload ring; returns its offset (UINT64_MAX when full)
    // Bytes copied per kind (constants, shared, vertices, indices, textures, buffers), printed with SIMPSONS_PROFIL_GPU
    uint64_t g_uploadBytes[6];
    uint64_t g_renderPasses = 0, g_renderPassPixels = 0;
    uint64_t g_resolveImagesCreated = 0; // copy textures created (not taken from the pool)
    int g_passEndLine = 0; // source line that ended the previous rendering pass (SIMPSONS_TRACE_PASSES)
    void EndRenderingAt(int line); // rendering passes begun (and their areas), for SIMPSONS_PROFIL_GPU
    VkDeviceSize Upload(const void* data, size_t size, size_t alignment = 256, int kind = 0)
    {
        g_uploadBytes[kind] += size;
        VkDeviceSize offset = (g_uploadOffset + alignment - 1) & ~VkDeviceSize(alignment - 1);
        if (offset + size > g_uploadLimit)
            return UINT64_MAX;
        memcpy(g_uploadMapped + offset, data, size);
        g_uploadOffset = offset + size;
        return offset;
    }

    void ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess, uint32_t layers = 1)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.image = image;
        barrier.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers };
        vkCmdPipelineBarrier(g_commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void BeginFrameIfNeeded()
    {
        if (g_recording)
            return;
        // Geometry cache full: empty it once no frame in flight reads it any more
        if (g_geometryFull || g_geometry.size() > 200000)
        {
            if (g_geometryFull)
            {
                vkDeviceWaitIdle(g_device);
                fprintf(stderr, "[rendu] cache de geometrie plein (%llu Mo), vide\n", (unsigned long long)(g_geometryOffset >> 20));
                g_geometry.clear();
                g_geometryOffset = 0;
                g_geometryFull = false;
            }
            else // arrays seen only once: forgotten
                std::erase_if(g_geometry, [](const auto& item) { return item.second.offset == UINT64_MAX; });
        }
        // Next slot: wait for the frame recorded in it two frames ago (the other one may still be drawing)
        g_retiredTexturesBySlot[g_frameSlot] = std::move(g_retiredTextures);
        g_retiredViewsBySlot[g_frameSlot] = std::move(g_retiredViews);
        g_retiredTextures.clear();
        g_retiredViews.clear();
        g_frameSlot = (g_frameSlot + 1) % FRAMES_IN_FLIGHT;
        g_commandBuffer = g_commandBuffers[g_frameSlot];
        g_frameFence = g_frameFences[g_frameSlot];
        g_imageAvailable = g_imageAvailableSemaphores[g_frameSlot];
        vkWaitForFences(g_device, 1, &g_frameFence, VK_TRUE, UINT64_MAX);
        vkResetCommandBuffer(g_commandBuffer, 0);
        for (auto& [image, index] : g_retiredTexturesBySlot[g_frameSlot])
        {
            vkDestroyImageView(g_device, image.view, nullptr);
            vkDestroyImage(g_device, image.image, nullptr);
            vkFreeMemory(g_device, image.memory, nullptr);
            if (index != 0)
                g_freeTextureIndices.push_back(index);
        }
        g_retiredTexturesBySlot[g_frameSlot].clear();
        for (auto& [view, index] : g_retiredViewsBySlot[g_frameSlot])
        {
            vkDestroyImageView(g_device, view, nullptr);
            g_freeTextureIndices.push_back(index);
        }
        g_retiredViewsBySlot[g_frameSlot].clear();
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_commandBuffer, &beginInfo);
        g_recording = true;
        if (g_timestamps != VK_NULL_HANDLE)
        {
            uint64_t times[2];
            if (g_timestampsWritten[g_frameSlot] && vkGetQueryPoolResults(g_device, g_timestamps, g_frameSlot * 2, 2, sizeof(times), times,
                    sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && times[1] > times[0])
            {
                g_gpuNanoseconds += double(times[1] - times[0]) * g_timestampPeriod;
                g_gpuFrames++;
            }
            g_timestampsWritten[g_frameSlot] = false;
            vkCmdResetQueryPool(g_commandBuffer, g_timestamps, g_frameSlot * 2, 2);
            vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_timestamps, g_frameSlot * 2);
        }
        if (TracingPasses())
        {
            if (g_passTimestamps == VK_NULL_HANDLE)
            {
                VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                queryInfo.queryCount = 256;
                vkCreateQueryPool(g_device, &queryInfo, nullptr, &g_passTimestamps);
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(g_physicalDevice, &properties);
                g_timestampPeriod = properties.limits.timestampPeriod;
            }
            vkCmdResetQueryPool(g_commandBuffer, g_passTimestamps, 0, 256);
            vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_passTimestamps, 0);
            g_passNames.assign(1, "debut");
        }
        // Surfaces and textures are shared with the frame still in flight: order everything after it
        VkMemoryBarrier full{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        full.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        full.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &full,
            0, nullptr, 0, nullptr);
        g_uploadOffset = g_frameSlot * (UPLOAD_SIZE / 2);
        g_uploadLimit = g_uploadOffset + UPLOAD_SIZE / 2;
        g_uploadEpoch++;
        g_drawsThisFrame = 0;

        if (!g_targetsInitialized)
        {
            // Placeholder textures: opaque white
            VkImage dummies[3] = { g_dummy2D.image, g_dummy3D.image, g_dummyCube.image };
            uint32_t layers[3] = { 1, 1, 6 };
            VkClearColorValue white{ { 1, 1, 1, 1 } };
            for (uint32_t i = 0; i < 3; i++)
            {
                ImageBarrier(dummies[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, layers[i]);
                VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers[i] };
                vkCmdClearColorImage(g_commandBuffer, dummies[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
                ImageBarrier(dummies[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, layers[i]);
            }
            g_targetsInitialized = true;
        }
    }

    // Execution and memory dependency for surfaces kept in GENERAL layout (attachment <-> transfer)
    void SurfaceBarrier(VkImage image, VkImageAspectFlags aspect)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = image;
        barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Finds or creates an EDRAM surface (new ones start cleared, in GENERAL layout)
    RenderTarget* GetRenderTarget(bool depth, uint32_t base, VkFormat format = TARGET_FORMAT)
    {
        if (depth)
            format = DEPTH_FORMAT;
        uint64_t key = (uint64_t(depth) << 63) | (uint64_t(format) << 32) | base;
        RenderTarget& target = g_renderTargets[key];
        if (target.image.image != VK_NULL_HANDLE)
            return &target;
        VkImageAspectFlags aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        uint32_t size = SURFACE_SIZE * g_resolutionScale;
        if (!CreateImage(target.image, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format, { size, size, 1 }, 1, usage, aspect))
        {
            g_renderTargets.erase(key);
            return nullptr;
        }
        target.width = size;
        target.height = size;
        target.format = format;
        fprintf(stderr, "[rendu] surface %s base %u format %d\n", depth ? "profondeur" : "couleur", base, int(format));

        BeginFrameIfNeeded();
        ImageBarrier(target.image.image, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
        if (depth)
        {
            VkClearDepthStencilValue value{ 0.0f, 0 };
            vkCmdClearDepthStencilImage(g_commandBuffer, target.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        }
        else
        {
            VkClearColorValue value{};
            vkCmdClearColorImage(g_commandBuffer, target.image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        }
        SurfaceBarrier(target.image.image, aspect);
        return &target;
    }

    void BeginRenderingIfNeeded()
    {
        BeginFrameIfNeeded();
        if (g_rendering)
            return;
        if (g_color == nullptr)
            g_color = GetRenderTarget(false, 0);
        if (g_depth == nullptr)
            g_depth = GetRenderTarget(true, 720);
        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = g_color->image.view;
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView = g_depth->image.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        uint32_t width = std::min({ g_color->width, g_depth->width, g_renderWidth });
        uint32_t height = std::min({ g_color->height, g_depth->height, g_renderHeight });
        info.renderArea = { { 0, 0 }, { width, height } };
        g_renderPasses++;
        g_renderPassPixels += uint64_t(width) * height;
        if (TracingPasses() && g_passNames.size() < 255)
        {
            char name[160];
            snprintf(name, sizeof(name), "%ux%u couleur fmt %d (fin ligne %d)", width, height, int(g_color->format), g_passEndLine);
            vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_passTimestamps, uint32_t(g_passNames.size()));
            g_passNames.push_back(name);
        }
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        info.pStencilAttachment = &depth;
        vkCmdBeginRendering(g_commandBuffer, &info);
        g_rendering = true;

        VkViewport viewport{ 0, 0, float(g_color->width), float(g_color->height), 0, 1 };
        VkRect2D scissor{ { 0, 0 }, { width, height } };
        vkCmdSetViewport(g_commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(g_commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 4, g_descriptorSets, 0, nullptr);
    }

    VkBlendFactor BlendFactor(uint32_t xenos)
    {
        static const VkBlendFactor factors[] = {
            VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
            VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
            VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
            VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE };
        return xenos < std::size(factors) ? factors[xenos] : VK_BLEND_FACTOR_ONE;
    }

    VkBlendOp BlendOp(uint32_t xenos)
    {
        static const VkBlendOp ops[] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT };
        return xenos < std::size(ops) ? ops[xenos] : VK_BLEND_OP_ADD;
    }

    VkPipeline GetPipeline(const RendererDrawState& state, const ShaderEntry& vs, const ShaderEntry& ps)
    {
        uint32_t attributeCount = uint32_t(vs.info.attributes.size());
        uint32_t specConstants = state.alphaTest ? SPEC_CONSTANT_ALPHA_TEST : 0;
        uint64_t key[] = { state.vertexShader, state.pixelShader, uint64_t(state.topology) | (uint64_t(state.primitiveRestart) << 8) |
            (uint64_t(state.colorMask) << 16) | (uint64_t(specConstants) << 24), uint64_t(state.blendControl) | (uint64_t(state.depthControl) << 32),
            uint64_t(state.modeControl) | (uint64_t(g_color ? g_color->format : TARGET_FORMAT) << 32) };
        uint64_t hash = XXH3_64bits(key, sizeof(key));
        auto it = g_pipelines.find(hash);
        if (it != g_pipelines.end())
            return it->second;

        VkSpecializationMapEntry specEntry{ 0, 0, 4 };
        VkSpecializationInfo specInfo{ 1, &specEntry, 4, &specConstants };
        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs.module;
        stages[0].pName = "main";
        stages[0].pSpecializationInfo = &specInfo;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps.module;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = &specInfo;

        VkVertexInputBindingDescription binding{ 0, attributeCount * 16, VK_VERTEX_INPUT_RATE_VERTEX };
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (uint32_t i = 0; i < attributeCount; i++)
            attributes.push_back({ vs.info.attributes[i].location, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16 });
        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = attributeCount ? 1 : 0;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = attributeCount;
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        static const VkPrimitiveTopology topologies[] = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP };
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = topologies[uint32_t(state.topology)];
        inputAssembly.primitiveRestartEnable = state.primitiveRestart;

        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // PA_SU_SC_MODE_CNTL: bit 0 cull front, bit 1 cull back, bit 2 front face is clockwise
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = ((state.modeControl & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((state.modeControl & 2) ? VK_CULL_MODE_BACK_BIT : 0);
        static const int cullTest = getenv("SIMPSONS_CULL_TEST") ? atoi(getenv("SIMPSONS_CULL_TEST")) : 0; // 1 swap, 2 none
        bool clockwise = (state.modeControl & 4) != 0;
        raster.frontFace = (clockwise != (cullTest == 1)) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        static const char* noCull = getenv("SIMPSONS_NOCULL_PS"); // hashes of pixel shaders drawn without culling
        char psName[20];
        snprintf(psName, sizeof(psName), "%016llX", (unsigned long long)state.pixelShader);
        if (cullTest == 2 || (noCull && strstr(noCull, psName)))
            raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // RB_DEPTHCONTROL: bit 1 depth test, bit 2 depth write, bits 4-6 compare function (same order as Vulkan)
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = (state.depthControl >> 1) & 1;
        depth.depthWriteEnable = (state.depthControl >> 2) & 1;
        depth.depthCompareOp = VkCompareOp((state.depthControl >> 4) & 7);

        // RB_BLENDCONTROL: colour src/op/dst in bits 0-12, alpha src/op/dst in bits 16-28
        uint32_t b = state.blendControl;
        VkPipelineColorBlendAttachmentState blend{};
        blend.srcColorBlendFactor = BlendFactor(b & 0x1F);
        blend.colorBlendOp = BlendOp((b >> 5) & 7);
        blend.dstColorBlendFactor = BlendFactor((b >> 8) & 0x1F);
        blend.srcAlphaBlendFactor = BlendFactor((b >> 16) & 0x1F);
        blend.alphaBlendOp = BlendOp((b >> 21) & 7);
        blend.dstAlphaBlendFactor = BlendFactor((b >> 24) & 0x1F);
        blend.blendEnable = !(blend.srcColorBlendFactor == VK_BLEND_FACTOR_ONE && blend.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
            blend.colorBlendOp == VK_BLEND_OP_ADD && blend.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
            blend.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && blend.alphaBlendOp == VK_BLEND_OP_ADD);
        blend.colorWriteMask = (ps.info.pixelOutputs & 1) ? (state.colorMask & 0xF) : 0;
        VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
        VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamic.dynamicStateCount = 3;
        dynamic.pDynamicStates = dynamicStates;

        VkFormat colorFormat = g_color ? g_color->format : TARGET_FORMAT;
        VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = DEPTH_FORMAT;
        rendering.stencilAttachmentFormat = DEPTH_FORMAT;

        VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = g_pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        auto start = std::chrono::steady_clock::now();
        if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
            fprintf(stderr, "[rendu] echec de creation de pipeline (VS %016llX, PS %016llX)\n",
                (unsigned long long)state.vertexShader, (unsigned long long)state.pixelShader);
        g_pipelineMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        g_pipelines[hash] = pipeline;
        return pipeline;
    }
}

void InputHandleEvent(const SDL_Event& event);
void InputPoll();
bool InputQuitRequested();

bool RendererInit()
{
    // macOS full screen (its own Space): the menu bar and the title bar with the green button appear when the mouse
    // reaches the top of the screen
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
#ifdef SIMPSONS_TVOS
    // The Siri Remote is not a gamepad for the game (it would take player 1 from the real controller). Back to the
    // Apple TV home screen: hold B (InputQuitRequested)
    SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "[rendu] SDL_Init : %s\n", SDL_GetError());
        return false;
    }
    // Full screen by default (SIMPSONS_FENETRE=1: window), real Retina resolution; Cmd+F toggles
    // Metal window: the Vulkan surface is created by MoltenVK from the window's Metal layer (SDL does not load a
    // Vulkan library of its own, which a Mac without the Vulkan SDK does not have)
#ifdef _WIN32
    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#elif !defined(__APPLE__)
    // Linux: SDL creates the Vulkan surface (X11 or Wayland)
    SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#elif defined(SIMPSONS_TVOS)
    // tvOS: always the whole screen
    SDL_WindowFlags windowFlags = SDL_WINDOW_METAL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const bool coverScreen = false;
#else
    SDL_WindowFlags windowFlags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    // Shown only once it covers the screen (full screen follows), so the desktop never shows around it
    const bool coverScreen = getenv("SIMPSONS_FENETRE") == nullptr;
    if (coverScreen)
        windowFlags |= SDL_WINDOW_HIDDEN;
#endif
    g_window = SDL_CreateWindow("Les Simpson : le jeu", TARGET_WIDTH, TARGET_HEIGHT, windowFlags);

    if (g_window == nullptr)
    {
        fprintf(stderr, "[rendu] SDL_CreateWindow : %s\n", SDL_GetError());
        return false;
    }
#ifdef __APPLE__
    if (coverScreen)
    {
        void PlatformCoverScreen(SDL_Window* window);
        PlatformCoverScreen(g_window);
        SDL_ShowWindow(g_window);
    }
#endif

    // Only the extensions present: MoltenVK linked directly (self-contained build) has no Vulkan loader, hence
    // no VK_KHR_portability_enumeration
    uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
    auto hasExtension = [&](const char* name) {
        for (auto& extension : available)
            if (strcmp(extension.extensionName, name) == 0)
                return true;
        return false;
    };
    std::vector<const char*> extensions;
#ifdef _WIN32
    for (const char* name : { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif !defined(__APPLE__)
    // Linux: the surface extensions SDL needs for its window system (X11 or Wayland)
    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    for (Uint32 i = 0; i < sdlExtensionCount; i++)
        if (hasExtension(sdlExtensions[i]) && strcmp(sdlExtensions[i], VK_KHR_SURFACE_EXTENSION_NAME) != 0)
            extensions.push_back(sdlExtensions[i]);
    for (const char* name : { VK_KHR_SURFACE_EXTENSION_NAME,
#else
    for (const char* name : { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME,
#endif
             VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME })
        if (hasExtension(name))
            extensions.push_back(name);

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "SimpsonsRecomp";
    appInfo.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.flags = hasExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = uint32_t(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &g_instance));

#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (surfaceInfo.hwnd == nullptr || vkCreateWin32SurfaceKHR(g_instance, &surfaceInfo, nullptr, &g_surface) != VK_SUCCESS)
    {
        fprintf(stderr, "[rendu] creation de la surface Windows impossible : %s\n", SDL_GetError());
        return false;
    }
#elif !defined(__APPLE__)
    if (!SDL_Vulkan_CreateSurface(g_window, g_instance, nullptr, &g_surface))
    {
        fprintf(stderr, "[rendu] creation de la surface Vulkan impossible : %s\n", SDL_GetError());
        return false;
    }
#else
    SDL_MetalView metalView = SDL_Metal_CreateView(g_window);
    auto createMetalSurface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(vkGetInstanceProcAddr(g_instance, "vkCreateMetalSurfaceEXT"));
    VkMetalSurfaceCreateInfoEXT surfaceInfo{ VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
    surfaceInfo.pLayer = metalView ? static_cast<const CAMetalLayer*>(SDL_Metal_GetLayer(metalView)) : nullptr;
    if (surfaceInfo.pLayer == nullptr || createMetalSurface == nullptr ||
        createMetalSurface(g_instance, &surfaceInfo, nullptr, &g_surface) != VK_SUCCESS)
    {
        fprintf(stderr, "[rendu] creation de la surface Metal impossible : %s\n", SDL_GetError());
        return false;
    }
#endif
    if (!CreateDevice() || !CreateSwapchain() || !CreateResources())
        return false;

    g_running = true;
    fprintf(stderr, "[rendu] fenetre %ux%u prete\n", g_swapchainExtent.width, g_swapchainExtent.height);
    return true;
}

bool PlatformIsFullscreen(SDL_Window* window);
void PlatformToggleFullscreen(SDL_Window* window);
int PlatformMenuCount();
uint64_t GpuFrameCount(); // gpu.cpp

// In a window, the title bar shows the images per second, measured over the last half second
static void UpdateWindowTitle()
{
    static Uint64 lastTicks = SDL_GetTicks();
    static uint64_t lastFrame = GpuFrameCount();
    static std::string lastTitle;
    Uint64 ticks = SDL_GetTicks();
    if (ticks - lastTicks < 500)
        return;
    uint64_t frame = GpuFrameCount();
    std::string title = "Les Simpson : le jeu";
    if (!PlatformIsFullscreen(g_window))
    {
        char fps[32];
        snprintf(fps, sizeof(fps), " - %.0f images/s", (frame - lastFrame) * 1000.0 / (ticks - lastTicks));
        title += fps;
    }
    lastTicks = ticks;
    lastFrame = frame;
    if (title != lastTitle)
    {
        SDL_SetWindowTitle(g_window, title.c_str());
        lastTitle = title;
    }
}

void RendererShowStartupError()
{
    // Without this box the game kept running with no window at all (Vulkan missing or driver too old)
    if (g_window != nullptr)
        SDL_HideWindow(g_window);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Les Simpson - Le Jeu",
        "Le jeu n'a pas pu démarrer l'affichage (Vulkan).\n\n"
        "Il faut une carte graphique compatible Vulkan et un pilote graphique à jour : "
        "installez la dernière version du pilote (Intel, AMD ou NVIDIA, ou Windows Update) puis relancez le jeu.\n\n"
        "Le détail de l'erreur se trouve dans le fichier jeu.log.",
        nullptr);
}

void RendererRunEventLoop()
{
    // Native macOS full screen, requested once the window is on screen (SIMPSONS_FENETRE=1: stay in a window)
    bool fullscreenRequested = getenv("SIMPSONS_FENETRE") != nullptr;
    Uint64 started = SDL_GetTicks();
    // The mouse pointer disappears after 2 s without moving, and comes back as soon as the mouse moves
    Uint64 lastMouseMove = SDL_GetTicks();
    bool cursorShown = true;
    while (g_running)
    {
        if (!fullscreenRequested && SDL_GetTicks() - started > 300)
        {
            fullscreenRequested = true;
            if (!PlatformIsFullscreen(g_window))
                PlatformToggleFullscreen(g_window);
        }
        static bool reported = false;
        if (!reported && SDL_GetTicks() - started > 4000)
        {
            reported = true;
            fprintf(stderr, "[fenetre] plein ecran macOS : %s, menus : %d\n", PlatformIsFullscreen(g_window) ? "oui" : "non", PlatformMenuCount());
        }
        if (cursorShown && SDL_GetTicks() - lastMouseMove > 2000)
        {
            SDL_HideCursor();
            cursorShown = false;
        }
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 8))
        {
            if (event.type == SDL_EVENT_MOUSE_MOTION)
            {
                lastMouseMove = SDL_GetTicks();
                if (!cursorShown)
                {
                    SDL_ShowCursor();
                    cursorShown = true;
                }
            }
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                g_running = false;
            // Cmd+F, Alt+Enter (Windows) or F11: full screen <-> window
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                ((event.key.key == SDLK_F && (event.key.mod & SDL_KMOD_GUI)) || event.key.key == SDLK_F11 ||
                    (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT))))
                PlatformToggleFullscreen(g_window);
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                g_swapchainOutdated = true;
            InputHandleEvent(event);
            InputPoll();
        }
        InputPoll();
        if (InputQuitRequested())
            g_running = false;
        UpdateWindowTitle();
    }
}

bool RendererIsRunning()
{
    return g_running;
}

// Shader cache on disk (cache-shaders/ in the working directory): translating a shader takes 2-40 ms, which made the
// game skip frames the first time each shader appeared, at every start. One subfolder per program build (its size
// and date), older ones are deleted. SIMPSONS_SANS_CACHE_SHADERS=1 disables (as do the shader debugging options).
static const std::filesystem::path& ShaderCacheFolder()
{
    static const std::filesystem::path folder = []
    {
        if (getenv("SIMPSONS_SANS_CACHE_SHADERS") || getenv("SIMPSONS_DUMP_SHADERS") || getenv("SIMPSONS_DEBUG_PS"))
            return std::filesystem::path();
        // One folder per version of the translator (hash of its sources and of DXC, empreinte_traducteur.cmake):
        // translated shaders survive rebuilds of the rest of the program
        const char* build = SHADER_TRANSLATOR_VERSION;
        std::error_code error;
        std::filesystem::path root = "cache-shaders";
        if (std::filesystem::exists(root, error))
            for (const auto& old : std::filesystem::directory_iterator(root, error))
                if (old.path().filename() != build)
                    std::filesystem::remove_all(old.path(), error);
        std::filesystem::create_directories(root / build, error);
        return error ? std::filesystem::path() : root / build;
    }();
    return folder;
}

static std::filesystem::path ShaderCacheFile(uint64_t hash)
{
    char name[24];
    snprintf(name, sizeof(name), "%016llX.bin", (unsigned long long)hash);
    return ShaderCacheFolder() / name;
}

static bool LoadCachedShader(uint64_t hash, XenosShader& shader)
{
    if (ShaderCacheFolder().empty())
        return false;
    FILE* f = fopen(ShaderCacheFile(hash).string().c_str(), "rb");
    if (f == nullptr)
        return false;
    uint32_t header[4]; // magic, attribute count, SPIR-V words, pixel outputs
    bool ok = fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x31435853 && header[1] < 64 && header[2] < (16u << 20);
    if (ok)
    {
        shader.pixelOutputs = header[3];
        shader.attributes.resize(header[1]);
        shader.spirv.resize(header[2]);
        ok = fread(&shader.textureSlots, 4, 1, f) == 1 && fread(shader.textureDimensions, 32, 1, f) == 1 &&
            fread(shader.textureFilters, 32, 1, f) == 1 &&
            (header[1] == 0 || fread(shader.attributes.data(), sizeof(XenosVertexAttribute), header[1], f) == header[1]) &&
            fread(shader.spirv.data(), 4, header[2], f) == header[2];
    }
    fclose(f);
    shader.valid = ok;
    if (!ok)
        shader = XenosShader();
    return ok;
}

static void SaveCachedShader(uint64_t hash, const XenosShader& shader)
{
    if (ShaderCacheFolder().empty() || !shader.valid)
        return;
    // Written under another name then renamed: a game stopped while writing leaves no truncated file
    std::filesystem::path file = ShaderCacheFile(hash), temporary = file;
    temporary += ".tmp";
    FILE* f = fopen(temporary.string().c_str(), "wb");
    if (f == nullptr)
        return;
    uint32_t header[4] = { 0x31435853, uint32_t(shader.attributes.size()), uint32_t(shader.spirv.size()), shader.pixelOutputs };
    bool ok = fwrite(header, sizeof(header), 1, f) == 1 && fwrite(&shader.textureSlots, 4, 1, f) == 1 &&
        fwrite(shader.textureDimensions, 32, 1, f) == 1 && fwrite(shader.textureFilters, 32, 1, f) == 1 &&
        fwrite(shader.attributes.data(), sizeof(XenosVertexAttribute), shader.attributes.size(), f) == shader.attributes.size() &&
        fwrite(shader.spirv.data(), 4, shader.spirv.size(), f) == shader.spirv.size();
    ok = fclose(f) == 0 && ok;
    std::error_code error;
    if (ok)
        std::filesystem::rename(temporary, file, error);
    else
        std::filesystem::remove(temporary, error);
}

uint64_t RendererSetShader(bool pixelShader, const uint8_t* microcode, uint32_t sizeBytes)
{
    uint64_t hash = XXH3_64bits(microcode, sizeBytes) ^ (pixelShader ? 1 : 0);
    auto it = g_shaders.find(hash);
    if (it == g_shaders.end())
    {
        ShaderEntry entry;
        auto start = std::chrono::steady_clock::now();
        bool cached = LoadCachedShader(hash, entry.info);
        if (!cached)
        {
            entry.info = TranslateXenosShader(microcode, sizeBytes, pixelShader);
            SaveCachedShader(hash, entry.info);
        }
        if (entry.info.valid && g_device != VK_NULL_HANDLE)
        {
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = entry.info.spirv.size() * 4;
            info.pCode = entry.info.spirv.data();
            if (vkCreateShaderModule(g_device, &info, nullptr, &entry.module) != VK_SUCCESS)
                entry.info.valid = false;
        }
        // SIMPSONS_DUMP_SHADERS=1: shaders that failed to translate, =all: every shader
        const char* dump = getenv("SIMPSONS_DUMP_SHADERS");
        if (dump && (!entry.info.valid || strcmp(dump, "all") == 0))
        {
            char name[96];
            snprintf(name, sizeof(name), "recomp/shader_%016llX_%s.hlsl", (unsigned long long)hash, pixelShader ? "ps" : "vs");
            if (FILE* f = fopen(name, "w"))
            {
                fputs(entry.info.hlsl.c_str(), f);
                fclose(f);
            }
            snprintf(name, sizeof(name), "recomp/shader_%016llX_%s.bin", (unsigned long long)hash, pixelShader ? "ps" : "vs");
            if (FILE* f = fopen(name, "wb"))
            {
                fwrite(microcode, 1, sizeBytes, f);
                fclose(f);
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[rendu] shader %s %016llX : %s (%zu attributs, %.1f ms)\n", pixelShader ? "pixel" : "vertex",
            (unsigned long long)hash, cached ? "lu dans le cache" : entry.info.valid ? "traduit" : entry.info.error.c_str(),
            entry.info.attributes.size(), ms);
        it = g_shaders.emplace(hash, std::move(entry)).first;
    }
    return it->second.info.valid ? hash : 0;
}

const XenosShader* RendererGetShader(uint64_t hash)
{
    auto it = g_shaders.find(hash);
    return it != g_shaders.end() && it->second.info.valid ? &it->second.info : nullptr;
}

void RendererDraw(const RendererDrawState& state, const float* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
{
    if (!g_running || state.vertexShader == 0 || state.pixelShader == 0 || indexCount == 0)
        return;
    auto& vs = g_shaders[state.vertexShader];
    auto& ps = g_shaders[state.pixelShader];
    if (!vs.info.valid || !ps.info.valid)
        return;

    BeginRenderingIfNeeded();
    VkPipeline pipeline = GetPipeline(state, vs, ps);
    if (pipeline == VK_NULL_HANDLE)
        return;

    // Shared constants: 2D texture descriptor indices (3D and cube: placeholder 0), sampler indices,
    // booleans, alpha threshold, host viewport transform at 288/304, loop constants at 320
    uint8_t shared[SHARED_CONSTANTS_SIZE]{};
    memcpy(shared, state.textureIndices, sizeof(state.textureIndices));
    memcpy(shared + 192, state.samplerIndices, sizeof(state.samplerIndices));
    memcpy(shared + 320, state.loopConstants, sizeof(state.loopConstants));
    memcpy(shared + 448, &state.vertexIndexBase, 4);
    memcpy(shared + 456, state.fetchBuffers, sizeof(state.fetchBuffers));
    float pixelScale = 1.0f / g_resolutionScale; // host pixel position -> console pixel position (pixel shaders)
    memcpy(shared + 520, &pixelScale, 4);
    memcpy(shared + 256, &state.booleans, 4);
    memcpy(shared + 272, &state.alphaReference, 4);
    // Window coordinates (pixels) -> clip space of the bound surface; Y negated because the vertex
    // shaders are compiled with -fvk-invert-y, which flips it once more
    float sx = g_sampleScale[0] * g_resolutionScale * 2.0f / g_color->width, sy = g_sampleScale[1] * g_resolutionScale * 2.0f / g_color->height;
    float scale[4] = { state.positionScale[0] * sx, -state.positionScale[1] * sy, state.positionScale[2], 0.0f };
    float offset[4] = { state.positionOffset[0] * sx - 1.0f, -(state.positionOffset[1] * sy - 1.0f), state.positionOffset[2], 0.0f };
    memcpy(shared + 288, scale, 16);
    memcpy(shared + 304, offset, 16);

    // Constant blocks are uploaded again only when they changed since the previous draw of the frame
    static float lastConstants[2][256 * 4];
    static VkDeviceSize lastOffsets[2] = { UINT64_MAX, UINT64_MAX };
    static uint64_t lastEpoch[2] = { UINT64_MAX, UINT64_MAX };
    auto uploadConstants = [&](int which, const float* constants) {
        if (lastEpoch[which] == g_uploadEpoch && memcmp(lastConstants[which], constants, sizeof(lastConstants[which])) == 0)
            return lastOffsets[which];
        VkDeviceSize offset = Upload(constants, 256 * 16);
        memcpy(lastConstants[which], constants, sizeof(lastConstants[which]));
        lastOffsets[which] = offset;
        lastEpoch[which] = g_uploadEpoch;
        return offset;
    };
    VkDeviceSize vsConstants = uploadConstants(0, state.vertexConstants);
    VkDeviceSize psConstants = uploadConstants(1, state.pixelConstants);
    VkDeviceSize sharedConstants = Upload(shared, sizeof(shared), 256, 1);
    uint32_t attributeCount = uint32_t(vs.info.attributes.size());
    // Vertex arrays drawn several times in a frame (depth, shadows, colour) are uploaded once
    static std::unordered_map<uint64_t, VkDeviceSize> uploadedVertices;
    static uint64_t uploadedEpoch = UINT64_MAX;
    if (uploadedEpoch != g_uploadEpoch)
    {
        uploadedVertices.clear();
        uploadedEpoch = g_uploadEpoch;
    }
    VkDeviceSize vertexOffset = 0;
    VkBuffer vertexBuffer = g_uploadBuffer, indexBuffer = g_uploadBuffer;
    size_t vertexBytes = size_t(vertexCount) * attributeCount * 16;
    if (attributeCount && (vertexOffset = KeptGeometry(state.vertexDataKey, vertices, vertexBytes, 16)) != UINT64_MAX)
        vertexBuffer = g_geometryBuffer;
    else if (attributeCount)
    {
        vertexOffset = 0;
        auto found = state.vertexDataKey ? uploadedVertices.find(state.vertexDataKey) : uploadedVertices.end();
        if (found != uploadedVertices.end())
            vertexOffset = found->second;
        else
        {
            vertexOffset = Upload(vertices, vertexBytes, 16, 2);
            if (state.vertexDataKey && vertexOffset != UINT64_MAX)
                uploadedVertices[state.vertexDataKey] = vertexOffset;
        }
    }
    static std::unordered_map<uint64_t, VkDeviceSize> uploadedIndices;
    static uint64_t uploadedIndicesEpoch = UINT64_MAX;
    if (uploadedIndicesEpoch != g_uploadEpoch)
    {
        uploadedIndices.clear();
        uploadedIndicesEpoch = g_uploadEpoch;
    }
    VkDeviceSize indexOffset = KeptGeometry(state.indexDataKey, indices, size_t(indexCount) * 4, 4);
    auto foundIndices = state.indexDataKey ? uploadedIndices.find(state.indexDataKey) : uploadedIndices.end();
    if (indexOffset != UINT64_MAX)
        indexBuffer = g_geometryBuffer;
    else if (foundIndices != uploadedIndices.end())
        indexOffset = foundIndices->second;
    else
    {
        indexOffset = Upload(indices, size_t(indexCount) * 4, 4, 3);
        if (state.indexDataKey && indexOffset != UINT64_MAX)
            uploadedIndices[state.indexDataKey] = indexOffset;
    }
    if (vsConstants == UINT64_MAX || psConstants == UINT64_MAX || sharedConstants == UINT64_MAX ||
        vertexOffset == UINT64_MAX || indexOffset == UINT64_MAX)
        return;

    uint64_t push[3] = { g_uploadAddress + vsConstants, g_uploadAddress + psConstants, g_uploadAddress + sharedConstants };
    vkCmdBindPipeline(g_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(g_commandBuffer, g_pipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(push), push);
    if (attributeCount)
        vkCmdBindVertexBuffers(g_commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(g_commandBuffer, indexBuffer, indexOffset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g_commandBuffer, indexCount, 1, 0, 0, 0);
    g_drawsThisFrame++;
}

void RendererClear(bool clearColor, const float color[4], bool clearDepth, float depth, uint8_t stencil,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!g_running || (!clearColor && !clearDepth))
        return;
    // The clear may reach beyond the current scissor: the rendering pass must cover it
    uint32_t neededWidth = uint32_t((x + width) * g_sampleScale[0]) * g_resolutionScale;
    uint32_t neededHeight = uint32_t((y + height) * g_sampleScale[1]) * g_resolutionScale;
    if (neededWidth > g_renderWidth || neededHeight > g_renderHeight)
    {
        EndRenderingAt(__LINE__);
        g_renderWidth = std::max(g_renderWidth, neededWidth);
        g_renderHeight = std::max(g_renderHeight, neededHeight);
    }
    BeginRenderingIfNeeded();
    VkClearAttachment attachments[2];
    uint32_t count = 0;
    if (clearColor)
    {
        attachments[count] = { VK_IMAGE_ASPECT_COLOR_BIT, 0 };
        memcpy(attachments[count].clearValue.color.float32, color, 16);
        count++;
    }
    if (clearDepth)
    {
        attachments[count] = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0 };
        attachments[count].clearValue.depthStencil = { depth, stencil };
        count++;
    }
    // In samples (the surfaces are addressed in samples, see RendererSetTargets)
    uint32_t limitX = std::min(g_color->width, g_depth->width), limitY = std::min(g_color->height, g_depth->height);
    float scaleX = g_sampleScale[0] * g_resolutionScale, scaleY = g_sampleScale[1] * g_resolutionScale;
    uint32_t sx = std::min(uint32_t(x * scaleX), limitX), sy = std::min(uint32_t(y * scaleY), limitY);
    uint32_t w = std::min(uint32_t(width * scaleX), limitX - sx);
    uint32_t h = std::min(uint32_t(height * scaleY), limitY - sy);
    if (w == 0 || h == 0)
        return;
    VkClearRect rect{ { { int32_t(sx), int32_t(sy) }, { w, h } }, 0, 1 };
    vkCmdClearAttachments(g_commandBuffer, count, attachments, 1, &rect);
}

void RendererPresent(uint64_t frame)
{
    if (!g_running)
        return;
    BeginFrameIfNeeded();
    if (g_rendering)
    {
        vkCmdEndRendering(g_commandBuffer);
        g_rendering = false;
    }
    if (g_swapchainOutdated.exchange(false))
    {
        // Window resized or full screen toggled: new swapchain at the new pixel size (this frame is dropped)
        vkEndCommandBuffer(g_commandBuffer);
        g_recording = false;
        vkDeviceWaitIdle(g_device);
        CreateSwapchain();
        return;
    }

    uint32_t imageIndex;
    VkResult acquired = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vkEndCommandBuffer(g_commandBuffer);
        g_recording = false;
        vkDeviceWaitIdle(g_device);
        CreateSwapchain();
        return;
    }
    vkResetFences(g_device, 1, &g_frameFence);

    // Front buffer (last colour copy of the frame, or the current surface) -> window (scaled)
    VkImage image = g_swapchainImages[imageIndex];
    VkImage source = VK_NULL_HANDLE;
    uint32_t sourceWidth = 0, sourceHeight = 0;
    VkFormat sourceFormat = TARGET_FORMAT;
    bool resolved = PresentSource(&source, &sourceWidth, &sourceHeight, &sourceFormat);
    if (!resolved)
    {
        if (g_color == nullptr)
            g_color = GetRenderTarget(false, 0);
        source = g_color->image.image;
        sourceFormat = g_color->format;
        sourceWidth = std::min(g_color->width, TARGET_WIDTH * g_resolutionScale);
        sourceHeight = std::min(g_color->height, TARGET_HEIGHT * g_resolutionScale);
        SurfaceBarrier(source, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    else
        ImageBarrier(source, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { int32_t(sourceWidth), int32_t(sourceHeight), 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    // Keep the 16:9 picture: black bars on the sides or top and bottom
    VkClearColorValue black{};
    VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(g_commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &whole);
    uint32_t outWidth = g_swapchainExtent.width, outHeight = g_swapchainExtent.height;
    if (uint64_t(outWidth) * sourceHeight > uint64_t(outHeight) * sourceWidth)
        outWidth = uint32_t(uint64_t(outHeight) * sourceWidth / std::max(sourceHeight, 1u));
    else
        outHeight = uint32_t(uint64_t(outWidth) * sourceHeight / std::max(sourceWidth, 1u));
    int32_t left = int32_t(g_swapchainExtent.width - outWidth) / 2, top = int32_t(g_swapchainExtent.height - outHeight) / 2;
    blit.dstOffsets[0] = { left, top, 0 };
    blit.dstOffsets[1] = { left + int32_t(outWidth), top + int32_t(outHeight), 1 };
    vkCmdBlitImage(g_commandBuffer, source, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    // SIMPSONS_CAPTURE=<frame>,...: copy the presented image to the end of the upload ring
    bool capture = false;
    VkDeviceSize captureOffset = g_uploadLimit - VkDeviceSize(sourceWidth) * sourceHeight * 8;
    if (const char* frames = getenv("SIMPSONS_CAPTURE"))
    {
        std::string list = std::string(",") + frames + ",";
        capture = list.find("," + std::to_string(frame) + ",") != std::string::npos && g_uploadOffset < captureOffset;
    }
    if (capture)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = captureOffset;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { sourceWidth, sourceHeight, 1 };
        vkCmdCopyImageToBuffer(g_commandBuffer, source, VK_IMAGE_LAYOUT_GENERAL, g_uploadBuffer, 1, &region);
    }
    if (resolved)
        ImageBarrier(source, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    else
        SurfaceBarrier(source, VK_IMAGE_ASPECT_COLOR_BIT);
    ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    bool tracedPasses = TracingPasses() && !g_passNames.empty();
    if (tracedPasses)
    {
        vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_passTimestamps, uint32_t(g_passNames.size()));
        g_passNames.push_back("fin");
    }
    if (g_timestamps != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(g_commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_timestamps, g_frameSlot * 2 + 1);
        g_timestampsWritten[g_frameSlot] = true;
    }
    vkEndCommandBuffer(g_commandBuffer);
    g_recording = false;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &g_imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g_renderFinished[imageIndex];
    vkQueueSubmit(g_queue, 1, &submit, g_frameFence);
    if (tracedPasses)
    {
        vkDeviceWaitIdle(g_device);
        std::vector<uint64_t> times(g_passNames.size());
        vkGetQueryPoolResults(g_device, g_passTimestamps, 0, uint32_t(times.size()), times.size() * 8, times.data(), 8,
            VK_QUERY_RESULT_64_BIT);
        for (size_t i = 0; i + 1 < times.size(); i++)
            fprintf(stderr, "[passe] %-40s %.3f ms\n", g_passNames[i].c_str(), double(times[i + 1] - times[i]) * g_timestampPeriod / 1e6);
        fprintf(stderr, "[passe] total %.3f ms\n", double(times.back() - times[0]) * g_timestampPeriod / 1e6);
        g_passNames.clear();
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &g_renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &g_swapchain;
    present.pImageIndices = &imageIndex;
    VkResult presented = vkQueuePresentKHR(g_queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(g_device);
        CreateSwapchain();
    }

    g_presentedFrames = frame;
    if (!g_copyCaptures.empty())
    {
        vkWaitForFences(g_device, 1, &g_frameFence, VK_TRUE, UINT64_MAX);
        for (size_t n = 0; n < g_copyCaptures.size(); n++)
        {
            auto& c = g_copyCaptures[n];
            char name[96];
            snprintf(name, sizeof(name), "recomp/copie_%llu_%02zu_%08X.ppm", (unsigned long long)frame, n, c.address);
            if (c.depth)
            {
                const float* depth = reinterpret_cast<const float*>(g_uploadMapped + c.offset);
                size_t count = size_t(c.width) * c.height;
                auto [low, high] = std::minmax_element(depth, depth + count);
                fprintf(stderr, "[rendu] copie de profondeur 0x%08X : min %g max %g\n", c.address, *low, *high);
                std::vector<__fp16> pixels(count * 4);
                float range = std::max(*high - *low, 1e-6f);
                for (size_t i = 0; i < count; i++)
                    pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = __fp16((depth[i] - *low) / range);
                WritePpm(name, reinterpret_cast<const uint8_t*>(pixels.data()), c.width, c.height, c.width);
            }
            else
                WritePpm(name, g_uploadMapped + c.offset, c.width, c.height, c.rowPixels, c.format);
        }
        g_copyCaptures.clear();
    }
    if (capture)
    {
        vkWaitForFences(g_device, 1, &g_frameFence, VK_TRUE, UINT64_MAX);
        char name[64];
        const char* prefix = getenv("SIMPSONS_CAPTURE_NOM");
        snprintf(name, sizeof(name), "recomp/%s%llu.ppm", prefix ? prefix : "capture_", (unsigned long long)frame);
        WritePpm(name, g_uploadMapped + captureOffset, sourceWidth, sourceHeight, sourceWidth, sourceFormat);
        fprintf(stderr, "[rendu] capture %s\n", name);
    }
    g_lastResolveAddress = UINT32_MAX;

    if (frame <= 3 || frame % 300 == 0)
        fprintf(stderr, "[rendu] image %llu : %llu dessins, %.1f Mo transferes\n", (unsigned long long)frame,
            (unsigned long long)g_drawsThisFrame, g_uploadOffset / 1048576.0);
    static const bool profile = getenv("SIMPSONS_PROFIL_GPU") != nullptr;
    if (profile && frame % 600 == 0)
    {
        fprintf(stderr, "[profil] copies Mo/image : constantes %.2f partagees %.2f sommets %.2f indices %.2f textures %.2f tampons %.2f\n",
            g_uploadBytes[0] / 6e8, g_uploadBytes[1] / 6e8, g_uploadBytes[2] / 6e8, g_uploadBytes[3] / 6e8, g_uploadBytes[4] / 6e8, g_uploadBytes[5] / 6e8);
        memset(g_uploadBytes, 0, sizeof(g_uploadBytes));
        fprintf(stderr, "[profil] passes de rendu par image : %.1f (%.1f Mpixels), textures de copie creees : %llu\n", g_renderPasses / 600.0,
            g_renderPassPixels / 6e8, (unsigned long long)g_resolveImagesCreated);
        g_renderPasses = g_renderPassPixels = g_resolveImagesCreated = 0;
        fprintf(stderr, "[profil] creation de pipelines : %.1f ms\n", g_pipelineMs);
        g_pipelineMs = 0;
        if (g_gpuFrames)
            fprintf(stderr, "[profil] temps GPU par image : %.2f ms\n", g_gpuNanoseconds / 1e6 / g_gpuFrames);
        g_gpuNanoseconds = 0;
        g_gpuFrames = 0;
    }
}

// ---------------------------------------------------------------------------
// Textures and samplers

namespace
{
    VkFormat HostFormat(RendererTextureFormat format)
    {
        static const VkFormat formats[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM,
            VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT,
            VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
            VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK };
        return formats[uint32_t(format)];
    }

    void EndRenderingAt(int line)
    {
        if (g_rendering)
        {
            g_passEndLine = line;
            vkCmdEndRendering(g_commandBuffer);
            g_rendering = false;
        }
    }

    VkComponentSwizzle Swizzle(uint8_t select)
    {
        static const VkComponentSwizzle swizzles[] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
        return select < std::size(swizzles) ? swizzles[select] : VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

bool RendererSupportsFormat(RendererTextureFormat format)
{
    int8_t& support = g_formatSupport[uint32_t(format)];
    if (support < 0)
    {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(g_physicalDevice, HostFormat(format), &properties);
        support = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : 0;
        // SIMPSONS_SANS_BC=1 (tests): BC textures decompressed by the CPU as on the Apple TV
        if (getenv("SIMPSONS_SANS_BC") && format >= RendererTextureFormat::BC1 && format <= RendererTextureFormat::BC5)
            support = 0;
        if (!support)
            fprintf(stderr, "[rendu] format de texture hote %u non supporte\n", uint32_t(format));
    }
    return support > 0;
}

uint32_t RendererFindTexture(uint64_t key)
{
    auto it = g_textures.find(key);
    return it != g_textures.end() ? it->second.descriptorIndex : 0;
}

uint32_t RendererUploadTexture(uint64_t key, const RendererTextureDesc& desc)
{
    if (!g_running || desc.width == 0 || desc.height == 0)
        return 0;
    BeginFrameIfNeeded();

    TextureEntry& entry = g_textures[key];
    uint32_t layers = desc.cube ? 6 : 1;
    if (entry.image.image == VK_NULL_HANDLE || entry.width != desc.width || entry.height != desc.height || entry.format != desc.format ||
        entry.levels != desc.levels || entry.cube != desc.cube)
    {
        if (entry.image.image != VK_NULL_HANDLE)
            g_retiredTextures.emplace_back(entry.image, entry.descriptorIndex);
        entry = {};
        VkComponentMapping components{ Swizzle(desc.swizzle[0]), Swizzle(desc.swizzle[1]), Swizzle(desc.swizzle[2]), Swizzle(desc.swizzle[3]) };
        if (!CreateImage(entry.image, VK_IMAGE_TYPE_2D, desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, HostFormat(desc.format),
                { desc.width, desc.height, 1 }, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, components, desc.levels))
        {
            g_textures.erase(key);
            return 0;
        }
        entry.width = desc.width;
        entry.height = desc.height;
        entry.format = desc.format;
        entry.levels = desc.levels;
        entry.cube = desc.cube;
        if (!g_freeTextureIndices.empty())
        {
            entry.descriptorIndex = g_freeTextureIndices.back();
            g_freeTextureIndices.pop_back();
        }
        else if (g_nextTextureIndex < DESCRIPTOR_HEAP_SIZE)
            entry.descriptorIndex = g_nextTextureIndex++;
        else
        {
            fprintf(stderr, "[rendu] table de textures pleine\n");
            return 0;
        }

        VkDescriptorImageInfo imageInfo{ VK_NULL_HANDLE, entry.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = g_descriptorSets[desc.cube ? 2 : 0];
        write.dstArrayElement = entry.descriptorIndex;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    }

    VkDeviceSize offset = Upload(desc.data, desc.size, 256, 4);
    if (offset == UINT64_MAX)
        return entry.descriptorIndex;

    // Copies are not allowed inside dynamic rendering: interrupt it (resumed by the next draw)
    EndRenderingAt(__LINE__);
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, layers);
    VkBufferImageCopy regions[16]{};
    for (uint32_t level = 0; level < desc.levels; level++)
    {
        regions[level].bufferOffset = offset + desc.levelOffsets[level];
        regions[level].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers };
        regions[level].imageExtent = { std::max(desc.width >> level, 1u), std::max(desc.height >> level, 1u), 1 };
    }
    vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, desc.levels, regions);
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, layers);
    return entry.descriptorIndex;
}

uint64_t RendererUploadEpoch()
{
    BeginFrameIfNeeded();
    return g_uploadEpoch;
}

uint64_t RendererUploadBuffer(const void* data, size_t size)
{
    if (!g_running)
        return 0;
    BeginFrameIfNeeded();
    VkDeviceSize offset = Upload(data, size, 16, 5);
    return offset == UINT64_MAX ? 0 : g_uploadAddress + offset;
}

uint32_t RendererGetSampler(uint32_t addressU, uint32_t addressV, uint32_t addressW, uint32_t magFilter,
    uint32_t minFilter, uint32_t mipFilter)
{
    uint32_t key = addressU | (addressV << 3) | (addressW << 6) | (magFilter << 9) | (minFilter << 10) | (mipFilter << 11);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end())
        return it->second;
    if (!g_running || g_samplers.size() + 1 >= DESCRIPTOR_HEAP_SIZE)
        return 0;

    static const VkSamplerAddressMode modes[] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = magFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = minFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = modes[std::min<uint32_t>(addressU, 4)];
    info.addressModeV = modes[std::min<uint32_t>(addressV, 4)];
    info.addressModeW = modes[std::min<uint32_t>(addressW, 4)];
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.maxLod = mipFilter == 2 ? 0.0f : VK_LOD_CLAMP_NONE; // 2 = base map only
    VkSampler sampler;
    if (vkCreateSampler(g_device, &info, nullptr, &sampler) != VK_SUCCESS)
        return 0;

    uint32_t index = uint32_t(g_samplers.size()) + 1; // 0 = placeholder sampler
    VkDescriptorImageInfo imageInfo{ sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = g_descriptorSets[3];
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    g_samplers[key] = index;
    return index;
}

// ---------------------------------------------------------------------------
// Resolved render targets, used as textures

namespace
{
    struct ResolvedTexture
    {
        Image image; // TARGET_FORMAT, or R32_SFLOAT for depth copies; one view per swizzle below
        uint32_t width = 0, height = 0;
        bool swapRedBlue = false;
        bool depth = false;
        VkFormat format = TARGET_FORMAT; // of the copied surface
        std::unordered_map<uint32_t, std::pair<VkImageView, uint32_t>> views; // packed swizzle -> view, index
    };
    std::unordered_map<uint32_t, ResolvedTexture> g_resolved;
    // Copy textures whose destination changed size (several copies of different sizes to one address in each frame)
    // are kept here with their views and reused, instead of being destroyed and created again at every frame:
    // creating Metal textures and their first descriptor writes cost a good part of the GPU thread.
    std::unordered_multimap<uint64_t, ResolvedTexture> g_resolvedPool;

    uint64_t ResolvedPoolKey(uint32_t width, uint32_t height, VkFormat format, bool depth)
    {
        return (uint64_t(width) << 40) ^ (uint64_t(height) << 16) ^ (uint64_t(format) << 1) ^ uint64_t(depth);
    }

    void RetireResolved(ResolvedTexture& entry)
    {
        for (auto& [key, view] : entry.views)
            g_retiredViews.push_back(view);
        g_retiredTextures.emplace_back(entry.image, 0);
    }

    // Replaces <entry> by a texture of that size and format: from the pool when one is free (true), else empty
    // (false, to be created by the caller). The previous texture goes to the pool.
    bool ReuseResolved(ResolvedTexture& entry, uint32_t width, uint32_t height, VkFormat format, bool depth)
    {
        if (entry.image.image != VK_NULL_HANDLE)
        {
            g_resolvedPool.emplace(ResolvedPoolKey(entry.width, entry.height, entry.format, entry.depth), std::move(entry));
            if (g_resolvedPool.size() > 64)
            {
                auto oldest = g_resolvedPool.begin();
                RetireResolved(oldest->second);
                g_resolvedPool.erase(oldest);
            }
        }
        entry = {};
        auto pooled = g_resolvedPool.find(ResolvedPoolKey(width, height, format, depth));
        if (pooled == g_resolvedPool.end() || pooled->second.width != width || pooled->second.height != height ||
            pooled->second.format != format || pooled->second.depth != depth)
        {
            g_resolveImagesCreated++;
            return false;
        }
        entry = std::move(pooled->second);
        g_resolvedPool.erase(pooled);
        entry.swapRedBlue = false;
        return true;
    }

    uint32_t AllocateTextureIndex()
    {
        if (!g_freeTextureIndices.empty())
        {
            uint32_t index = g_freeTextureIndices.back();
            g_freeTextureIndices.pop_back();
            return index;
        }
        return g_nextTextureIndex < DESCRIPTOR_HEAP_SIZE ? g_nextTextureIndex++ : 0;
    }
}

void RendererResolve(uint32_t address, uint32_t width, uint32_t height, uint32_t x, uint32_t y, uint32_t copyWidth,
    uint32_t copyHeight, bool swapRedBlue)
{
    if (!g_running || width == 0 || height == 0)
        return;
    // A garbage copy pitch (seen once: 14295 pixels wide in DOLPHINS) exceeds both the Xenos limit (8192) and,
    // once scaled, Metal's 16384 limit, which aborts the process. Such copies are ignored.
    if (width > 8192 || height > 8192 || width * g_resolutionScale > 16384 || height * g_resolutionScale > 16384)
    {
        static bool logged = false;
        if (!logged)
            fprintf(stderr, "[rendu] copie impossible ignoree : %ux%u\n", width, height);
        logged = true;
        return;
    }
    // Console pixels -> host pixels
    width *= g_resolutionScale;
    height *= g_resolutionScale;
    x *= g_resolutionScale;
    y *= g_resolutionScale;
    copyWidth *= g_resolutionScale;
    copyHeight *= g_resolutionScale;
    BeginFrameIfNeeded();
    if (g_color == nullptr)
        g_color = GetRenderTarget(false, 0);
    if (g_color == nullptr)
        return;

    ResolvedTexture& entry = g_resolved[address];
    bool keep = true; // a partial copy keeps the rest of the texture
    if (entry.image.image == VK_NULL_HANDLE || entry.width != width || entry.height != height || entry.format != g_color->format ||
        entry.depth)
    {
        if (ReuseResolved(entry, width, height, g_color->format, false))
            ;
        else if (!CreateImage(entry.image, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, g_color->format, { width, height, 1 }, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        {
            g_resolved.erase(address);
            return;
        }
        entry.width = width;
        entry.height = height;
        entry.format = g_color->format;
        keep = false;
    }
    entry.swapRedBlue = swapRedBlue;
    g_lastResolveAddress = address;

    EndRenderingAt(__LINE__);
    SurfaceBarrier(g_color->image.image, VK_IMAGE_ASPECT_COLOR_BIT);
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, keep ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    // Source rectangle -> same position in the destination
    x = std::min(x, width - 1);
    y = std::min(y, height - 1);
    region.srcOffset = { int32_t(x), int32_t(y), 0 };
    region.dstOffset = { int32_t(x), int32_t(y), 0 };
    region.extent = { std::min({ copyWidth, width - x, g_color->width - x }), std::min({ copyHeight, height - y, g_color->height - y }), 1 };
    vkCmdCopyImage(g_commandBuffer, g_color->image.image, VK_IMAGE_LAYOUT_GENERAL, entry.image.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    static const char* captureFrame = getenv("SIMPSONS_CAPTURE_COPIES");
    if (captureFrame && uint64_t(atoll(captureFrame)) == g_presentedFrames + 1)
    {
        VkDeviceSize size = VkDeviceSize(region.extent.width) * region.extent.height * FormatBytes(g_color->format);
        VkDeviceSize offset = (g_uploadOffset + 255) & ~VkDeviceSize(255);
        if (offset + size <= g_uploadLimit)
        {
            g_uploadOffset = offset + size;
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = region.extent;
            vkCmdCopyImageToBuffer(g_commandBuffer, g_color->image.image, VK_IMAGE_LAYOUT_GENERAL, g_uploadBuffer, 1, &copy);
            g_copyCaptures.push_back({ offset, address, region.extent.width, region.extent.height, region.extent.width, false, g_color->format });
        }
    }
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    // Split screen: the game resolves each half to a destination address offset inside a larger texture (base +
    // whole 32x32 tiles). Also write the copy at that place in the enclosing texture, which is sampled later.
    uint32_t tileBytes = 32 * 32 * FormatBytes(g_color->format);
    for (auto& [base, outer] : g_resolved)
    {
        if (base >= address || outer.depth || outer.format != g_color->format || outer.width < 32)
            continue;
        uint32_t delta = address - base;
        uint32_t outerWidth = outer.width / g_resolutionScale, outerHeight = outer.height / g_resolutionScale; // console pixels
        if (delta % tileBytes != 0 || delta >= outerWidth * outerHeight * FormatBytes(outer.format))
            continue;
        uint32_t tilesPerRow = outerWidth / 32;
        uint32_t tile = delta / tileBytes;
        uint32_t ox = (tile % tilesPerRow) * 32 * g_resolutionScale + x, oy = (tile / tilesPerRow) * 32 * g_resolutionScale + y;
        if (ox >= outer.width || oy >= outer.height)
            continue;
        VkImageCopy inner = region;
        inner.dstOffset = { int32_t(ox), int32_t(oy), 0 };
        inner.extent.width = std::min(inner.extent.width, outer.width - ox);
        inner.extent.height = std::min(inner.extent.height, outer.height - oy);
        ImageBarrier(outer.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(g_commandBuffer, g_color->image.image, VK_IMAGE_LAYOUT_GENERAL, outer.image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &inner);
        ImageBarrier(outer.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    SurfaceBarrier(g_color->image.image, VK_IMAGE_ASPECT_COLOR_BIT);
}

namespace
{
    bool PresentSource(VkImage* image, uint32_t* width, uint32_t* height, VkFormat* format)
    {
        auto it = g_resolved.find(g_lastResolveAddress);
        if (it == g_resolved.end())
            return false;
        *image = it->second.image.image;
        *format = it->second.format;
        *width = it->second.width;
        *height = it->second.height;
        return true;
    }
}

void RendererSetTargets(uint32_t msaa, uint32_t colorBase, uint32_t colorFormat, uint32_t depthBase, uint32_t scissorWidth,
    uint32_t scissorHeight)
{
    if (!g_running)
        return;
    // Samples per pixel: 2x MSAA doubles the height, 4x both dimensions
    g_sampleScale[0] = msaa >= 2 ? 2.0f : 1.0f;
    g_sampleScale[1] = msaa >= 1 ? 2.0f : 1.0f;
    RenderTarget* color = GetRenderTarget(false, colorBase, ColorTargetFormat(colorFormat));
    RenderTarget* depth = GetRenderTarget(true, depthBase);
    if (color == nullptr || depth == nullptr)
        return;
    static const bool wholeSurface = getenv("SIMPSONS_ZONE_COMPLETE") != nullptr; // tests: no rendering area limit
    if (wholeSurface)
        scissorWidth = scissorHeight = 8192;
    uint32_t renderWidth = std::max(1u, uint32_t(scissorWidth * g_sampleScale[0]) * g_resolutionScale);
    uint32_t renderHeight = std::max(1u, uint32_t(scissorHeight * g_sampleScale[1]) * g_resolutionScale);
    if (color == g_color && depth == g_depth && renderWidth == g_renderWidth && renderHeight == g_renderHeight)
        return;
    g_renderWidth = renderWidth;
    g_renderHeight = renderHeight;
    EndRenderingAt(__LINE__);
    g_color = color;
    g_depth = depth;
}

void RendererResolveDepth(uint32_t address, uint32_t width, uint32_t height)
{
    if (!g_running || width == 0 || height == 0)
        return;
    width *= g_resolutionScale; // console pixels -> host pixels
    height *= g_resolutionScale;
    BeginFrameIfNeeded();
    if (g_depth == nullptr)
        g_depth = GetRenderTarget(true, 720);
    if (g_depth == nullptr)
        return;
    width = std::min(width, g_depth->width);
    height = std::min(height, g_depth->height);

    ResolvedTexture& entry = g_resolved[address];
    if (entry.image.image == VK_NULL_HANDLE || !entry.depth || entry.width != width || entry.height != height)
    {
        if (ReuseResolved(entry, width, height, VK_FORMAT_R32_SFLOAT, true))
            ;
        else if (!CreateImage(entry.image, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_SFLOAT, { width, height, 1 }, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        {
            g_resolved.erase(address);
            return;
        }
        entry.width = width;
        entry.height = height;
        entry.depth = true;
        entry.format = VK_FORMAT_R32_SFLOAT;
    }

    // SIMPSONS_SANS_OMBRES=1 (tests): depth copies stay empty, nothing is in shadow
    static const bool noShadows = getenv("SIMPSONS_SANS_OMBRES") != nullptr;
    if (noShadows)
    {
        EndRenderingAt(__LINE__);
        ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(g_commandBuffer, entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        return;
    }

    // Depth cannot be copied to a colour image directly: go through the upload ring
    VkDeviceSize size = VkDeviceSize(width) * height * 4;
    VkDeviceSize offset = (g_uploadOffset + 255) & ~VkDeviceSize(255);
    if (offset + size > g_uploadLimit)
        return;
    g_uploadOffset = offset + size;

    EndRenderingAt(__LINE__);
    SurfaceBarrier(g_depth->image.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkBufferImageCopy toBuffer{};
    toBuffer.bufferOffset = offset;
    toBuffer.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    toBuffer.imageExtent = { width, height, 1 };
    vkCmdCopyImageToBuffer(g_commandBuffer, g_depth->image.image, VK_IMAGE_LAYOUT_GENERAL, g_uploadBuffer, 1, &toBuffer);
    VkBufferMemoryBarrier bufferBarrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bufferBarrier.buffer = g_uploadBuffer;
    bufferBarrier.offset = offset;
    bufferBarrier.size = size;
    vkCmdPipelineBarrier(g_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy toImage{};
    toImage.bufferOffset = offset;
    toImage.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    toImage.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(g_commandBuffer, g_uploadBuffer, entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toImage);
    static const char* captureFrame = getenv("SIMPSONS_CAPTURE_COPIES");
    if (captureFrame && uint64_t(atoll(captureFrame)) == g_presentedFrames + 1)
        g_copyCaptures.push_back({ offset, address, width, height, width, true });
    ImageBarrier(entry.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    SurfaceBarrier(g_depth->image.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
}

uint32_t RendererFindResolved(uint32_t address, uint32_t width, uint32_t height, const uint8_t swizzle[4])
{
    auto it = g_resolved.find(address);
    if (it == g_resolved.end())
        return 0;
    (void)width;
    (void)height;
    ResolvedTexture& entry = it->second;

    // Memory components X, Y, Z, W -> render target channels (the copy may exchange red and blue)
    static const uint8_t straight[4] = { 0, 1, 2, 3 }, swapped[4] = { 2, 1, 0, 3 };
    const uint8_t* channels = entry.swapRedBlue ? swapped : straight;
    uint8_t mapping[4];
    for (uint32_t c = 0; c < 4; c++)
        mapping[c] = swizzle[c] < 4 ? (entry.depth ? 0 : channels[swizzle[c]]) : swizzle[c]; // depth: every component reads it
    uint32_t key = mapping[0] | (mapping[1] << 8) | (mapping[2] << 16) | (mapping[3] << 24);

    auto view = entry.views.find(key);
    if (view != entry.views.end())
        return view->second.second;

    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = entry.image.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = entry.format;
    info.components = { Swizzle(mapping[0]), Swizzle(mapping[1]), Swizzle(mapping[2]), Swizzle(mapping[3]) };
    info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView imageView;
    if (vkCreateImageView(g_device, &info, nullptr, &imageView) != VK_SUCCESS)
        return 0;
    uint32_t index = AllocateTextureIndex();
    if (index == 0)
        return 0;
    VkDescriptorImageInfo imageInfo{ VK_NULL_HANDLE, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = g_descriptorSets[0];
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    entry.views[key] = { imageView, index };
    return index;
}
