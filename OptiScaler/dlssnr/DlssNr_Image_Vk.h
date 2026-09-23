#pragma once

#include <shaders/Shader_Vk.h>

namespace DlssNr
{
inline uint32_t FindVkMemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

// Storage only: callers retire GPU work before resizing or destroying an image.
struct ImageVk
{
    VkImageInfo info {};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool Valid() const { return info.ImageView != VK_NULL_HANDLE; }

    void Destroy(VkDevice device)
    {
        if (info.ImageView)
            vkDestroyImageView(device, info.ImageView, nullptr);
        if (info.Image)
            vkDestroyImage(device, info.Image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        *this = {};
    }

    bool Ensure(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format)
    {
        if (Valid() && info.Width == width && info.Height == height && info.Format == format)
            return true;
        Destroy(device);
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = { width, height, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &ci, nullptr, &info.Image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements need {};
        vkGetImageMemoryRequirements(device, info.Image, &need);
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = need.size;
        ai.memoryTypeIndex = FindVkMemoryType(physical, need.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = info.Image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &ai, nullptr, &memory) != VK_SUCCESS ||
            vkBindImageMemory(device, info.Image, memory, 0) != VK_SUCCESS ||
            vkCreateImageView(device, &vi, nullptr, &info.ImageView) != VK_SUCCESS)
        {
            Destroy(device);
            return false;
        }
        info.SubresourceRange = vi.subresourceRange;
        info.Format = format;
        info.Width = width;
        info.Height = height;
        return true;
    }
};
} // namespace DlssNr
