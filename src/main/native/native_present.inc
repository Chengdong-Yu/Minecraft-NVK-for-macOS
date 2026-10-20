static void capture_native(Swapchain*s,NativeOutput*o,unsigned long frame){
    const char *directory=getenv("MC_RENDER_CAPTURE_DIR");if(!directory||(frame!=120&&frame!=900&&frame!=1800&&frame!=3600))return;
    if(IOSurfaceLock(o->surface,kIOSurfaceLockReadOnly,NULL)!=kIOReturnSuccess)return;
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,IOSurfaceGetBaseAddress(o->surface),o->size,NULL);
    CGImageRef image=CGImageCreate(s->width,s->height,8,32,o->pitch,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,NULL,false,kCGRenderingIntentDefault);
    char path[2048];snprintf(path,sizeof(path),"%s/native-%04lu.png",directory,frame);
    NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dest=CGImageDestinationCreateWithURL((CFURLRef)url,CFSTR("public.png"),1,NULL);
    if(dest&&image){CGImageDestinationAddImage(dest,image,NULL);if(CGImageDestinationFinalize(dest))fprintf(stderr,"[NVK-NATIVE] frame capture %s\n",path);}
    if(dest)CFRelease(dest);if(image)CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);
    IOSurfaceUnlock(o->surface,kIOSurfaceLockReadOnly,NULL);
}
static void mark_vram_newest(Swapchain*s,NativeOutput*o){
    struct {uint32_t magic,seed,generation;} tag={0x564d5634u,IOSurfaceGetSeed(o->surface),0};
    CFTypeRef previous=IOSurfaceCopyValue(o->surface,CFSTR("NVMTLVramAt:p0"));
    if(previous&&CFGetTypeID(previous)==CFDataGetTypeID()&&CFDataGetLength(previous)==sizeof(tag)){
        const uint32_t*p=(const uint32_t*)CFDataGetBytePtr(previous);tag.generation=p[2]+1;
    }else tag.generation=1;
    if(previous)CFRelease(previous);
    CFDataRef data=CFDataCreate(NULL,(const UInt8*)&tag,sizeof(tag));IOSurfaceSetValue(o->surface,CFSTR("NVMTLVramAt:p0"),data);CFRelease(data);
    (void)s;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info){@autoreleasepool{
    audit_install();
    Queue*q=find_queue(queue);if(!q)return VK_ERROR_INITIALIZATION_FAILED;VkResult overall=VK_SUCCESS;
    for(unsigned k=0;k<info->swapchainCount;k++){
        Swapchain*s=chain_ptr(info->pSwapchains[k]);uint32_t image_index=info->pImageIndices[k];
        if(!s||image_index>=s->count||!s->acquired[image_index])return VK_ERROR_OUT_OF_DATE_KHR;
        if(s->present_mode==VK_PRESENT_MODE_FIFO_KHR&&s->last_present_ns){
            uint64_t deadline=s->last_present_ns+s->refresh_period_ns;
            for(;;){uint64_t now=now_ns();if(now>=deadline)break;uint64_t remaining=deadline-now;
                if(remaining>100000)usleep((useconds_t)(remaining/1000));else break;}}
        uint64_t begin_ns=now_ns();int output_index=-1;NativeOutput*o=native_output_acquire(s,&output_index);
        if(!o){fprintf(stderr,"[NVK-NATIVE] IOSurface ring stalled\n");return VK_ERROR_SURFACE_LOST_KHR;}
        uint64_t acquired_ns=now_ns();s->stats_wait_ns+=acquired_ns-begin_ns;
        if(o->mode==2&&IOSurfaceLock(o->surface,0,NULL)!=kIOReturnSuccess)return VK_ERROR_SURFACE_LOST_KHR;
        TRY(ensure_command(s,q));VkDevice d=s->owner->device;VkCommandBuffer cb=s->command;
        TRY(R(ResetCommandBuffer)(cb,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};TRY(R(BeginCommandBuffer)(cb,&begin));
        if(s->timing){R(CmdResetQueryPool)(cb,s->timing,0,2);R(CmdWriteTimestamp)(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,s->timing,0);}
        if(s->fused_present){
            VkImageMemoryBarrier sampled={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
              .oldLayout=s->source_layout,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
              .image=s->images[image_index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&sampled);
            uint32_t extent[4]={s->width,s->height,(uint32_t)(o->pitch/4),s->flip_y};
            R(CmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->owner->fused_pipeline);
            R(CmdBindDescriptorSets)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->owner->fused_layout,0,1,&o->fused_descriptors[image_index],0,NULL);
            R(CmdPushConstants)(cb,s->owner->fused_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(extent),extent);
            R(CmdDispatch)(cb,(s->width*s->height+255)/256,1,1);
            sampled.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;sampled.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            sampled.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;sampled.newLayout=s->source_layout;
            R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&sampled);
        }else{
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
          .oldLayout=s->source_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
          .image=s->images[image_index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copy={.bufferRowLength=(uint32_t)(o->pitch/4),.bufferImageHeight=s->height,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={s->width,s->height,1}};
        R(CmdCopyImageToBuffer)(cb,s->images[image_index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,o->buffer,1,&copy);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=s->source_layout;
        R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferMemoryBarrier compute={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
          .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=o->buffer,.size=VK_WHOLE_SIZE};
        R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&compute,0,NULL);
        uint32_t extent[3]={s->width,s->height,(uint32_t)(o->pitch/4)};
        R(CmdBindPipeline)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->owner->opaque_pipeline);
        R(CmdBindDescriptorSets)(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->owner->opaque_layout,0,1,&o->descriptor,0,NULL);
        R(CmdPushConstants)(cb,s->owner->opaque_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(extent),extent);
        R(CmdDispatch)(cb,(s->width*s->height+255)/256,1,1);
        }
        VkBufferMemoryBarrier visible={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=o->mode==2?VK_ACCESS_HOST_READ_BIT:VK_ACCESS_MEMORY_READ_BIT,
          .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=o->buffer,.size=VK_WHOLE_SIZE};
        R(CmdPipelineBarrier)(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,o->mode==2?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,1,&visible,0,NULL);if(s->timing)R(CmdWriteTimestamp)(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s->timing,1);TRY(R(EndCommandBuffer)(cb));
        uint32_t waits=k?0:info->waitSemaphoreCount;VkPipelineStageFlags*stages=waits?calloc(waits,sizeof(*stages)):NULL;
        if(waits&&!stages)return VK_ERROR_OUT_OF_HOST_MEMORY;for(unsigned i=0;i<waits;i++)stages[i]=s->fused_present?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=waits,.pWaitSemaphores=info->pWaitSemaphores,.pWaitDstStageMask=stages,.commandBufferCount=1,.pCommandBuffers=&cb};
        TRY(R(ResetFences)(d,1,&s->fence));VkResult submitted=R(QueueSubmit)(queue,1,&submit,s->fence);free(stages);if(submitted!=VK_SUCCESS)return submitted;
        TRY(R(WaitForFences)(d,1,&s->fence,VK_TRUE,30000000000ULL));
        if(s->timing){uint64_t ticks[2];if(R(GetQueryPoolResults)(d,s->timing,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)s->stats_gpu_ns+=(uint64_t)((ticks[1]-ticks[0])*s->timestamp_period);}
        uint64_t copied_ns=now_ns();s->stats_copy_ns+=copied_ns-acquired_ns;
        if(o->mode==1){uint32_t dirty=surface_dirty(d,IOSurfaceGetID(o->surface),0);
            if(dirty){fprintf(stderr,"[NVK-NATIVE] VRAM dirty refused %#x\n",dirty);return VK_ERROR_SURFACE_LOST_KHR;}mark_vram_newest(s,o);
        }else IOSurfaceUnlock(o->surface,0,NULL);
        publish_surface(s->surface,o->surface);s->current=output_index;o->published_ns=now_ns();s->last_present_ns=o->published_ns;
        s->stats_publish_ns+=o->published_ns-copied_ns;s->stats_frames++;s->acquired[image_index]=VK_FALSE;
        unsigned long frame=++present_count;
        if(frame<=3||frame%600==0)fprintf(stderr,"[NVK-NATIVE] frame %lu GPU -> IOSurface %s -> CALayer; zero CPU pixel copies; graphics pipelines %lu\n",frame,o->mode==1?"VRAM":"host",graphics_pipelines);
        capture_native(s,o,frame);
        uint64_t elapsed=now_ns()-s->stats_start_ns;
        if(elapsed>=5000000000ULL){double n=s->stats_frames;
            fprintf(stderr,"[NVK-NATIVE] STATS %.1f present/s; wait %.3f ms; GPU/fence %.3f ms; publish %.3f ms; output GPU %.3f ms\n",n*1e9/elapsed,s->stats_wait_ns/n/1e6,s->stats_copy_ns/n/1e6,s->stats_publish_ns/n/1e6,s->stats_gpu_ns/n/1e6);
            audit_report();
            s->stats_start_ns=now_ns();s->stats_frames=0;s->stats_wait_ns=s->stats_copy_ns=s->stats_publish_ns=s->stats_gpu_ns=0;}
        if(info->pResults)info->pResults[k]=VK_SUCCESS;
    }
    return overall;
}}
