// Native NVK WSI: GPU writes directly into IOSurface memory; CALayer hands it to WindowServer.
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_metal.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char *);
static void report_backend(const char *backend) {
    static const char *previous;
    if (previous && !strcmp(previous,backend)) return;
    previous=backend;
    fprintf(stderr,"[NVK-ROUTE] Actual game graphics API: %s\n",backend);
    const char *path=getenv("MC_ROUTE_STATUS_FILE");
    if(path){FILE *file=fopen(path,"w");if(file){fprintf(file,"backend=%s\nselection=actual-api-call\n",backend);fclose(file);}}
}
static PFN_vkGetInstanceProcAddr icd;
static int audit_on;
static VkInstance active_instance;
typedef struct { uint32_t version,surfaceID,plane,pad; uint64_t size_B; } SurfaceImport;
static void (*stage_import)(const SurfaceImport *);
static uint32_t (*surface_dirty)(VkDevice,uint32_t,uint32_t);
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static _Atomic unsigned long graphics_pipelines, compute_pipelines, present_count;
static void init_bridge(void) {
    audit_on=getenv("MC_GRAPHICS_AUDIT")&&strcmp(getenv("MC_GRAPHICS_AUDIT"),"0");
    void *h = dlopen("/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib", RTLD_NOW | RTLD_LOCAL);
    if (h) {icd = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");stage_import=dlsym(h,"nvmtl_nvk_stage_import");surface_dirty=dlsym(h,"nvmtl_nvk_surface_dirty");}
    Dl_info route={0};if(icd&&dladdr((void*)icd,&route))fprintf(stderr,"[GRAPHICS-AUDIT] real Vulkan ICD: %s\n",route.dli_fname);
    fprintf(stderr, "[NVK-NATIVE] bridge loaded; real NVK ICD %s\n", icd ? "available" : "MISSING");
}
static PFN_vkVoidFunction real_proc(const char *name) {
    pthread_once(&init_once, init_bridge);
    PFN_vkVoidFunction f=icd ? icd(active_instance, name) : NULL;
    if(!f)fprintf(stderr,"[NVK-NATIVE] MISSING real NVK function %s (instance %p)\n",name,(void*)active_instance);
    return f;
}
#define R(n) ({ static _Atomic(PFN_vk##n) cached; PFN_vk##n fn=atomic_load(&cached); if(!fn){fn=(PFN_vk##n)real_proc("vk" #n);if(fn)atomic_store(&cached,fn);} fn; })
#define TRY(x) do { VkResult result_ = (x); if (result_ != VK_SUCCESS) { \
    fprintf(stderr,"[NVK-NATIVE] %s failed: %d\n", #x, result_); return result_; } } while (0)

typedef struct Device {
    VkDevice device; VkPhysicalDevice physical; VkQueue signal_queue; uint32_t signal_family;
    VkPhysicalDeviceMemoryProperties memory; VkBool32 host_import,fd_import;
    VkDescriptorSetLayout opaque_set_layout; VkPipelineLayout opaque_layout; VkPipeline opaque_pipeline;
    VkDescriptorSetLayout fused_set_layout;VkPipelineLayout fused_layout;VkPipeline fused_pipeline;
} Device;
static Device devices[8]; static unsigned num_devices;
typedef struct Queue { VkQueue queue; Device *device; uint32_t family; } Queue;
static Queue queues[32]; static unsigned num_queues;
typedef struct Surface {
    CALayer *container,*layer;
} Surface;
enum {OUTPUT_COUNT=16};
typedef struct { IOSurfaceRef surface; VkBuffer buffer; VkDeviceMemory memory; int mode; size_t pitch,size; uint64_t published_ns; VkDescriptorSet descriptor,fused_descriptors[4]; } NativeOutput;
typedef struct Swapchain {
    struct Swapchain *next;
    Device *owner; Surface *surface;
    VkFormat format; uint32_t width, height, count, cursor;
    VkImage images[4]; VkDeviceMemory memories[4]; VkBool32 acquired[4];
    NativeOutput output[OUTPUT_COUNT];unsigned output_count; int current; uint32_t output_cursor; uint64_t recycle_delay_ns;
    VkCommandPool pool; VkCommandBuffer command;
    uint32_t pool_family; VkFence fence;
    VkBool32 fused_present,external_images;VkFormat source_format;VkImageLayout source_layout;uint32_t flip_y;VkSampler fused_sampler;VkImageView fused_views[4];VkDescriptorPool fused_pool;
    VkQueryPool timing;float timestamp_period;uint64_t stats_gpu_ns;
    uint64_t stats_start_ns,stats_wait_ns,stats_copy_ns,stats_publish_ns; unsigned stats_frames;
    VkPresentModeKHR present_mode;uint64_t refresh_period_ns,last_present_ns; VkDescriptorPool descriptor_pool;
} Swapchain;
static Swapchain *swapchains;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static Device *find_device(VkDevice d) { for(unsigned i=0;i<num_devices;i++) if(devices[i].device==d)return &devices[i]; return NULL; }
static Queue *find_queue(VkQueue q) { for(unsigned i=0;i<num_queues;i++)if(queues[i].queue==q)return &queues[i]; return NULL; }
static void track_queue(Device *d,VkQueue q,uint32_t family) {
    pthread_mutex_lock(&state_lock);
    if (!find_queue(q) && num_queues < 32) queues[num_queues++] = (Queue){q,d,family};
    if (!d->signal_queue) {d->signal_queue=q;d->signal_family=family;}
    pthread_mutex_unlock(&state_lock);
}
static Surface *surface_ptr(VkSurfaceKHR s) { return (Surface *)(uintptr_t)s; }
static Swapchain *chain_ptr(VkSwapchainKHR s) { return (Swapchain *)(uintptr_t)s; }
static VkBool32 is_swap_image(VkImage image) {
    for(Swapchain*s=swapchains;s;s=s->next)for(unsigned i=0;i<s->count;i++)if(s->images[i]==image)return VK_TRUE;
    return VK_FALSE;
}
static uint32_t memory_type(Device *d,uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) {
    for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<d->memory.memoryTypeCount;i++) {
        VkMemoryPropertyFlags f=d->memory.memoryTypes[i].propertyFlags;
        if((bits&(1u<<i))&&(f&required)==required&&(pass||(f&preferred)==preferred))return i;
    }
    return UINT32_MAX;
}
static const char *instance_additions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_METAL_SURFACE_EXTENSION_NAME,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
static VkBool32 instance_owned(const char *n) {for(unsigned i=0;i<3;i++)if(!strcmp(n,instance_additions[i]))return VK_TRUE;return VK_FALSE;}
static VkResult append_extensions(uint32_t base_count,VkExtensionProperties *base,const char *const *extra,unsigned extra_count,uint32_t *count,VkExtensionProperties *out) {
    VkExtensionProperties *all=calloc(base_count+extra_count,sizeof(*all));
    if(!all)return VK_ERROR_OUT_OF_HOST_MEMORY;
    if(base_count)memcpy(all,base,base_count*sizeof(*all));unsigned total=base_count;
    for(unsigned i=0;i<extra_count;i++) {VkBool32 found=0;for(unsigned j=0;j<total;j++)if(!strcmp(all[j].extensionName,extra[i]))found=1;
        if(!found){strncpy(all[total].extensionName,extra[i],VK_MAX_EXTENSION_NAME_SIZE-1);
            all[total++].specVersion=!strcmp(extra[i],VK_KHR_SWAPCHAIN_EXTENSION_NAME)?VK_KHR_SWAPCHAIN_SPEC_VERSION:
              !strcmp(extra[i],VK_KHR_SURFACE_EXTENSION_NAME)?VK_KHR_SURFACE_SPEC_VERSION:1;}}
    VkResult result=VK_SUCCESS;
    if(out){uint32_t n=*count<total?*count:total;memcpy(out,all,n*sizeof(*all));if(n<total)result=VK_INCOMPLETE;*count=n;}else *count=total;
    free(all);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer,uint32_t *count,VkExtensionProperties *out) {
    pthread_once(&init_once,init_bridge);if(!icd)return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties f=(void*)icd(NULL,"vkEnumerateInstanceExtensionProperties");
    if(layer)return f(layer,count,out);uint32_t n=0;TRY(f(NULL,&n,NULL));VkExtensionProperties *base=calloc(n,sizeof(*base));
    if(n&&!base)return VK_ERROR_OUT_OF_HOST_MEMORY;VkResult result=f(NULL,&n,base);
    if(result==VK_SUCCESS)result=append_extensions(n,base,instance_additions,3,count,out);free(base);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count,VkLayerProperties *out) {(void)out;*count=0;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {pthread_once(&init_once,init_bridge);PFN_vkEnumerateInstanceVersion f=icd?(void*)icd(NULL,"vkEnumerateInstanceVersion"):NULL;if(!f)return VK_ERROR_INITIALIZATION_FAILED;return f(version);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *alloc,VkInstance *out) {
    pthread_once(&init_once,init_bridge);if(!icd)return VK_ERROR_INITIALIZATION_FAILED;
    VkInstanceCreateInfo copy=*info;const char **names=calloc(info->enabledExtensionCount,sizeof(char*));unsigned n=0;
    for(unsigned i=0;i<info->enabledExtensionCount;i++){fprintf(stderr,"[NVK-NATIVE] instance extension %s\n",info->ppEnabledExtensionNames[i]);if(!instance_owned(info->ppEnabledExtensionNames[i]))names[n++]=info->ppEnabledExtensionNames[i];}
    copy.ppEnabledExtensionNames=names;copy.enabledExtensionCount=n;
    VkResult result=((PFN_vkCreateInstance)icd(NULL,"vkCreateInstance"))(&copy,alloc,out);free(names);
    if(result==VK_SUCCESS)active_instance=*out;fprintf(stderr,"[NVK-NATIVE] real NVK vkCreateInstance: %d\n",result);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p,const char *layer,uint32_t *count,VkExtensionProperties *out) {
    if(layer)return R(EnumerateDeviceExtensionProperties)(p,layer,count,out);
    uint32_t n=0;TRY(R(EnumerateDeviceExtensionProperties)(p,NULL,&n,NULL));VkExtensionProperties *base=calloc(n,sizeof(*base));
    if(n&&!base)return VK_ERROR_OUT_OF_HOST_MEMORY;VkResult result=R(EnumerateDeviceExtensionProperties)(p,NULL,&n,base);
    const char *extra[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};if(result==VK_SUCCESS)result=append_extensions(n,base,extra,1,count,out);free(base);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *alloc,VkDevice *out) {
    if(num_devices==8)return VK_ERROR_TOO_MANY_OBJECTS;
    VkDeviceCreateInfo copy=*info;const char **names=calloc(info->enabledExtensionCount+2,sizeof(char*));unsigned n=0;
    for(unsigned i=0;i<info->enabledExtensionCount;i++)if(strcmp(info->ppEnabledExtensionNames[i],VK_KHR_SWAPCHAIN_EXTENSION_NAME))names[n++]=info->ppEnabledExtensionNames[i];
    uint32_t ext_count=0;R(EnumerateDeviceExtensionProperties)(physical,NULL,&ext_count,NULL);
    VkExtensionProperties*ext=calloc(ext_count,sizeof(*ext));R(EnumerateDeviceExtensionProperties)(physical,NULL,&ext_count,ext);
    VkBool32 host=VK_FALSE,fd=VK_FALSE;
    for(unsigned i=0;i<ext_count;i++){if(!strcmp(ext[i].extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))host=VK_TRUE;if(!strcmp(ext[i].extensionName,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))fd=VK_TRUE;}free(ext);
    VkBool32 named_host=0,named_fd=0;for(unsigned i=0;i<n;i++){named_host|=!strcmp(names[i],VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);named_fd|=!strcmp(names[i],VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);}
    if(host&&!named_host)names[n++]=VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;if(fd&&!named_fd)names[n++]=VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    copy.ppEnabledExtensionNames=names;copy.enabledExtensionCount=n;VkResult result=R(CreateDevice)(physical,&copy,alloc,out);free(names);
    if(result!=VK_SUCCESS)return result;Device*d=&devices[num_devices++];*d=(Device){.device=*out,.physical=physical,.host_import=host,.fd_import=fd};R(GetPhysicalDeviceMemoryProperties)(physical,&d->memory);
    for(unsigned i=0;i<info->queueCreateInfoCount;i++){const VkDeviceQueueCreateInfo*q=&info->pQueueCreateInfos[i];if(!q->flags){VkQueue queue;R(GetDeviceQueue)(*out,q->queueFamilyIndex,0,&queue);track_queue(d,queue,q->queueFamilyIndex);}}
    VkPhysicalDeviceProperties properties;R(GetPhysicalDeviceProperties)(physical,&properties);fprintf(stderr,"[NVK-NATIVE] rendering device: %s\n",properties.deviceName);return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice d,uint32_t family,uint32_t index,VkQueue *out){R(GetDeviceQueue)(d,family,index,out);Device*device=find_device(d);if(device)track_queue(device,*out,family);}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice d,const VkDeviceQueueInfo2 *info,VkQueue *out){R(GetDeviceQueue2)(d,info,out);Device*device=find_device(d);if(device)track_queue(device,*out,info->queueFamilyIndex);}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateMetalSurfaceEXT(VkInstance instance,const VkMetalSurfaceCreateInfoEXT *info,const VkAllocationCallbacks *alloc,VkSurfaceKHR *out) {
    (void)instance;(void)alloc;@autoreleasepool {
    Surface*s=calloc(1,sizeof(*s));if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->container=[(CALayer*)info->pLayer retain];s->layer=[[CALayer alloc] init];
    s->layer.frame=s->container.bounds;s->layer.autoresizingMask=kCALayerWidthSizable|kCALayerHeightSizable;
    s->layer.contentsGravity=kCAGravityResize;s->layer.opaque=YES;s->layer.contentsScale=s->container.contentsScale;
    [CATransaction begin];[CATransaction setDisableActions:YES];[s->container addSublayer:s->layer];[CATransaction commit];
    *out=(VkSurfaceKHR)(uintptr_t)s;fprintf(stderr,"[NVK-NATIVE] window container -> ordinary CALayer; no MTLDevice/MTLQueue/MTLDrawable\n");return VK_SUCCESS;
    }
}
VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance,VkSurfaceKHR surface,const VkAllocationCallbacks *alloc){(void)instance;(void)alloc;Surface*s=surface_ptr(surface);if(!s)return;[s->layer removeFromSuperlayer];[s->container release];[s->layer release];free(s);}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p,uint32_t family,VkSurfaceKHR surface,VkBool32 *out) {
    fprintf(stderr,"[NVK-NATIVE] surface support query family %u\n",family);
    (void)surface;uint32_t n=0;R(GetPhysicalDeviceQueueFamilyProperties)(p,&n,NULL);*out=VK_FALSE;
    if(family>=n)return VK_SUCCESS;VkQueueFamilyProperties *q=calloc(n,sizeof(*q));if(!q)return VK_ERROR_OUT_OF_HOST_MEMORY;R(GetPhysicalDeviceQueueFamilyProperties)(p,&n,q);*out=!!(q[family].queueFlags&VK_QUEUE_GRAPHICS_BIT);free(q);return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,VkSurfaceKHR surface,VkSurfaceCapabilitiesKHR *out) {
    fprintf(stderr,"[NVK-NATIVE] surface capabilities query\n");
    (void)p;(void)surface;*out=(VkSurfaceCapabilitiesKHR){.minImageCount=2,.maxImageCount=3,.currentExtent={UINT32_MAX,UINT32_MAX},
      .minImageExtent={1,1},.maxImageExtent={16384,16384},.maxImageArrayLayers=1,.supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,.supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .supportedUsageFlags=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};return VK_SUCCESS;
}
static const VkSurfaceFormatKHR surface_formats[]={
 {VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},{VK_FORMAT_B8G8R8A8_SRGB,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,VkSurfaceKHR s,uint32_t *count,VkSurfaceFormatKHR *out){(void)p;(void)s;if(!out){*count=2;return VK_SUCCESS;}uint32_t n=*count<2?*count:2;memcpy(out,surface_formats,n*sizeof(*out));*count=n;return n<2?VK_INCOMPLETE:VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p,VkSurfaceKHR s,uint32_t *count,VkPresentModeKHR *out){
    (void)p;(void)s;const VkPresentModeKHR modes[]={VK_PRESENT_MODE_FIFO_KHR,VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_IMMEDIATE_KHR};
    if(!out){*count=3;return VK_SUCCESS;}uint32_t n=*count<3?*count:3;memcpy(out,modes,n*sizeof(*out));*count=n;return n<3?VK_INCOMPLETE:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice p,const VkPhysicalDeviceSurfaceInfo2KHR *info,VkSurfaceCapabilities2KHR *out){return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p,info->surface,&out->surfaceCapabilities);}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice p,const VkPhysicalDeviceSurfaceInfo2KHR *info,uint32_t *count,VkSurfaceFormat2KHR *out){(void)p;(void)info;if(!out){*count=2;return VK_SUCCESS;}uint32_t n=*count<2?*count:2;for(unsigned i=0;i<n;i++)out[i].surfaceFormat=surface_formats[i];*count=n;return n<2?VK_INCOMPLETE:VK_SUCCESS;}

#include "graphics_audit.inc"
#include "opaque_pipeline.inc"
#include "fused_pipeline.inc"
#include "native_outputs.inc"
static void free_chain(Swapchain *s) {
    if(!s)return;VkDevice d=s->owner->device;
    if(s->timing)R(DestroyQueryPool)(d,s->timing,NULL);
    if(s->pool)R(DestroyCommandPool)(d,s->pool,NULL);if(s->fence)R(DestroyFence)(d,s->fence,NULL);
    native_outputs_destroy(s);
    if(!s->external_images)for(unsigned i=0;i<s->count;i++){if(s->images[i])R(DestroyImage)(d,s->images[i],NULL);if(s->memories[i])R(FreeMemory)(d,s->memories[i],NULL);}
    free(s);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device,const VkSwapchainCreateInfoKHR *info,const VkAllocationCallbacks *alloc,VkSwapchainKHR *out) {
    report_backend("vulkan");
    fprintf(stderr,"[NVK-NATIVE] enter CreateSwapchain: %ux%u\n",info->imageExtent.width,info->imageExtent.height);
    (void)alloc;@autoreleasepool {
    Device*d=find_device(device);if(!d||!info->surface)return VK_ERROR_INITIALIZATION_FAILED;
    if(info->imageArrayLayers!=1||info->minImageCount>3||!info->imageExtent.width||!info->imageExtent.height)return VK_ERROR_INITIALIZATION_FAILED;
    if(info->imageFormat!=VK_FORMAT_B8G8R8A8_UNORM&&info->imageFormat!=VK_FORMAT_B8G8R8A8_SRGB)return VK_ERROR_FORMAT_NOT_SUPPORTED;
    Swapchain*s=calloc(1,sizeof(*s));if(!s)return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->owner=d;s->surface=surface_ptr(info->surface);s->width=info->imageExtent.width;s->height=info->imageExtent.height;
    s->source_format=VK_FORMAT_B8G8R8A8_UNORM;s->source_layout=VK_IMAGE_LAYOUT_GENERAL;
    s->format=info->imageFormat;s->count=info->minImageCount<2?2:info->minImageCount;s->pool_family=UINT32_MAX;s->present_mode=info->presentMode;
    const char*buffers=getenv("MC_OUTPUT_BUFFERS");s->output_count=buffers?(unsigned)atoi(buffers):12;if(s->output_count<3||s->output_count>OUTPUT_COUNT)s->output_count=12;
    const char*fused=getenv("MC_FUSED_PRESENT");s->fused_present=!(fused&&!strcmp(fused,"0"));
    VkResult result=VK_SUCCESS;
#define CTRY(call) do { result=(call);if(result!=VK_SUCCESS){fprintf(stderr,"[NVK-NATIVE] swapchain %s failed %d\n",#call,result);goto fail;} }while(0)
    VkImageCreateInfo image={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .flags=s->fused_present?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0,.format=s->format,.extent={s->width,s->height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=info->imageUsage|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode=info->imageSharingMode,.queueFamilyIndexCount=info->queueFamilyIndexCount,.pQueueFamilyIndices=info->pQueueFamilyIndices};
    for(unsigned i=0;i<s->count;i++) {
        CTRY(R(CreateImage)(device,&image,NULL,&s->images[i]));VkMemoryRequirements req;R(GetImageMemoryRequirements)(device,s->images[i],&req);
        uint32_t type=memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);if(type==UINT32_MAX){result=VK_ERROR_OUT_OF_DEVICE_MEMORY;goto fail;}
        VkMemoryAllocateInfo memory={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
        CTRY(R(AllocateMemory)(device,&memory,NULL,&s->memories[i]));CTRY(R(BindImageMemory)(device,s->images[i],s->memories[i],0));
    }
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CTRY(R(CreateFence)(device,&fence,NULL,&s->fence));
    CTRY(native_outputs_create(s));
    pthread_mutex_lock(&state_lock);s->next=swapchains;swapchains=s;pthread_mutex_unlock(&state_lock);
    *out=(VkSwapchainKHR)(uintptr_t)s;fprintf(stderr,"[NVK-NATIVE] swapchain %ux%u format %u images %u mode %u; NVK GPU direct writes -> IOSurface -> CALayer\n",s->width,s->height,s->format,s->count,s->present_mode);fprintf(stderr,"[NVK-NATIVE] output mode: %s\n",s->fused_present?"fused GPU sample + BGRA/alpha (one pass)":"GPU transfer + alpha (two passes)");return VK_SUCCESS;
fail: free_chain(s);return result;
#undef CTRY
    }
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device,VkSwapchainKHR chain,const VkAllocationCallbacks *alloc) {
    (void)alloc;Swapchain*s=chain_ptr(chain);if(!s)return;R(DeviceWaitIdle)(device);
    pthread_mutex_lock(&state_lock);Swapchain **p=&swapchains;while(*p&&*p!=s)p=&(*p)->next;if(*p)*p=s->next;pthread_mutex_unlock(&state_lock);free_chain(s);
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR chain,uint32_t *count,VkImage *out) {
    (void)d;Swapchain*s=chain_ptr(chain);if(!s)return VK_ERROR_OUT_OF_DATE_KHR;if(!out){*count=s->count;return VK_SUCCESS;}
    uint32_t n=*count<s->count?*count:s->count;memcpy(out,s->images,n*sizeof(*out));*count=n;return n<s->count?VK_INCOMPLETE:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device,VkSwapchainKHR chain,uint64_t timeout,VkSemaphore semaphore,VkFence fence,uint32_t *index) {
    (void)timeout;Swapchain*s=chain_ptr(chain);Device*d=find_device(device);if(!s||!d||!d->signal_queue)return VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t chosen=UINT32_MAX;for(unsigned i=0;i<s->count;i++){uint32_t n=(s->cursor+i)%s->count;if(!s->acquired[n]){chosen=n;break;}}
    if(chosen==UINT32_MAX)return timeout?VK_TIMEOUT:VK_NOT_READY;
    VkSubmitInfo signal={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.signalSemaphoreCount=semaphore?1:0,.pSignalSemaphores=semaphore?&semaphore:NULL};
    TRY(R(QueueSubmit)(d->signal_queue,1,&signal,fence));s->acquired[chosen]=VK_TRUE;s->cursor=(chosen+1)%s->count;*index=chosen;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice d,const VkAcquireNextImageInfoKHR *info,uint32_t *index) {
    return vkAcquireNextImageKHR(d,info->swapchain,info->timeout,info->semaphore,info->fence,index);
}
static VkResult ensure_command(Swapchain*s,Queue*q) {
    if(s->pool&&s->pool_family==q->family)return VK_SUCCESS;
    VkDevice d=s->owner->device;if(s->pool)R(DestroyCommandPool)(d,s->pool,NULL);s->pool=VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=q->family};
    TRY(R(CreateCommandPool)(d,&pool,NULL,&s->pool));s->pool_family=q->family;
    if(!s->timing){VkQueryPoolCreateInfo query={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        VkResult qr=R(CreateQueryPool)(d,&query,NULL,&s->timing);VkPhysicalDeviceProperties prop;R(GetPhysicalDeviceProperties)(s->owner->physical,&prop);s->timestamp_period=prop.limits.timestampPeriod;if(qr!=VK_SUCCESS)s->timing=VK_NULL_HANDLE;}
    VkCommandBufferAllocateInfo info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=s->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    return R(AllocateCommandBuffers)(d,&info,&s->command);
}
#include "native_present.inc"
#include "zink_present.inc"

static VkImageLayout real_layout(VkImage image,VkImageLayout layout){return layout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR&&is_swap_image(image)?VK_IMAGE_LAYOUT_GENERAL:layout;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
 uint32_t memory_count,const VkMemoryBarrier *memory,uint32_t buffer_count,const VkBufferMemoryBarrier *buffers,uint32_t image_count,const VkImageMemoryBarrier *images) {
    VkBool32 replace=VK_FALSE;
    for(unsigned i=0;i<image_count;i++)if(images[i].oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR||images[i].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR){replace=VK_TRUE;break;}
    if(!replace){R(CmdPipelineBarrier)(cb,src,dst,flags,memory_count,memory,buffer_count,buffers,image_count,images);return;}
    VkImageMemoryBarrier local[8],*copy=image_count<=8?local:malloc(image_count*sizeof(*copy));if(!copy)abort();
    for(unsigned i=0;i<image_count;i++){copy[i]=images[i];copy[i].oldLayout=real_layout(copy[i].image,copy[i].oldLayout);copy[i].newLayout=real_layout(copy[i].image,copy[i].newLayout);}
    R(CmdPipelineBarrier)(cb,src,dst,flags,memory_count,memory,buffer_count,buffers,image_count,copy);if(copy!=local)free(copy);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cb,const VkDependencyInfo *info) {
    static _Atomic(PFN_vkCmdPipelineBarrier2) cached;PFN_vkCmdPipelineBarrier2 fn=atomic_load(&cached);
    if(!fn){fn=(PFN_vkCmdPipelineBarrier2)icd(active_instance,"vkCmdPipelineBarrier2KHR");if(!fn)fn=(PFN_vkCmdPipelineBarrier2)icd(active_instance,"vkCmdPipelineBarrier2");if(fn)atomic_store(&cached,fn);}
    if(!fn){fprintf(stderr,"[NVK-NATIVE] No synchronization2 entry point\n");abort();}
    uint32_t n=info->imageMemoryBarrierCount;VkBool32 replace=VK_FALSE;
    for(unsigned i=0;i<n;i++)if(info->pImageMemoryBarriers[i].oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR||info->pImageMemoryBarriers[i].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR){replace=VK_TRUE;break;}
    if(!replace){fn(cb,info);return;}
    VkDependencyInfo dependency=*info;VkImageMemoryBarrier2 local[8],*copy=n<=8?local:malloc(n*sizeof(*copy));if(!copy)abort();
    for(unsigned i=0;i<n;i++){copy[i]=info->pImageMemoryBarriers[i];copy[i].oldLayout=real_layout(copy[i].image,copy[i].oldLayout);copy[i].newLayout=real_layout(copy[i].image,copy[i].newLayout);}
    dependency.pImageMemoryBarriers=copy;fn(cb,&dependency);if(copy!=local)free(copy);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(VkCommandBuffer cb,const VkDependencyInfo *info){vkCmdPipelineBarrier2(cb,info);}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo *info,const VkAllocationCallbacks *alloc,VkPipeline *out) {
    VkResult result=R(CreateGraphicsPipelines)(d,cache,count,info,alloc,out);if(result==VK_SUCCESS)graphics_pipelines+=count;
    if(result!=VK_SUCCESS||graphics_pipelines<=3)fprintf(stderr,"[NVK-NATIVE] NVK graphics pipelines: result %d total %lu\n",result,graphics_pipelines);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo *info,const VkAllocationCallbacks *alloc,VkPipeline *out) {
    VkResult result=R(CreateComputePipelines)(d,cache,count,info,alloc,out);if(result==VK_SUCCESS)compute_pipelines+=count;
    if(result!=VK_SUCCESS||compute_pipelines<=3)fprintf(stderr,"[NVK-NATIVE] NVK compute pipelines: result %d total %lu\n",result,compute_pipelines);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSetDebugUtilsObjectNameEXT(VkDevice d,const VkDebugUtilsObjectNameInfoEXT *info) {
    if(info->objectType==VK_OBJECT_TYPE_SURFACE_KHR||info->objectType==VK_OBJECT_TYPE_SWAPCHAIN_KHR)return VK_SUCCESS;
    PFN_vkSetDebugUtilsObjectNameEXT f=R(SetDebugUtilsObjectNameEXT);return f?f(d,info):VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSetDebugUtilsObjectTagEXT(VkDevice d,const VkDebugUtilsObjectTagInfoEXT *info) {
    if(info->objectType==VK_OBJECT_TYPE_SURFACE_KHR||info->objectType==VK_OBJECT_TYPE_SWAPCHAIN_KHR)return VK_SUCCESS;
    PFN_vkSetDebugUtilsObjectTagEXT f=R(SetDebugUtilsObjectTagEXT);return f?f(d,info):VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice d,VkDeviceGroupPresentCapabilitiesKHR *out){(void)d;memset(out->presentMask,0,sizeof(out->presentMask));out->presentMask[0]=1;out->modes=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceGroupSurfacePresentModesKHR(VkDevice d,VkSurfaceKHR s,VkDeviceGroupPresentModeFlagsKHR *out){(void)d;(void)s;*out=VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice p,VkSurfaceKHR surface,uint32_t *count,VkRect2D *out){(void)p;if(!out){*count=1;return VK_SUCCESS;}if(!*count)return VK_INCOMPLETE;Surface*s=surface_ptr(surface);out[0]=(VkRect2D){.extent={(uint32_t)MAX(1,s->layer.bounds.size.width*s->layer.contentsScale),(uint32_t)MAX(1,s->layer.bounds.size.height*s->layer.contentsScale)}};*count=1;return VK_SUCCESS;}

static PFN_vkVoidFunction wrapper(const char *name) {
#define W(n) if(!strcmp(name,"vk" #n))return (PFN_vkVoidFunction)vk##n
    W(GetInstanceProcAddr);W(GetDeviceProcAddr);W(CreateInstance);W(EnumerateInstanceVersion);
    W(EnumerateInstanceExtensionProperties);W(EnumerateInstanceLayerProperties);W(EnumerateDeviceExtensionProperties);
    W(CreateDevice);W(GetDeviceQueue);W(GetDeviceQueue2);W(CreateMetalSurfaceEXT);W(DestroySurfaceKHR);
    W(GetPhysicalDeviceSurfaceSupportKHR);W(GetPhysicalDeviceSurfaceCapabilitiesKHR);W(GetPhysicalDeviceSurfaceFormatsKHR);
    W(GetPhysicalDeviceSurfacePresentModesKHR);W(GetPhysicalDeviceSurfaceCapabilities2KHR);W(GetPhysicalDeviceSurfaceFormats2KHR);
    W(CreateSwapchainKHR);W(DestroySwapchainKHR);W(GetSwapchainImagesKHR);W(AcquireNextImageKHR);W(AcquireNextImage2KHR);W(QueuePresentKHR);
    W(CmdPipelineBarrier);W(CmdPipelineBarrier2);W(CmdPipelineBarrier2KHR);W(CreateGraphicsPipelines);W(CreateComputePipelines);
    W(CmdCopyImageToBuffer);W(CmdCopyImageToBuffer2);W(CmdCopyImageToBuffer2KHR);
    W(CmdDrawMultiEXT);W(CmdDrawMultiIndexedEXT);W(CmdDraw);W(CmdDrawIndexed);W(CmdDrawIndirect);W(CmdDrawIndexedIndirect);W(CmdDrawIndirectCount);W(CmdDrawIndexedIndirectCount);W(CmdDispatch);W(QueueSubmit);W(QueueSubmit2);W(QueueSubmit2KHR);
    W(SetDebugUtilsObjectNameEXT);W(SetDebugUtilsObjectTagEXT);W(GetDeviceGroupPresentCapabilitiesKHR);
    W(GetDeviceGroupSurfacePresentModesKHR);W(GetPhysicalDevicePresentRectanglesKHR);
#undef W
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    pthread_once(&init_once,init_bridge);if(!icd)return NULL;PFN_vkVoidFunction f=wrapper(name);return f?f:icd(instance,name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    PFN_vkVoidFunction f=wrapper(name);if(f)return f;PFN_vkGetDeviceProcAddr get=R(GetDeviceProcAddr);return get?get(device,name):NULL;
}
