/* What the files of the Vulkan back end share across translation units. nvn_vk.c is still one unit with the files it
 * includes (nvn_vk_draw.c, which includes nvn_vk_m4.c and the others); a file that is a unit of its own reaches them
 * through this header, which the unit includes where that file used to be included. */
#ifndef NVN_VK_INT_H
#define NVN_VK_INT_H
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
/* Session 2: the Khronos headers (reference\upstream\Vulkan-Headers\include, -I in build.py) replace our own vk_min.h subset. */
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS                       /* VK_KHR_portability_subset (MoltenVK) is declared in vulkan_beta.h */
#endif
#include <vulkan/vulkan.h>
LONG64 tk(void);                                        /* nvn_vk_draw.c: performance counter ticks */
uint64_t fnv(uint64_t h, const void *p, size_t n);      /* nvn_vk_m4.c */
extern int g_m4_quit;                                   /* nvn_vk_m4.c: the game is closing */
extern volatile int g_store_pct;                        /* nvn_vk_m4.c: how much of data.arc the store has read */
int vks_build(const char *dir);                         /* nvn_vk_store.c: the container store, from data.arc */
/* ---------------------------------------------------------------- the loader's functions
 * VKF: the loader's and the instance's (nvn_vk.c nvn_vk_init loads them); VKD: the device's (nvn_vk_draw.c vkd_init).
 * The pointers are pfn_<function>, defined in those two files: a global vkCreateDevice of ours would have the name of
 * the loader's own export. The defines after the tables keep the Vulkan name at every call. A function added to VKF
 * or VKD needs its define too; without it, its calls do not compile. */
#ifdef _WIN32
#define VKF_SURFACE(X) X(VkResult, vkCreateWin32SurfaceKHR, (VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *))
#else
#define VKF_SURFACE(X)
#endif
#define VKF(X) \
    X(VkResult, vkEnumerateInstanceLayerProperties, (uint32_t *, VkLayerProperties *)) \
    X(VkResult, vkEnumerateInstanceExtensionProperties, (const char *, uint32_t *, VkExtensionProperties *)) \
    X(VkResult, vkCreateInstance, (const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *)) \
    X(void, vkDestroyInstance, (VkInstance, const VkAllocationCallbacks *)) \
    X(PFN_vkVoidFunction, vkGetInstanceProcAddr, (VkInstance, const char *)) \
    X(VkResult, vkEnumeratePhysicalDevices, (VkInstance, uint32_t *, VkPhysicalDevice *)) \
    X(void, vkGetPhysicalDeviceProperties, (VkPhysicalDevice, VkPhysicalDeviceProperties *)) \
    X(void, vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *)) \
    X(VkResult, vkEnumerateDeviceExtensionProperties, (VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *)) \
    VKF_SURFACE(X) \
    X(void, vkDestroySurfaceKHR, (VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceFormatsKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfacePresentModesKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *)) \
    X(VkResult, vkCreateDevice, (VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *)) \
    X(void, vkDestroyDevice, (VkDevice, const VkAllocationCallbacks *)) \
    X(void, vkGetDeviceQueue, (VkDevice, uint32_t, uint32_t, VkQueue *)) \
    X(VkResult, vkDeviceWaitIdle, (VkDevice)) \
    X(VkResult, vkCreateSwapchainKHR, (VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *)) \
    X(void, vkDestroySwapchainKHR, (VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetSwapchainImagesKHR, (VkDevice, VkSwapchainKHR, uint32_t *, VkImage *)) \
    X(VkResult, vkAcquireNextImageKHR, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *)) \
    X(VkResult, vkQueuePresentKHR, (VkQueue, const VkPresentInfoKHR *)) \
    X(VkResult, vkQueueSubmit, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence)) \
    X(VkResult, vkCreateCommandPool, (VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *)) \
    X(void, vkDestroyCommandPool, (VkDevice, VkCommandPool, const VkAllocationCallbacks *)) \
    X(VkResult, vkAllocateCommandBuffers, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *)) \
    X(VkResult, vkBeginCommandBuffer, (VkCommandBuffer, const VkCommandBufferBeginInfo *)) \
    X(VkResult, vkEndCommandBuffer, (VkCommandBuffer)) \
    X(VkResult, vkResetCommandBuffer, (VkCommandBuffer, VkFlags)) \
    X(void, vkCmdPipelineBarrier, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *)) \
    X(void, vkCmdClearColorImage, (VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)) \
    X(void, vkCmdCopyImage, (VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *)) \
    X(VkResult, vkCreateSemaphore, (VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *)) \
    X(void, vkDestroySemaphore, (VkDevice, VkSemaphore, const VkAllocationCallbacks *)) \
    X(VkResult, vkCreateFence, (VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *)) \
    X(void, vkDestroyFence, (VkDevice, VkFence, const VkAllocationCallbacks *)) \
    X(VkResult, vkWaitForFences, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t)) \
    X(VkResult, vkResetFences, (VkDevice, uint32_t, const VkFence *))
#define X(ret, name, args) extern ret (VKAPI_PTR *pfn_##name) args;
VKF(X)
#undef X
#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache) X(vkCmdCopyBuffer)
#define X(n) extern PFN_##n pfn_##n;
VKD(X)
#undef X
#ifdef _WIN32
#define vkCreateWin32SurfaceKHR pfn_vkCreateWin32SurfaceKHR
#endif
#define vkEnumerateInstanceLayerProperties pfn_vkEnumerateInstanceLayerProperties
#define vkEnumerateInstanceExtensionProperties pfn_vkEnumerateInstanceExtensionProperties
#define vkCreateInstance pfn_vkCreateInstance
#define vkDestroyInstance pfn_vkDestroyInstance
#define vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr
#define vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices
#define vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties
#define vkEnumerateDeviceExtensionProperties pfn_vkEnumerateDeviceExtensionProperties
#define vkDestroySurfaceKHR pfn_vkDestroySurfaceKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR pfn_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR pfn_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR pfn_vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkCreateDevice pfn_vkCreateDevice
#define vkDestroyDevice pfn_vkDestroyDevice
#define vkGetDeviceQueue pfn_vkGetDeviceQueue
#define vkDeviceWaitIdle pfn_vkDeviceWaitIdle
#define vkCreateSwapchainKHR pfn_vkCreateSwapchainKHR
#define vkDestroySwapchainKHR pfn_vkDestroySwapchainKHR
#define vkGetSwapchainImagesKHR pfn_vkGetSwapchainImagesKHR
#define vkAcquireNextImageKHR pfn_vkAcquireNextImageKHR
#define vkQueuePresentKHR pfn_vkQueuePresentKHR
#define vkQueueSubmit pfn_vkQueueSubmit
#define vkCreateCommandPool pfn_vkCreateCommandPool
#define vkDestroyCommandPool pfn_vkDestroyCommandPool
#define vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers
#define vkBeginCommandBuffer pfn_vkBeginCommandBuffer
#define vkEndCommandBuffer pfn_vkEndCommandBuffer
#define vkResetCommandBuffer pfn_vkResetCommandBuffer
#define vkCmdPipelineBarrier pfn_vkCmdPipelineBarrier
#define vkCmdClearColorImage pfn_vkCmdClearColorImage
#define vkCmdCopyImage pfn_vkCmdCopyImage
#define vkCreateSemaphore pfn_vkCreateSemaphore
#define vkDestroySemaphore pfn_vkDestroySemaphore
#define vkCreateFence pfn_vkCreateFence
#define vkDestroyFence pfn_vkDestroyFence
#define vkWaitForFences pfn_vkWaitForFences
#define vkResetFences pfn_vkResetFences
#define vkCreateBuffer pfn_vkCreateBuffer
#define vkDestroyBuffer pfn_vkDestroyBuffer
#define vkGetBufferMemoryRequirements pfn_vkGetBufferMemoryRequirements
#define vkAllocateMemory pfn_vkAllocateMemory
#define vkFreeMemory pfn_vkFreeMemory
#define vkBindBufferMemory pfn_vkBindBufferMemory
#define vkMapMemory pfn_vkMapMemory
#define vkCreateImage pfn_vkCreateImage
#define vkDestroyImage pfn_vkDestroyImage
#define vkGetImageMemoryRequirements pfn_vkGetImageMemoryRequirements
#define vkBindImageMemory pfn_vkBindImageMemory
#define vkCreateImageView pfn_vkCreateImageView
#define vkDestroyImageView pfn_vkDestroyImageView
#define vkCreateSampler pfn_vkCreateSampler
#define vkDestroySampler pfn_vkDestroySampler
#define vkCreateShaderModule pfn_vkCreateShaderModule
#define vkDestroyShaderModule pfn_vkDestroyShaderModule
#define vkCreateDescriptorSetLayout pfn_vkCreateDescriptorSetLayout
#define vkDestroyDescriptorSetLayout pfn_vkDestroyDescriptorSetLayout
#define vkCreatePipelineLayout pfn_vkCreatePipelineLayout
#define vkDestroyPipelineLayout pfn_vkDestroyPipelineLayout
#define vkCreateGraphicsPipelines pfn_vkCreateGraphicsPipelines
#define vkDestroyPipeline pfn_vkDestroyPipeline
#define vkCreateDescriptorPool pfn_vkCreateDescriptorPool
#define vkDestroyDescriptorPool pfn_vkDestroyDescriptorPool
#define vkResetDescriptorPool pfn_vkResetDescriptorPool
#define vkAllocateDescriptorSets pfn_vkAllocateDescriptorSets
#define vkUpdateDescriptorSets pfn_vkUpdateDescriptorSets
#define vkCmdBeginRendering pfn_vkCmdBeginRendering
#define vkCmdEndRendering pfn_vkCmdEndRendering
#define vkCmdBindPipeline pfn_vkCmdBindPipeline
#define vkCmdBindDescriptorSets pfn_vkCmdBindDescriptorSets
#define vkCmdPushConstants pfn_vkCmdPushConstants
#define vkCmdSetViewport pfn_vkCmdSetViewport
#define vkCmdSetScissor pfn_vkCmdSetScissor
#define vkCmdSetDepthBias pfn_vkCmdSetDepthBias
#define vkCmdSetBlendConstants pfn_vkCmdSetBlendConstants
#define vkCmdSetStencilCompareMask pfn_vkCmdSetStencilCompareMask
#define vkCmdSetStencilWriteMask pfn_vkCmdSetStencilWriteMask
#define vkCmdSetStencilReference pfn_vkCmdSetStencilReference
#define vkCmdSetLineWidth pfn_vkCmdSetLineWidth
#define vkCmdSetCullMode pfn_vkCmdSetCullMode
#define vkCmdSetFrontFace pfn_vkCmdSetFrontFace
#define vkCmdSetPrimitiveTopology pfn_vkCmdSetPrimitiveTopology
#define vkCmdSetDepthTestEnable pfn_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable pfn_vkCmdSetDepthWriteEnable
#define vkCmdSetDepthCompareOp pfn_vkCmdSetDepthCompareOp
#define vkCmdSetStencilTestEnable pfn_vkCmdSetStencilTestEnable
#define vkCmdSetStencilOp pfn_vkCmdSetStencilOp
#define vkCmdSetDepthBiasEnable pfn_vkCmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable pfn_vkCmdSetPrimitiveRestartEnable
#define vkCmdSetRasterizerDiscardEnable pfn_vkCmdSetRasterizerDiscardEnable
#define vkCmdBindVertexBuffers pfn_vkCmdBindVertexBuffers
#define vkCmdBindIndexBuffer pfn_vkCmdBindIndexBuffer
#define vkCmdDraw pfn_vkCmdDraw
#define vkCmdDrawIndexed pfn_vkCmdDrawIndexed
#define vkCmdClearAttachments pfn_vkCmdClearAttachments
#define vkCmdCopyBufferToImage pfn_vkCmdCopyBufferToImage
#define vkCmdCopyImageToBuffer pfn_vkCmdCopyImageToBuffer
#define vkCmdBlitImage pfn_vkCmdBlitImage
#define vkCmdResolveImage pfn_vkCmdResolveImage
#define vkCmdClearDepthStencilImage pfn_vkCmdClearDepthStencilImage
#define vkGetFenceStatus pfn_vkGetFenceStatus
#define vkCreateComputePipelines pfn_vkCreateComputePipelines
#define vkCmdDispatch pfn_vkCmdDispatch
#define vkCreateQueryPool pfn_vkCreateQueryPool
#define vkDestroyQueryPool pfn_vkDestroyQueryPool
#define vkCmdResetQueryPool pfn_vkCmdResetQueryPool
#define vkCmdWriteTimestamp pfn_vkCmdWriteTimestamp
#define vkGetQueryPoolResults pfn_vkGetQueryPoolResults
#define vkCreatePipelineCache pfn_vkCreatePipelineCache
#define vkGetPipelineCacheData pfn_vkGetPipelineCacheData
#define vkDestroyPipelineCache pfn_vkDestroyPipelineCache
#define vkCmdCopyBuffer pfn_vkCmdCopyBuffer
#endif
