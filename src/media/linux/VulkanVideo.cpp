#include "media/linux/VulkanVideo.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>

namespace sashfold::media::vulkan {

Device::~Device()
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

bool Device::open(Codec const& codec, std::string& error)
{
    m_codec = codec;
    m_extensions = { "VK_KHR_video_queue", "VK_KHR_video_decode_queue", codec.extension };
    if (!load(error))
        return false;
    std::optional<Candidate> const chosen = choose(error);
    return chosen && create_device(*chosen, error);
}

bool Device::load(std::string& error)
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

// Why the device cannot decode the codec here, or nothing when it can.
std::optional<std::string> Device::examine(VkPhysicalDevice device, VkPhysicalDeviceProperties const& properties, Candidate& candidate)
{
    std::string const no_decoding = std::string("no ") + m_codec.name + " decoding";
    if (properties.deviceType == device_cpu)
        return "a CPU implementation";
    if (properties.apiVersion < api_version_1_3)
        return "Vulkan older than 1.3";
    std::uint32_t count = 0;
    m_vk.enumerate_device_extension_properties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    if (m_vk.enumerate_device_extension_properties(device, nullptr, &count, extensions.data()) != vk_success)
        return "its extensions could not be listed";
    for (char const* wanted : m_extensions) {
        bool const present = std::any_of(extensions.begin(), extensions.begin() + count,
            [wanted](VkExtensionProperties const& extension) { return std::strcmp(extension.extensionName, wanted) == 0; });
        if (!present)
            return no_decoding;
    }
    // The codec's feature, where it has one, chained after synchronization2.
    std::array<std::uint8_t, 24> codec_features {};
    if (m_codec.features)
        std::memcpy(codec_features.data(), m_codec.features, codec_features.size());
    VkPhysicalDeviceSynchronization2Features synchronization { st_physical_device_synchronization_2_features, m_codec.features ? codec_features.data() : nullptr, 0 };
    VkPhysicalDeviceFeatures2 features { st_physical_device_features_2, &synchronization, {} };
    m_vk.get_physical_device_features2(device, &features);
    if (m_codec.features) {
        VkBool32 supported = 0;
        std::memcpy(&supported, codec_features.data() + 16, sizeof supported);
        if (!supported)
            return no_decoding;
    }
    if (!synchronization.synchronization2)
        return no_decoding;

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
        if (!decode_family && (flags & queue_video_decode) && (video[i].videoCodecOperations & m_codec.operation))
            decode_family = i;
        if (!copy_family && (flags & (queue_graphics | queue_compute | queue_transfer)))
            copy_family = i;
    }
    if (!decode_family)
        return std::string("no queue that decodes ") + m_codec.name;
    // Copies out on the decoding queue when it copies at all.
    if (families[*decode_family].queueFamilyProperties.queueFlags & queue_transfer)
        copy_family = decode_family;
    if (!copy_family)
        return "no queue to copy pictures out on";

    VkVideoDecodeCapabilitiesKHR decode_capabilities { st_video_decode_capabilities, m_codec.codec_capabilities, 0 };
    VkVideoCapabilitiesKHR capabilities {};
    capabilities.sType = st_video_capabilities;
    capabilities.pNext = &decode_capabilities;
    if (m_vk.get_physical_device_video_capabilities(device, m_codec.profile, &capabilities) != vk_success)
        return std::string(m_codec.name) + "'s profile not decoded";
    if (!(decode_capabilities.flags & decode_output_coincides))
        return "writes decoded pictures apart from its references, which is not written yet";
    if (!(capabilities.flags & capability_separate_reference_images))
        return "keeps its references in one layered image, which is not written yet";
    if (capabilities.maxDpbSlots < m_codec.pictures || capabilities.maxActiveReferencePictures < m_codec.active_references)
        return "too few reference pictures";

    // The pictures' format: NV12, in images the decoder writes, reads as
    // references, and lets be copied out.
    VkPhysicalDeviceVideoFormatInfoKHR const format_info { st_physical_device_video_format_info, m_codec.profile_list,
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

std::optional<Candidate> Device::choose(std::string& error)
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
        error = std::string("no device decodes ") + m_codec.name + ": " + tried;
    return best;
}

bool Device::create_device(Candidate const& chosen, std::string& error)
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
    // The codec's feature turned on, where it has one.
    std::array<std::uint8_t, 24> codec_features {};
    if (m_codec.features) {
        std::memcpy(codec_features.data(), m_codec.features, codec_features.size());
        VkBool32 const on = 1;
        std::memcpy(codec_features.data() + 16, &on, sizeof on);
    }
    VkPhysicalDeviceSynchronization2Features synchronization { st_physical_device_synchronization_2_features, m_codec.features ? codec_features.data() : nullptr, 1 };
    VkPhysicalDeviceFeatures2 const features { st_physical_device_features_2, &synchronization, {} };
    VkDeviceCreateInfo const info { st_device_create_info, &features, 0, m_copy_family == m_decode_family ? 1u : 2u, queues.data(), 0, nullptr,
        static_cast<std::uint32_t>(m_extensions.size()), m_extensions.data(), nullptr };
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
        && device_function("vkCmdDecodeVideoKHR", m_vk.cmd_decode_video)
        && device_function("vkCreateVideoSessionParametersKHR", m_vk.create_video_session_parameters)
        && device_function("vkDestroyVideoSessionParametersKHR", m_vk.destroy_video_session_parameters);
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

std::optional<std::uint32_t> Device::memory_type(std::uint32_t allowed, VkFlags wanted, VkFlags preferred) const
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

VkDeviceMemory Device::allocate(VkMemoryRequirements const& requirements, VkFlags wanted, VkFlags preferred)
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
bool Device::ensure_buffer(HostBuffer& buffer, std::uint64_t size, VkFlags usage, void const* next, VkFlags preferred)
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

bool Device::ensure_bitstream(std::uint64_t size)
{
    return ensure_buffer(m_bitstream, size, buffer_usage_decode_src, m_codec.profile_list, 0);
}

void Device::release_buffer(HostBuffer& buffer)
{
    if (buffer.buffer)
        m_vk.destroy_buffer(m_device, buffer.buffer, nullptr);
    if (buffer.memory)
        m_vk.free_memory(m_device, buffer.memory, nullptr); // unmaps it too
    buffer = {};
}

void Device::release_session()
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
    m_reset_pending = true;
}

bool Device::ensure_session(std::uint32_t width, std::uint32_t height, bool& recreated)
{
    recreated = false;
    if (m_lost)
        return false;
    if (width < m_capabilities.minCodedExtent.width || height < m_capabilities.minCodedExtent.height
        || width > m_capabilities.maxCodedExtent.width || height > m_capabilities.maxCodedExtent.height)
        return false;
    if (m_session != 0 && width <= m_extent.width && height <= m_extent.height)
        return true;
    recreated = true;
    return create_session(width, height);
}

bool Device::create_session(std::uint32_t width, std::uint32_t height)
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
    std::strncpy(std_header.extensionName, m_codec.std_header, sizeof std_header.extensionName - 1);
    std_header.specVersion = m_codec.std_version;
    VkVideoSessionCreateInfoKHR const info { st_video_session_create_info, nullptr, m_decode_family, 0, m_codec.profile, format_nv12, extent,
        format_nv12, m_codec.pictures, m_codec.active_references, &std_header };
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
    m_pictures.resize(m_codec.pictures);
    m_resources.resize(m_codec.pictures);
    for (std::uint32_t i = 0; i < m_codec.pictures; ++i) {
        Picture& picture = m_pictures[i];
        VkImageCreateInfo const image { st_image_create_info, m_codec.profile_list, 0, image_type_2d, format_nv12, { extent.width, extent.height, 1 }, 1,
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
    m_reset_pending = true;
    return true;
}

VkVideoSessionParametersKHR Device::create_parameters(void const* next)
{
    if (m_session == 0)
        return 0;
    VkVideoSessionParametersCreateInfoKHR const info { st_video_session_parameters_create_info, next, 0, 0, m_session };
    VkVideoSessionParametersKHR parameters = 0;
    return m_vk.create_video_session_parameters(m_device, &info, nullptr, &parameters) == vk_success ? parameters : 0;
}

void Device::destroy_parameters(VkVideoSessionParametersKHR parameters)
{
    if (parameters != 0 && m_device) {
        m_vk.device_wait_idle(m_device);
        m_vk.destroy_video_session_parameters(m_device, parameters, nullptr);
    }
}

VkImageMemoryBarrier2 Device::image_barrier(VkImage image, VkFlags64 source_stage, VkFlags64 source_access, VkFlags64 destination_stage,
    VkFlags64 destination_access, std::int32_t from, std::int32_t to)
{
    return { st_image_memory_barrier_2, nullptr, source_stage, source_access, destination_stage, destination_access, from, to,
        queue_family_ignored, queue_family_ignored, image, { aspect_color, 0, 1, 0, 1 } };
}

bool Device::submit_and_wait(VkQueue queue, VkCommandBuffer commands)
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

bool Device::decode(VkVideoDecodeInfoKHR const& info, int target, std::span<VkVideoReferenceSlotInfoKHR const> bound,
    VkVideoSessionParametersKHR parameters)
{
    if (m_lost || target < 0 || static_cast<std::size_t>(target) >= m_pictures.size())
        return false;
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
    VkVideoBeginCodingInfoKHR const coding { st_video_begin_coding_info, nullptr, 0, m_session, parameters,
        static_cast<std::uint32_t>(bound.size()), bound.data() };
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
    return true;
}

std::optional<Nv12Picture> Device::read(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_pictures.size())
        return std::nullopt;
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
    auto const crop_x = static_cast<std::size_t>(std::max(0, picture.crop_x));
    auto const crop_y = static_cast<std::size_t>(std::max(0, picture.crop_y));
    if (crop_x + width > m_extent.width || crop_y + static_cast<std::size_t>(picture.height) > m_extent.height)
        return std::nullopt;
    out.luma.resize(width * static_cast<std::size_t>(picture.height));
    for (std::size_t y = 0; y < static_cast<std::size_t>(picture.height); ++y)
        std::memcpy(out.luma.data() + y * width, m_readback.mapped + (y + crop_y) * pitch + crop_x, width);
    std::size_t const chroma_row = static_cast<std::size_t>(out.chroma_width()) * 2;
    std::size_t const chroma_pitch = static_cast<std::size_t>(m_extent.width / 2) * 2;
    out.chroma.resize(chroma_row * static_cast<std::size_t>(out.chroma_height()));
    for (std::size_t y = 0; y < static_cast<std::size_t>(out.chroma_height()); ++y)
        std::memcpy(out.chroma.data() + y * chroma_row, m_readback.mapped + chroma_offset + (y + crop_y / 2) * chroma_pitch + (crop_x / 2) * 2, chroma_row);
    return out;
}

}
