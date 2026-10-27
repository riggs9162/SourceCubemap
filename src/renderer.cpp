#include <scm/capture.hpp>
#include <volk.h>
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>
#include <bit>

namespace scm {
namespace {
void checked(VkResult result, std::string_view operation) { require(result == VK_SUCCESS, std::format("{} failed (Vulkan result {}).", operation, int(result))); }
float linear(float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); }
struct Buffer { VkBuffer buffer{}; VmaAllocation allocation{}; void* mapping{}; VkDeviceSize size{}; };
struct GpuImage { VkImage image{}; VmaAllocation allocation{}; VkImageView view{}; uint32_t mips = 1; };
struct Push { glm::mat4 projection; glm::vec4 tint; glm::vec4 controls; glm::uvec4 flags; };
static_assert(sizeof(Push) == 112);
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
    VkShaderModule vertexShader{}, fragmentShader{};
    VkSampler wrapSampler{}, clampSampler{};
    VkQueryPool queries{};
    bool anisotropy = false;
    std::map<unsigned, VkPipeline> pipelines;
    std::vector<GpuImage> images;
    std::vector<VkDescriptorSet> descriptors;
    Buffer vertices, indices, readback;
    GpuImage color, depth;
    std::array<VkImageView, 6> colorViews{}, depthViews{};
    uint32_t targetSize = 0;
    double gpuSeconds = 0;
    uint64_t captureCount = 0;

    explicit Impl(RenderOptions settings) : options(std::move(settings)) {}
    ~Impl() {
        if (device) { vkDeviceWaitIdle(device); }
        releaseScene(); releaseTargets();
        for (auto [key, pipeline] : pipelines) { vkDestroyPipeline(device, pipeline, nullptr); }
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
        for (auto& image : images) { destroy(image); }
        images.clear(); descriptors.clear(); destroy(vertices); destroy(indices);
    }
    void releaseTargets() {
        for (auto& view : colorViews) { if (view) { vkDestroyImageView(device, view, nullptr); view = {}; } }
        for (auto& view : depthViews) { if (view) { vkDestroyImageView(device, view, nullptr); view = {}; } }
        destroy(color); destroy(depth); destroy(readback); targetSize = 0;
    }
    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        Buffer buffer; buffer.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
        VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO;
        if (host) { allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT; }
        VmaAllocationInfo result{};
        checked(vmaCreateBuffer(allocator, &info, &allocation, &buffer.buffer, &buffer.allocation, &result), "Create buffer");
        buffer.mapping = result.pMappedData;
        return buffer;
    }
    VkImageView makeView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t firstLayer, uint32_t layers, uint32_t mips) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY; info.format = format;
        info.subresourceRange = {aspect, 0, mips, firstLayer, layers};
        VkImageView view{}; checked(vkCreateImageView(device, &info, nullptr, &view), "Create image view"); return view;
    }
    GpuImage makeImage(uint32_t width, uint32_t height, uint32_t layers, uint32_t mips, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        GpuImage result; result.mips = mips;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
        info.mipLevels = mips; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
        VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        checked(vmaCreateImage(allocator, &info, &allocation, &result.image, &result.allocation, nullptr), "Create image");
        result.view = makeView(result.image, format, aspect, 0, layers, mips);
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
        VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &supported;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        require(supported.dynamicRendering && supported.synchronization2, "GPU lacks Vulkan dynamic rendering or synchronization2.");
        anisotropy = features.features.samplerAnisotropy != 0;
        uint32_t queueCount = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount); vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
        bool found = false;
        for (uint32_t i = 0; i < queueCount; ++i) { if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueIndex = i; found = true; break; } }
        require(found, "GPU has no graphics queue.");
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queueInfo.queueFamilyIndex = queueIndex; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; enabled.dynamicRendering = VK_TRUE; enabled.synchronization2 = VK_TRUE;
        VkPhysicalDeviceFeatures basic{}; basic.samplerAnisotropy = anisotropy;
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
        std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) { bindings[i] = {i, i < 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size()); layoutInfo.pBindings = bindings.data();
        checked(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout), "Create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipelineInfo.setLayoutCount = 1; pipelineInfo.pSetLayouts = &descriptorLayout; pipelineInfo.pushConstantRangeCount = 1; pipelineInfo.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout), "Create pipeline layout");
        VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; checked(vkCreatePipelineCache(device, &cacheInfo, nullptr, &pipelineCache), "Create pipeline cache");
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
        if (pipelines.contains(key)) { return pipelines.at(key); }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "VSMain", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "PSMain", nullptr};
        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 7> attributes{{
            {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,position)}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,normal)},
            {2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,tangent)}, {3,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv)},
            {4,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,lightUV)}, {5,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,color)}, {6,0,VK_FORMAT_R32_SFLOAT,offsetof(Vertex,blend)}}};
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
        VkPipeline value{}; checked(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &value), "Create graphics pipeline"); pipelines.emplace(key, value); return value;
    }
    void targets(uint32_t size) {
        if (targetSize == size) { return; }
        releaseTargets();
        require(size <= properties.limits.maxImageDimension2D, "Capture resolution exceeds the GPU limit.");
        targetSize = size;
        color = makeImage(size, size, 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        depth = makeImage(size, size, 6, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        for (uint32_t i = 0; i < 6; ++i) {
            colorViews[i] = makeView(color.image, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 1);
            depthViews[i] = makeView(depth.image, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, i, 1, 1);
        }
        readback = makeBuffer(VkDeviceSize(size) * size * 6 * sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    void prepare(const Scene& scene) {
        checked(vkDeviceWaitIdle(device), "Wait before replacing scene"); releaseScene();
        require(!scene.vertices.empty() && !scene.indices.empty(), "The scene contains no renderable geometry.");
        vertices = makeBuffer(scene.vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        indices = makeBuffer(scene.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(vertices.mapping, scene.vertices.data(), static_cast<size_t>(vertices.size)); std::memcpy(indices.mapping, scene.indices.data(), static_cast<size_t>(indices.size));
        checked(vmaFlushAllocation(allocator, vertices.allocation, 0, VK_WHOLE_SIZE), "Flush vertices"); checked(vmaFlushAllocation(allocator, indices.allocation, 0, VK_WHOLE_SIZE), "Flush indices");
        for (const auto& source : scene.images) {
            uint32_t mipCount = source.name.starts_with("lightmap-") ? 1 : std::bit_width(std::max(source.width, source.height));
            auto image = makeImage(source.width, source.height, 1, mipCount, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            auto staging = makeBuffer(source.pixels.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            auto* pixels = static_cast<glm::vec4*>(staging.mapping);
            for (size_t i = 0; i < source.pixels.size(); ++i) {
                pixels[i] = source.pixels[i];
                if (source.srgb) { pixels[i].r = linear(pixels[i].r); pixels[i].g = linear(pixels[i].g); pixels[i].b = linear(pixels[i].b); }
            }
            checked(vmaFlushAllocation(allocator, staging.allocation, 0, VK_WHOLE_SIZE), "Flush texture staging");
            begin(); barrier(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, mipCount, 1);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {source.width, source.height, 1};
            vkCmdCopyBufferToImage(command, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            for (uint32_t mip = 1; mip < mipCount; ++mip) {
                barrier(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, mip - 1);
                VkImageBlit blit{}; blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1}; blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
                blit.srcOffsets[1] = {int32_t(std::max(1u, source.width >> (mip - 1))), int32_t(std::max(1u, source.height >> (mip - 1))), 1};
                blit.dstOffsets[1] = {int32_t(std::max(1u, source.width >> mip)), int32_t(std::max(1u, source.height >> mip)), 1};
                vkCmdBlitImage(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, mip - 1);
            }
            barrier(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, mipCount - 1);
            submit(); destroy(staging); images.push_back(image);
        }
        uint32_t count = static_cast<uint32_t>(scene.draws.size());
        std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 8}, {VK_DESCRIPTOR_TYPE_SAMPLER, count * 2}}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; poolInfo.maxSets = count; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes.data();
        checked(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "Create descriptor pool");
        std::vector<VkDescriptorSetLayout> layouts(count, descriptorLayout); descriptors.resize(count);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool = descriptorPool; allocation.descriptorSetCount = count; allocation.pSetLayouts = layouts.data();
        checked(vkAllocateDescriptorSets(device, &allocation, descriptors.data()), "Allocate material descriptors");
        for (uint32_t drawIndex = 0; drawIndex < count; ++drawIndex) {
            const auto& draw = scene.draws[drawIndex]; const auto& material = scene.materials[draw.material];
            std::array<uint32_t, 8> slots{material.base, material.second, material.normal, draw.lightmaps[0], draw.lightmaps[1], draw.lightmaps[2], draw.lightmaps[3], material.detail};
            std::array<VkDescriptorImageInfo, 10> values{}; std::array<VkWriteDescriptorSet, 10> writes{};
            for (uint32_t i = 0; i < 10; ++i) {
                if (i < 8) { values[i].imageView = images.at(slots[i]).view; values[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
                else { values[i].sampler = (i == 9 || scene.images[material.base].clamp) ? clampSampler : wrapSampler; }
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptors[drawIndex], i, 0, 1, i < 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, &values[i], nullptr, nullptr};
            }
            vkUpdateDescriptorSets(device, 10, writes.data(), 0, nullptr);
            unsigned key = (material.translucent ? 1u : 0u) | (material.additive ? 2u : 0u) | (material.noCull ? 4u : 0u) | (draw.skyBox ? 8u : 0u);
            pipeline(key);
        }
    }
    Cube capture(const Scene& scene, const Probe& probe) {
        uint32_t factor = options.quality == "high" ? 2 : 1;
        uint32_t size = probe.resolution * factor; targets(size);
        begin();
        if (queries) { vkCmdResetQueryPool(command, queries, 0, 2); vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, 0); }
        barrier(color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6);
        barrier(depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 6);
        const std::array<glm::vec3, 6> forward{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
        const std::array<glm::vec3, 6> up{{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}}};
        auto projection = glm::perspectiveLH_ZO(glm::radians(90.f), 1.f, 1.f, 1000000.f); projection[1][1] *= -1;
        for (uint32_t face = 0; face < 6; ++face) {
            VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; colorAttachment.imageView = colorViews[face]; colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; colorAttachment.clearValue.color = {{0,0,0,1}};
            VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depthAttachment.imageView = depthViews[face]; depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; depthAttachment.clearValue.depthStencil.depth = 1;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = {size,size}; rendering.layerCount = 1; rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &colorAttachment; rendering.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(command, &rendering);
            VkViewport viewport{0,0,float(size),float(size),0,1}; VkRect2D scissor{{0,0},{size,size}};
            vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
            VkDeviceSize offset = 0; vkCmdBindVertexBuffers(command, 0, 1, &vertices.buffer, &offset); vkCmdBindIndexBuffer(command, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            for (unsigned pass = 0; pass < 3; ++pass) {
                if (pass == 2) {
                    VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil.depth = 1;
                    VkClearRect rectangle{{{0,0},{size,size}},0,1}; vkCmdClearAttachments(command, 1, &clear, 1, &rectangle);
                }
                glm::vec3 origin = pass == 0 ? glm::vec3(0) : pass == 1 && scene.skyScale > 0 ? glm::vec3(probe.origin) / scene.skyScale + scene.skyOrigin : glm::vec3(probe.origin);
                auto viewProjection = projection * glm::lookAtLH(origin, origin + forward[face], up[face]);
                for (unsigned transparent = 0; transparent < 2; ++transparent) {
                    for (uint32_t i = 0; i < scene.draws.size(); ++i) {
                        const auto& draw = scene.draws[i]; const auto& material = scene.materials[draw.material];
                        if ((draw.skyBox ? 0u : draw.skyWorld ? 1u : 2u) != pass || bool(transparent) != (material.translucent || material.additive)) { continue; }
                        unsigned key = (material.translucent ? 1u : 0u) | (material.additive ? 2u : 0u) | (material.noCull ? 4u : 0u) | (draw.skyBox ? 8u : 0u);
                        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key));
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptors[i], 0, nullptr);
                        Push push{viewProjection, material.tint, {material.alphaReference, material.detailFactor, material.detailScale, 0}, {material.flags | (draw.lightmaps[0] ? BumpLight : 0u), uint32_t(material.detailMode), 0, 0}};
                        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
                        vkCmdDrawIndexed(command, draw.indexCount, 1, draw.firstIndex, 0, 0);
                    }
                }
            }
            vkCmdEndRendering(command);
        }
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
Cube Renderer::capture(const Scene& scene, const Probe& probe) { return impl->capture(scene, probe); }
Json Renderer::statistics() const { return {{"device", impl->properties.deviceName}, {"driver_version", impl->properties.driverVersion}, {"gpu_seconds", impl->gpuSeconds}, {"captures", impl->captureCount}, {"swapchain", false}}; }
}
