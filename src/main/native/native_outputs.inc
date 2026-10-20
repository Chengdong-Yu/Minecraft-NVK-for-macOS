static uint64_t now_ns(void){return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);}
static void publish_surface(Surface*s,IOSurfaceRef surface){
    [CATransaction begin];[CATransaction setDisableActions:YES];s->layer.contents=(id)surface;
    s->layer.frame=s->container.bounds;[CATransaction commit];[CATransaction flush];
}
static VkResult import_vram(Swapchain*s,NativeOutput*o){
    if(!s->owner->fd_import||!stage_import||!surface_dirty)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDevice d=s->owner->device;int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);if(fd<0)return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    VkImportMemoryFdInfoKHR imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,.fd=fd};
    SurfaceImport share={3,IOSurfaceGetID(o->surface),0,0,o->size};
    uint32_t type=memory_type(s->owner,UINT32_MAX,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=o->size,.memoryTypeIndex=type};
    stage_import(&share);VkResult result=R(AllocateMemory)(d,&allocation,NULL,&o->memory);stage_import(NULL);
    if(result!=VK_SUCCESS){close(fd);return result;}
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=o->size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    result=R(CreateBuffer)(d,&info,NULL,&o->buffer);
    if(result==VK_SUCCESS)result=R(BindBufferMemory)(d,o->buffer,o->memory,0);
    if(result==VK_SUCCESS)o->mode=1;
    else {if(o->buffer)R(DestroyBuffer)(d,o->buffer,NULL);o->buffer=VK_NULL_HANDLE;R(FreeMemory)(d,o->memory,NULL);o->memory=VK_NULL_HANDLE;}
    return result;
}
static VkResult import_host(Swapchain*s,NativeOutput*o){
    if(!s->owner->host_import)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDevice d=s->owner->device;IOSurfaceLock(o->surface,0,NULL);void *base=IOSurfaceGetBaseAddress(o->surface);
    VkMemoryHostPointerPropertiesEXT properties={.sType=VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult result=R(GetMemoryHostPointerPropertiesEXT)(d,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,base,&properties);
    if(result!=VK_SUCCESS){IOSurfaceUnlock(o->surface,0,NULL);return result;}
    VkExternalMemoryBufferCreateInfo external={.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.pNext=&external,.size=o->size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    result=R(CreateBuffer)(d,&info,NULL,&o->buffer);if(result!=VK_SUCCESS)goto done;
    VkMemoryRequirements requirements;R(GetBufferMemoryRequirements)(d,o->buffer,&requirements);
    if(requirements.size>o->size){result=VK_ERROR_OUT_OF_HOST_MEMORY;goto done;}
    uint32_t type=memory_type(s->owner,requirements.memoryTypeBits&properties.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if(type==UINT32_MAX){result=VK_ERROR_INVALID_EXTERNAL_HANDLE;goto done;}
    VkImportMemoryHostPointerInfoEXT imp={.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,.pHostPointer=base};
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,.allocationSize=o->size,.memoryTypeIndex=type};
    result=R(AllocateMemory)(d,&allocation,NULL,&o->memory);
    if(result==VK_SUCCESS)result=R(BindBufferMemory)(d,o->buffer,o->memory,0);
    if(result==VK_SUCCESS)o->mode=2;
done:IOSurfaceUnlock(o->surface,0,NULL);
    if(result!=VK_SUCCESS){if(o->buffer)R(DestroyBuffer)(d,o->buffer,NULL);o->buffer=VK_NULL_HANDLE;if(o->memory)R(FreeMemory)(d,o->memory,NULL);o->memory=VK_NULL_HANDLE;}
    return result;
}
static VkResult native_outputs_create(Swapchain*s){
    s->current=-1;CGDisplayModeRef mode=CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz=mode?CGDisplayModeGetRefreshRate(mode):0;if(mode)CGDisplayModeRelease(mode);if(hz<20)hz=60;
    s->recycle_delay_ns=(uint64_t)(2e9/hz);
    s->refresh_period_ns=(uint64_t)(1e9/hz);
    for(unsigned i=0;i<s->output_count;i++){
        NativeOutput*o=&s->output[i];size_t pitch=((s->width*4+63)/64)*64;
        size_t size=((pitch*s->height+65535)/65536)*65536;
        NSDictionary *desc=@{(id)kIOSurfaceWidth:@(s->width),(id)kIOSurfaceHeight:@(s->height),
          (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@(pitch),
          (id)kIOSurfacePixelFormat:@((uint32_t)0x42475241),(id)kIOSurfaceAllocSize:@(size)};
        o->surface=IOSurfaceCreate((CFDictionaryRef)desc);if(!o->surface)return VK_ERROR_OUT_OF_HOST_MEMORY;
        o->pitch=IOSurfaceGetBytesPerRow(o->surface);o->size=IOSurfaceGetAllocSize(o->surface);
        // Bootstrap the IOSurface's driver cache through WindowServer once, not per frame.
        IOSurfaceLock(o->surface,0,NULL);uint32_t*base=IOSurfaceGetBaseAddress(o->surface);
        for(size_t n=0;n<o->size/4;n++)base[n]=0xff202020;IOSurfaceUnlock(o->surface,0,NULL);
        publish_surface(s->surface,o->surface);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.04]];
        VkResult vram=import_vram(s,o);
        if(vram!=VK_SUCCESS){VkResult host=import_host(s,o);
            fprintf(stderr,"[NVK-NATIVE] output %u surface %u: VRAM import %d; host import %d\n",i,IOSurfaceGetID(o->surface),vram,host);
            if(host!=VK_SUCCESS)return host;}
        fprintf(stderr,"[NVK-NATIVE] output %u: GPU writes IOSurface %s directly, pitch %zu allocation %zu; no CPU pixel copy\n",i,o->mode==1?"VRAM":"host pages",o->pitch,o->size);
    }
    publish_surface(s->surface,NULL);
    if(s->fused_present){TRY(fused_descriptors_init(s));}else{TRY(opaque_descriptors_init(s));}
    s->stats_start_ns=now_ns();return VK_SUCCESS;
}
static void native_outputs_destroy(Swapchain*s){
    if(s->surface&&s->surface->layer.contents)publish_surface(s->surface,NULL);
    if(s->fused_pool)R(DestroyDescriptorPool)(s->owner->device,s->fused_pool,NULL);
    for(unsigned i=0;i<s->count;i++)if(s->fused_views[i])R(DestroyImageView)(s->owner->device,s->fused_views[i],NULL);
    if(s->fused_sampler)R(DestroySampler)(s->owner->device,s->fused_sampler,NULL);
    if(s->descriptor_pool)R(DestroyDescriptorPool)(s->owner->device,s->descriptor_pool,NULL);
    for(unsigned i=0;i<s->output_count;i++){NativeOutput*o=&s->output[i];
        if(o->buffer)R(DestroyBuffer)(s->owner->device,o->buffer,NULL);
        if(o->memory)R(FreeMemory)(s->owner->device,o->memory,NULL);
        if(o->surface)CFRelease(o->surface);}
}
static NativeOutput* native_output_acquire(Swapchain*s,int *index){
    uint64_t start=now_ns();
    do {for(unsigned n=0;n<s->output_count;n++){unsigned i=(s->output_cursor+n)%s->output_count;NativeOutput*o=&s->output[i];
        if((int)i==s->current||IOSurfaceIsInUse(o->surface)||now_ns()-o->published_ns<s->recycle_delay_ns)continue;
        *index=i;s->output_cursor=(i+1)%s->output_count;return o;}
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.0002]];
    }while(now_ns()-start<2000000000ULL);
    return NULL;
}
