#pragma once

// Vulkan Video as the engine's decoders share it: the loader opened at
// run time (libvulkan.so.1), Vulkan's own types declared here with the
// layout the headers give them on a 64-bit target — measured, and held by
// the asserts — so the build needs none of the headers; the device chosen
// (a discrete GPU before an integrated one, never a CPU implementation)
// for the codec a decoder names; the video session with its memory and a
// picture per decoded picture buffer slot; a bitstream buffer the host
// writes; the decode recorded, submitted and waited for; a picture read
// back as NV12. What differs by codec — the profile, the capabilities
// asked after, the picture information handed with each frame, which
// slots hold what — is each decoder's own (Vp9Vulkan.cpp, H264Vulkan.cpp).

#include "media/Vp9Accelerator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sashfold::media::vulkan {

static_assert(sizeof(void*) == 8, "Vulkan's handles are declared here as a 64-bit target has them");

// Handles: the dispatchable ones are pointers; the others are pointers to
// opaque types on a 64-bit target, held here as the 64-bit values they are.
using VkInstance = struct VkInstanceOpaque*;
using VkPhysicalDevice = struct VkPhysicalDeviceOpaque*;
using VkDevice = struct VkDeviceOpaque*;
using VkQueue = struct VkQueueOpaque*;
using VkCommandBuffer = struct VkCommandBufferOpaque*;
using VkDeviceMemory = std::uint64_t;
using VkBuffer = std::uint64_t;
using VkImage = std::uint64_t;
using VkImageView = std::uint64_t;
using VkCommandPool = std::uint64_t;
using VkFence = std::uint64_t;
using VkVideoSessionKHR = std::uint64_t;

using VkResult = std::int32_t;
using VkStructureType = std::int32_t;
using VkFlags = std::uint32_t;
using VkFlags64 = std::uint64_t;
using VkBool32 = std::uint32_t;
using VkDeviceSize = std::uint64_t;

constexpr VkResult vk_success = 0;
constexpr VkResult vk_incomplete = 5;

constexpr VkStructureType st_application_info = 0;
constexpr VkStructureType st_instance_create_info = 1;
constexpr VkStructureType st_device_queue_create_info = 2;
constexpr VkStructureType st_device_create_info = 3;
constexpr VkStructureType st_submit_info = 4;
constexpr VkStructureType st_memory_allocate_info = 5;
constexpr VkStructureType st_fence_create_info = 8;
constexpr VkStructureType st_buffer_create_info = 12;
constexpr VkStructureType st_image_create_info = 14;
constexpr VkStructureType st_image_view_create_info = 15;
constexpr VkStructureType st_command_pool_create_info = 39;
constexpr VkStructureType st_command_buffer_allocate_info = 40;
constexpr VkStructureType st_command_buffer_begin_info = 42;
constexpr VkStructureType st_physical_device_features_2 = 1000059000;
constexpr VkStructureType st_queue_family_properties_2 = 1000059005;
constexpr VkStructureType st_video_profile_info = 1000023000;
constexpr VkStructureType st_video_capabilities = 1000023001;
constexpr VkStructureType st_video_picture_resource_info = 1000023002;
constexpr VkStructureType st_video_session_memory_requirements = 1000023003;
constexpr VkStructureType st_bind_video_session_memory_info = 1000023004;
constexpr VkStructureType st_video_session_create_info = 1000023005;
constexpr VkStructureType st_video_begin_coding_info = 1000023008;
constexpr VkStructureType st_video_end_coding_info = 1000023009;
constexpr VkStructureType st_video_coding_control_info = 1000023010;
constexpr VkStructureType st_video_reference_slot_info = 1000023011;
constexpr VkStructureType st_queue_family_video_properties = 1000023012;
constexpr VkStructureType st_video_profile_list_info = 1000023013;
constexpr VkStructureType st_physical_device_video_format_info = 1000023014;
constexpr VkStructureType st_video_format_properties = 1000023015;
constexpr VkStructureType st_video_decode_info = 1000024000;
constexpr VkStructureType st_video_decode_capabilities = 1000024001;
constexpr VkStructureType st_memory_barrier_2 = 1000314000;
constexpr VkStructureType st_image_memory_barrier_2 = 1000314002;
constexpr VkStructureType st_dependency_info = 1000314003;
constexpr VkStructureType st_physical_device_synchronization_2_features = 1000314007;
constexpr VkStructureType st_physical_device_video_decode_vp9_features = 1000514000;
constexpr VkStructureType st_video_decode_vp9_capabilities = 1000514001;
constexpr VkStructureType st_video_decode_vp9_picture_info = 1000514002;
constexpr VkStructureType st_video_decode_vp9_profile_info = 1000514003;

constexpr std::uint32_t api_version_1_3 = (1u << 22) | (3u << 12);
constexpr std::uint32_t vp9_std_version_1_0_0 = 1u << 22;
constexpr std::uint32_t queue_family_ignored = ~0u;

constexpr VkFlags queue_graphics = 0x1;
constexpr VkFlags queue_compute = 0x2;
constexpr VkFlags queue_transfer = 0x4;
constexpr VkFlags queue_video_decode = 0x20;
constexpr VkFlags codec_decode_vp9 = 0x8;
constexpr VkFlags chroma_subsampling_420 = 0x2;
constexpr VkFlags component_depth_8 = 0x1;
constexpr VkFlags capability_separate_reference_images = 0x2;
constexpr VkFlags decode_output_coincides = 0x1;
constexpr VkFlags coding_control_reset = 0x1;

constexpr std::int32_t format_nv12 = 1000156003; // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
constexpr std::int32_t image_type_2d = 1;
constexpr std::int32_t view_type_2d = 1;
constexpr std::int32_t tiling_optimal = 0;
constexpr VkFlags sample_count_1 = 0x1;
constexpr std::int32_t sharing_exclusive = 0;
constexpr std::int32_t sharing_concurrent = 1;
constexpr VkFlags image_usage_transfer_src = 0x1;
constexpr VkFlags image_usage_decode_dst = 0x400;
constexpr VkFlags image_usage_decode_dpb = 0x1000;
constexpr VkFlags buffer_usage_transfer_dst = 0x2;
constexpr VkFlags buffer_usage_decode_src = 0x2000;
constexpr std::int32_t layout_undefined = 0;
constexpr std::int32_t layout_transfer_src = 6;
constexpr std::int32_t layout_decode_dpb = 1000024002;
constexpr VkFlags aspect_color = 0x1;
constexpr VkFlags aspect_plane_0 = 0x10;
constexpr VkFlags aspect_plane_1 = 0x20;
constexpr VkFlags memory_device_local = 0x1;
constexpr VkFlags memory_host_visible = 0x2;
constexpr VkFlags memory_host_coherent = 0x4;
constexpr VkFlags memory_host_cached = 0x8;
constexpr std::int32_t device_integrated = 1;
constexpr std::int32_t device_discrete = 2;
constexpr std::int32_t device_virtual = 3;
constexpr std::int32_t device_cpu = 4;
constexpr VkFlags pool_reset_command_buffer = 0x2;
constexpr VkFlags command_buffer_one_time_submit = 0x1;
constexpr std::int32_t command_buffer_primary = 0;

constexpr VkFlags64 stage_host = 0x4000;
constexpr VkFlags64 stage_all_commands = 0x10000;
constexpr VkFlags64 stage_video_decode = 0x04000000;
constexpr VkFlags64 stage_copy = 0x100000000;
constexpr VkFlags64 access_transfer_read = 0x800;
constexpr VkFlags64 access_transfer_write = 0x1000;
constexpr VkFlags64 access_host_read = 0x2000;
constexpr VkFlags64 access_memory_write = 0x10000;
constexpr VkFlags64 access_decode_read = 0x800000000;
constexpr VkFlags64 access_decode_write = 0x1000000000;

// H.264's codec operation and structure types (VK_KHR_video_decode_h264).
constexpr VkFlags codec_decode_h264 = 0x1;
constexpr VkStructureType st_video_session_parameters_create_info = 1000023006;
constexpr VkStructureType st_video_decode_h264_capabilities = 1000040000;
constexpr VkStructureType st_video_decode_h264_picture_info = 1000040001;
constexpr VkStructureType st_video_decode_h264_profile_info = 1000040003;
constexpr VkStructureType st_video_decode_h264_session_parameters_create_info = 1000040004;
constexpr VkStructureType st_video_decode_h264_session_parameters_add_info = 1000040005;
constexpr VkStructureType st_video_decode_h264_dpb_slot_info = 1000040006;
constexpr std::uint32_t h264_std_version_1_0_0 = 1u << 22;
using VkVideoSessionParametersKHR = std::uint64_t;

struct VkExtent2D {
    std::uint32_t width;
    std::uint32_t height;
};

struct VkExtent3D {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
};

struct VkOffset2D {
    std::int32_t x;
    std::int32_t y;
};

struct VkOffset3D {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

struct VkApplicationInfo {
    VkStructureType sType;
    void const* pNext;
    char const* pApplicationName;
    std::uint32_t applicationVersion;
    char const* pEngineName;
    std::uint32_t engineVersion;
    std::uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkApplicationInfo const* pApplicationInfo;
    std::uint32_t enabledLayerCount;
    char const* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    char const* const* ppEnabledExtensionNames;
};

// Only the head is read; the limits and sparse properties are kept as bytes.
struct VkPhysicalDeviceProperties {
    std::uint32_t apiVersion;
    std::uint32_t driverVersion;
    std::uint32_t vendorID;
    std::uint32_t deviceID;
    std::int32_t deviceType;
    char deviceName[256];
    std::uint8_t pipelineCacheUUID[16];
    alignas(8) std::uint8_t limitsAndSparseProperties[528];
};

struct VkQueueFamilyProperties {
    VkFlags queueFlags;
    std::uint32_t queueCount;
    std::uint32_t timestampValidBits;
    VkExtent3D minImageTransferGranularity;
};

struct VkQueueFamilyProperties2 {
    VkStructureType sType;
    void* pNext;
    VkQueueFamilyProperties queueFamilyProperties;
};

struct VkQueueFamilyVideoPropertiesKHR {
    VkStructureType sType;
    void* pNext;
    VkFlags videoCodecOperations;
};

struct VkExtensionProperties {
    char extensionName[256];
    std::uint32_t specVersion;
};

// Vulkan 1.0's features, all left off.
struct VkPhysicalDeviceFeatures2 {
    VkStructureType sType;
    void* pNext;
    VkBool32 features[55];
};

struct VkPhysicalDeviceSynchronization2Features {
    VkStructureType sType;
    void* pNext;
    VkBool32 synchronization2;
};

struct VkPhysicalDeviceVideoDecodeVP9FeaturesKHR {
    VkStructureType sType;
    void* pNext;
    VkBool32 videoDecodeVP9;
};

struct VkDeviceQueueCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    std::uint32_t queueFamilyIndex;
    std::uint32_t queueCount;
    float const* pQueuePriorities;
};

struct VkDeviceCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    std::uint32_t queueCreateInfoCount;
    VkDeviceQueueCreateInfo const* pQueueCreateInfos;
    std::uint32_t enabledLayerCount;
    char const* const* ppEnabledLayerNames;
    std::uint32_t enabledExtensionCount;
    char const* const* ppEnabledExtensionNames;
    void const* pEnabledFeatures;
};

struct VkMemoryType {
    VkFlags propertyFlags;
    std::uint32_t heapIndex;
};

struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags flags;
};

struct VkPhysicalDeviceMemoryProperties {
    std::uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[32];
    std::uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[16];
};

struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    std::uint32_t memoryTypeBits;
};

struct VkMemoryAllocateInfo {
    VkStructureType sType;
    void const* pNext;
    VkDeviceSize allocationSize;
    std::uint32_t memoryTypeIndex;
};

struct VkBufferCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    std::int32_t sharingMode;
    std::uint32_t queueFamilyIndexCount;
    std::uint32_t const* pQueueFamilyIndices;
};

struct VkImageCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    std::int32_t imageType;
    std::int32_t format;
    VkExtent3D extent;
    std::uint32_t mipLevels;
    std::uint32_t arrayLayers;
    VkFlags samples;
    std::int32_t tiling;
    VkFlags usage;
    std::int32_t sharingMode;
    std::uint32_t queueFamilyIndexCount;
    std::uint32_t const* pQueueFamilyIndices;
    std::int32_t initialLayout;
};

struct VkComponentMapping {
    std::int32_t r;
    std::int32_t g;
    std::int32_t b;
    std::int32_t a;
};

struct VkImageSubresourceRange {
    VkFlags aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct VkImageViewCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkImage image;
    std::int32_t viewType;
    std::int32_t format;
    VkComponentMapping components;
    VkImageSubresourceRange subresourceRange;
};

struct VkCommandPoolCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    std::uint32_t queueFamilyIndex;
};

struct VkCommandBufferAllocateInfo {
    VkStructureType sType;
    void const* pNext;
    VkCommandPool commandPool;
    std::int32_t level;
    std::uint32_t commandBufferCount;
};

struct VkCommandBufferBeginInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    void const* pInheritanceInfo;
};

struct VkSubmitInfo {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t waitSemaphoreCount;
    std::uint64_t const* pWaitSemaphores;
    VkFlags const* pWaitDstStageMask;
    std::uint32_t commandBufferCount;
    VkCommandBuffer const* pCommandBuffers;
    std::uint32_t signalSemaphoreCount;
    std::uint64_t const* pSignalSemaphores;
};

struct VkFenceCreateInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
};

struct VkMemoryBarrier2 {
    VkStructureType sType;
    void const* pNext;
    VkFlags64 srcStageMask;
    VkFlags64 srcAccessMask;
    VkFlags64 dstStageMask;
    VkFlags64 dstAccessMask;
};

struct VkImageMemoryBarrier2 {
    VkStructureType sType;
    void const* pNext;
    VkFlags64 srcStageMask;
    VkFlags64 srcAccessMask;
    VkFlags64 dstStageMask;
    VkFlags64 dstAccessMask;
    std::int32_t oldLayout;
    std::int32_t newLayout;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
};

struct VkDependencyInfo {
    VkStructureType sType;
    void const* pNext;
    VkFlags dependencyFlags;
    std::uint32_t memoryBarrierCount;
    VkMemoryBarrier2 const* pMemoryBarriers;
    std::uint32_t bufferMemoryBarrierCount;
    void const* pBufferMemoryBarriers;
    std::uint32_t imageMemoryBarrierCount;
    VkImageMemoryBarrier2 const* pImageMemoryBarriers;
};

struct VkImageSubresourceLayers {
    VkFlags aspectMask;
    std::uint32_t mipLevel;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct VkBufferImageCopy {
    VkDeviceSize bufferOffset;
    std::uint32_t bufferRowLength;
    std::uint32_t bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    VkOffset3D imageOffset;
    VkExtent3D imageExtent;
};

struct VkVideoProfileInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags videoCodecOperation;
    VkFlags chromaSubsampling;
    VkFlags lumaBitDepth;
    VkFlags chromaBitDepth;
};

struct VkVideoDecodeVP9ProfileInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::int32_t stdProfile;
};

struct VkVideoProfileListInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t profileCount;
    VkVideoProfileInfoKHR const* pProfiles;
};

struct VkVideoCapabilitiesKHR {
    VkStructureType sType;
    void* pNext;
    VkFlags flags;
    VkDeviceSize minBitstreamBufferOffsetAlignment;
    VkDeviceSize minBitstreamBufferSizeAlignment;
    VkExtent2D pictureAccessGranularity;
    VkExtent2D minCodedExtent;
    VkExtent2D maxCodedExtent;
    std::uint32_t maxDpbSlots;
    std::uint32_t maxActiveReferencePictures;
    VkExtensionProperties stdHeaderVersion;
};

struct VkVideoDecodeCapabilitiesKHR {
    VkStructureType sType;
    void* pNext;
    VkFlags flags;
};

struct VkVideoDecodeVP9CapabilitiesKHR {
    VkStructureType sType;
    void* pNext;
    std::int32_t maxLevel;
};

struct VkPhysicalDeviceVideoFormatInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags imageUsage;
};

struct VkVideoFormatPropertiesKHR {
    VkStructureType sType;
    void* pNext;
    std::int32_t format;
    VkComponentMapping componentMapping;
    VkFlags imageCreateFlags;
    std::int32_t imageType;
    std::int32_t imageTiling;
    VkFlags imageUsageFlags;
};

struct VkVideoSessionCreateInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t queueFamilyIndex;
    VkFlags flags;
    VkVideoProfileInfoKHR const* pVideoProfile;
    std::int32_t pictureFormat;
    VkExtent2D maxCodedExtent;
    std::int32_t referencePictureFormat;
    std::uint32_t maxDpbSlots;
    std::uint32_t maxActiveReferencePictures;
    VkExtensionProperties const* pStdHeaderVersion;
};

struct VkVideoSessionMemoryRequirementsKHR {
    VkStructureType sType;
    void* pNext;
    std::uint32_t memoryBindIndex;
    VkMemoryRequirements memoryRequirements;
};

struct VkBindVideoSessionMemoryInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::uint32_t memoryBindIndex;
    VkDeviceMemory memory;
    VkDeviceSize memoryOffset;
    VkDeviceSize memorySize;
};

struct VkVideoPictureResourceInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkOffset2D codedOffset;
    VkExtent2D codedExtent;
    std::uint32_t baseArrayLayer;
    VkImageView imageViewBinding;
};

struct VkVideoReferenceSlotInfoKHR {
    VkStructureType sType;
    void const* pNext;
    std::int32_t slotIndex;
    VkVideoPictureResourceInfoKHR const* pPictureResource;
};

struct VkVideoBeginCodingInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkVideoSessionKHR videoSession;
    std::uint64_t videoSessionParameters;
    std::uint32_t referenceSlotCount;
    VkVideoReferenceSlotInfoKHR const* pReferenceSlots;
};

struct VkVideoEndCodingInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
};

struct VkVideoCodingControlInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
};

struct VkVideoDecodeInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkBuffer srcBuffer;
    VkDeviceSize srcBufferOffset;
    VkDeviceSize srcBufferRange;
    VkVideoPictureResourceInfoKHR dstPictureResource;
    VkVideoReferenceSlotInfoKHR const* pSetupReferenceSlot;
    std::uint32_t referenceSlotCount;
    VkVideoReferenceSlotInfoKHR const* pReferenceSlots;
};


struct VkVideoSessionParametersCreateInfoKHR {
    VkStructureType sType;
    void const* pNext;
    VkFlags flags;
    VkVideoSessionParametersKHR videoSessionParametersTemplate;
    VkVideoSessionKHR videoSession;
};

using VkVoidFunction = void (*)();

// The entry points, as the loader hands them out.
struct Vk {
    void* library = nullptr;
    VkVoidFunction (*get_instance_proc_addr)(VkInstance, char const*) = nullptr;
    VkResult (*enumerate_instance_version)(std::uint32_t*) = nullptr;
    VkResult (*create_instance)(VkInstanceCreateInfo const*, void const*, VkInstance*) = nullptr;

    void (*destroy_instance)(VkInstance, void const*) = nullptr;
    VkResult (*enumerate_physical_devices)(VkInstance, std::uint32_t*, VkPhysicalDevice*) = nullptr;
    void (*get_physical_device_properties)(VkPhysicalDevice, VkPhysicalDeviceProperties*) = nullptr;
    void (*get_physical_device_queue_family_properties2)(VkPhysicalDevice, std::uint32_t*, VkQueueFamilyProperties2*) = nullptr;
    VkResult (*enumerate_device_extension_properties)(VkPhysicalDevice, char const*, std::uint32_t*, VkExtensionProperties*) = nullptr;
    void (*get_physical_device_features2)(VkPhysicalDevice, VkPhysicalDeviceFeatures2*) = nullptr;
    void (*get_physical_device_memory_properties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*) = nullptr;
    VkResult (*get_physical_device_video_capabilities)(VkPhysicalDevice, VkVideoProfileInfoKHR const*, VkVideoCapabilitiesKHR*) = nullptr;
    VkResult (*get_physical_device_video_format_properties)(
        VkPhysicalDevice, VkPhysicalDeviceVideoFormatInfoKHR const*, std::uint32_t*, VkVideoFormatPropertiesKHR*)
        = nullptr;
    VkResult (*create_device)(VkPhysicalDevice, VkDeviceCreateInfo const*, void const*, VkDevice*) = nullptr;
    VkVoidFunction (*get_device_proc_addr)(VkDevice, char const*) = nullptr;

    void (*destroy_device)(VkDevice, void const*) = nullptr;
    void (*get_device_queue)(VkDevice, std::uint32_t, std::uint32_t, VkQueue*) = nullptr;
    VkResult (*device_wait_idle)(VkDevice) = nullptr;
    VkResult (*create_command_pool)(VkDevice, VkCommandPoolCreateInfo const*, void const*, VkCommandPool*) = nullptr;
    void (*destroy_command_pool)(VkDevice, VkCommandPool, void const*) = nullptr;
    VkResult (*allocate_command_buffers)(VkDevice, VkCommandBufferAllocateInfo const*, VkCommandBuffer*) = nullptr;
    VkResult (*begin_command_buffer)(VkCommandBuffer, VkCommandBufferBeginInfo const*) = nullptr;
    VkResult (*end_command_buffer)(VkCommandBuffer) = nullptr;
    VkResult (*queue_submit)(VkQueue, std::uint32_t, VkSubmitInfo const*, VkFence) = nullptr;
    VkResult (*create_fence)(VkDevice, VkFenceCreateInfo const*, void const*, VkFence*) = nullptr;
    void (*destroy_fence)(VkDevice, VkFence, void const*) = nullptr;
    VkResult (*wait_for_fences)(VkDevice, std::uint32_t, VkFence const*, VkBool32, std::uint64_t) = nullptr;
    VkResult (*reset_fences)(VkDevice, std::uint32_t, VkFence const*) = nullptr;
    VkResult (*allocate_memory)(VkDevice, VkMemoryAllocateInfo const*, void const*, VkDeviceMemory*) = nullptr;
    void (*free_memory)(VkDevice, VkDeviceMemory, void const*) = nullptr;
    VkResult (*map_memory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**) = nullptr;
    VkResult (*create_buffer)(VkDevice, VkBufferCreateInfo const*, void const*, VkBuffer*) = nullptr;
    void (*destroy_buffer)(VkDevice, VkBuffer, void const*) = nullptr;
    void (*get_buffer_memory_requirements)(VkDevice, VkBuffer, VkMemoryRequirements*) = nullptr;
    VkResult (*bind_buffer_memory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) = nullptr;
    VkResult (*create_image)(VkDevice, VkImageCreateInfo const*, void const*, VkImage*) = nullptr;
    void (*destroy_image)(VkDevice, VkImage, void const*) = nullptr;
    void (*get_image_memory_requirements)(VkDevice, VkImage, VkMemoryRequirements*) = nullptr;
    VkResult (*bind_image_memory)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) = nullptr;
    VkResult (*create_image_view)(VkDevice, VkImageViewCreateInfo const*, void const*, VkImageView*) = nullptr;
    void (*destroy_image_view)(VkDevice, VkImageView, void const*) = nullptr;
    void (*cmd_pipeline_barrier2)(VkCommandBuffer, VkDependencyInfo const*) = nullptr;
    void (*cmd_copy_image_to_buffer)(VkCommandBuffer, VkImage, std::int32_t, VkBuffer, std::uint32_t, VkBufferImageCopy const*) = nullptr;
    VkResult (*create_video_session)(VkDevice, VkVideoSessionCreateInfoKHR const*, void const*, VkVideoSessionKHR*) = nullptr;
    void (*destroy_video_session)(VkDevice, VkVideoSessionKHR, void const*) = nullptr;
    VkResult (*get_video_session_memory_requirements)(VkDevice, VkVideoSessionKHR, std::uint32_t*, VkVideoSessionMemoryRequirementsKHR*) = nullptr;
    VkResult (*bind_video_session_memory)(VkDevice, VkVideoSessionKHR, std::uint32_t, VkBindVideoSessionMemoryInfoKHR const*) = nullptr;
    void (*cmd_begin_video_coding)(VkCommandBuffer, VkVideoBeginCodingInfoKHR const*) = nullptr;
    void (*cmd_end_video_coding)(VkCommandBuffer, VkVideoEndCodingInfoKHR const*) = nullptr;
    void (*cmd_control_video_coding)(VkCommandBuffer, VkVideoCodingControlInfoKHR const*) = nullptr;
    void (*cmd_decode_video)(VkCommandBuffer, VkVideoDecodeInfoKHR const*) = nullptr;
    VkResult (*create_video_session_parameters)(VkDevice, VkVideoSessionParametersCreateInfoKHR const*, void const*, VkVideoSessionParametersKHR*)
        = nullptr;
    void (*destroy_video_session_parameters)(VkDevice, VkVideoSessionParametersKHR, void const*) = nullptr;
};

static_assert(sizeof(VkPhysicalDeviceProperties) == 824 && offsetof(VkPhysicalDeviceProperties, deviceType) == 16
        && offsetof(VkPhysicalDeviceProperties, deviceName) == 20 && offsetof(VkPhysicalDeviceProperties, limitsAndSparseProperties) == 296,
    "Vulkan's physical device properties");
static_assert(sizeof(VkPhysicalDeviceFeatures2) == 240, "Vulkan's features");
static_assert(sizeof(VkApplicationInfo) == 48 && sizeof(VkInstanceCreateInfo) == 64, "Vulkan's instance creation");
static_assert(sizeof(VkQueueFamilyProperties2) == 40 && sizeof(VkQueueFamilyVideoPropertiesKHR) == 24, "Vulkan's queue families");
static_assert(sizeof(VkExtensionProperties) == 260, "Vulkan's extension properties");
static_assert(sizeof(VkDeviceQueueCreateInfo) == 40 && sizeof(VkDeviceCreateInfo) == 72, "Vulkan's device creation");
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520 && sizeof(VkMemoryRequirements) == 24 && sizeof(VkMemoryAllocateInfo) == 32,
    "Vulkan's memory");
static_assert(sizeof(VkBufferCreateInfo) == 56 && sizeof(VkImageCreateInfo) == 88 && sizeof(VkImageViewCreateInfo) == 80,
    "Vulkan's buffers and images");
static_assert(sizeof(VkCommandPoolCreateInfo) == 24 && sizeof(VkCommandBufferAllocateInfo) == 32 && sizeof(VkCommandBufferBeginInfo) == 32
        && sizeof(VkSubmitInfo) == 72 && sizeof(VkFenceCreateInfo) == 24,
    "Vulkan's commands");
static_assert(sizeof(VkMemoryBarrier2) == 48 && sizeof(VkImageMemoryBarrier2) == 96 && sizeof(VkDependencyInfo) == 64, "Vulkan's barriers");
static_assert(sizeof(VkBufferImageCopy) == 56, "Vulkan's copies");
static_assert(sizeof(VkVideoProfileInfoKHR) == 32 && sizeof(VkVideoDecodeVP9ProfileInfoKHR) == 24 && sizeof(VkVideoProfileListInfoKHR) == 32,
    "Vulkan's video profiles");
static_assert(sizeof(VkVideoCapabilitiesKHR) == 336 && offsetof(VkVideoCapabilitiesKHR, maxDpbSlots) == 64
        && sizeof(VkVideoDecodeCapabilitiesKHR) == 24 && sizeof(VkVideoDecodeVP9CapabilitiesKHR) == 24,
    "Vulkan's video capabilities");
static_assert(sizeof(VkPhysicalDeviceVideoFormatInfoKHR) == 24 && sizeof(VkVideoFormatPropertiesKHR) == 56, "Vulkan's video formats");
static_assert(sizeof(VkVideoSessionCreateInfoKHR) == 64 && sizeof(VkVideoSessionMemoryRequirementsKHR) == 48
        && sizeof(VkBindVideoSessionMemoryInfoKHR) == 48,
    "Vulkan's video sessions");
static_assert(sizeof(VkVideoPictureResourceInfoKHR) == 48 && sizeof(VkVideoReferenceSlotInfoKHR) == 32 && sizeof(VkVideoBeginCodingInfoKHR) == 56
        && sizeof(VkVideoEndCodingInfoKHR) == 24 && sizeof(VkVideoCodingControlInfoKHR) == 24,
    "Vulkan's video coding");
static_assert(sizeof(VkVideoDecodeInfoKHR) == 120 && offsetof(VkVideoDecodeInfoKHR, dstPictureResource) == 48
        && offsetof(VkVideoDecodeInfoKHR, pSetupReferenceSlot) == 96,
    "Vulkan's video decoding");

static_assert(sizeof(VkVideoSessionParametersCreateInfoKHR) == 40 && offsetof(VkVideoSessionParametersCreateInfoKHR, videoSession) == 32,
    "Vulkan's session parameters");

template<typename F>
bool resolve(VkVoidFunction function, F& into)
{
    into = reinterpret_cast<F>(function);
    return into != nullptr;
}

static_assert(sizeof(VkPhysicalDeviceProperties) == 824 && offsetof(VkPhysicalDeviceProperties, deviceType) == 16
        && offsetof(VkPhysicalDeviceProperties, deviceName) == 20 && offsetof(VkPhysicalDeviceProperties, limitsAndSparseProperties) == 296,
    "Vulkan's physical device properties");
static_assert(sizeof(VkPhysicalDeviceFeatures2) == 240, "Vulkan's features");
static_assert(sizeof(VkApplicationInfo) == 48 && sizeof(VkInstanceCreateInfo) == 64, "Vulkan's instance creation");
static_assert(sizeof(VkQueueFamilyProperties2) == 40 && sizeof(VkQueueFamilyVideoPropertiesKHR) == 24, "Vulkan's queue families");
static_assert(sizeof(VkExtensionProperties) == 260, "Vulkan's extension properties");
static_assert(sizeof(VkDeviceQueueCreateInfo) == 40 && sizeof(VkDeviceCreateInfo) == 72, "Vulkan's device creation");
static_assert(sizeof(VkPhysicalDeviceMemoryProperties) == 520 && sizeof(VkMemoryRequirements) == 24 && sizeof(VkMemoryAllocateInfo) == 32,
    "Vulkan's memory");
static_assert(sizeof(VkBufferCreateInfo) == 56 && sizeof(VkImageCreateInfo) == 88 && sizeof(VkImageViewCreateInfo) == 80,
    "Vulkan's buffers and images");
static_assert(sizeof(VkCommandPoolCreateInfo) == 24 && sizeof(VkCommandBufferAllocateInfo) == 32 && sizeof(VkCommandBufferBeginInfo) == 32
        && sizeof(VkSubmitInfo) == 72 && sizeof(VkFenceCreateInfo) == 24,
    "Vulkan's commands");
static_assert(sizeof(VkMemoryBarrier2) == 48 && sizeof(VkImageMemoryBarrier2) == 96 && sizeof(VkDependencyInfo) == 64, "Vulkan's barriers");
static_assert(sizeof(VkBufferImageCopy) == 56, "Vulkan's copies");
static_assert(sizeof(VkVideoProfileInfoKHR) == 32 && sizeof(VkVideoDecodeVP9ProfileInfoKHR) == 24 && sizeof(VkVideoProfileListInfoKHR) == 32,
    "Vulkan's video profiles");
static_assert(sizeof(VkVideoCapabilitiesKHR) == 336 && offsetof(VkVideoCapabilitiesKHR, maxDpbSlots) == 64
        && sizeof(VkVideoDecodeCapabilitiesKHR) == 24 && sizeof(VkVideoDecodeVP9CapabilitiesKHR) == 24,
    "Vulkan's video capabilities");
static_assert(sizeof(VkPhysicalDeviceVideoFormatInfoKHR) == 24 && sizeof(VkVideoFormatPropertiesKHR) == 56, "Vulkan's video formats");
static_assert(sizeof(VkVideoSessionCreateInfoKHR) == 64 && sizeof(VkVideoSessionMemoryRequirementsKHR) == 48
        && sizeof(VkBindVideoSessionMemoryInfoKHR) == 48,
    "Vulkan's video sessions");
static_assert(sizeof(VkVideoPictureResourceInfoKHR) == 48 && sizeof(VkVideoReferenceSlotInfoKHR) == 32 && sizeof(VkVideoBeginCodingInfoKHR) == 56
        && sizeof(VkVideoEndCodingInfoKHR) == 24 && sizeof(VkVideoCodingControlInfoKHR) == 24,
    "Vulkan's video coding");
static_assert(sizeof(VkVideoDecodeInfoKHR) == 120 && offsetof(VkVideoDecodeInfoKHR, dstPictureResource) == 48
        && offsetof(VkVideoDecodeInfoKHR, pSetupReferenceSlot) == 96,
    "Vulkan's video decoding");

using VkVoidFunction = void (*)();
static_assert(sizeof(VkVideoSessionParametersCreateInfoKHR) == 40 && offsetof(VkVideoSessionParametersCreateInfoKHR, videoSession) == 32,
    "Vulkan's session parameters");


// A decode or a copy that has not finished in this long has hung the device.
constexpr std::uint64_t fence_timeout_ns = 2'000'000'000;

inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
{
    return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

// What a codec's decoder asks of the device: its profile (the codec's own
// information chained to it), the extension and std header it needs, the
// feature it must have (none for H.264), the capabilities it reads after,
// and how many pictures it keeps.
struct Codec {
    char const* name = ""; // "VP9", for the reasons given
    VkFlags operation = 0;
    char const* extension = nullptr;
    char const* std_header = nullptr;
    std::uint32_t std_version = 0;
    VkVideoProfileInfoKHR const* profile = nullptr;
    VkVideoProfileListInfoKHR const* profile_list = nullptr;
    // A features structure (sType set, pNext null) whose VkBool32 at byte 16
    // says whether the device decodes the codec; null when the codec has none.
    void* features = nullptr;
    // The codec's capabilities structure (sType set), chained after the
    // decode capabilities; null when none is wanted.
    void* codec_capabilities = nullptr;
    std::uint32_t pictures = 0; // decoded picture buffer slots, the picture decoded included
    std::uint32_t active_references = 0;
};

// A physical device that decodes the codec, and what it was found to offer.
struct Candidate {
    VkPhysicalDevice device = nullptr;
    std::string name;
    int rank = -1;
    std::uint32_t decode_family = 0;
    std::uint32_t copy_family = 0;
    VkVideoCapabilitiesKHR capabilities {};
};

struct Picture {
    VkImage image = 0;
    VkDeviceMemory memory = 0;
    VkImageView view = 0;
    int width = 0; // of the frame decoded into it
    int height = 0;
};

struct HostBuffer {
    VkBuffer buffer = 0;
    VkDeviceMemory memory = 0;
    std::uint8_t* mapped = nullptr;
    std::uint64_t size = 0;
};

class Device {
public:
    Device() = default;
    Device(Device const&) = delete;
    Device& operator=(Device const&) = delete;
    ~Device();

    // The loader, the device and its queues for the codec; false with why not.
    bool open(Codec const& codec, std::string& error);
    std::string const& name() const { return m_name; }
    bool lost() const { return m_lost; }
    VkVideoCapabilitiesKHR const& capabilities() const { return m_capabilities; }
    Vk const& vk() const { return m_vk; }
    VkDevice device() const { return m_device; }

    // A session with pictures at least this large: made anew when there is
    // none or the frame outgrew it (`recreated` says so: the decoder's slots
    // are then empty). False when the device refuses.
    bool ensure_session(std::uint32_t width, std::uint32_t height, bool& recreated);
    bool has_session() const { return m_session != 0; }
    VkVideoSessionKHR session() const { return m_session; }
    VkExtent2D extent() const { return m_extent; }
    std::size_t picture_count() const { return m_pictures.size(); }
    Picture& picture(std::size_t i) { return m_pictures[i]; }
    VkVideoPictureResourceInfoKHR& resource(std::size_t i) { return m_resources[i]; }

    // The bitstream buffer, grown to `size`, mapped for the host to write.
    bool ensure_bitstream(std::uint64_t size);
    std::uint8_t* bitstream() { return m_bitstream.mapped; }
    VkBuffer bitstream_buffer() const { return m_bitstream.buffer; }

    // One decode into picture `target`: the barriers, the coding scope with
    // `bound` slots (and parameters, when the codec has them), a reset of
    // the session when one is pending, the decode, submitted and waited for.
    bool decode(VkVideoDecodeInfoKHR const& info, int target, std::span<VkVideoReferenceSlotInfoKHR const> bound,
        VkVideoSessionParametersKHR parameters);
    // The next decode begins the session afresh (a new stream).
    void request_reset() { m_reset_pending = true; }
    // A picture read back, cropped to the frame decoded into it.
    std::optional<Nv12Picture> read(int index);

    // Session parameters (H.264's parameter sets), owned by the caller.
    VkVideoSessionParametersKHR create_parameters(void const* next);
    void destroy_parameters(VkVideoSessionParametersKHR parameters);

private:
    template<typename F>
    bool instance_function(char const* name, F& into)
    {
        return resolve(m_vk.get_instance_proc_addr(m_instance, name), into);
    }
    template<typename F>
    bool device_function(char const* name, F& into)
    {
        return resolve(m_vk.get_device_proc_addr(m_device, name), into);
    }
    bool load(std::string& error);
    std::optional<std::string> examine(VkPhysicalDevice device, VkPhysicalDeviceProperties const& properties, Candidate& candidate);
    std::optional<Candidate> choose(std::string& error);
    bool create_device(Candidate const& chosen, std::string& error);
    std::optional<std::uint32_t> memory_type(std::uint32_t allowed, VkFlags wanted, VkFlags preferred) const;
    VkDeviceMemory allocate(VkMemoryRequirements const& requirements, VkFlags wanted, VkFlags preferred);
    bool ensure_buffer(HostBuffer& buffer, std::uint64_t size, VkFlags usage, void const* next, VkFlags preferred);
    void release_buffer(HostBuffer& buffer);
    void release_session();
    bool create_session(std::uint32_t width, std::uint32_t height);
    bool submit_and_wait(VkQueue queue, VkCommandBuffer commands);
    static VkImageMemoryBarrier2 image_barrier(VkImage image, VkFlags64 source_stage, VkFlags64 source_access, VkFlags64 destination_stage,
        VkFlags64 destination_access, std::int32_t from, std::int32_t to);

    Vk m_vk;
    Codec m_codec;
    std::vector<char const*> m_extensions;
    VkInstance m_instance = nullptr;
    VkPhysicalDevice m_physical = nullptr;
    VkDevice m_device = nullptr;
    std::string m_name;
    VkPhysicalDeviceMemoryProperties m_memory {};
    VkVideoCapabilitiesKHR m_capabilities {};
    std::uint32_t m_decode_family = 0;
    std::uint32_t m_copy_family = 0;
    VkQueue m_decode_queue = nullptr;
    VkQueue m_copy_queue = nullptr;
    VkCommandPool m_decode_pool = 0;
    VkCommandPool m_copy_pool = 0;
    VkCommandBuffer m_decode_commands = nullptr;
    VkCommandBuffer m_copy_commands = nullptr;
    VkFence m_fence = 0;
    bool m_lost = false;

    VkVideoSessionKHR m_session = 0;
    std::vector<VkDeviceMemory> m_session_memory;
    VkExtent2D m_extent {};
    std::vector<Picture> m_pictures;
    std::vector<VkVideoPictureResourceInfoKHR> m_resources;
    HostBuffer m_bitstream;
    HostBuffer m_readback;
    bool m_reset_pending = true;
};

}
