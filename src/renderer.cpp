#include <scm/capture.hpp>
#include <scm/cache.hpp>
#include <volk.h>
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <bit>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace scm {
namespace {
void checked(VkResult result, std::string_view operation) { require(result == VK_SUCCESS, "{} failed (Vulkan result {}).", operation, int(result)); }
float linear(float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); }
VkFormat textureFormat(const Image& image) {
    switch (image.format) {
        case PixelFormat::RGBA8: return image.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::BGRA8: return image.srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        case PixelFormat::BC1: return image.srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case PixelFormat::BC2: return image.srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        case PixelFormat::BC3: return image.srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        case PixelFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    throw Error("Unknown texture pixel format.");
}
struct Buffer { VkBuffer buffer{}; VmaAllocation allocation{}; void* mapping{}; VkDeviceSize size{}; };
struct GpuImage { VkImage image{}; VmaAllocation allocation{}; VkImageView view{}; uint32_t mips = 1; };
struct ImageCopy { VkImage image{}; uint32_t mips = 1; std::vector<VkBufferImageCopy> regions; };
enum class Memory { Device, Upload, Readback };
struct Push { glm::mat4 projection; glm::vec4 eye, clipPlane, waterView; };
struct DrawUniform {
    glm::vec4 tint, controls;
    glm::uvec4 flags;
    glm::vec4 fog;
    glm::vec4 skyTop, skyBottom, skySun, skySunColor, skyDusk, skyControls, stars;
    glm::vec4 phong, phongFresnel, phongTint;
    std::array<glm::vec4, 6> ambient;
    std::array<PropLight, 4> lights;
    glm::vec4 waterFog, waterControls, waterReflect, waterRefract, waterScroll, waterLayers, waterTime;
    glm::vec4 selfIllumTint;
};
static_assert(sizeof(Push) == 112 && sizeof(DrawUniform) == 704 && sizeof(PropLight) == 64);
}

struct Renderer::Impl {
    RenderOptions options;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queueIndex{};
    VkPhysicalDeviceProperties properties{};
    VmaAllocator allocator{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkDescriptorSetLayout descriptorLayout{};
    VkDescriptorPool descriptorPool{};
    VkPipelineLayout pipelineLayout{};
    VkPipelineCache pipelineCache{};
    fs::path cacheFile;
    VkShaderModule vertexShader{}, fragmentShader{};
    VkSampler wrapSampler{}, clampSampler{};
    VkQueryPool queries{};
    bool anisotropy = false;
    std::array<VkPipeline, 16> pipelines{};
    std::vector<GpuImage> images;
    std::unordered_map<std::string, GpuImage> imageCache;
    std::vector<VkDescriptorSet> descriptors;
    std::vector<VkPipeline> drawPipelines;
    std::array<std::array<std::vector<uint32_t>, 2>, 3> passDraws, viewDraws;
    std::vector<std::pair<float, uint32_t>> viewOrder;
    Buffer vertices, indices, readback, uniforms, staging;
    std::vector<ImageCopy> imageCopies;
    VkDeviceSize stagingUsed = 0;
    bool transferOpen = false;
    GpuImage color, depth, waterReflection, waterRefraction;
    std::array<VkImageView, 6> colorViews{}, depthViews{};
    uint32_t targetSize = 0;
    bool targetWater = false;
    double gpuSeconds = 0;
    uint64_t captureCount = 0;
    uint64_t drawCalls = 0, culledDraws = 0;
    uint64_t waterViews = 0;
    uint64_t textureUploadBytes = 0, uploadSubmissions = 0, reusedTextures = 0;
    double uploadWaitSeconds = 0, uploadTotalSeconds = 0, pipelineSeconds = 0;

    explicit Impl(RenderOptions settings) : options(std::move(settings)) {}
    ~Impl() {
        if (device) { vkDeviceWaitIdle(device); }
        if (device && pipelineCache && !cacheFile.empty()) {
            try {
                size_t count = 0; checked(vkGetPipelineCacheData(device, pipelineCache, &count, nullptr), "Read pipeline cache size");
                if (count <= (64u << 20)) { Bytes data(count); checked(vkGetPipelineCacheData(device, pipelineCache, &count, data.data()), "Read pipeline cache"); data.resize(count); writeCacheFile(cacheFile, data); }
            } catch (const std::exception& error) { spdlog::warn("Pipeline cache write failed: {}", error.what()); }
        }
        releaseScene(); releaseTargets(); releaseImages(); destroy(staging);
        for (auto pipeline : pipelines) { if (pipeline) { vkDestroyPipeline(device, pipeline, nullptr); } }
        if (vertexShader) { vkDestroyShaderModule(device, vertexShader, nullptr); }
        if (fragmentShader) { vkDestroyShaderModule(device, fragmentShader, nullptr); }
        if (queries) { vkDestroyQueryPool(device, queries, nullptr); }
        if (wrapSampler) { vkDestroySampler(device, wrapSampler, nullptr); }
        if (clampSampler) { vkDestroySampler(device, clampSampler, nullptr); }
        if (pipelineCache) { vkDestroyPipelineCache(device, pipelineCache, nullptr); }
        if (pipelineLayout) { vkDestroyPipelineLayout(device, pipelineLayout, nullptr); }
        if (descriptorLayout) { vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr); }
        if (fence) { vkDestroyFence(device, fence, nullptr); }
        if (pool) { vkDestroyCommandPool(device, pool, nullptr); }
        if (allocator) { vmaDestroyAllocator(allocator); }
        if (device) { vkDestroyDevice(device, nullptr); }
        if (instance) { vkDestroyInstance(instance, nullptr); }
    }
    void destroy(Buffer& buffer) {
        if (buffer.buffer) { vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation); buffer = {}; }
    }
    void destroy(GpuImage& image) {
        if (image.view) { vkDestroyImageView(device, image.view, nullptr); }
        if (image.image) { vmaDestroyImage(allocator, image.image, image.allocation); }
        image = {};
    }
    void releaseScene() {
        if (descriptorPool) { vkDestroyDescriptorPool(device, descriptorPool, nullptr); descriptorPool = {}; }
        images.clear(); descriptors.clear(); drawPipelines.clear(); destroy(vertices); destroy(indices); destroy(uniforms);
        for (auto& pass : passDraws) { for (auto& draws : pass) { draws.clear(); } }
    }
    void releaseImages() {
        for (auto& [key, image] : imageCache) { destroy(image); }
        imageCache.clear(); images.clear();
    }
    void releaseTargets() {
        for (auto& view : colorViews) { if (view) { vkDestroyImageView(device, view, nullptr); view = {}; } }
        for (auto& view : depthViews) { if (view) { vkDestroyImageView(device, view, nullptr); view = {}; } }
        destroy(color); destroy(depth); destroy(waterReflection); destroy(waterRefraction); destroy(readback); targetSize = 0;
    }
    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) {
        Buffer buffer; buffer.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
        VmaAllocationCreateInfo allocation{}; allocation.usage = memory == Memory::Device ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO;
        if (memory == Memory::Upload) { allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; }
        if (memory == Memory::Readback) { allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; }
        VmaAllocationInfo result{};
        checked(vmaCreateBuffer(allocator, &info, &allocation, &buffer.buffer, &buffer.allocation, &result), "Create buffer");
        buffer.mapping = result.pMappedData;
        return buffer;
    }
    VkImageView makeView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t firstLayer, uint32_t layers, uint32_t mips, bool cube = false) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY; info.format = format;
        info.subresourceRange = {aspect, 0, mips, firstLayer, layers};
        VkImageView view{}; checked(vkCreateImageView(device, &info, nullptr, &view), "Create image view"); return view;
    }
    GpuImage makeImage(uint32_t width, uint32_t height, uint32_t layers, uint32_t mips, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool cube = false) {
        require(width > 0 && height > 0 && width <= properties.limits.maxImageDimension2D && height <= properties.limits.maxImageDimension2D && layers <= properties.limits.maxImageArrayLayers, "Image dimensions exceed the GPU limits.");
        GpuImage result; result.mips = mips;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
        info.mipLevels = mips; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
        VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        checked(vmaCreateImage(allocator, &info, &allocation, &result.image, &result.allocation, nullptr), "Create image");
        try { result.view = makeView(result.image, format, aspect, 0, layers, mips, cube); }
        catch (...) { destroy(result); throw; }
        return result;
    }
    void begin() {
        checked(vkResetCommandBuffer(command, 0), "Reset command buffer");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &info), "Begin command buffer");
    }
    void submit() {
        checked(vkEndCommandBuffer(command), "End command buffer");
        checked(vkResetFences(device, 1, &fence), "Reset fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount = 1; info.pCommandBuffers = &command;
        checked(vkQueueSubmit(queue, 1, &info, fence), "Submit GPU work");
        checked(vkWaitForFences(device, 1, &fence, VK_TRUE, 120'000'000'000ULL), "Wait for GPU");
    }
    void barrier(VkImage image, VkImageLayout before, VkImageLayout after, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers, uint32_t baseMip = 0) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = before; barrier.newLayout = after; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image; barrier.subresourceRange = {aspect, baseMip, mips, 0, layers};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }
    VkShaderModule shader(const fs::path& path) {
        auto data = readFile(path);
        require(data.size() % 4 == 0, "Invalid SPIR-V byte count.");
        std::vector<uint32_t> aligned(data.size() / 4); std::memcpy(aligned.data(), data.data(), data.size());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = data.size(); info.pCode = aligned.data();
        VkShaderModule module{}; checked(vkCreateShaderModule(device, &info, nullptr, &module), "Create shader"); return module;
    }
    void initialize() {
        checked(volkInitialize(), "Load Vulkan");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "SourceCubemap"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
        checked(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance"); volkLoadInstance(instance);
        uint32_t count = 0; checked(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count); checked(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate GPUs");
        int best = -1;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_3) { continue; }
            if (!options.device.empty() && lower(props.deviceName).find(lower(options.device)) == std::string::npos) { continue; }
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) { best = score; physical = candidate; properties = props; }
        }
        require(physical != VK_NULL_HANDLE, "No matching Vulkan 1.3 GPU is available.");
        for (auto format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}) {
            VkFormatProperties support{}; vkGetPhysicalDeviceFormatProperties(physical, format, &support);
            auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
            require((support.optimalTilingFeatures & required) == required, "GPU lacks the required floating-point texture filtering formats.");
            if (format == VK_FORMAT_R32G32B32A32_SFLOAT) { require((support.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0, "GPU lacks floating-point capture targets."); }
        }
        for (auto format : {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK}) {
            VkFormatProperties support{}; vkGetPhysicalDeviceFormatProperties(physical, format, &support);
            auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            require((support.optimalTilingFeatures & required) == required, "GPU lacks the required block-compressed texture formats.");
        }
        VkFormatProperties depthSupport{}; vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &depthSupport);
        require((depthSupport.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "GPU lacks D32 depth attachments.");
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        require(supported.dynamicRendering && supported.synchronization2, "GPU lacks Vulkan dynamic rendering or synchronization2.");
        require(features.features.textureCompressionBC, "GPU lacks block-compressed texture support.");
        anisotropy = features.features.samplerAnisotropy != 0;
        uint32_t queueCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount); vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
        bool found = false;
        for (uint32_t i = 0; i < queueCount; ++i) { if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueIndex = i; found = true; break; } }
        require(found, "GPU has no graphics queue.");
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queueInfo.queueFamilyIndex = queueIndex; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; enabled.dynamicRendering = VK_TRUE; enabled.synchronization2 = VK_TRUE;
        VkPhysicalDeviceFeatures basic{}; basic.samplerAnisotropy = anisotropy; basic.textureCompressionBC = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; deviceInfo.pNext = &enabled; deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo; deviceInfo.pEnabledFeatures = &basic;
        checked(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "Create Vulkan device"); volkLoadDevice(device); vkGetDeviceQueue(device, queueIndex, 0, &queue);
        VmaVulkanFunctions functions{}; functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr; functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo allocatorInfo{}; allocatorInfo.instance = instance; allocatorInfo.physicalDevice = physical; allocatorInfo.device = device; allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3; allocatorInfo.pVulkanFunctions = &functions;
        checked(vmaCreateAllocator(&allocatorInfo, &allocator), "Create GPU allocator");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex = queueIndex;
        checked(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; commandInfo.commandPool = pool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(device, &commandInfo, &command), "Allocate command buffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; checked(vkCreateFence(device, &fenceInfo, nullptr, &fence), "Create fence");
        std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
        for (uint32_t i = 0; i < 10; ++i) { bindings[i] = {i, i < 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; }
        bindings[10] = {10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[11] = {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[12] = {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[13] = {13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[14] = {14, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size()); layoutInfo.pBindings = bindings.data();
        checked(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout), "Create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipelineInfo.setLayoutCount = 1; pipelineInfo.pSetLayouts = &descriptorLayout; pipelineInfo.pushConstantRangeCount = 1; pipelineInfo.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout), "Create pipeline layout");
        Bytes cachedPipelines;
        if (!options.cacheDirectory.empty()) {
            std::string uuid; for (auto value : properties.pipelineCacheUUID) { uuid += std::format("{:02x}", value); }
            cacheFile = options.cacheDirectory / "pipelines" / (uuid + ".bin");
            try {
                if (fs::exists(cacheFile) && fs::file_size(cacheFile) <= (64u << 20)) {
                    cachedPipelines = readFile(cacheFile); Reader cache(cachedPipelines, "pipeline cache");
                    bool valid = cachedPipelines.size() >= 32 && cache.at<uint32_t>(0) == 32 && cache.at<uint32_t>(4) == 1 && cache.at<uint32_t>(8) == properties.vendorID && cache.at<uint32_t>(12) == properties.deviceID && std::memcmp(cachedPipelines.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
                    if (!valid) { cachedPipelines.clear(); }
                }
            } catch (const std::exception& error) { spdlog::warn("Ignoring pipeline cache: {}", error.what()); cachedPipelines.clear(); }
        }
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; cacheInfo.initialDataSize = cachedPipelines.size(); cacheInfo.pInitialData = cachedPipelines.data();
        checked(vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache), "Create pipeline cache");
        vertexShader = shader(options.shaderDirectory / "scene.vert.spv"); fragmentShader = shader(options.shaderDirectory / "scene.frag.spv");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sampler.maxLod = VK_LOD_CLAMP_NONE;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.anisotropyEnable = anisotropy && options.quality != "fast"; sampler.maxAnisotropy = std::min(options.quality == "high" ? 16.f : 4.f, properties.limits.maxSamplerAnisotropy);
        checked(vkCreateSampler(device, &sampler, nullptr, &wrapSampler), "Create material sampler");
        sampler.anisotropyEnable = false; sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checked(vkCreateSampler(device, &sampler, nullptr, &clampSampler), "Create lightmap sampler");
        if (queues[queueIndex].timestampValidBits) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP; queryInfo.queryCount = 2;
            checked(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "Create GPU timer");
        }
        spdlog::info("Rendering on {}", properties.deviceName);
    }
    VkPipeline pipeline(unsigned key) {
        if (pipelines[key]) { return pipelines[key]; }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "VSMain", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "PSMain", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 8> attributes{{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
            {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,tangent)}, {3,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},
            {4,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,lightUV)}, {5,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,color)}, {6,0,VK_FORMAT_R32_SFLOAT,offsetof(Vertex,blend)}, {7,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,secondaryUV)}}};
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; vertex.vertexBindingDescriptionCount = 1; vertex.pVertexBindingDescriptions = &binding; vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size()); vertex.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = (key & 4) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depthState.depthTestEnable = !(key & 8); depthState.depthWriteEnable = !(key & (8 | 1 | 2)); depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 15; blend.blendEnable = (key & 3) != 0;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; blend.dstColorBlendFactor = key & 2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blendState.attachmentCount = 1; blendState.pAttachments = &blend;
        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamicStates.data();
        VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &format; rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.pNext = &rendering; info.stageCount = 2; info.pStages = stages.data(); info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly; info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &multisample; info.pDepthStencilState = &depthState; info.pColorBlendState = &blendState; info.pDynamicState = &dynamic; info.layout = pipelineLayout;
        Stopwatch compiling;
        VkPipeline value{}; checked(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &value), "Create graphics pipeline"); pipelineSeconds += compiling.seconds(); pipelines[key] = value; return value;
    }
    void targets(uint32_t size, bool water) {
        if (targetSize == size && targetWater == water) { return; }
        releaseTargets();
        require(size <= properties.limits.maxImageDimension2D, "Capture resolution exceeds the GPU limit.");
        targetSize = size;
        targetWater = water;
        color = makeImage(size, size, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        depth = makeImage(size, size, 6, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        for (uint32_t i = 0; i < 6; ++i) {
            colorViews[i] = makeView(color.image, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 1);
            depthViews[i] = makeView(depth.image, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, i, 1, 1);
        }
        readback = makeBuffer(VkDeviceSize(size) * size * 6 * sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback);
        uint32_t waterSize = water ? size : 1;
        waterReflection = makeImage(waterSize, waterSize, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, true);
        waterRefraction = makeImage(waterSize, waterSize, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, true);
        begin();
        for (auto* image : {&waterReflection, &waterRefraction}) {
            barrier(image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
            VkClearColorValue clear{{0,0,0,1000000}}; VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,6};
            vkCmdClearColorImage(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            barrier(image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        }
        submit();
        writeWaterDescriptors();
    }
    void writeWaterDescriptors() {
        std::array<VkDescriptorImageInfo, 2> targets{{{VK_NULL_HANDLE,waterReflection.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,waterRefraction.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
        std::vector<VkWriteDescriptorSet> writes; writes.reserve(descriptors.size() * 2);
        for (auto descriptor : descriptors) {
            for (uint32_t i = 0; i < 2; ++i) { writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,descriptor,11+i,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&targets[i],nullptr,nullptr}); }
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    /// Submits queued transfers, moving every queued image from undefined contents to shader-readable data.
    void flushTransfers() {
        if (!transferOpen) { return; }
        std::vector<VkImageMemoryBarrier2> before, after;
        for (const auto& copy : imageCopies) {
            VkImageMemoryBarrier2 item{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            item.srcQueueFamilyIndex = item.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; item.image = copy.image; item.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, copy.mips, 0, 1};
            item.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; item.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; item.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; item.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            before.push_back(item);
            item.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; item.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; item.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; item.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT; item.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; item.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            after.push_back(item);
        }
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = static_cast<uint32_t>(before.size()); dependency.pImageMemoryBarriers = before.data();
        if (!before.empty()) { vkCmdPipelineBarrier2(command, &dependency); }
        for (const auto& copy : imageCopies) { vkCmdCopyBufferToImage(command, staging.buffer, copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copy.regions.size()), copy.regions.data()); }
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2}; memory.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT; memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(after.size()); dependency.pImageMemoryBarriers = after.data(); dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(command, &dependency);
        checked(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE), "Flush upload staging");
        Stopwatch waiting; submit(); uploadWaitSeconds += waiting.seconds(); ++uploadSubmissions;
        imageCopies.clear(); stagingUsed = 0; transferOpen = false;
    }
    /// Returns a shared staging offset for the next transfer, submitting queued work when the buffer is full.
    VkDeviceSize reserveTransfer(VkDeviceSize size) {
        if (staging.size < size) { flushTransfers(); destroy(staging); staging = makeBuffer(std::max(size, VkDeviceSize(256) << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload); }
        if (stagingUsed + size > staging.size) { flushTransfers(); }
        if (!transferOpen) { begin(); transferOpen = true; }
        auto alignment = std::max<VkDeviceSize>(16, properties.limits.optimalBufferCopyOffsetAlignment);
        auto offset = stagingUsed; stagingUsed = std::min(staging.size, (offset + size + alignment - 1) / alignment * alignment);
        return offset;
    }
    void uploadBuffer(VkBuffer target, std::span<const std::byte> data) {
        for (VkDeviceSize written = 0; written < data.size();) {
            auto length = std::min(VkDeviceSize(data.size()) - written, VkDeviceSize(64) << 20);
            auto offset = reserveTransfer(length);
            std::memcpy(static_cast<std::byte*>(staging.mapping) + offset, data.data() + written, static_cast<size_t>(length));
            VkBufferCopy copy{offset, written, length}; vkCmdCopyBuffer(command, staging.buffer, target, 1, &copy);
            written += length;
        }
    }
    GpuImage uploadImage(const Image& source) {
        uint32_t mipCount = source.mipCount;
        require(mipCount > 0 && mipCount <= std::bit_width(std::max(source.width, source.height)), "Invalid scene texture mip count.");
        ImageCopy copy; copy.mips = mipCount;
        VkDeviceSize total = 0;
        for (uint32_t mip = 0; mip < mipCount; ++mip) {
            VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            region.imageExtent = {std::max(1u, source.width >> mip), std::max(1u, source.height >> mip), 1};
            region.bufferOffset = total;
            total += pixelBytes(source.format, region.imageExtent.width, region.imageExtent.height);
            copy.regions.push_back(region);
        }
        require(total == source.pixels.size(), "Scene texture pixels do not match its mip layout.");
        auto image = makeImage(source.width, source.height, 1, mipCount, textureFormat(source), VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        try {
            auto offset = reserveTransfer(total);
            std::memcpy(static_cast<std::byte*>(staging.mapping) + offset, source.pixels.data(), source.pixels.size());
            for (auto& region : copy.regions) { region.bufferOffset += offset; }
            copy.image = image.image; imageCopies.push_back(std::move(copy));
        } catch (...) { destroy(image); throw; }
        textureUploadBytes += total;
        return image;
    }
    /// Keeps GPU copies of unchanged textures, so a later lighting variant uploads only what differs.
    void uploadImages(const Scene& scene) {
        std::vector<std::string> keys; keys.reserve(scene.images.size());
        for (const auto& source : scene.images) { keys.push_back(std::format("{}:{}:{}:{}:{}:{}", digest(source.pixels), source.width, source.height, source.mipCount, static_cast<uint32_t>(source.format), source.srgb)); }
        std::unordered_set<std::string> wanted(keys.begin(), keys.end());
        for (auto entry = imageCache.begin(); entry != imageCache.end();) {
            if (wanted.contains(entry->first)) { ++entry; }
            else { destroy(entry->second); entry = imageCache.erase(entry); }
        }
        images.reserve(scene.images.size());
        for (size_t i = 0; i < scene.images.size(); ++i) {
            auto found = imageCache.find(keys[i]);
            if (found == imageCache.end()) { found = imageCache.emplace(keys[i], uploadImage(scene.images[i])).first; }
            else { ++reusedTextures; }
            images.push_back(found->second);
        }
    }
    void writeDraws(const Scene& scene) {
        uint32_t count = static_cast<uint32_t>(scene.draws.size());
        auto alignment = properties.limits.minUniformBufferOffsetAlignment;
        auto uniformStride = (sizeof(DrawUniform) + alignment - 1) / alignment * alignment;
        uniforms = makeBuffer(std::max(VkDeviceSize(1), count * uniformStride), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Device);
        std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 12}, {VK_DESCRIPTOR_TYPE_SAMPLER, count * 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets = count; poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size()); poolInfo.pPoolSizes = sizes.data();
        checked(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "Create descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(count, descriptorLayout); descriptors.resize(count);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptorPool; allocation.descriptorSetCount = count; allocation.pSetLayouts = layouts.data();
        checked(vkAllocateDescriptorSets(device, &allocation, descriptors.data()), "Allocate material descriptors");
        Bytes uniformData(count * uniformStride);
        std::vector<VkDescriptorImageInfo> imageInfos; imageInfos.reserve(size_t(count) * 12);
        std::vector<VkDescriptorBufferInfo> bufferInfos; bufferInfos.reserve(count);
        std::vector<VkWriteDescriptorSet> writes; writes.reserve(size_t(count) * 13);
        drawPipelines.resize(count);
        auto writeImage = [&](uint32_t drawIndex, uint32_t binding, VkDescriptorType type, VkDescriptorImageInfo info) {
            imageInfos.push_back(info);
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptors[drawIndex], binding, 0, 1, type, &imageInfos.back(), nullptr, nullptr});
        };
        auto writeTexture = [&](uint32_t drawIndex, uint32_t binding, uint32_t image) { writeImage(drawIndex, binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, {VK_NULL_HANDLE, images.at(image).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}); };
        for (uint32_t drawIndex = 0; drawIndex < count; ++drawIndex) {
            const auto& draw = scene.draws[drawIndex]; const auto& material = scene.materials[draw.material];
            std::array<uint32_t, 8> slots{material.base, material.second, material.normal, draw.lightmaps[0], draw.lightmaps[1], draw.lightmaps[2], draw.lightmaps[3], material.detail};
            for (uint32_t i = 0; i < 8; ++i) { writeTexture(drawIndex, i, slots[i]); }
            writeImage(drawIndex, 8, VK_DESCRIPTOR_TYPE_SAMPLER, {scene.images[material.base].clamp ? clampSampler : wrapSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            writeImage(drawIndex, 9, VK_DESCRIPTOR_TYPE_SAMPLER, {clampSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            float fogStart = draw.skyWorld ? scene.skyFogStart : scene.fogStart, fogEnd = draw.skyWorld ? scene.skyFogEnd : scene.fogEnd;
            auto fogColor = draw.skyWorld ? scene.skyFogColor : scene.fogColor;
            bool fogEnabled = !draw.skyBox && !material.noFog && fogEnd > fogStart;
            DrawUniform uniform{};
            uniform.tint = material.tint;
            uniform.selfIllumTint = {material.selfIllumTint, 0};
            uniform.controls = {material.alphaReference, material.detailFactor, material.detailScale, fogEnd};
            uniform.flags = {material.flags | (draw.lightmaps[0] ? BumpLight : 0u), draw.lighting, std::bit_cast<uint32_t>(fogStart), fogEnabled ? 1u : 0u};
            uniform.fog = {linear(fogColor.r), linear(fogColor.g), linear(fogColor.b), 1};
            const auto& sky = scene.paintedSky;
            uniform.skyTop = {sky.top, sky.fadeBias}; uniform.skyBottom = {sky.bottom, scene.hdr ? sky.hdrScale : 1.0f};
            uniform.skySun = {sky.sunDirection, sky.sunSize}; uniform.skySunColor = {sky.sunColor, 0};
            uniform.skyDusk = {sky.duskColor, sky.duskIntensity}; uniform.skyControls = {sky.duskScale, float(sky.starLayers), 0, 0};
            uniform.stars = {sky.starScale, sky.starFade, sky.starPosition, 0};
            const auto& lighting = scene.propLighting.at(draw.lighting);
            uniform.phong = {material.phongExponent, material.phongBoost, float(lighting.count), lighting.baked && !(material.flags & Phong) ? 1.0f : 0.0f};
            uniform.phongFresnel = {material.phongFresnel, material.phongExponentFactor}; uniform.phongTint = {material.phongTint, material.blendTintOverBase};
            for (size_t i = 0; i < lighting.ambient.size(); ++i) { uniform.ambient[i] = {lighting.ambient[i], 0}; }
            uniform.lights = lighting.lights;
            const auto& water = scene.hasWater && !(material.flags & Water) ? scene.materials.at(scene.waterMaterial).water : material.water;
            uniform.waterFog = {glm::pow(glm::max(water.fogColor, glm::vec3(0)), glm::vec3(2.2f)), water.fogStart};
            uniform.waterControls = {water.reflectAmount, water.refractAmount, water.fogEnd, water.above ? 1.0f : 0.0f};
            uniform.waterReflect = {water.reflectTint, water.noFresnel ? 1.0f : 0.0f};
            uniform.waterRefract = {water.refractTint, water.reflection ? 1.0f : 0.0f};
            uniform.waterScroll = {water.scroll.x, water.scroll.y, water.scale.x, water.scale.y};
            uniform.waterLayers = {water.scroll1.x, water.scroll1.y, water.scroll2.x, water.scroll2.y};
            uniform.waterTime = {water.scroll1.z, water.scroll2.z, static_cast<float>(scene.captureTime), 0};
            std::memcpy(uniformData.data() + drawIndex * uniformStride, &uniform, sizeof(uniform));
            bufferInfos.push_back({uniforms.buffer, drawIndex * uniformStride, sizeof(DrawUniform)});
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptors[drawIndex], 10, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfos.back(), nullptr});
            writeTexture(drawIndex, 13, material.blendMask);
            writeTexture(drawIndex, 14, material.selfIllumMask);
            unsigned key = (material.translucent ? 1u : 0u) | (material.additive ? 2u : 0u) | (material.noCull ? 4u : 0u) | (draw.skyBox ? 8u : 0u);
            drawPipelines[drawIndex] = pipeline(key);
            passDraws[draw.skyBox ? 0 : draw.skyWorld ? 1 : 2][material.translucent || material.additive ? 1 : 0].push_back(drawIndex);
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        uploadBuffer(uniforms.buffer, uniformData);
    }
    void prepare(const Scene& scene) {
        checked(vkDeviceWaitIdle(device), "Wait before replacing scene"); releaseScene();
        require(!scene.vertices.empty() && !scene.indices.empty(), "The scene contains no renderable geometry.");
        vertices = makeBuffer(scene.vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Device);
        indices = makeBuffer(scene.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Device);
        Stopwatch uploadTimer;
        try {
            uploadBuffer(vertices.buffer, std::as_bytes(std::span(scene.vertices)));
            uploadBuffer(indices.buffer, std::as_bytes(std::span(scene.indices)));
            uploadImages(scene);
            writeDraws(scene);
            flushTransfers();
        } catch (...) { vkDeviceWaitIdle(device); imageCopies.clear(); stagingUsed = 0; transferOpen = false; releaseImages(); throw; }
        uploadTotalSeconds += uploadTimer.seconds();
        if (targetSize) { writeWaterDescriptors(); }
    }
    /// Back-to-front draw order for one camera origin; every face of a view shares it, so it is sorted once per pass.
    void orderDraws(const Scene& scene, unsigned pass, glm::vec3 origin, size_t probeIndex, unsigned mode) {
        for (unsigned transparent = 0; transparent < 2; ++transparent) {
            viewOrder.clear();
            for (uint32_t i : passDraws[pass][transparent]) {
                const auto& draw = scene.draws[i]; const auto& material = scene.materials[draw.material];
                if (mode && ((material.flags & Water) || material.translucent || material.additive)) { continue; }
                if (mode == 1 && draw.lighting && !scene.materials.at(scene.waterMaterial).water.reflectEntities) { continue; }
                if (!mode && !draw.visibility.empty() && probeIndex < scene.probes.size() && !(draw.visibility.at(probeIndex / 64) & (uint64_t(1) << (probeIndex % 64)))) { culledDraws += 6; continue; }
                auto delta = draw.center - origin;
                viewOrder.emplace_back(glm::dot(delta, delta), i);
            }
            std::stable_sort(viewOrder.begin(), viewOrder.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            auto& ordered = viewDraws[pass][transparent]; ordered.resize(viewOrder.size());
            for (size_t i = 0; i < viewOrder.size(); ++i) { ordered[i] = viewOrder[i].second; }
        }
    }
    void renderViews(const Scene& scene, glm::vec3 cameraOrigin, size_t probeIndex, bool skyOnly, unsigned mode, bool submerged, glm::vec4 clipPlane) {
        uint32_t size = targetSize;
        barrier(color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        barrier(depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 6);
        const std::array<glm::vec3, 6> forward{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
        const std::array<glm::vec3, 6> up{{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};
        auto projection = glm::perspectiveLH_ZO(glm::radians(90.f), 1.f, 1.f, 1000000.f); projection[1][1] *= -1;
        std::array<glm::vec3, 3> origins;
        for (unsigned pass = 0; pass < 3; ++pass) {
            origins[pass] = pass == 0 ? glm::vec3(0) : pass == 1 && scene.skyScale > 0 ? cameraOrigin / scene.skyScale + scene.skyOrigin : cameraOrigin;
            if (!skyOnly || pass == 0) { orderDraws(scene, pass, origins[pass], probeIndex, mode); }
        }
        VkPipeline bound{};
        for (uint32_t face = 0; face < 6; ++face) {
            VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; colorAttachment.imageView = colorViews[face]; colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; colorAttachment.clearValue.color = {{0,0,0,1000000}};
            VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depthAttachment.imageView = depthViews[face]; depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; depthAttachment.clearValue.depthStencil.depth = 1;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = {size,size}; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &colorAttachment; rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(command, &rendering);
            VkViewport viewport{0,0,float(size),float(size),0,1}; VkRect2D scissor{{0,0},{size,size}};
            vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
            VkDeviceSize offset = 0; vkCmdBindVertexBuffers(command, 0, 1, &vertices.buffer, &offset); vkCmdBindIndexBuffer(command, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            auto right = glm::cross(up[face], forward[face]);
            for (unsigned pass = 0; pass < 3; ++pass) {
                if (skyOnly && pass != 0) { continue; }
                if (pass == 2) {
                    VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil.depth = 1;
                    VkClearRect rectangle{{{0,0},{size,size}},0,1}; vkCmdClearAttachments(command, 1, &clear, 1, &rectangle);
                }
                auto origin = origins[pass];
                auto viewProjection = projection * glm::lookAtLH(origin, origin + forward[face], up[face]);
                Push push{viewProjection, {origin, float(pass)}, pass == 2 ? clipPlane : glm::vec4(0), {scene.waterHeight, submerged ? 1.0f : 0.0f, float(mode), 0}};
                vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                for (unsigned transparent = 0; transparent < 2; ++transparent) {
                    for (uint32_t i : viewDraws[pass][transparent]) {
                        const auto& draw = scene.draws[i];
                        auto relative = draw.center - origin;
                        float z = glm::dot(relative, forward[face]), x = glm::dot(relative, right), y = glm::dot(relative, up[face]);
                        if (!draw.skyBox && (z + draw.radius < 1 || std::abs(x) - z > draw.radius * 1.414214f || std::abs(y) - z > draw.radius * 1.414214f)) { ++culledDraws; continue; }
                        if (drawPipelines[i] != bound) { bound = drawPipelines[i]; vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, bound); }
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptors[i], 0, nullptr);
                        vkCmdDrawIndexed(command, draw.indexCount, 1, draw.firstIndex, 0, 0);
                        ++drawCalls;
                    }
                }
            }
            vkCmdEndRendering(command);
        }
    }
    void copyWater(GpuImage& destination) {
        barrier(color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        barrier(destination.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,6}; copy.extent = {targetSize,targetSize,1};
        vkCmdCopyImage(command, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
    }
    Cube capture(const Scene& scene, const Probe& probe, bool skyOnly) {
        uint32_t factor = options.quality == "high" ? 2 : 1;
        uint32_t size = probe.resolution * factor; targets(size, scene.hasWater);
        auto found = std::find_if(scene.probes.begin(), scene.probes.end(), [&](const auto& value) { return value.origin == probe.origin; });
        size_t probeIndex = static_cast<size_t>(found - scene.probes.begin());
        bool submerged = scene.hasWater && probeIndex < scene.submergedProbes.size() && scene.submergedProbes[probeIndex];
        begin();
        if (queries) { vkCmdResetQueryPool(command, queries, 0, 2); vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0); }
        if (scene.hasWater && !skyOnly) {
            float side = submerged ? -1.0f : 1.0f;
            auto reflected = glm::vec3(probe.origin); reflected.z = 2 * scene.waterHeight - reflected.z;
            renderViews(scene, reflected, probeIndex, false, 1, false, {0,0,side,-scene.waterHeight*side});
            copyWater(waterReflection);
            renderViews(scene, glm::vec3(probe.origin), probeIndex, false, 2, false, {0,0,-side,scene.waterHeight*side});
            copyWater(waterRefraction);
            waterViews += 12;
        }
        renderViews(scene, glm::vec3(probe.origin), probeIndex, skyOnly, 0, submerged, glm::vec4(0));
        barrier(color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,6}; copy.imageExtent = {size,size,1};
        vkCmdCopyImageToBuffer(command, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        if (queries) { vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queries, 1); }
        submit(); checked(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE), "Invalidate GPU readback");
        if (queries) {
            std::array<uint64_t,2> timestamps{};
            checked(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(timestamps), timestamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "Read GPU timings");
            gpuSeconds += double(timestamps[1] - timestamps[0]) * properties.limits.timestampPeriod * 1e-9;
        }
        Cube cube; cube.resolution = probe.resolution;
        const auto* pixels = static_cast<const glm::vec4*>(readback.mapping);
        for (unsigned face = 0; face < 6; ++face) {
            auto& output = cube.faces[face]; output.resize(size_t(cube.resolution) * cube.resolution);
            if (factor == 1) { std::memcpy(output.data(), pixels + size_t(face) * size * size, output.size() * sizeof(glm::vec4)); continue; }
            for (uint32_t y = 0; y < cube.resolution; ++y) { for (uint32_t x = 0; x < cube.resolution; ++x) {
                glm::vec4 value(0);
                for (uint32_t dy = 0; dy < factor; ++dy) { for (uint32_t dx = 0; dx < factor; ++dx) { value += pixels[(size_t(face) * size + y * factor + dy) * size + x * factor + dx]; } }
                output[size_t(y) * cube.resolution + x] = value / float(factor * factor);
            } }
        }
        ++captureCount; return cube;
    }
};
Renderer::Renderer(const RenderOptions& options) : impl(std::make_unique<Impl>(options)) { impl->initialize(); }
Renderer::~Renderer() = default;
void Renderer::prepare(const Scene& scene) { impl->prepare(scene); }
Cube Renderer::capture(const Scene& scene, const Probe& probe, bool skyOnly) { return impl->capture(scene, probe, skyOnly); }
Json Renderer::statistics() const {
    return {{"device", impl->properties.deviceName}, {"driver_version", impl->properties.driverVersion}, {"gpu_seconds", impl->gpuSeconds}, {"captures", impl->captureCount}, {"draw_calls", impl->drawCalls}, {"culled_draws", impl->culledDraws}, {"water_secondary_views", impl->waterViews}, {"water_reflection_levels", impl->waterViews ? 1 : 0}, {"texture_upload_bytes", impl->textureUploadBytes}, {"reused_textures", impl->reusedTextures}, {"upload_submissions", impl->uploadSubmissions}, {"upload_total_seconds", impl->uploadTotalSeconds}, {"upload_wait_seconds", impl->uploadWaitSeconds}, {"pipeline_seconds", impl->pipelineSeconds}, {"swapchain", false}};
}
}
