#import <objc/runtime.h>
static _Atomic unsigned long audit_readbacks,audit_large_readbacks;
static _Atomic unsigned long audit_draws,audit_indexed,audit_indirect,audit_dispatches,audit_submits;
static _Atomic unsigned long audit_metal_commit,audit_metal_render,audit_metal_compute,audit_metal_blit;
static void audit_hook_encoder(Class cls,const char *name,_Atomic unsigned long *count,int argument){
    SEL sel=sel_registerName(name);Method method=class_getInstanceMethod(cls,sel);if(!method)return;
    IMP original=method_getImplementation(method),replacement;
    if(argument==2)replacement=imp_implementationWithBlock(^id(id self,NSUInteger type){atomic_fetch_add(count,1);return ((id(*)(id,SEL,NSUInteger))original)(self,sel,type);});
    else if(argument)replacement=imp_implementationWithBlock(^id(id self,id descriptor){atomic_fetch_add(count,1);return ((id(*)(id,SEL,id))original)(self,sel,descriptor);});
    else replacement=imp_implementationWithBlock(^id(id self){atomic_fetch_add(count,1);return ((id(*)(id,SEL))original)(self,sel);});
    class_replaceMethod(cls,sel,replacement,method_getTypeEncoding(method));
}
static void audit_install(void){
    if(!audit_on)return;static int installed;
    if(installed)return;Class cls=objc_getClass("NVMTLCommandBuffer");if(!cls)return;
    SEL sel=sel_registerName("commit");Method method=class_getInstanceMethod(cls,sel);if(!method)return;
    IMP original=method_getImplementation(method);
    class_replaceMethod(cls,sel,imp_implementationWithBlock(^void(id self){atomic_fetch_add(&audit_metal_commit,1);((void(*)(id,SEL))original)(self,sel);}),method_getTypeEncoding(method));
    audit_hook_encoder(cls,"renderCommandEncoderWithDescriptor:",&audit_metal_render,1);
    audit_hook_encoder(cls,"computeCommandEncoder",&audit_metal_compute,0);
    audit_hook_encoder(cls,"computeCommandEncoderWithDispatchType:",&audit_metal_compute,2);
    audit_hook_encoder(cls,"blitCommandEncoder",&audit_metal_blit,0);
    installed=1;fprintf(stderr,"[GRAPHICS-AUDIT] observing NVMTLCommandBuffer commit/render/compute/blit in this game process\n");
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
