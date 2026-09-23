#include "media/linux/Vp9Accelerators.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <vector>

// VP9 through Vulkan Video (VK_KHR_video_decode_vp9): the GPU's video engine
// as NVIDIA's own driver offers it — the only way to it without a
// translation layer the user would have to install — and as Mesa offers
// AMD's and Intel's. The loader (libvulkan.so.1) is opened at run time, as
// the VA-API libraries are, and the types below are Vulkan's own
// (vulkan_core.h and the VP9 video std headers), declared with the layout
// those headers give them on a 64-bit target — measured, and held by the
// asserts — so the build needs none of the headers.
//
// The device is the one with the most to decode with: a discrete GPU
// before an integrated one, never a CPU implementation. Each picture of the
// decoded picture buffer is an image of its own, and the frame decoded is
// written into its reference picture (the DPB and the output coincide, as
// on NVIDIA's decoder); that picture is what read_last() copies out.
// Implementations that keep all references in one layered image, or that
// only write the output apart from the reference, are declined here with
// their reason, and VA-API is tried in their place.

namespace sashfold::media {

namespace {

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

// The VP9 video std headers (vulkan_video_codec_vp9std.h and its decode
// companion): the frame header as the application parsed it.
struct StdVideoVP9ColorConfigFlags {
    std::uint32_t color_range : 1;
    std::uint32_t reserved : 31;
};

struct StdVideoVP9ColorConfig {
    StdVideoVP9ColorConfigFlags flags;
    std::uint8_t BitDepth;
    std::uint8_t subsampling_x;
    std::uint8_t subsampling_y;
    std::uint8_t reserved1;
    std::int32_t color_space;
};

struct StdVideoVP9LoopFilterFlags {
    std::uint32_t loop_filter_delta_enabled : 1;
    std::uint32_t loop_filter_delta_update : 1;
    std::uint32_t reserved : 30;
};

struct StdVideoVP9LoopFilter {
    StdVideoVP9LoopFilterFlags flags;
    std::uint8_t loop_filter_level;
    std::uint8_t loop_filter_sharpness;
    std::uint8_t update_ref_delta;
    std::int8_t loop_filter_ref_deltas[4];
    std::uint8_t update_mode_delta;
    std::int8_t loop_filter_mode_deltas[2];
};

struct StdVideoVP9SegmentationFlags {
    std::uint32_t segmentation_update_map : 1;
    std::uint32_t segmentation_temporal_update : 1;
    std::uint32_t segmentation_update_data : 1;
    std::uint32_t segmentation_abs_or_delta_update : 1;
    std::uint32_t reserved : 28;
};

struct StdVideoVP9Segmentation {
    StdVideoVP9SegmentationFlags flags;
    std::uint8_t segmentation_tree_probs[7];
    std::uint8_t segmentation_pred_prob[3];
    std::uint8_t FeatureEnabled[8];
    std::int16_t FeatureData[8][4];
};

struct StdVideoDecodeVP9PictureInfoFlags {
    std::uint32_t error_resilient_mode : 1;
    std::uint32_t intra_only : 1;
    std::uint32_t allow_high_precision_mv : 1;
    std::uint32_t refresh_frame_context : 1;
    std::uint32_t frame_parallel_decoding_mode : 1;
    std::uint32_t segmentation_enabled : 1;
    std::uint32_t show_frame : 1;
    std::uint32_t UsePrevFrameMvs : 1;
    std::uint32_t reserved : 24;
};

struct StdVideoDecodeVP9PictureInfo {
    StdVideoDecodeVP9PictureInfoFlags flags;
    std::int32_t profile;
    std::int32_t frame_type;
    std::uint8_t frame_context_idx;
    std::uint8_t reset_frame_context;
    std::uint8_t refresh_frame_flags;
    std::uint8_t ref_frame_sign_bias_mask;
    std::int32_t interpolation_filter;
    std::uint8_t base_q_idx;
    std::int8_t delta_q_y_dc;
    std::int8_t delta_q_uv_dc;
    std::int8_t delta_q_uv_ac;
    std::uint8_t tile_cols_log2;
    std::uint8_t tile_rows_log2;
    std::uint16_t reserved1[3];
    StdVideoVP9ColorConfig const* pColorConfig;
    StdVideoVP9LoopFilter const* pLoopFilter;
    StdVideoVP9Segmentation const* pSegmentation;
};

struct VkVideoDecodeVP9PictureInfoKHR {
    VkStructureType sType;
    void const* pNext;
    StdVideoDecodeVP9PictureInfo const* pStdPictureInfo;
    std::int32_t referenceNameSlotIndices[3];
    std::uint32_t uncompressedHeaderOffset;
    std::uint32_t compressedHeaderOffset;
    std::uint32_t tilesOffset;
};

// The layout the Vulkan headers give these (1.4.357, measured on x86-64),
// which a compiler that laid them out otherwise would silently break.
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
static_assert(sizeof(VkVideoDecodeVP9PictureInfoKHR) == 48 && offsetof(VkVideoDecodeVP9PictureInfoKHR, uncompressedHeaderOffset) == 36,
    "Vulkan's VP9 picture information");
static_assert(sizeof(StdVideoVP9ColorConfig) == 12 && sizeof(StdVideoVP9LoopFilter) == 16 && offsetof(StdVideoVP9LoopFilter, update_mode_delta) == 11,
    "VP9's colour configuration and loop filter");
static_assert(sizeof(StdVideoVP9Segmentation) == 88 && offsetof(StdVideoVP9Segmentation, FeatureData) == 22, "VP9's segmentation");
static_assert(sizeof(StdVideoDecodeVP9PictureInfo) == 56 && offsetof(StdVideoDecodeVP9PictureInfo, interpolation_filter) == 16
        && offsetof(StdVideoDecodeVP9PictureInfo, base_q_idx) == 20 && offsetof(StdVideoDecodeVP9PictureInfo, pColorConfig) == 32,
    "VP9's picture information");
static_assert(sizeof(VkPhysicalDeviceSynchronization2Features) == 24 && sizeof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR) == 24,
    "Vulkan's feature structures");

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
};

template<typename F>
bool resolve(VkVoidFunction function, F& into)
{
    into = reinterpret_cast<F>(function);
    return into != nullptr;
}

constexpr std::array<char const*, 3> required_extensions { "VK_KHR_video_queue", "VK_KHR_video_decode_queue", "VK_KHR_video_decode_vp9" };

// Eight references, the frame being decoded, and the frame decoded last,
// which may still be read back while the next is decoded.
constexpr std::uint32_t picture_count = 10;

// A decode or a copy that has not finished in this long has hung the device.
constexpr std::uint64_t fence_timeout_ns = 2'000'000'000;

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
{
    return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

// VP9 profile 0 at 8 bits, 4:2:0, as every call that names a video profile
// wants it, and the list that carries it to image and buffer creation.
struct Profile {
    Profile() = default;
    Profile(Profile const&) = delete;
    Profile& operator=(Profile const&) = delete;

    VkVideoDecodeVP9ProfileInfoKHR vp9 { st_video_decode_vp9_profile_info, nullptr, 0 };
    VkVideoProfileInfoKHR profile { st_video_profile_info, &vp9, codec_decode_vp9, chroma_subsampling_420, component_depth_8, component_depth_8 };
    VkVideoProfileListInfoKHR list { st_video_profile_list_info, nullptr, 1, &profile };
};

// A physical device that decodes VP9, and what it was found to offer.
struct Candidate {
    VkPhysicalDevice device = nullptr;
    std::string name;
    int rank = -1;
    std::uint32_t decode_family = 0;
    std::uint32_t copy_family = 0;
    VkVideoCapabilitiesKHR capabilities {};
};

class VulkanVp9 final : public Vp9Accelerator {
public:
    VulkanVp9() { m_slots.fill(-1); }
    VulkanVp9(VulkanVp9 const&) = delete;
    VulkanVp9& operator=(VulkanVp9 const&) = delete;

    ~VulkanVp9() override
    {
        if (m_device) {
            m_vk.device_wait_idle(m_device);
            release_session();
            release_buffer(m_bitstream);
            release_buffer(m_readback);
            if (m_fence)
                m_vk.destroy_fence(m_device, m_fence, nullptr);
            if (m_copy_pool)
                m_vk.destroy_command_pool(m_device, m_copy_pool, nullptr);
            if (m_decode_pool)
                m_vk.destroy_command_pool(m_device, m_decode_pool, nullptr);
            m_vk.destroy_device(m_device, nullptr);
        }
        if (m_instance)
            m_vk.destroy_instance(m_instance, nullptr);
        if (m_vk.library)
            dlclose(m_vk.library);
    }

    bool open(std::string& error)
    {
        if (!load(error))
            return false;
        std::optional<Candidate> const chosen = choose(error);
        return chosen && create_device(*chosen, error);
    }

    std::string const& device() const override { return m_name; }

    void reset() override
    {
        m_slots.fill(-1);
        m_last = -1;
        m_previous = {};
        m_reset_pending = true;
    }

    bool decode(std::span<std::uint8_t const> frame, Vp9FrameHeader const& header) override
    {
        if (m_lost || header.show_existing_frame || header.profile != 0 || header.bit_depth != 8 || !header.subsampling_x || !header.subsampling_y)
            return false;
        auto const width = static_cast<std::uint32_t>(header.width);
        auto const height = static_cast<std::uint32_t>(header.height);
        if (width < m_capabilities.minCodedExtent.width || height < m_capabilities.minCodedExtent.height
            || width > m_capabilities.maxCodedExtent.width || height > m_capabilities.maxCodedExtent.height)
            return false;
        // Pictures as large as the stream needs; a key frame of a larger
        // size starts them afresh, which it may, since it needs no
        // references.
        if (m_session == 0 || width > m_extent.width || height > m_extent.height) {
            if (!header.key_frame && m_session != 0)
                return false;
            if (!create_session(width, height))
                return false;
        }
        int const target = free_picture();
        if (target < 0)
            return false;

        // The references this frame reads, by name, each DPB slot listed
        // once; the slot index of a picture is its index.
        std::array<std::int32_t, 3> names { -1, -1, -1 };
        std::array<VkVideoReferenceSlotInfoKHR, 4> slots {};
        std::uint32_t reference_count = 0;
        if (!header.intra()) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                int const picture = m_slots[static_cast<std::size_t>(header.ref_frame_idx[i])];
                if (picture < 0)
                    return false; // a reference slot never filled: the stream is broken
                names[i] = picture;
                bool const listed = std::any_of(slots.begin(), slots.begin() + reference_count,
                    [picture](VkVideoReferenceSlotInfoKHR const& slot) { return slot.slotIndex == picture; });
                if (!listed)
                    slots[reference_count++] = { st_video_reference_slot_info, nullptr, picture, &m_resources[static_cast<std::size_t>(picture)] };
            }
        }
        VkVideoPictureResourceInfoKHR& resource = m_resources[static_cast<std::size_t>(target)];
        resource.codedExtent = { width, height };
        VkVideoReferenceSlotInfoKHR const setup { st_video_reference_slot_info, nullptr, target, &resource };
        // Bound for the coding scope: the references, and the target, which
        // has no slot until the decode activates one.
        std::array<VkVideoReferenceSlotInfoKHR, 4> bound = slots;
        bound[reference_count] = setup;
        bound[reference_count].slotIndex = -1;

        // The bitstream, padded with zeros to the size the decoder reads in.
        std::uint64_t const range = align_up(frame.size(), m_capabilities.minBitstreamBufferSizeAlignment);
        if (!ensure_buffer(m_bitstream, range, buffer_usage_decode_src, &m_profile.list, 0))
            return false;
        std::memcpy(m_bitstream.mapped, frame.data(), frame.size());
        std::memset(m_bitstream.mapped + frame.size(), 0, static_cast<std::size_t>(range - frame.size()));

        StdVideoVP9ColorConfig color {};
        color.flags.color_range = header.color_range;
        color.BitDepth = 8;
        color.subsampling_x = 1;
        color.subsampling_y = 1;
        color.color_space = header.color_space;

        Vp9LoopFilter const& lf = header.loop_filter;
        StdVideoVP9LoopFilter filter {};
        filter.flags.loop_filter_delta_enabled = lf.delta_enabled;
        filter.flags.loop_filter_delta_update = lf.delta_update;
        filter.loop_filter_level = static_cast<std::uint8_t>(lf.level);
        filter.loop_filter_sharpness = static_cast<std::uint8_t>(lf.sharpness);
        for (std::size_t i = 0; i < lf.ref_deltas.size(); ++i) {
            filter.loop_filter_ref_deltas[i] = static_cast<std::int8_t>(lf.ref_deltas[i]);
            if (lf.ref_deltas_updated[i])
                filter.update_ref_delta = static_cast<std::uint8_t>(filter.update_ref_delta | (1u << i));
        }
        for (std::size_t i = 0; i < lf.mode_deltas.size(); ++i) {
            filter.loop_filter_mode_deltas[i] = static_cast<std::int8_t>(lf.mode_deltas[i]);
            if (lf.mode_deltas_updated[i])
                filter.update_mode_delta = static_cast<std::uint8_t>(filter.update_mode_delta | (1u << i));
        }

        Vp9Segmentation const& seg = header.segmentation;
        StdVideoVP9Segmentation segmentation {};
        segmentation.flags.segmentation_update_map = seg.update_map;
        segmentation.flags.segmentation_temporal_update = seg.temporal_update;
        segmentation.flags.segmentation_update_data = seg.update_data;
        segmentation.flags.segmentation_abs_or_delta_update = seg.abs_or_delta_update;
        std::copy(seg.tree_probs.begin(), seg.tree_probs.end(), segmentation.segmentation_tree_probs);
        std::copy(seg.pred_probs.begin(), seg.pred_probs.end(), segmentation.segmentation_pred_prob);
        for (std::size_t s = 0; s < 8; ++s) {
            for (std::size_t f = 0; f < 4; ++f) {
                if (seg.feature_enabled[s][f])
                    segmentation.FeatureEnabled[s] = static_cast<std::uint8_t>(segmentation.FeatureEnabled[s] | (1u << f));
                segmentation.FeatureData[s][f] = static_cast<std::int16_t>(seg.feature_data[s][f]);
            }
        }

        StdVideoDecodeVP9PictureInfo picture {};
        picture.flags.error_resilient_mode = header.error_resilient;
        picture.flags.intra_only = header.intra_only;
        picture.flags.allow_high_precision_mv = header.allow_high_precision_mv;
        picture.flags.refresh_frame_context = header.refresh_frame_context;
        picture.flags.frame_parallel_decoding_mode = header.frame_parallel_decoding;
        picture.flags.segmentation_enabled = seg.enabled;
        picture.flags.show_frame = header.show_frame;
        // The motion vectors of the frame decoded before this one (not one
        // shown again, which decodes nothing) are candidates only when it
        // was shown, had this size, and had motion to give (§7.2). NVIDIA's
        // decoder works this out for itself — forced either way, its
        // pictures do not change (measured 2026-09-23) — so only a driver
        // that reads the flag tests this line.
        picture.flags.UsePrevFrameMvs = !header.intra() && !header.error_resilient && m_previous.decoded && m_previous.shown
            && !m_previous.intra && m_previous.width == header.width && m_previous.height == header.height;
        picture.profile = 0;
        picture.frame_type = header.key_frame ? 0 : 1;
        picture.frame_context_idx = static_cast<std::uint8_t>(header.frame_context_idx);
        picture.reset_frame_context = static_cast<std::uint8_t>(header.reset_frame_context);
        picture.refresh_frame_flags = header.refresh_frame_flags;
        for (std::size_t reference = 1; reference < header.ref_sign_bias.size(); ++reference) {
            if (header.ref_sign_bias[reference])
                picture.ref_frame_sign_bias_mask = static_cast<std::uint8_t>(picture.ref_frame_sign_bias_mask | (1u << reference));
        }
        picture.interpolation_filter = header.interp_filter;
        picture.base_q_idx = static_cast<std::uint8_t>(header.base_q_idx);
        picture.delta_q_y_dc = static_cast<std::int8_t>(header.delta_q_y_dc);
        picture.delta_q_uv_dc = static_cast<std::int8_t>(header.delta_q_uv_dc);
        picture.delta_q_uv_ac = static_cast<std::int8_t>(header.delta_q_uv_ac);
        picture.tile_cols_log2 = static_cast<std::uint8_t>(header.tile_cols_log2);
        picture.tile_rows_log2 = static_cast<std::uint8_t>(header.tile_rows_log2);
        picture.pColorConfig = &color;
        picture.pLoopFilter = &filter;
        picture.pSegmentation = &segmentation;

        VkVideoDecodeVP9PictureInfoKHR vp9 {};
        vp9.sType = st_video_decode_vp9_picture_info;
        vp9.pStdPictureInfo = &picture;
        std::copy(names.begin(), names.end(), vp9.referenceNameSlotIndices);
        vp9.uncompressedHeaderOffset = 0;
        vp9.compressedHeaderOffset = static_cast<std::uint32_t>(header.uncompressed_header_size);
        vp9.tilesOffset = static_cast<std::uint32_t>(header.uncompressed_header_size + header.compressed_header_size);

        VkVideoDecodeInfoKHR info {};
        info.sType = st_video_decode_info;
        info.pNext = &vp9;
        info.srcBuffer = m_bitstream.buffer;
        info.srcBufferOffset = 0;
        info.srcBufferRange = range;
        info.dstPictureResource = resource;
        info.pSetupReferenceSlot = &setup;
        info.referenceSlotCount = reference_count;
        info.pReferenceSlots = slots.data();

        VkCommandBufferBeginInfo const begin { st_command_buffer_begin_info, nullptr, command_buffer_one_time_submit, nullptr };
        if (m_vk.begin_command_buffer(m_decode_commands, &begin) != vk_success)
            return false;
        // What went before (earlier decodes, the copies' layout changes)
        // seen by this decode; the target's old contents discarded.
        VkMemoryBarrier2 const before { st_memory_barrier_2, nullptr, stage_all_commands, access_memory_write, stage_video_decode,
            access_decode_read | access_decode_write };
        VkImageMemoryBarrier2 const to_dpb = image_barrier(m_pictures[static_cast<std::size_t>(target)].image, stage_all_commands, 0,
            stage_video_decode, access_decode_read | access_decode_write, layout_undefined, layout_decode_dpb);
        VkDependencyInfo const dependency { st_dependency_info, nullptr, 0, 1, &before, 0, nullptr, 1, &to_dpb };
        m_vk.cmd_pipeline_barrier2(m_decode_commands, &dependency);
        VkVideoBeginCodingInfoKHR const coding { st_video_begin_coding_info, nullptr, 0, m_session, 0, reference_count + 1, bound.data() };
        m_vk.cmd_begin_video_coding(m_decode_commands, &coding);
        if (m_reset_pending) {
            VkVideoCodingControlInfoKHR const control { st_video_coding_control_info, nullptr, coding_control_reset };
            m_vk.cmd_control_video_coding(m_decode_commands, &control);
        }
        m_vk.cmd_decode_video(m_decode_commands, &info);
        VkVideoEndCodingInfoKHR const end { st_video_end_coding_info, nullptr, 0 };
        m_vk.cmd_end_video_coding(m_decode_commands, &end);
        if (m_vk.end_command_buffer(m_decode_commands) != vk_success || !submit_and_wait(m_decode_queue, m_decode_commands))
            return false;
        m_reset_pending = false;

        Picture& decoded = m_pictures[static_cast<std::size_t>(target)];
        decoded.width = header.width;
        decoded.height = header.height;
        for (std::size_t slot = 0; slot < m_slots.size(); ++slot) {
            if (header.refresh_frame_flags & (1u << slot))
                m_slots[slot] = target;
        }
        m_last = target;
        m_previous = { true, header.show_frame, header.intra(), header.width, header.height };
        return true;
    }

    std::optional<Nv12Picture> read_last() override { return m_last >= 0 ? read(m_last) : std::nullopt; }

    std::optional<Nv12Picture> read_slot(int slot) override
    {
        if (slot < 0 || slot > 7 || m_slots[static_cast<std::size_t>(slot)] < 0)
            return std::nullopt;
        return read(m_slots[static_cast<std::size_t>(slot)]);
    }

private:
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

    // What the frame before told about itself (UsePrevFrameMvs).
    struct Previous {
        bool decoded = false;
        bool shown = false;
        bool intra = false;
        int width = 0;
        int height = 0;
    };

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

    bool load(std::string& error)
    {
        m_vk.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!m_vk.library) {
            error = "Vulkan is not installed (libvulkan.so.1)";
            return false;
        }
        m_vk.get_instance_proc_addr = reinterpret_cast<decltype(m_vk.get_instance_proc_addr)>(dlsym(m_vk.library, "vkGetInstanceProcAddr"));
        if (!m_vk.get_instance_proc_addr || !instance_function("vkCreateInstance", m_vk.create_instance)) {
            error = "the Vulkan loader has no entry point";
            return false;
        }
        std::uint32_t version = 0;
        if (!instance_function("vkEnumerateInstanceVersion", m_vk.enumerate_instance_version) || m_vk.enumerate_instance_version(&version) != vk_success
            || version < api_version_1_3) {
            error = "the Vulkan loader is older than 1.3";
            return false;
        }
        VkApplicationInfo const application { st_application_info, nullptr, "Sashfold", 1, "Sashfold", 1, api_version_1_3 };
        VkInstanceCreateInfo const info { st_instance_create_info, nullptr, 0, &application, 0, nullptr, 0, nullptr };
        if (m_vk.create_instance(&info, nullptr, &m_instance) != vk_success) {
            m_instance = nullptr;
            error = "no Vulkan driver would start";
            return false;
        }
        bool const all = instance_function("vkDestroyInstance", m_vk.destroy_instance)
            && instance_function("vkEnumeratePhysicalDevices", m_vk.enumerate_physical_devices)
            && instance_function("vkGetPhysicalDeviceProperties", m_vk.get_physical_device_properties)
            && instance_function("vkGetPhysicalDeviceQueueFamilyProperties2", m_vk.get_physical_device_queue_family_properties2)
            && instance_function("vkEnumerateDeviceExtensionProperties", m_vk.enumerate_device_extension_properties)
            && instance_function("vkGetPhysicalDeviceFeatures2", m_vk.get_physical_device_features2)
            && instance_function("vkGetPhysicalDeviceMemoryProperties", m_vk.get_physical_device_memory_properties)
            && instance_function("vkCreateDevice", m_vk.create_device) && instance_function("vkGetDeviceProcAddr", m_vk.get_device_proc_addr);
        if (!all) {
            error = "the Vulkan loader is missing an entry point";
            return false;
        }
        // A loader that predates Vulkan Video has none of these.
        if (!instance_function("vkGetPhysicalDeviceVideoCapabilitiesKHR", m_vk.get_physical_device_video_capabilities)
            || !instance_function("vkGetPhysicalDeviceVideoFormatPropertiesKHR", m_vk.get_physical_device_video_format_properties)) {
            error = "the Vulkan loader predates Vulkan Video";
            return false;
        }
        return true;
    }

    // Why the device cannot decode VP9 here, or nothing when it can.
    std::optional<std::string> examine(VkPhysicalDevice device, VkPhysicalDeviceProperties const& properties, Candidate& candidate)
    {
        if (properties.deviceType == device_cpu)
            return "a CPU implementation";
        if (properties.apiVersion < api_version_1_3)
            return "Vulkan older than 1.3";
        std::uint32_t count = 0;
        m_vk.enumerate_device_extension_properties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        if (m_vk.enumerate_device_extension_properties(device, nullptr, &count, extensions.data()) != vk_success)
            return "its extensions could not be listed";
        for (char const* wanted : required_extensions) {
            bool const present = std::any_of(extensions.begin(), extensions.begin() + count,
                [wanted](VkExtensionProperties const& extension) { return std::strcmp(extension.extensionName, wanted) == 0; });
            if (!present)
                return "no VP9 decoding";
        }
        VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9 { st_physical_device_video_decode_vp9_features, nullptr, 0 };
        VkPhysicalDeviceSynchronization2Features synchronization { st_physical_device_synchronization_2_features, &vp9, 0 };
        VkPhysicalDeviceFeatures2 features { st_physical_device_features_2, &synchronization, {} };
        m_vk.get_physical_device_features2(device, &features);
        if (!vp9.videoDecodeVP9 || !synchronization.synchronization2)
            return "no VP9 decoding";

        m_vk.get_physical_device_queue_family_properties2(device, &count, nullptr);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(count, { st_queue_family_video_properties, nullptr, 0 });
        std::vector<VkQueueFamilyProperties2> families(count);
        for (std::uint32_t i = 0; i < count; ++i)
            families[i] = { st_queue_family_properties_2, &video[i], {} };
        m_vk.get_physical_device_queue_family_properties2(device, &count, families.data());
        std::optional<std::uint32_t> decode_family;
        std::optional<std::uint32_t> copy_family;
        for (std::uint32_t i = 0; i < count; ++i) {
            VkFlags const flags = families[i].queueFamilyProperties.queueFlags;
            if (!decode_family && (flags & queue_video_decode) && (video[i].videoCodecOperations & codec_decode_vp9))
                decode_family = i;
            if (!copy_family && (flags & (queue_graphics | queue_compute | queue_transfer)))
                copy_family = i;
        }
        if (!decode_family)
            return "no queue that decodes VP9";
        // Copies out on the decoding queue when it copies at all.
        if (families[*decode_family].queueFamilyProperties.queueFlags & queue_transfer)
            copy_family = decode_family;
        if (!copy_family)
            return "no queue to copy pictures out on";

        VkVideoDecodeVP9CapabilitiesKHR vp9_capabilities { st_video_decode_vp9_capabilities, nullptr, 0 };
        VkVideoDecodeCapabilitiesKHR decode_capabilities { st_video_decode_capabilities, &vp9_capabilities, 0 };
        VkVideoCapabilitiesKHR capabilities {};
        capabilities.sType = st_video_capabilities;
        capabilities.pNext = &decode_capabilities;
        if (m_vk.get_physical_device_video_capabilities(device, &m_profile.profile, &capabilities) != vk_success)
            return "VP9 profile 0 not decoded";
        if (!(decode_capabilities.flags & decode_output_coincides))
            return "writes decoded pictures apart from its references, which is not written yet";
        if (!(capabilities.flags & capability_separate_reference_images))
            return "keeps its references in one layered image, which is not written yet";
        if (capabilities.maxDpbSlots < picture_count || capabilities.maxActiveReferencePictures < 3)
            return "too few reference pictures";

        // The pictures' format: NV12, in images the decoder writes, reads as
        // references, and lets be copied out.
        VkPhysicalDeviceVideoFormatInfoKHR const format_info { st_physical_device_video_format_info, &m_profile.list,
            image_usage_decode_dpb | image_usage_decode_dst };
        count = 0;
        m_vk.get_physical_device_video_format_properties(device, &format_info, &count, nullptr);
        std::vector<VkVideoFormatPropertiesKHR> formats(count);
        for (auto& format : formats)
            format.sType = st_video_format_properties;
        VkResult const listed = m_vk.get_physical_device_video_format_properties(device, &format_info, &count, formats.data());
        bool const nv12 = (listed == vk_success || listed == vk_incomplete)
            && std::any_of(formats.begin(), formats.begin() + std::min<std::size_t>(count, formats.size()), [](VkVideoFormatPropertiesKHR const& format) {
                   return format.format == format_nv12 && format.imageTiling == tiling_optimal && (format.imageUsageFlags & image_usage_transfer_src);
               });
        if (!nv12)
            return "its pictures cannot be copied out as NV12";

        candidate.device = device;
        candidate.name = properties.deviceName;
        candidate.decode_family = *decode_family;
        candidate.copy_family = *copy_family;
        candidate.capabilities = capabilities;
        candidate.rank = properties.deviceType == device_discrete ? 3
            : properties.deviceType == device_integrated          ? 2
            : properties.deviceType == device_virtual             ? 1
                                                                  : 0;
        return std::nullopt;
    }

    std::optional<Candidate> choose(std::string& error)
    {
        std::uint32_t count = 0;
        m_vk.enumerate_physical_devices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        if (count == 0 || m_vk.enumerate_physical_devices(m_instance, &count, devices.data()) != vk_success) {
            error = "no Vulkan device";
            return std::nullopt;
        }
        std::optional<Candidate> best;
        std::string tried;
        for (std::uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties properties {};
            m_vk.get_physical_device_properties(devices[i], &properties);
            Candidate candidate;
            if (std::optional<std::string> const why = examine(devices[i], properties, candidate)) {
                tried += (tried.empty() ? "" : ", ") + std::string(properties.deviceName) + " (" + *why + ")";
                continue;
            }
            if (!best || candidate.rank > best->rank)
                best = candidate;
        }
        if (!best)
            error = "no device decodes VP9: " + tried;
        return best;
    }

    bool create_device(Candidate const& chosen, std::string& error)
    {
        m_physical = chosen.device;
        m_capabilities = chosen.capabilities;
        m_decode_family = chosen.decode_family;
        m_copy_family = chosen.copy_family;
        float const priority = 1.0f;
        std::array<VkDeviceQueueCreateInfo, 2> const queues { {
            { st_device_queue_create_info, nullptr, 0, m_decode_family, 1, &priority },
            { st_device_queue_create_info, nullptr, 0, m_copy_family, 1, &priority },
        } };
        VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9 { st_physical_device_video_decode_vp9_features, nullptr, 1 };
        VkPhysicalDeviceSynchronization2Features synchronization { st_physical_device_synchronization_2_features, &vp9, 1 };
        VkPhysicalDeviceFeatures2 const features { st_physical_device_features_2, &synchronization, {} };
        VkDeviceCreateInfo const info { st_device_create_info, &features, 0, m_copy_family == m_decode_family ? 1u : 2u, queues.data(), 0, nullptr,
            static_cast<std::uint32_t>(required_extensions.size()), required_extensions.data(), nullptr };
        if (m_vk.create_device(m_physical, &info, nullptr, &m_device) != vk_success) {
            m_device = nullptr;
            error = chosen.name + " would not open for decoding";
            return false;
        }
        bool const all = device_function("vkDestroyDevice", m_vk.destroy_device) && device_function("vkGetDeviceQueue", m_vk.get_device_queue)
            && device_function("vkDeviceWaitIdle", m_vk.device_wait_idle) && device_function("vkCreateCommandPool", m_vk.create_command_pool)
            && device_function("vkDestroyCommandPool", m_vk.destroy_command_pool)
            && device_function("vkAllocateCommandBuffers", m_vk.allocate_command_buffers)
            && device_function("vkBeginCommandBuffer", m_vk.begin_command_buffer) && device_function("vkEndCommandBuffer", m_vk.end_command_buffer)
            && device_function("vkQueueSubmit", m_vk.queue_submit) && device_function("vkCreateFence", m_vk.create_fence)
            && device_function("vkDestroyFence", m_vk.destroy_fence) && device_function("vkWaitForFences", m_vk.wait_for_fences)
            && device_function("vkResetFences", m_vk.reset_fences) && device_function("vkAllocateMemory", m_vk.allocate_memory)
            && device_function("vkFreeMemory", m_vk.free_memory) && device_function("vkMapMemory", m_vk.map_memory)
            && device_function("vkCreateBuffer", m_vk.create_buffer) && device_function("vkDestroyBuffer", m_vk.destroy_buffer)
            && device_function("vkGetBufferMemoryRequirements", m_vk.get_buffer_memory_requirements)
            && device_function("vkBindBufferMemory", m_vk.bind_buffer_memory) && device_function("vkCreateImage", m_vk.create_image)
            && device_function("vkDestroyImage", m_vk.destroy_image)
            && device_function("vkGetImageMemoryRequirements", m_vk.get_image_memory_requirements)
            && device_function("vkBindImageMemory", m_vk.bind_image_memory) && device_function("vkCreateImageView", m_vk.create_image_view)
            && device_function("vkDestroyImageView", m_vk.destroy_image_view)
            && device_function("vkCmdPipelineBarrier2", m_vk.cmd_pipeline_barrier2)
            && device_function("vkCmdCopyImageToBuffer", m_vk.cmd_copy_image_to_buffer)
            && device_function("vkCreateVideoSessionKHR", m_vk.create_video_session)
            && device_function("vkDestroyVideoSessionKHR", m_vk.destroy_video_session)
            && device_function("vkGetVideoSessionMemoryRequirementsKHR", m_vk.get_video_session_memory_requirements)
            && device_function("vkBindVideoSessionMemoryKHR", m_vk.bind_video_session_memory)
            && device_function("vkCmdBeginVideoCodingKHR", m_vk.cmd_begin_video_coding)
            && device_function("vkCmdEndVideoCodingKHR", m_vk.cmd_end_video_coding)
            && device_function("vkCmdControlVideoCodingKHR", m_vk.cmd_control_video_coding)
            && device_function("vkCmdDecodeVideoKHR", m_vk.cmd_decode_video);
        if (!all) {
            if (m_vk.destroy_device)
                m_vk.destroy_device(m_device, nullptr);
            m_device = nullptr;
            error = chosen.name + " is missing a video entry point";
            return false;
        }
        m_vk.get_device_queue(m_device, m_decode_family, 0, &m_decode_queue);
        m_vk.get_device_queue(m_device, m_copy_family, 0, &m_copy_queue);
        m_vk.get_physical_device_memory_properties(m_physical, &m_memory);
        VkCommandPoolCreateInfo const decode_pool { st_command_pool_create_info, nullptr, pool_reset_command_buffer, m_decode_family };
        VkCommandPoolCreateInfo const copy_pool { st_command_pool_create_info, nullptr, pool_reset_command_buffer, m_copy_family };
        VkFenceCreateInfo const fence { st_fence_create_info, nullptr, 0 };
        bool ready = m_vk.create_command_pool(m_device, &decode_pool, nullptr, &m_decode_pool) == vk_success
            && m_vk.create_command_pool(m_device, &copy_pool, nullptr, &m_copy_pool) == vk_success
            && m_vk.create_fence(m_device, &fence, nullptr, &m_fence) == vk_success;
        if (ready) {
            VkCommandBufferAllocateInfo const decode_commands { st_command_buffer_allocate_info, nullptr, m_decode_pool, command_buffer_primary, 1 };
            VkCommandBufferAllocateInfo const copy_commands { st_command_buffer_allocate_info, nullptr, m_copy_pool, command_buffer_primary, 1 };
            ready = m_vk.allocate_command_buffers(m_device, &decode_commands, &m_decode_commands) == vk_success
                && m_vk.allocate_command_buffers(m_device, &copy_commands, &m_copy_commands) == vk_success;
        }
        if (!ready) {
            error = chosen.name + " would not give command buffers";
            return false;
        }
        m_name = chosen.name + " (Vulkan Video)";
        return true;
    }

    std::optional<std::uint32_t> memory_type(std::uint32_t allowed, VkFlags wanted, VkFlags preferred) const
    {
        std::optional<std::uint32_t> fallback;
        for (std::uint32_t i = 0; i < m_memory.memoryTypeCount; ++i) {
            VkFlags const flags = m_memory.memoryTypes[i].propertyFlags;
            if (!(allowed & (1u << i)) || (flags & wanted) != wanted)
                continue;
            if ((flags & preferred) == preferred)
                return i;
            if (!fallback)
                fallback = i;
        }
        return fallback;
    }

    VkDeviceMemory allocate(VkMemoryRequirements const& requirements, VkFlags wanted, VkFlags preferred)
    {
        std::optional<std::uint32_t> const type = memory_type(requirements.memoryTypeBits, wanted, preferred);
        if (!type)
            return 0;
        VkMemoryAllocateInfo const info { st_memory_allocate_info, nullptr, requirements.size, *type };
        VkDeviceMemory memory = 0;
        return m_vk.allocate_memory(m_device, &info, nullptr, &memory) == vk_success ? memory : 0;
    }

    // A buffer the host writes or reads through a lasting mapping, grown to
    // at least `size`.
    bool ensure_buffer(HostBuffer& buffer, std::uint64_t size, VkFlags usage, void const* next, VkFlags preferred)
    {
        if (buffer.buffer != 0 && buffer.size >= size)
            return true;
        release_buffer(buffer);
        std::uint64_t const capacity = std::max<std::uint64_t>(size, 1 << 20);
        VkBufferCreateInfo const info { st_buffer_create_info, next, 0, capacity, usage, sharing_exclusive, 0, nullptr };
        if (m_vk.create_buffer(m_device, &info, nullptr, &buffer.buffer) != vk_success) {
            buffer.buffer = 0;
            return false;
        }
        VkMemoryRequirements requirements {};
        m_vk.get_buffer_memory_requirements(m_device, buffer.buffer, &requirements);
        buffer.memory = allocate(requirements, memory_host_visible | memory_host_coherent, preferred);
        void* mapped = nullptr;
        if (buffer.memory == 0 || m_vk.bind_buffer_memory(m_device, buffer.buffer, buffer.memory, 0) != vk_success
            || m_vk.map_memory(m_device, buffer.memory, 0, capacity, 0, &mapped) != vk_success) {
            release_buffer(buffer);
            return false;
        }
        buffer.mapped = static_cast<std::uint8_t*>(mapped);
        buffer.size = capacity;
        return true;
    }

    void release_buffer(HostBuffer& buffer)
    {
        if (buffer.buffer)
            m_vk.destroy_buffer(m_device, buffer.buffer, nullptr);
        if (buffer.memory)
            m_vk.free_memory(m_device, buffer.memory, nullptr); // unmaps it too
        buffer = {};
    }

    void release_session()
    {
        for (Picture& picture : m_pictures) {
            if (picture.view)
                m_vk.destroy_image_view(m_device, picture.view, nullptr);
            if (picture.image)
                m_vk.destroy_image(m_device, picture.image, nullptr);
            if (picture.memory)
                m_vk.free_memory(m_device, picture.memory, nullptr);
        }
        m_pictures.clear();
        m_resources.clear();
        if (m_session)
            m_vk.destroy_video_session(m_device, m_session, nullptr);
        m_session = 0;
        for (VkDeviceMemory const memory : m_session_memory)
            m_vk.free_memory(m_device, memory, nullptr);
        m_session_memory.clear();
        m_extent = {};
        reset();
    }

    bool create_session(std::uint32_t width, std::uint32_t height)
    {
        m_vk.device_wait_idle(m_device);
        release_session();
        // The pictures as large as the frame, rounded up to the block the
        // decoder writes at once, and no smaller than the least it takes.
        VkExtent2D const extent {
            std::max(static_cast<std::uint32_t>(align_up(width, std::max(2u, m_capabilities.pictureAccessGranularity.width))),
                m_capabilities.minCodedExtent.width),
            std::max(static_cast<std::uint32_t>(align_up(height, std::max(2u, m_capabilities.pictureAccessGranularity.height))),
                m_capabilities.minCodedExtent.height),
        };
        VkExtensionProperties std_header {};
        std::strcpy(std_header.extensionName, "VK_STD_vulkan_video_codec_vp9_decode");
        std_header.specVersion = vp9_std_version_1_0_0;
        VkVideoSessionCreateInfoKHR const info { st_video_session_create_info, nullptr, m_decode_family, 0, &m_profile.profile, format_nv12, extent,
            format_nv12, picture_count, 3, &std_header };
        if (m_vk.create_video_session(m_device, &info, nullptr, &m_session) != vk_success) {
            m_session = 0;
            return false;
        }
        std::uint32_t count = 0;
        m_vk.get_video_session_memory_requirements(m_device, m_session, &count, nullptr);
        std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(count, { st_video_session_memory_requirements, nullptr, 0, {} });
        if (m_vk.get_video_session_memory_requirements(m_device, m_session, &count, requirements.data()) != vk_success) {
            release_session();
            return false;
        }
        std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
        for (VkVideoSessionMemoryRequirementsKHR const& requirement : requirements) {
            VkDeviceMemory const memory = allocate(requirement.memoryRequirements, 0, memory_device_local);
            if (memory == 0) {
                release_session();
                return false;
            }
            m_session_memory.push_back(memory);
            binds.push_back({ st_bind_video_session_memory_info, nullptr, requirement.memoryBindIndex, memory, 0, requirement.memoryRequirements.size });
        }
        if (!binds.empty()
            && m_vk.bind_video_session_memory(m_device, m_session, static_cast<std::uint32_t>(binds.size()), binds.data()) != vk_success) {
            release_session();
            return false;
        }

        // Each picture an image of its own, which the decoder writes and
        // reads as a reference, and the copy reads; shared by both queue
        // families where they differ.
        std::array<std::uint32_t, 2> const families { m_decode_family, m_copy_family };
        bool const shared = m_decode_family != m_copy_family;
        m_pictures.resize(picture_count);
        m_resources.resize(picture_count);
        for (std::uint32_t i = 0; i < picture_count; ++i) {
            Picture& picture = m_pictures[i];
            VkImageCreateInfo const image { st_image_create_info, &m_profile.list, 0, image_type_2d, format_nv12, { extent.width, extent.height, 1 }, 1,
                1, sample_count_1, tiling_optimal, image_usage_decode_dpb | image_usage_decode_dst | image_usage_transfer_src,
                shared ? sharing_concurrent : sharing_exclusive, shared ? 2u : 0u, shared ? families.data() : nullptr, layout_undefined };
            if (m_vk.create_image(m_device, &image, nullptr, &picture.image) != vk_success) {
                picture.image = 0;
                release_session();
                return false;
            }
            VkMemoryRequirements memory {};
            m_vk.get_image_memory_requirements(m_device, picture.image, &memory);
            picture.memory = allocate(memory, 0, memory_device_local);
            if (picture.memory == 0 || m_vk.bind_image_memory(m_device, picture.image, picture.memory, 0) != vk_success) {
                release_session();
                return false;
            }
            VkImageViewCreateInfo const view { st_image_view_create_info, nullptr, 0, picture.image, view_type_2d, format_nv12, { 0, 0, 0, 0 },
                { aspect_color, 0, 1, 0, 1 } };
            if (m_vk.create_image_view(m_device, &view, nullptr, &picture.view) != vk_success) {
                picture.view = 0;
                release_session();
                return false;
            }
            m_resources[i] = { st_video_picture_resource_info, nullptr, { 0, 0 }, extent, 0, picture.view };
        }
        m_extent = extent;
        reset();
        return true;
    }

    // A picture no reference slot holds, other than the one decoded last
    // (which may still be read back).
    int free_picture() const
    {
        for (int i = 0; i < static_cast<int>(m_pictures.size()); ++i) {
            if (i != m_last && std::find(m_slots.begin(), m_slots.end(), i) == m_slots.end())
                return i;
        }
        return -1;
    }

    static VkImageMemoryBarrier2 image_barrier(VkImage image, VkFlags64 source_stage, VkFlags64 source_access, VkFlags64 destination_stage,
        VkFlags64 destination_access, std::int32_t from, std::int32_t to)
    {
        return { st_image_memory_barrier_2, nullptr, source_stage, source_access, destination_stage, destination_access, from, to,
            queue_family_ignored, queue_family_ignored, image, { aspect_color, 0, 1, 0, 1 } };
    }

    bool submit_and_wait(VkQueue queue, VkCommandBuffer commands)
    {
        VkSubmitInfo submit {};
        submit.sType = st_submit_info;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (m_vk.queue_submit(queue, 1, &submit, m_fence) != vk_success) {
            m_lost = true;
            return false;
        }
        if (m_vk.wait_for_fences(m_device, 1, &m_fence, 1, fence_timeout_ns) != vk_success) {
            m_lost = true; // hung or lost: nothing more is asked of it
            return false;
        }
        m_vk.reset_fences(m_device, 1, &m_fence);
        return true;
    }

    std::optional<Nv12Picture> read(int index)
    {
        Picture const& picture = m_pictures[static_cast<std::size_t>(index)];
        if (m_lost || picture.width <= 0 || picture.height <= 0)
            return std::nullopt;
        // The whole image, a plane at a time — a queue that only copies may
        // copy only whole blocks of it — cropped to the frame below.
        std::uint64_t const luma_size = std::uint64_t { m_extent.width } * m_extent.height;
        std::uint64_t const chroma_offset = align_up(luma_size, 4);
        std::uint64_t const chroma_size = std::uint64_t { m_extent.width / 2 } * 2 * (m_extent.height / 2);
        if (!ensure_buffer(m_readback, chroma_offset + chroma_size, buffer_usage_transfer_dst, nullptr, memory_host_cached))
            return std::nullopt;

        VkCommandBufferBeginInfo const begin { st_command_buffer_begin_info, nullptr, command_buffer_one_time_submit, nullptr };
        if (m_vk.begin_command_buffer(m_copy_commands, &begin) != vk_success)
            return std::nullopt;
        VkMemoryBarrier2 const decoded { st_memory_barrier_2, nullptr, stage_all_commands, access_memory_write, stage_copy, access_transfer_read };
        VkImageMemoryBarrier2 const to_copy
            = image_barrier(picture.image, stage_all_commands, 0, stage_copy, access_transfer_read, layout_decode_dpb, layout_transfer_src);
        VkDependencyInfo const before { st_dependency_info, nullptr, 0, 1, &decoded, 0, nullptr, 1, &to_copy };
        m_vk.cmd_pipeline_barrier2(m_copy_commands, &before);
        std::array<VkBufferImageCopy, 2> const regions { {
            { 0, 0, 0, { aspect_plane_0, 0, 0, 1 }, { 0, 0, 0 }, { m_extent.width, m_extent.height, 1 } },
            { chroma_offset, 0, 0, { aspect_plane_1, 0, 0, 1 }, { 0, 0, 0 }, { m_extent.width / 2, m_extent.height / 2, 1 } },
        } };
        m_vk.cmd_copy_image_to_buffer(m_copy_commands, picture.image, layout_transfer_src, m_readback.buffer, 2, regions.data());
        // Back to a reference for the frames after, and the copy seen by
        // the host.
        VkMemoryBarrier2 const copied { st_memory_barrier_2, nullptr, stage_copy, access_transfer_write, stage_host, access_host_read };
        VkImageMemoryBarrier2 const to_dpb = image_barrier(picture.image, stage_copy, 0, stage_all_commands, 0, layout_transfer_src, layout_decode_dpb);
        VkDependencyInfo const after { st_dependency_info, nullptr, 0, 1, &copied, 0, nullptr, 1, &to_dpb };
        m_vk.cmd_pipeline_barrier2(m_copy_commands, &after);
        if (m_vk.end_command_buffer(m_copy_commands) != vk_success || !submit_and_wait(m_copy_queue, m_copy_commands))
            return std::nullopt;

        Nv12Picture out;
        out.width = picture.width;
        out.height = picture.height;
        auto const width = static_cast<std::size_t>(picture.width);
        auto const pitch = static_cast<std::size_t>(m_extent.width);
        out.luma.resize(width * static_cast<std::size_t>(picture.height));
        for (std::size_t y = 0; y < static_cast<std::size_t>(picture.height); ++y)
            std::memcpy(out.luma.data() + y * width, m_readback.mapped + y * pitch, width);
        std::size_t const chroma_row = static_cast<std::size_t>(out.chroma_width()) * 2;
        std::size_t const chroma_pitch = static_cast<std::size_t>(m_extent.width / 2) * 2;
        out.chroma.resize(chroma_row * static_cast<std::size_t>(out.chroma_height()));
        for (std::size_t y = 0; y < static_cast<std::size_t>(out.chroma_height()); ++y)
            std::memcpy(out.chroma.data() + y * chroma_row, m_readback.mapped + chroma_offset + y * chroma_pitch, chroma_row);
        return out;
    }

    Vk m_vk;
    Profile m_profile;
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

    std::array<int, 8> m_slots {};
    int m_last = -1;
    Previous m_previous;
};

}

std::unique_ptr<Vp9Accelerator> open_vulkan_vp9(std::string& error)
{
    auto accelerator = std::make_unique<VulkanVp9>();
    if (!accelerator->open(error))
        return nullptr;
    return accelerator;
}

}
