// SPDX-License-Identifier: GPL-3.0-or-later
// vulkan - implementation of module daemon.ui.vulkan.
module;
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
module daemon.ui.vulkan;
import std;
import daemon.base.util;
import daemon.ui.draw;
import daemon.ui.font;

namespace ide {

const char* resultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "VkResult(other)";
    }
}

}  // namespace ide

namespace ide {

std::expected<void, std::string> Renderer::init(void* wlDisplay, void* wlSurface) {
        try {
            display_ = static_cast<wl_display*>(wlDisplay);
            wlSurface_ = static_cast<wl_surface*>(wlSurface);
            createInstance();
            createSurface(wlSurface_);
            pickDevice();
            createDevice();
            createAtlas();
            createDescriptors();
            createFrames();
            createSwapchain();
        } catch (const VkFailure& e) {
            return std::unexpected(std::string(e.what()));
        }
        return {};
    }

bool Renderer::translucent() {
        if (!probed_ && phys_ && surface_) {
            VkSurfaceCapabilitiesKHR caps;
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) == VK_SUCCESS)
                translucent_ = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0 && !noAlpha_;
            probed_ = true;
        }
        return translucent_;
    }

RenderStatus Renderer::render(const DrawList& dl, FontAtlas& font, Color clear) {
        if (needInit_) {  // an earlier rebuild failed (GPU not back yet after resume): retry at a gentle pace
            if (std::chrono::steady_clock::now() < nextRebuild_) return RenderStatus::Failed;
            if (!rebuild()) return RenderStatus::Failed;
        }
        static const bool testRebuild = std::getenv("DAEMON_TEST_REBUILD") != nullptr;  // debug: simulate device loss
        if (testRebuild && ++testFrames_ % 4 == 0) { rebuild(); return RenderStatus::Failed; }
        try {
            stalled_ = false;
            RenderStatus st = renderImpl(dl, font, clear);
            // Watchdog: fence/acquire timeouts that never end (GPU or compositor connection wedged after
            // suspend without reporting an error) -> rebuild the renderer instead of freezing the picture.
            const auto now = std::chrono::steady_clock::now();
            if (st == RenderStatus::Presented || !stalled_) stallSince_ = {};
            else if (stallSince_ == std::chrono::steady_clock::time_point{}) stallSince_ = now;
            else if (now - stallSince_ > std::chrono::seconds(3)) {
                logLine("renderer stalled for 3 s, rebuilding");
                stallSince_ = {};
                rebuild();
                return RenderStatus::Failed;
            }
            return st;
        } catch (const VkFailure& e) {
            logLine(std::format("render error: {}", e.what()));
            // A lost device or surface (suspend/resume, GPU reset, driver restart) cannot be repaired by
            // recreating the swapchain: rebuild the whole Vulkan state, the atlas is re-uploaded in full.
            if (e.result == VK_ERROR_DEVICE_LOST || e.result == VK_ERROR_SURFACE_LOST_KHR ||
                e.result == VK_ERROR_INITIALIZATION_FAILED || e.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                rebuild();
            } else {
                try { recoverFrames(); } catch (const VkFailure&) { rebuild(); }
            }
            return RenderStatus::Failed;
        }
    }

bool Renderer::rebuild() {
        const std::uint32_t w = reqW_, h = reqH_;
        destroy();
        needInit_ = true;
        nextRebuild_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        auto r = init(display_, wlSurface_);
        if (!r) { logLine(std::format("renderer rebuild failed: {}", r.error())); destroy(); return false; }
        reqW_ = w; reqH_ = h; swapDirty_ = true;
        needInit_ = false;
        logLine(std::format("renderer rebuilt on {}", deviceName_));
        return true;
    }

// After a failed frame the acquire semaphore may still be signaled and the swapchain stale
// (surface lost, suspend/resume): rebuild the per-frame sync objects and the swapchain.
void Renderer::recoverFrames() {
    vkDeviceWaitIdle(device_);
    for (auto& f : frames_) {
        if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        f.imageAvailable = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &si, nullptr, &f.imageAvailable);
        if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        f.fence = VK_NULL_HANDLE;
        vkCreateFence(device_, &fi, nullptr, &f.fence);
    }
    swapDirty_ = true;
}

void Renderer::createInstance() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "ide";
        app.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> exts{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
        std::vector<const char*> layers;
        if (const char* v = std::getenv("IDE_VALIDATE"); v && *v == '1') layers.push_back("VK_LAYER_KHRONOS_validation");

        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        ci.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        ci.ppEnabledLayerNames = layers.data();
        check(vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance (is a Vulkan 1.3 driver with Wayland support installed?)");
    }

void Renderer::pickDevice() {
        std::uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        if (n == 0) throw VkFailure("no Vulkan physical devices");
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance_, &n, devs.data());

        int forced = -1;
        if (const char* g = std::getenv("IDE_GPU")) forced = std::atoi(g);

        int bestScore = -1;
        for (std::uint32_t d = 0; d < n; ++d) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devs[d], &props);
            if (props.apiVersion < VK_API_VERSION_1_3) continue;

            std::uint32_t en = 0;
            vkEnumerateDeviceExtensionProperties(devs[d], nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> exts(en);
            vkEnumerateDeviceExtensionProperties(devs[d], nullptr, &en, exts.data());
            bool swap = std::ranges::any_of(exts, [](const auto& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            });
            if (!swap) continue;

            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
            vkGetPhysicalDeviceFeatures2(devs[d], &f2);
            if (!f13.dynamicRendering || !f13.synchronization2) continue;

            std::uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qn, qs.data());
            int family = -1;
            for (std::uint32_t q = 0; q < qn; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, surface_, &present);
                if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { family = static_cast<int>(q); break; }
            }
            if (family < 0) continue;

            int score = 1;
            switch (props.deviceType) {
                // A 2D UI needs no discrete GPU: the integrated one draws far less power, keeps the dGPU
                // asleep and avoids cross-GPU copies to the compositor. IDE_GPU=<index> overrides.
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 1000; break;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 500; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 100; break;
                default: break;
            }
            if (forced == static_cast<int>(d)) score = 100000;
            if (score > bestScore) {
                bestScore = score;
                phys_ = devs[d];
                queueFamily_ = static_cast<std::uint32_t>(family);
                deviceName_ = props.deviceName;
            }
        }
        if (phys_ == VK_NULL_HANDLE)
            throw VkFailure("no Vulkan 1.3 device with dynamic rendering + Wayland presentation found");
        vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    }

void Renderer::createDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;

        const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qci;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = exts;
        check(vkCreateDevice(phys_, &ci, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

void Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
                      VkDeviceMemory& mem, void** map) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &bi, nullptr, &buf), "vkCreateBuffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
        check(vkAllocateMemory(device_, &ai, nullptr, &mem), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, buf, mem, 0), "vkBindBufferMemory");
        if (map) check(vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, map), "vkMapMemory");
    }

void Renderer::createAtlas() {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8_UNORM;
        ii.extent = {FontAtlas::Size, FontAtlas::Size, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &ii, nullptr, &atlasImage_), "vkCreateImage(atlas)");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, atlasImage_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &ai, nullptr, &atlasMem_), "vkAllocateMemory(atlas)");
        check(vkBindImageMemory(device_, atlasImage_, atlasMem_, 0), "vkBindImageMemory");

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = atlasImage_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &vi, nullptr, &atlasView_), "vkCreateImageView(atlas)");

        // Glyphs are placed on whole pixels, so nearest filtering keeps them perfectly sharp.
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device_, &si, nullptr, &sampler_), "vkCreateSampler");
    }

void Renderer::createDescriptors() {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        check(vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

        VkSamplerCreateInfo lsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        lsi.magFilter = lsi.minFilter = VK_FILTER_LINEAR;
        lsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        lsi.addressModeU = lsi.addressModeV = lsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device_, &lsi, nullptr, &linearSampler_), "vkCreateSampler(linear)");

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        check(vkCreateDescriptorPool(device_, &pi, nullptr, &descPool_), "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        check(vkAllocateDescriptorSets(device_, &ai, &descSet_), "vkAllocateDescriptorSets");
        check(vkAllocateDescriptorSets(device_, &ai, &glassSet_), "vkAllocateDescriptorSets(glass)");

        VkDescriptorImageInfo img{sampler_, atlasView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = descSet_;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

        VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 2};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pc;
        check(vkCreatePipelineLayout(device_, &pli, nullptr, &pipeLayout_), "vkCreatePipelineLayout");
    }

void Renderer::createFrames() {
        for (auto& f : frames_) {
            VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = queueFamily_;
            check(vkCreateCommandPool(device_, &pi, nullptr, &f.pool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device_, &ai, &f.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device_, &fi, nullptr, &f.fence), "vkCreateFence");
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device_, &si, nullptr, &f.imageAvailable), "vkCreateSemaphore");

            const auto hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            createBuffer(kMaxInstances * sizeof(Instance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible,
                         f.instances, f.instancesMem, &f.instancesMap);
            createBuffer(static_cast<VkDeviceSize>(FontAtlas::Size) * FontAtlas::Size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         hostVisible, f.staging, f.stagingMem, &f.stagingMap);
        }
    }

VkShaderModule Renderer::makeShader(const std::uint32_t* code, std::size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m;
        check(vkCreateShaderModule(device_, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }

void Renderer::createPipeline() {
        if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        VkShaderModule vs = makeShader(kVertSpv, sizeof(kVertSpv));
        VkShaderModule fs = makeShader(kFragSpv, sizeof(kFragSpv));

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

        VkVertexInputBindingDescription bind{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
        VkVertexInputAttributeDescription attrs[4] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 32},
            {3, 0, VK_FORMAT_R32_SFLOAT, 36},
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;

        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 3;
        ds.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &format_;

        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        pi.stageCount = 2;
        pi.pStages = stages;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &ds;
        pi.layout = pipeLayout_;
        VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);
        if (r == VK_SUCCESS) {
            // Erase variant: colour lerps to the (premultiplied) source, alpha lerps to the constant frame alpha.
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            if (erasePipeline_) vkDestroyPipeline(device_, erasePipeline_, nullptr);
            r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &erasePipeline_);
        }
        VkShaderModule gs = makeShader(kGlassSpv, sizeof(kGlassSpv));
        if (r == VK_SUCCESS) {
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            stages[1].module = gs;
            if (glassPipeline_) vkDestroyPipeline(device_, glassPipeline_, nullptr);
            r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &glassPipeline_);
        }
        vkDestroyShaderModule(device_, gs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        check(r, "vkCreateGraphicsPipelines");
    }

void Renderer::createSwapchain() {
        vkDeviceWaitIdle(device_);
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps), "surface capabilities");

        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            ext.width = std::clamp(reqW_, caps.minImageExtent.width, caps.maxImageExtent.width);
            ext.height = std::clamp(reqH_, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (ext.width == 0 || ext.height == 0 || reqW_ == 0 || reqH_ == 0) return;

        std::uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fn, fmts.data());
        VkSurfaceFormatKHR chosen = fmts.at(0);
        // We blend in gamma space (colours are authored as sRGB values), so prefer a UNORM target.
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32})
            for (auto& f : fmts)
                if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; goto picked; }
    picked:
        const bool formatChanged = chosen.format != format_;
        format_ = chosen.format;

        std::uint32_t count = std::max(3u, caps.minImageCount);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        canShot_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        if (canShot_) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        translucent_ = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0 && !noAlpha_;
        ci.compositeAlpha = translucent_ ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
                            : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        // MAILBOX never blocks the caller when the compositor stops releasing buffers (hidden / unfocused
        // window); FIFO present can block the whole UI thread forever on some drivers. The app paces
        // frames itself with wl_surface.frame callbacks.
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        {
            std::uint32_t pn = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pn, nullptr);
            std::vector<VkPresentModeKHR> modes(pn);
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pn, modes.data());
            if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain_;

        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &fresh);
        destroySwapchainViews();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        check(r, "vkCreateSwapchainKHR");
        swapchain_ = fresh;
        extent_ = ext;

        std::uint32_t n = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        images_.resize(n);
        vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
        for (VkImage img : images_) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            check(vkCreateImageView(device_, &vi, nullptr, &view), "vkCreateImageView");
            views_.push_back(view);
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore sem;
            check(vkCreateSemaphore(device_, &si, nullptr, &sem), "vkCreateSemaphore");
            renderDone_.push_back(sem);
        }
        if (!pipeline_ || formatChanged) createPipeline();
        if (canShot_) createScene();
        swapDirty_ = false;
    }

void Renderer::createScene() {
        destroyScene();
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format_;
        ii.extent = {extent_.width, extent_.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &ii, nullptr, &sceneImage_), "vkCreateImage(scene)");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, sceneImage_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &ai, nullptr, &sceneMem_), "vkAllocateMemory(scene)");
        check(vkBindImageMemory(device_, sceneImage_, sceneMem_, 0), "vkBindImageMemory(scene)");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = sceneImage_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &vi, nullptr, &sceneView_), "vkCreateImageView(scene)");
        VkDescriptorImageInfo img{linearSampler_, sceneView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = glassSet_;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

void Renderer::destroySwapchainViews() {
        for (auto s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
        for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
        renderDone_.clear();
        views_.clear();
        images_.clear();
    }

void Renderer::barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
    }

RenderStatus Renderer::renderImpl(const DrawList& dl, FontAtlas& font, Color clear) {
        if (swapDirty_ || !swapchain_) createSwapchain();
        if (!swapchain_) return RenderStatus::Skipped;

        Frame& f = frames_[frameIdx_];
        {   // bounded: a stalled GPU (e.g. after suspend) must not freeze the UI thread
            VkResult wr = vkWaitForFences(device_, 1, &f.fence, VK_TRUE, 200'000'000);
            if (wr == VK_TIMEOUT) { stalled_ = true; return RenderStatus::Skipped; }
            check(wr, "vkWaitForFences");
        }

        // A bounded wait keeps the app responsive if the compositor stops releasing buffers
        // (e.g. the window is hidden) so Wayland pings are still answered.
        std::uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, 50'000'000, f.imageAvailable, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { swapDirty_ = true; return RenderStatus::Skipped; }
        if (ar == VK_TIMEOUT || ar == VK_NOT_READY) { stalled_ = true; return RenderStatus::Skipped; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) check(ar, "vkAcquireNextImageKHR");
        if (ar == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
        // --- upload instance data
        std::size_t count = std::min(dl.size(), kMaxInstances);
        if (dl.size() > kMaxInstances) std::println(std::cerr, "warning: draw list truncated ({} quads)", dl.size());
        std::memcpy(f.instancesMap, dl.items().data(), count * sizeof(Instance));

        // --- atlas upload (only the rows that changed)
        int y0 = 0, y1 = 0;
        bool atlasDirty = font.takeDirty(y0, y1);
        if (!atlasReady_) { atlasDirty = true; y0 = 0; y1 = FontAtlas::Size; }
        if (atlasDirty) {
            std::memcpy(static_cast<char*>(f.stagingMap) + static_cast<std::size_t>(y0) * FontAtlas::Size,
                        font.pixels() + static_cast<std::size_t>(y0) * FontAtlas::Size,
                        static_cast<std::size_t>(y1 - y0) * FontAtlas::Size);
        }

        VkCommandBuffer cb = f.cmd;
        check(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");

        if (atlasDirty) {
            barrier(cb, atlasImage_, atlasReady_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    atlasReady_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(y0) * FontAtlas::Size;
            region.bufferRowLength = FontAtlas::Size;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {0, y0, 0};
            region.imageExtent = {FontAtlas::Size, static_cast<std::uint32_t>(y1 - y0), 1};
            vkCmdCopyBufferToImage(cb, f.staging, atlasImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            barrier(cb, atlasImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            atlasReady_ = true;
        }

        VkImage image = images_[idx];
        barrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const float ca = translucent_ ? clear.a / 255.0f : 1.0f;  // premultiplied for the compositor
        // Split the draw list into runs of one mode. The first Glass run starts the overlay pass.
        struct Run { std::uint32_t from, to; DrawList::Mode mode; };
        std::vector<Run> runs;
        {
            std::uint32_t at = 0;
            DrawList::Mode m = DrawList::Mode::Normal;
            auto push = [&](std::uint32_t to) {
                to = std::min<std::uint32_t>(to, static_cast<std::uint32_t>(count));
                if (to > at) runs.push_back({at, to, m});
                at = std::max(at, to);
            };
            for (auto sg : dl.segs()) { push(sg.at); m = sg.mode; }
            push(static_cast<std::uint32_t>(count));
        }
        if (!canShot_) for (auto& r : runs) if (r.mode == DrawList::Mode::Glass) r.mode = DrawList::Mode::Normal;
        std::size_t overlayRun = runs.size();
        for (std::size_t i = 0; i < runs.size(); ++i) if (runs[i].mode == DrawList::Mode::Glass) { overlayRun = i; break; }

        auto beginPass = [&](bool clearIt) {
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = views_[idx];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = clearIt ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{clear.r / 255.0f * ca, clear.g / 255.0f * ca, clear.b / 255.0f * ca, ca}};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, extent_};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            vkCmdBeginRendering(cb, &ri);
            VkViewport vp{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
            VkRect2D sc{{0, 0}, extent_};
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            float viewport[2] = {static_cast<float>(extent_.width), static_cast<float>(extent_.height)};
            vkCmdPushConstants(cb, pipeLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewport), viewport);
            const float bc[4] = {0, 0, 0, ca};
            vkCmdSetBlendConstants(cb, bc);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &f.instances, &off);
        };
        auto drawRuns = [&](std::size_t lo, std::size_t hi) {
            for (std::size_t i = lo; i < hi; ++i) {
                const Run& r = runs[i];
                VkPipeline pl = r.mode == DrawList::Mode::Erase ? erasePipeline_
                                : r.mode == DrawList::Mode::Glass ? glassPipeline_ : pipeline_;
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
                VkDescriptorSet set = r.mode == DrawList::Mode::Glass ? glassSet_ : descSet_;
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set, 0, nullptr);
                vkCmdDraw(cb, 4, r.to - r.from, 0, r.from);
            }
        };

        beginPass(true);
        drawRuns(0, overlayRun);
        vkCmdEndRendering(cb);
        if (overlayRun < runs.size()) {
            barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            barrier(cb, sceneImage_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy cp{};
            cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {extent_.width, extent_.height, 1};
            vkCmdCopyImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sceneImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            barrier(cb, sceneImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
            beginPass(false);
            drawRuns(overlayRun, runs.size());
            vkCmdEndRendering(cb);
        }

        const bool shot = canShot_ && !shotPath_.empty();
        VkBuffer shotBuf = VK_NULL_HANDLE;
        VkDeviceMemory shotMem = VK_NULL_HANDLE;
        void* shotMap = nullptr;
        if (shot) {
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
            createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, shotBuf, shotMem,
                         &shotMap);
            barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuf, 1, &region);
            barrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
        } else {
            barrier(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_ACCESS_2_NONE);
        }
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone_[idx];
        check(vkResetFences(device_, 1, &f.fence), "vkResetFences");
        check(vkQueueSubmit(queue_, 1, &si, f.fence), "vkQueueSubmit");

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone_[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(queue_, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) swapDirty_ = true;
        else if (pr != VK_SUCCESS) check(pr, "vkQueuePresentKHR");

        if (shot) {
            check(vkWaitForFences(device_, 1, &f.fence, VK_TRUE, 5'000'000'000), "vkWaitForFences(screenshot)");
            std::vector<std::uint8_t> px(static_cast<std::size_t>(extent_.width) * extent_.height * 4);
            std::memcpy(px.data(), shotMap, px.size());
            const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
            for (std::size_t i = 0; i < px.size(); i += 4) {
                if (bgra) std::swap(px[i], px[i + 2]);
                if (translucent_ && px[i + 3] != 255) {  // composite over a stand-in wallpaper (the real one is blurred)
                    const int a = px[i + 3];
                    const int bgc[3] = {0x52, 0x5a, 0x74};
                    for (int k = 0; k < 3; ++k) px[i + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(std::min(255, px[i + static_cast<std::size_t>(k)] + bgc[k] * (255 - a) / 255));
                }
                px[i + 3] = 255;
            }
            if (writePng(shotPath_, extent_.width, extent_.height, px)) {
                std::println("screenshot written to {} ({}x{})", shotPath_, extent_.width, extent_.height);
            } else {
                std::println(std::cerr, "could not write screenshot to {}", shotPath_);
            }
            vkDestroyBuffer(device_, shotBuf, nullptr);
            vkFreeMemory(device_, shotMem, nullptr);
            shotPath_.clear();
            shotDone_ = true;
        }

        frameIdx_ = (frameIdx_ + 1) % kFramesInFlight;
        return RenderStatus::Presented;
    }

void Renderer::destroy() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            destroySwapchainViews();
            if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (erasePipeline_) vkDestroyPipeline(device_, erasePipeline_, nullptr);
            if (glassPipeline_) vkDestroyPipeline(device_, glassPipeline_, nullptr);
            destroyScene();
            if (linearSampler_) vkDestroySampler(device_, linearSampler_, nullptr);
            for (auto& f : frames_) {
                if (f.instances) vkDestroyBuffer(device_, f.instances, nullptr);
                if (f.instancesMem) vkFreeMemory(device_, f.instancesMem, nullptr);
                if (f.staging) vkDestroyBuffer(device_, f.staging, nullptr);
                if (f.stagingMem) vkFreeMemory(device_, f.stagingMem, nullptr);
                if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
                if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
                if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
            }
            if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
            if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
            if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (atlasView_) vkDestroyImageView(device_, atlasView_, nullptr);
            if (atlasImage_) vkDestroyImage(device_, atlasImage_, nullptr);
            if (atlasMem_) vkFreeMemory(device_, atlasMem_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) {
            if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
            vkDestroyInstance(instance_, nullptr);
        }
        device_ = VK_NULL_HANDLE;
        instance_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE; phys_ = VK_NULL_HANDLE; queue_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE; pipeline_ = erasePipeline_ = glassPipeline_ = VK_NULL_HANDLE;
        pipeLayout_ = VK_NULL_HANDLE; descPool_ = VK_NULL_HANDLE; setLayout_ = VK_NULL_HANDLE;
        sampler_ = linearSampler_ = VK_NULL_HANDLE;
        atlasView_ = VK_NULL_HANDLE; atlasImage_ = VK_NULL_HANDLE; atlasMem_ = VK_NULL_HANDLE;
        descSet_ = glassSet_ = VK_NULL_HANDLE;
        frames_ = {}; frameIdx_ = 0; atlasReady_ = false; probed_ = false; format_ = VK_FORMAT_UNDEFINED;
        extent_ = {}; images_.clear(); views_.clear(); renderDone_.clear(); swapDirty_ = true;
    }

}  // namespace ide
