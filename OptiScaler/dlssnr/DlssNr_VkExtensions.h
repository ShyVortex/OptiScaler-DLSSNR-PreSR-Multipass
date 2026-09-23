#pragma once

// NR's device extensions must be requested at vkCreateDevice, before model initialization.

#include <vulkan/vulkan.h>

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace DlssNr::VkExt
{

// Names taken from nvngx_dlssnr.dll. Append only supported extensions the game has not enabled.
inline const char* const kDevice[] = {
    "VK_NVX_binary_import",
    "VK_NVX_image_view_handle",
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
};

// Own the pointer array until vkCreateDevice returns; the names themselves are borrowed.
struct Merged
{
    std::vector<const char*> names;
};

inline std::vector<std::string> SupportedDeviceExtensions(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                          VkInstance instance, VkPhysicalDevice physicalDevice)
{
    std::vector<std::string> out;

    if (getInstanceProcAddr == nullptr || physicalDevice == VK_NULL_HANDLE)
        return out;

    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties) getInstanceProcAddr(
        instance, "vkEnumerateDeviceExtensionProperties");

    if (enumerate == nullptr)
        return out;

    uint32_t count = 0;

    if (enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return out;

    std::vector<VkExtensionProperties> props(count);

    if (enumerate(physicalDevice, nullptr, &count, props.data()) != VK_SUCCESS)
        return out;

    out.reserve(count);

    for (const auto& p : props)
        out.emplace_back(p.extensionName);

    return out;
}

inline bool Contains(const std::vector<std::string>& haystack, const char* needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

inline bool ListHas(const char* const* list, uint32_t count, const char* needle)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (list[i] != nullptr && std::string_view(list[i]) == needle)
            return true;
    }

    return false;
}

} // namespace DlssNr::VkExt
