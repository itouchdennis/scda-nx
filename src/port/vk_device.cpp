#include "port/vk_device.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "port/port.h"

namespace gpu::vk {

Funcs fn;

namespace {

constexpr VkDeviceSize kBlockSize = 32u << 20;

const char* const kDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};

bool HasExtension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all) {
        if (!std::strcmp(e.extensionName, name)) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice phys) {
    uint32_t n = 0;
    fn.vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    fn.vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, all.data());
    return all;
}

bool IsDepth(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D16_UNORM;
}

bool HasStencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

}  // namespace

bool Device::Init() {
    auto create = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!create) {
        LOGE("vk: no Vulkan driver");
        return false;
    }
    std::vector<const char*> exts = port::VulkanInstanceExtensions();
    if (exts.empty()) {
        LOGE("vk: the window takes no Vulkan surface");
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "scda";
    app.pEngineName = "scda";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    // SCDA_VK_VALIDATE=1: Khronos' validation layer, where it is installed
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (const char* v = std::getenv("SCDA_VK_VALIDATE"); v && *v == '1') {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        LOGI("vk: validation layer on");
    }
    VkResult r = create(&ci, nullptr, &instance);
    if (r != VK_SUCCESS) {
        LOGE("vk: no instance (%d)", r);
        return false;
    }
#define X(name) fn.name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    SCDA_VK_INSTANCE_FUNCS(X)
#undef X
    uint64_t s = 0;
    if (!port::CreateVulkanSurface(instance, &s)) return false;
    surface = reinterpret_cast<VkSurfaceKHR>(s);
    if (!PickDevice() || !CreateDevice()) return false;
    fn.vkGetDeviceQueue(dev, family, 0, &queue);
    fn.vkGetPhysicalDeviceMemoryProperties(phys, &mem_props_);
    for (uint32_t t = 0; t < mem_props_.memoryTypeCount; t++) {
        const VkMemoryType& mt = mem_props_.memoryTypes[t];
        VkMemoryPropertyFlags f = mt.propertyFlags;
        LOGI("vk: memory type %u: heap %u (%llu MB)%s%s%s%s", t, mt.heapIndex,
             (unsigned long long)(mem_props_.memoryHeaps[mt.heapIndex].size >> 20),
             f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " device" : "", f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host" : "",
             f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " coherent" : "",
             f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " cached" : "");
    }
    // the depth buffer: 24 bit as on the console where it can be sampled
    VkFormatProperties fp{};
    fn.vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    constexpr VkFormatFeatureFlags kDepthUse =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    depth_format = (fp.optimalTilingFeatures & kDepthUse) == kDepthUse ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                       : VK_FORMAT_D32_SFLOAT_S8_UINT;
    LOGI("vk: %s, Vulkan %u.%u.%u, driver %08X, depth %s%s", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion,
         depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
         custom_border ? ", custom border colours" : "");
    return true;
}

bool Device::PickDevice() {
    uint32_t n = 0;
    fn.vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    fn.vkEnumeratePhysicalDevices(instance, &n, all.data());
    // SCDA_VK_DEVICE=n: the nth device, as the driver lists them
    const char* pick = std::getenv("SCDA_VK_DEVICE");
    int best_rank = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        fn.vkGetPhysicalDeviceProperties(all[i], &p);
        std::vector<VkExtensionProperties> exts = DeviceExtensions(all[i]);
        bool ok = p.apiVersion >= VK_API_VERSION_1_3;
        for (const char* e : kDeviceExtensions) ok &= HasExtension(exts, e);
        uint32_t qn = 0;
        fn.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        fn.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &qn, qs.data());
        int q = -1;
        for (uint32_t j = 0; j < qn && q < 0; j++) {
            VkBool32 present = VK_FALSE;
            fn.vkGetPhysicalDeviceSurfaceSupportKHR(all[i], j, surface, &present);
            if ((qs[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) q = int(j);
        }
        LOGI("vk: device %u: %s%s", i, p.deviceName, ok && q >= 0 ? "" : " (lacks what the renderer needs)");
        if (!ok || q < 0) continue;
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                   : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                   : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                            : 1;
        if (pick) rank = std::atoi(pick) == int(i) ? 9 : -1;
        if (rank > best_rank) {
            best_rank = rank;
            phys = all[i];
            family = uint32_t(q);
            props = p;
        }
    }
    if (!phys) LOGE("vk: no device with Vulkan 1.3, shader objects and push descriptors");
    return phys != VK_NULL_HANDLE;
}

bool Device::CreateDevice() {
    std::vector<VkExtensionProperties> exts = DeviceExtensions(phys);
    std::vector<const char*> want(std::begin(kDeviceExtensions), std::end(kDeviceExtensions));
    custom_border = HasExtension(exts, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);

    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT object{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &v13;
    v13.pNext = &object;
    if (custom_border) object.pNext = &border;
    fn.vkGetPhysicalDeviceFeatures2(phys, &have);
    if (!v13.dynamicRendering || !object.shaderObject) {
        LOGE("vk: %s lacks dynamic rendering or shader objects", props.deviceName);
        return false;
    }
    custom_border &= border.customBorderColors && border.customBorderColorWithoutFormat;

    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_on{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    border_on.customBorderColors = VK_TRUE;
    border_on.customBorderColorWithoutFormat = VK_TRUE;
    VkPhysicalDeviceShaderObjectFeaturesEXT object_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    object_on.shaderObject = VK_TRUE;
    if (custom_border) {
        object_on.pNext = &border_on;
        want.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    }
    VkPhysicalDeviceVulkan13Features v13_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13_on.dynamicRendering = VK_TRUE;
    v13_on.pNext = &object_on;
    VkPhysicalDeviceFeatures2 on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    on.pNext = &v13_on;
    on.features.samplerAnisotropy = have.features.samplerAnisotropy;
    on.features.fillModeNonSolid = have.features.fillModeNonSolid;
    on.features.textureCompressionBC = have.features.textureCompressionBC;
    if (!have.features.textureCompressionBC) LOGW("vk: %s has no DXT textures", props.deviceName);
    max_anisotropy = have.features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &on;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = uint32_t(want.size());
    ci.ppEnabledExtensionNames = want.data();
    VkResult r = fn.vkCreateDevice(phys, &ci, nullptr, &dev);
    if (r != VK_SUCCESS) {
        LOGE("vk: no device (%d)", r);
        return false;
    }
    bool ok = true;
#define X(name)                                                                                                       \
    fn.name = reinterpret_cast<PFN_##name>(fn.vkGetDeviceProcAddr(dev, #name));                                       \
    if (!fn.name) {                                                                                                   \
        LOGE("vk: no " #name);                                                                                        \
        ok = false;                                                                                                   \
    }
    SCDA_VK_DEVICE_FUNCS(X)
#undef X

    VkPhysicalDeviceShaderObjectPropertiesEXT op{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &op;
    fn.vkGetPhysicalDeviceProperties2(phys, &p2);
    std::memcpy(binary_uuid, op.shaderBinaryUUID, VK_UUID_SIZE);
    binary_version = op.shaderBinaryVersion;
    return ok;
}

bool Device::Resize(uint32_t w, uint32_t h, bool want_vsync) {
    // a window with a size of its own (the Switch's) takes a new one only with no swap chain on it: the old one goes
    // first, then the caps below report the new size. Handed over as oldSwapchain, it kept the window at the old size
    // and every frame asked for a new swap chain again
    if (port::SurfaceSizeDiffers(w, h)) {
        for (VkImageView v : swap_views) fn.vkDestroyImageView(dev, v, nullptr);
        swap_views.clear();
        swap_images.clear();
        if (swapchain) fn.vkDestroySwapchainKHR(dev, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        port::SetSurfaceSize(w, h);
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;

    uint32_t nf = 0;
    fn.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    fn.vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, formats.data());
    VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        for (const auto& sf : formats) {
            if (sf.format == f && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) format = sf;
        }
    }
    uint32_t nm = 0;
    fn.vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    fn.vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!want_vsync) {
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) {
        for (uint32_t bit = 1; bit; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                ci.compositeAlpha = VkCompositeAlphaFlagBitsKHR(bit);
                break;
            }
        }
    }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    VkResult r = fn.vkCreateSwapchainKHR(dev, &ci, nullptr, &made);
    for (VkImageView v : swap_views) fn.vkDestroyImageView(dev, v, nullptr);
    swap_views.clear();
    swap_images.clear();
    if (swapchain) fn.vkDestroySwapchainKHR(dev, swapchain, nullptr);
    swapchain = made;
    if (r != VK_SUCCESS) {
        LOGE("vk: no swap chain (%d)", r);
        swapchain = VK_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    fn.vkGetSwapchainImagesKHR(dev, swapchain, &n, nullptr);
    swap_images.resize(n);
    fn.vkGetSwapchainImagesKHR(dev, swapchain, &n, swap_images.data());
    for (VkImage img : swap_images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        fn.vkCreateImageView(dev, &vi, nullptr, &v);
        swap_views.push_back(v);
    }
    swap_format = format.format;
    swap_w = extent.width;
    swap_h = extent.height;
    vsync = want_vsync;
    LOGI("vk: swap chain %ux%u, %u images, %s", swap_w, swap_h, n,
         mode == VK_PRESENT_MODE_FIFO_KHR        ? "vsync"
         : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                 : "mailbox");
    return true;
}

Memory Device::Allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid,
                        bool image) {
    std::lock_guard lock(lock_);
    // memory types that suit, the ones without the avoided properties first
    std::vector<uint32_t> types;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t t = 0; t < mem_props_.memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = mem_props_.memoryTypes[t].propertyFlags;
            if (!(req.memoryTypeBits >> t & 1) || (f & want) != want) continue;
            if ((pass == 0) == !(f & avoid)) types.push_back(t);
        }
    }
    // no type with these properties is no shortage: CreateBuffer asks for host cached memory, which not every GPU has,
    // and then takes plain host memory
    if (types.empty()) return {};
    VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 16);
    for (uint32_t type : types) {
        for (uint32_t b = 0; b < blocks_.size(); b++) {
            Block& blk = blocks_[b];
            if (!blk.memory || blk.type != type || blk.image != image) continue;
            for (auto it = blk.free.begin(); it != blk.free.end(); ++it) {
                VkDeviceSize off = (it->first + align - 1) & ~(align - 1);
                VkDeviceSize end = it->first + it->second;
                if (off + req.size > end) continue;
                VkDeviceSize start = it->first;
                blk.free.erase(it);
                if (off > start) blk.free[start] = off - start;
                if (off + req.size < end) blk.free[off + req.size] = end - off - req.size;
                return {b, off, req.size, blk.map ? blk.map + off : nullptr};
            }
        }
        // a new block; one bigger than half a block is one of its own
        Block blk;
        blk.type = type;
        blk.image = image;
        blk.size = req.size > kBlockSize / 2 ? req.size : kBlockSize;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = blk.size;
        ai.memoryTypeIndex = type;
        if (fn.vkAllocateMemory(dev, &ai, nullptr, &blk.memory) != VK_SUCCESS) continue;
        if (mem_props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            void* p = nullptr;
            fn.vkMapMemory(dev, blk.memory, 0, VK_WHOLE_SIZE, 0, &p);
            blk.map = static_cast<uint8_t*>(p);
        }
        if (req.size < blk.size) blk.free[req.size] = blk.size - req.size;
        uint32_t b = 0;
        while (b < blocks_.size() && blocks_[b].memory) b++;
        if (b == blocks_.size()) blocks_.emplace_back();
        blocks_[b] = std::move(blk);
        return {b, 0, req.size, blocks_[b].map};
    }
    LOGE("vk: out of GPU memory (%llu bytes)", (unsigned long long)req.size);
    return {};
}

void Device::Free(Memory& m) {
    if (!m) return;
    std::lock_guard lock(lock_);
    Block& blk = blocks_[m.block];
    auto next = blk.free.lower_bound(m.offset);
    VkDeviceSize off = m.offset, size = m.size;
    if (next != blk.free.end() && next->first == off + size) {
        size += next->second;
        next = blk.free.erase(next);
    }
    if (next != blk.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
            off = prev->first;
            size += prev->second;
            blk.free.erase(prev);
        }
    }
    blk.free[off] = size;
    // a block of its own goes back once empty
    if (blk.size > kBlockSize && off == 0 && size == blk.size) {
        fn.vkFreeMemory(dev, blk.memory, nullptr);
        blk = Block{};
    }
    m = {};
}

bool Device::CreateImage(Image& img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                         uint32_t levels, VkImageUsageFlags usage, bool cube) {
    img = Image{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn.vkCreateImage(dev, &ci, nullptr, &img.image) != VK_SUCCESS) {
        LOGE("vk: no %ux%ux%u image of format %d", w, h, d, fmt);
        img.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    fn.vkGetImageMemoryRequirements(dev, img.image, &req);
    img.mem = Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    if (!img.mem || fn.vkBindImageMemory(dev, img.image, MemoryOf(img.mem), img.mem.offset) != VK_SUCCESS) {
        DestroyImage(img);
        return false;
    }
    img.format = fmt;
    img.w = w;
    img.h = h;
    img.d = d;
    img.layers = layers;
    img.levels = levels;
    bool depth = IsDepth(fmt);
    img.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
                       : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : img.aspect, 0, levels, 0, layers};
    fn.vkCreateImageView(dev, &vi, nullptr, &img.view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {img.aspect, 0, 1, 0, 1};
        fn.vkCreateImageView(dev, &vi, nullptr, &img.attach);
    }
    return true;
}

void Device::DestroyImage(Image& img) {
    if (img.attach) fn.vkDestroyImageView(dev, img.attach, nullptr);
    if (img.view) fn.vkDestroyImageView(dev, img.view, nullptr);
    if (img.image) fn.vkDestroyImage(dev, img.image, nullptr);
    Free(img.mem);
    img = Image{};
}

bool Device::CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool cached) {
    b = Buffer{};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (fn.vkCreateBuffer(dev, &ci, nullptr, &b.buffer) != VK_SUCCESS) {
        b.buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    fn.vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (cached) b.mem = Allocate(req, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, false);
    if (!b.mem) b.mem = Allocate(req, want, 0, false);
    if (!b.mem || fn.vkBindBufferMemory(dev, b.buffer, MemoryOf(b.mem), b.mem.offset) != VK_SUCCESS) {
        DestroyBuffer(b);
        return false;
    }
    b.size = size;
    return true;
}

void Device::DestroyBuffer(Buffer& b) {
    if (b.buffer) fn.vkDestroyBuffer(dev, b.buffer, nullptr);
    Free(b.mem);
    b = Buffer{};
}

VkFormat FromDxgi(uint32_t f) {
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

}  // namespace gpu::vk
