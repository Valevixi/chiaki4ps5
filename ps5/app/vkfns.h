// Vulkan function table for chiaki-ps5. RADV on the PS5 has no loader, so every
// command is fetched through vk_icdGetInstanceProcAddr / vkGetDeviceProcAddr
// into global pointers named like the real commands (ps5-framegen uses them too).
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define VKFN_INSTANCE(X) \
	X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
	X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr) X(EnumerateDeviceExtensionProperties) \
	X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR) X(CreateDisplayPlaneSurfaceKHR) \
	X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR)

#define VKFN_DEVICE(X) \
	X(GetDeviceQueue) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
	X(ResetCommandBuffer) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(CreateFence) X(WaitForFences) \
	X(ResetFences) X(DestroyFence) X(CreateSemaphore) X(DestroySemaphore) X(CreateSwapchainKHR) \
	X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateImage) X(DestroyImage) \
	X(CreateImageView) X(DestroyImageView) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
	X(BindImageMemory) X(BindBufferMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
	X(MapMemory) X(UnmapMemory) X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) \
	X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
	X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) \
	X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdPipelineBarrier) \
	X(CmdCopyBufferToImage) X(CmdBlitImage) X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) \
	X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDraw) X(CmdSetViewport) X(CmdSetScissor) X(CmdDispatch) \
	X(CmdClearColorImage) X(CmdFillBuffer) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(CreateQueryPool) \
	X(DestroyQueryPool) X(GetQueryPoolResults)

#define VKFN_DECLARE(n) extern PFN_vk##n vk##n;
extern PFN_vkCreateInstance vkCreateInstance;
VKFN_INSTANCE(VKFN_DECLARE)
VKFN_DEVICE(VKFN_DECLARE)
#undef VKFN_DECLARE
