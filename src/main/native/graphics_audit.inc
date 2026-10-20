static _Atomic unsigned long audit_readbacks,audit_large_readbacks;
static _Atomic unsigned long audit_draws,audit_indexed,audit_indirect,audit_dispatches,audit_submits;
static _Atomic unsigned long audit_metal_commit,audit_metal_render,audit_metal_compute,audit_metal_blit;
static void audit_metal_hit(uint32_t kind){
    switch(kind){case 0:audit_metal_commit++;break;case 1:audit_metal_render++;break;case 2:audit_metal_compute++;break;case 3:audit_metal_blit++;break;}
}
static void audit_install(void){
    if(!audit_on)return;static int installed;if(installed)return;
    installed=mc_cocoa_audit_install(audit_metal_hit);
    if(installed)fprintf(stderr,"[GRAPHICS-AUDIT] Swift hooks observing NVMTLCommandBuffer commit/render/compute/blit\n");
}
static void audit_report(void){
    if(!audit_on)return;audit_install();
    void *(*currentGL)(void)=dlsym(RTLD_DEFAULT,"CGLGetCurrentContext");
    fprintf(stderr,"[GRAPHICS-AUDIT] cumulative Vulkan draw=%lu indexed=%lu indirect=%lu dispatch=%lu submits=%lu; image-to-buffer=%lu large=%lu; Metal commit=%lu render=%lu compute=%lu blit=%lu; current CGL context=%s\n",
      audit_draws,audit_indexed,audit_indirect,audit_dispatches,audit_submits,audit_readbacks,audit_large_readbacks,audit_metal_commit,audit_metal_render,audit_metal_compute,audit_metal_blit,currentGL&&currentGL()?"PRESENT":"none");
}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,uint32_t d){if(audit_on)audit_draws++;R(CmdDraw)(cb,a,b,c,d);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cb,uint32_t a,uint32_t b,uint32_t c,int32_t d,uint32_t e){if(audit_on)audit_indexed++;R(CmdDrawIndexed)(cb,a,b,c,d,e);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t stride){if(audit_on)audit_indirect++;R(CmdDrawIndirect)(cb,b,o,n,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t stride){if(audit_on)audit_indirect++;R(CmdDrawIndexedIndirect)(cb,b,o,n,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,VkBuffer c,VkDeviceSize co,uint32_t n,uint32_t stride){if(audit_on)audit_indirect++;R(CmdDrawIndirectCount)(cb,b,o,c,co,n,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb,VkBuffer b,VkDeviceSize o,VkBuffer c,VkDeviceSize co,uint32_t n,uint32_t stride){if(audit_on)audit_indirect++;R(CmdDrawIndexedIndirectCount)(cb,b,o,c,co,n,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z){if(audit_on)audit_dispatches++;R(CmdDispatch)(cb,x,y,z);}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){if(audit_on)audit_submits+=n;return R(QueueSubmit)(q,n,i,f);}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2*i,VkFence f){if(audit_on)audit_submits+=n;return R(QueueSubmit2)(q,n,i,f);}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue q,uint32_t n,const VkSubmitInfo2*i,VkFence f){if(audit_on)audit_submits+=n;return R(QueueSubmit2KHR)(q,n,i,f);}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage image,VkImageLayout layout,VkBuffer buffer,uint32_t n,const VkBufferImageCopy*regions){
    if(audit_on){audit_readbacks++;for(unsigned i=0;i<n;i++)if(regions[i].imageExtent.width>=640&&regions[i].imageExtent.height>=360)audit_large_readbacks++;}
    R(CmdCopyImageToBuffer)(cb,image,layout,buffer,n,regions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2(VkCommandBuffer cb,const VkCopyImageToBufferInfo2*info){
    if(audit_on){audit_readbacks++;for(unsigned i=0;i<info->regionCount;i++)if(info->pRegions[i].imageExtent.width>=640&&info->pRegions[i].imageExtent.height>=360)audit_large_readbacks++;}
    R(CmdCopyImageToBuffer2)(cb,info);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(VkCommandBuffer cb,const VkCopyImageToBufferInfo2*info){
    if(audit_on){audit_readbacks++;for(unsigned i=0;i<info->regionCount;i++)if(info->pRegions[i].imageExtent.width>=640&&info->pRegions[i].imageExtent.height>=360)audit_large_readbacks++;}
    R(CmdCopyImageToBuffer2KHR)(cb,info);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiEXT(VkCommandBuffer cb,uint32_t n,const VkMultiDrawInfoEXT*info,uint32_t instances,uint32_t first,uint32_t stride){if(audit_on)audit_draws+=n;R(CmdDrawMultiEXT)(cb,n,info,instances,first,stride);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawMultiIndexedEXT(VkCommandBuffer cb,uint32_t n,const VkMultiDrawIndexedInfoEXT*info,uint32_t instances,uint32_t first,uint32_t stride,const int32_t*offsets){if(audit_on)audit_indexed+=n;R(CmdDrawMultiIndexedEXT)(cb,n,info,instances,first,stride,offsets);}
