#pragma once
// The Vulkan renderer's device (port/gpu_vk.cpp): instance, device and queue, the window's swap chain, memory in
// large blocks handed out first fit, and images and buffers on that memory. Vulkan 1.3 with dynamic rendering, plus
// shader objects (VK_EXT_shader_object: every state dynamic, shaders compiled one by one and kept as binaries) and
// push descriptors, as RADV and NVK (the Switch, Mesa's Horizon port) both have them. Images stay in the GENERAL
// layout throughout; the renderer orders its passes with barriers.
//
// Only vkGetInstanceProcAddr is linked: the Switch's NVK is a static library without a loader, the PC's libvulkan a
// loader; everything else comes through it, as the functions below.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace gpu::vk {

#define SCDA_VK_INSTANCE_FUNCS(X)                                                                                     \
    X(vkDestroyInstance)                                                                                              \
    X(vkEnumeratePhysicalDevices)                                                                                     \
    X(vkGetPhysicalDeviceProperties)                                                                                  \
    X(vkGetPhysicalDeviceProperties2)                                                                                 \
    X(vkGetPhysicalDeviceFeatures2)                                                                                   \
    X(vkGetPhysicalDeviceFormatProperties)                                                                            \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                       \
    X(vkEnumerateDeviceExtensionProperties)                                                                           \
    X(vkCreateDevice)                                                                                                 \
    X(vkGetDeviceProcAddr)                                                                                            \
    X(vkDestroySurfaceKHR)                                                                                            \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                           \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                      \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                           \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define SCDA_VK_DEVICE_FUNCS(X)                                                                                       \
    X(vkDestroyDevice)                                                                                                \
    X(vkGetDeviceQueue)                                                                                               \
    X(vkDeviceWaitIdle)                                                                                               \
    X(vkQueueSubmit)                                                                                                  \
    X(vkAllocateMemory)                                                                                               \
    X(vkFreeMemory)                                                                                                   \
    X(vkMapMemory)                                                                                                    \
    X(vkCreateBuffer)                                                                                                 \
    X(vkDestroyBuffer)                                                                                                \
    X(vkGetBufferMemoryRequirements)                                                                                  \
    X(vkBindBufferMemory)                                                                                             \
    X(vkCreateImage)                                                                                                  \
    X(vkDestroyImage)                                                                                                 \
    X(vkGetImageMemoryRequirements)                                                                                   \
    X(vkBindImageMemory)                                                                                              \
    X(vkCreateImageView)                                                                                              \
    X(vkDestroyImageView)                                                                                             \
    X(vkCreateSampler)                                                                                                \
    X(vkCreateDescriptorSetLayout)                                                                                    \
    X(vkCreatePipelineLayout)                                                                                         \
    X(vkCreateCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                       \
    X(vkBeginCommandBuffer)                                                                                           \
    X(vkEndCommandBuffer)                                                                                             \
    X(vkResetCommandBuffer)                                                                                           \
    X(vkCreateFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                \
    X(vkResetFences)                                                                                                  \
    X(vkGetFenceStatus)                                                                                               \
    X(vkCreateSemaphore)                                                                                              \
    X(vkDestroySemaphore)                                                                                             \
    X(vkCmdPipelineBarrier)                                                                                           \
    X(vkCmdBeginRendering)                                                                                            \
    X(vkCmdEndRendering)                                                                                              \
    X(vkCmdCopyBuffer)                                                                                                \
    X(vkCmdCopyBufferToImage)                                                                                         \
    X(vkCmdCopyImageToBuffer)                                                                                         \
    X(vkCmdCopyImage)                                                                                                 \
    X(vkCmdClearColorImage)                                                                                           \
    X(vkCmdClearDepthStencilImage)                                                                                    \
    X(vkCmdClearAttachments)                                                                                          \
    X(vkCmdBindVertexBuffers)                                                                                         \
    X(vkCmdBindIndexBuffer)                                                                                           \
    X(vkCmdDraw)                                                                                                      \
    X(vkCmdDrawIndexed)                                                                                               \
    X(vkCmdSetViewportWithCount)                                                                                      \
    X(vkCmdSetScissorWithCount)                                                                                       \
    X(vkCmdSetRasterizerDiscardEnable)                                                                                \
    X(vkCmdSetDepthBiasEnable)                                                                                        \
    X(vkCmdSetDepthBias)                                                                                              \
    X(vkCmdSetDepthTestEnable)                                                                                        \
    X(vkCmdSetDepthWriteEnable)                                                                                       \
    X(vkCmdSetDepthCompareOp)                                                                                         \
    X(vkCmdSetDepthBoundsTestEnable)                                                                                  \
    X(vkCmdSetStencilTestEnable)                                                                                      \
    X(vkCmdSetStencilOp)                                                                                              \
    X(vkCmdSetStencilCompareMask)                                                                                     \
    X(vkCmdSetStencilWriteMask)                                                                                       \
    X(vkCmdSetStencilReference)                                                                                       \
    X(vkCmdSetCullMode)                                                                                               \
    X(vkCmdSetFrontFace)                                                                                              \
    X(vkCmdSetPrimitiveTopology)                                                                                      \
    X(vkCmdSetPrimitiveRestartEnable)                                                                                 \
    X(vkCmdSetBlendConstants)                                                                                         \
    X(vkCmdSetLineWidth)                                                                                              \
    X(vkCreateSwapchainKHR)                                                                                           \
    X(vkDestroySwapchainKHR)                                                                                          \
    X(vkGetSwapchainImagesKHR)                                                                                        \
    X(vkAcquireNextImageKHR)                                                                                          \
    X(vkQueuePresentKHR)                                                                                              \
    X(vkCmdPushDescriptorSetKHR)                                                                                      \
    X(vkCreateShadersEXT)                                                                                             \
    X(vkDestroyShaderEXT)                                                                                             \
    X(vkGetShaderBinaryDataEXT)                                                                                       \
    X(vkCmdBindShadersEXT)                                                                                            \
    X(vkCmdSetVertexInputEXT)                                                                                         \
    X(vkCmdSetPolygonModeEXT)                                                                                         \
    X(vkCmdSetRasterizationSamplesEXT)                                                                                \
    X(vkCmdSetSampleMaskEXT)                                                                                          \
    X(vkCmdSetAlphaToCoverageEnableEXT)                                                                               \
    X(vkCmdSetColorBlendEnableEXT)                                                                                    \
    X(vkCmdSetColorBlendEquationEXT)                                                                                  \
    X(vkCmdSetColorWriteMaskEXT)

struct Funcs {
#define X(name) PFN_##name name = nullptr;
    SCDA_VK_INSTANCE_FUNCS(X)
    SCDA_VK_DEVICE_FUNCS(X)
#undef X
};
extern Funcs fn;

// one suballocation
struct Memory {
    uint32_t block = ~0u;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* map = nullptr;  // host visible blocks
    explicit operator bool() const { return block != ~0u; }
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;    // the whole image as shaders sample it (the depth aspect of a depth image)
    VkImageView attach = VK_NULL_HANDLE;  // level 0 as a render target
    Memory mem;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, d = 1, layers = 1, levels = 1;
    VkImageAspectFlags aspect = 0;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Memory mem;
    VkDeviceSize size = 0;
};

class Device {
public:
    bool Init();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties props{};
    // the shader binaries' compatibility: a kept binary from another driver build is compiled again
    uint8_t binary_uuid[VK_UUID_SIZE] = {};
    uint32_t binary_version = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;  // D24S8 where the GPU has it, else D32S8
    bool custom_border = false;                   // VK_EXT_custom_border_color: any border colour, not just three
    float max_anisotropy = 1;

    // the swap chain, made again by Resize; false while the window has no size
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t swap_w = 0, swap_h = 0;
    std::vector<VkImage> swap_images;
    std::vector<VkImageView> swap_views;
    bool vsync = true;
    bool Resize(uint32_t w, uint32_t h, bool vsync);

    // images: usage gets TRANSFER_SRC/DST and SAMPLED besides; cube: 6 layers sampled as a cube
    bool CreateImage(Image& img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, bool cube = false);
    void DestroyImage(Image& img);
    // host visible: mapped, coherent; cached adds HOST_CACHED for reading back
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool cached = false);
    void DestroyBuffer(Buffer& b);

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t type = 0;
        bool image = false;  // images and buffers keep to blocks of their own: no granularity between them
        VkDeviceSize size = 0;
        uint8_t* map = nullptr;
        std::map<VkDeviceSize, VkDeviceSize> free;  // offset to size
    };
    Memory Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid,
                    bool image);
    void Free(Memory& m);
    VkDeviceMemory MemoryOf(const Memory& m) const { return blocks_[m.block].memory; }
    bool PickDevice();
    bool CreateDevice();
    std::vector<Block> blocks_;
    std::mutex lock_;
    VkPhysicalDeviceMemoryProperties mem_props_{};
};

// D3D's DXGI formats as the shared GPU code names them, as Vulkan vertex formats; UNDEFINED where there is none
VkFormat FromDxgi(uint32_t dxgi);

}  // namespace gpu::vk
