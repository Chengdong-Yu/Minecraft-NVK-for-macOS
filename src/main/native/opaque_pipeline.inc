#include "opaque_shader.inc"
static VkResult opaque_pipeline_init(Device*d){
    if(d->opaque_pipeline)return VK_SUCCESS;
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    TRY(R(CreateDescriptorSetLayout)(d->device,&layout,NULL,&d->opaque_set_layout));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=12};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&d->opaque_set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    TRY(R(CreatePipelineLayout)(d->device,&pl,NULL,&d->opaque_layout));
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(opaque_spirv),.pCode=opaque_spirv};
    VkShaderModule module;TRY(R(CreateShaderModule)(d->device,&shader,NULL,&module));
    VkComputePipelineCreateInfo info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=d->opaque_layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VkResult result=R(CreateComputePipelines)(d->device,VK_NULL_HANDLE,1,&info,NULL,&d->opaque_pipeline);
    R(DestroyShaderModule)(d->device,module,NULL);
    fprintf(stderr,"[NVK-NATIVE] GPU opaque-alpha pipeline result %d\n",result);return result;
}
static VkResult opaque_descriptors_init(Swapchain*s){
    Device*d=s->owner;TRY(opaque_pipeline_init(d));
    VkDescriptorPoolSize size={.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=s->output_count};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=s->output_count,.poolSizeCount=1,.pPoolSizes=&size};
    TRY(R(CreateDescriptorPool)(d->device,&pool,NULL,&s->descriptor_pool));
    VkDescriptorSetLayout layouts[OUTPUT_COUNT];VkDescriptorSet sets[OUTPUT_COUNT];
    for(unsigned i=0;i<s->output_count;i++)layouts[i]=d->opaque_set_layout;
    VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s->descriptor_pool,.descriptorSetCount=s->output_count,.pSetLayouts=layouts};
    TRY(R(AllocateDescriptorSets)(d->device,&alloc,sets));
    for(unsigned i=0;i<s->output_count;i++){
        s->output[i].descriptor=sets[i];VkDescriptorBufferInfo buffer={.buffer=s->output[i].buffer,.range=VK_WHOLE_SIZE};
        VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[i],.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer};
        R(UpdateDescriptorSets)(d->device,1,&write,0,NULL);
    }
    return VK_SUCCESS;
}
