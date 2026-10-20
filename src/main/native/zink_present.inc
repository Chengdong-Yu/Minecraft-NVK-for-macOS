#include "mc_zink_image.h"
__attribute__((visibility("default"))) int mc_nvk_present_zink(const MCZinkImage*image,void*layer){
    static Swapchain*s;static VkSurfaceKHR window;static void*previous_layer;
    if(!image){if(s){R(DeviceWaitIdle)(s->owner->device);free_chain(s);s=NULL;}return VK_SUCCESS;}
    report_backend("opengl");
    VkDevice device=(VkDevice)image->device;VkQueue queue=(VkQueue)image->queue;Device*d=find_device(device);
    if(!d||!image->image||!layer)return VK_ERROR_INITIALIZATION_FAILED;
    if(!find_queue(queue))track_queue(d,queue,image->family);
    if(s&&(s->width!=image->width||s->height!=image->height||s->images[0]!=(VkImage)image->image||previous_layer!=layer)){
        R(DeviceWaitIdle)(device);free_chain(s);s=NULL;
    }
    if(window&&previous_layer!=layer){vkDestroySurfaceKHR(active_instance,window,NULL);window=VK_NULL_HANDLE;}
    if(!window){VkMetalSurfaceCreateInfoEXT surface={.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,.pLayer=layer};
        TRY(vkCreateMetalSurfaceEXT(active_instance,&surface,NULL,&window));previous_layer=layer;}
    if(!s){
        s=calloc(1,sizeof(*s));if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
        s->owner=d;s->surface=surface_ptr(window);s->width=image->width;s->height=image->height;s->count=1;
        s->images[0]=(VkImage)image->image;s->external_images=VK_TRUE;s->format=(VkFormat)image->format;s->source_format=s->format;
        if(s->source_format==VK_FORMAT_B8G8R8A8_SRGB)s->source_format=VK_FORMAT_B8G8R8A8_UNORM;
        if(s->source_format==VK_FORMAT_R8G8B8A8_SRGB)s->source_format=VK_FORMAT_R8G8B8A8_UNORM;
        s->source_layout=(VkImageLayout)image->layout;s->pool_family=UINT32_MAX;s->present_mode=VK_PRESENT_MODE_IMMEDIATE_KHR;s->output_count=8;
        s->fused_present=!!(image->usage&VK_IMAGE_USAGE_SAMPLED_BIT);
        const char*flip=getenv("MC_ZINK_FLIP_Y");s->flip_y=flip&&!strcmp(flip,"1");
        VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkResult result=R(CreateFence)(device,&fence,NULL,&s->fence);
        if(result==VK_SUCCESS)result=native_outputs_create(s);
        if(result!=VK_SUCCESS){free_chain(s);s=NULL;return result;}
        fprintf(stderr,"[ZINK-NVK] OpenGL framebuffer %ux%u image format %u usage %#llx; NVK %s -> IOSurface VRAM; flipY=%u\n",
          s->width,s->height,s->format,(unsigned long long)image->usage,s->fused_present?"one-pass GPU sample":"GPU transfer + alpha",s->flip_y);
    }
    s->source_layout=(VkImageLayout)image->layout;s->acquired[0]=VK_TRUE;
    VkSwapchainKHR chain=(VkSwapchainKHR)(uintptr_t)s;uint32_t index=0;
    VkPresentInfoKHR info={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.swapchainCount=1,.pSwapchains=&chain,.pImageIndices=&index};
    return vkQueuePresentKHR(queue,&info);
}
