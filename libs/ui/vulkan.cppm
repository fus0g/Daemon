// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.vulkan — the renderer.
//
// One graphics pipeline draws everything: a single instanced triangle strip per quad,
// sampling an R8 coverage atlas (glyphs, plus a solid white block for rectangles).
// Vulkan 1.3 dynamic rendering + synchronization2, FIFO presentation on a Wayland surface.
module;
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
export module daemon.ui.vulkan;
import std;
import daemon.base.util;
import daemon.ui.draw;
import daemon.ui.font;

namespace ide {

// Module-linkage (not exported) helpers.
inline constexpr std::uint32_t kVertSpv[] = {
#include "quad.vert.inc"
};
inline constexpr std::uint32_t kFragSpv[] = {
#include "quad.frag.inc"
};

inline constexpr std::uint32_t kGlassSpv[] = {
#include "glass.frag.inc"
};

inline constexpr int kFramesInFlight = 2;
inline constexpr std::size_t kMaxInstances = 1u << 17;

struct VkFailure : std::runtime_error {
    explicit VkFailure(const std::string& m, VkResult r = VK_ERROR_UNKNOWN) : std::runtime_error(m), result(r) {}
    VkResult result;
};

const char* resultName(VkResult r);

inline void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw VkFailure(std::string(what) + " failed: " + resultName(r), r);
}

}  // namespace ide

export namespace ide {

enum class RenderStatus { Presented, Skipped, Failed };

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer() { destroy(); }

    // `wlDisplay` / `wlSurface` are the wl_display* and wl_surface* of the window.
    std::expected<void, std::string> init(void* wlDisplay, void* wlSurface);

    // Size of the drawable in physical pixels.
    void setExtent(std::uint32_t w, std::uint32_t h) {
        if (w != reqW_ || h != reqH_) { reqW_ = w; reqH_ = h; swapDirty_ = true; }
    }

    const std::string& deviceName() const { return deviceName_; }
    // The next presented frame is also written to `path` as a PNG.
    void requestScreenshot(std::string path) { shotPath_ = std::move(path); shotDone_ = false; }
    bool screenshotDone() const { return shotDone_; }
    // True when the swapchain can carry per-pixel alpha (the compositor blends / blurs it with what is behind).
    bool translucent();

    RenderStatus render(const DrawList& dl, FontAtlas& font, Color clear);

    // Tears everything down and builds it again (device or surface lost, e.g. after suspend/resume).
    bool rebuild();

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;
        VkDeviceMemory instancesMem = VK_NULL_HANDLE;
        void* instancesMap = nullptr;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void* stagingMap = nullptr;
    };

    // ------------------------------------------------------------ setup

    void createInstance();

    void createSurface(wl_surface* surface) {
        VkWaylandSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
        ci.display = display_;
        ci.surface = surface;
        check(vkCreateWaylandSurfaceKHR(instance_, &ci, nullptr, &surface_), "vkCreateWaylandSurfaceKHR");
    }

    void pickDevice();

    void createDevice();

    std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags want) const {
        for (std::uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
        throw VkFailure("no suitable Vulkan memory type");
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
                      VkDeviceMemory& mem, void** map);

    void createAtlas();

    void createDescriptors();

    void createFrames();

    VkShaderModule makeShader(const std::uint32_t* code, std::size_t bytes);

    void createPipeline();

    // (Re)creates the swapchain for the current requested extent. Leaves extent_ at 0x0
    // (and swapchain_ null) when the surface has no size yet.
    void createSwapchain();
    void recoverFrames();

    // A sampled copy of the frame (taken between the scene and overlay passes) that the glass shader blurs.
    void destroyScene() {
        if (sceneView_) vkDestroyImageView(device_, sceneView_, nullptr);
        if (sceneImage_) vkDestroyImage(device_, sceneImage_, nullptr);
        if (sceneMem_) vkFreeMemory(device_, sceneMem_, nullptr);
        sceneView_ = VK_NULL_HANDLE; sceneImage_ = VK_NULL_HANDLE; sceneMem_ = VK_NULL_HANDLE;
    }

    void createScene();

    void destroySwapchainViews();

    // ------------------------------------------------------------ per frame

    void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

    RenderStatus renderImpl(const DrawList& dl, FontAtlas& font, Color clear);

    // ------------------------------------------------------------ teardown

    void destroy();

    wl_display* display_ = nullptr;
    wl_surface* wlSurface_ = nullptr;
    bool needInit_ = false;
    int testFrames_ = 0;
    bool stalled_ = false;
    std::chrono::steady_clock::time_point stallSince_{};
    std::chrono::steady_clock::time_point nextRebuild_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    std::string deviceName_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> renderDone_;
    std::uint32_t reqW_ = 0, reqH_ = 0;
    bool swapDirty_ = true;

    VkImage atlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory atlasMem_ = VK_NULL_HANDLE;
    VkImageView atlasView_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    bool atlasReady_ = false;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline erasePipeline_ = VK_NULL_HANDLE;
    VkPipeline glassPipeline_ = VK_NULL_HANDLE;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkDescriptorSet glassSet_ = VK_NULL_HANDLE;
    VkImage sceneImage_ = VK_NULL_HANDLE;
    VkDeviceMemory sceneMem_ = VK_NULL_HANDLE;
    VkImageView sceneView_ = VK_NULL_HANDLE;
    bool translucent_ = false, probed_ = false, noAlpha_ = std::getenv("DAEMON_OPAQUE") != nullptr;

    std::array<Frame, kFramesInFlight> frames_{};
    int frameIdx_ = 0;

    std::string shotPath_;
    bool shotDone_ = false, canShot_ = false;
};

}  // namespace ide
