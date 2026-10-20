#include "fused_shader.inc"
static VkResult fused_pipeline_init(Device*d){
    if(d->fused_pipeline)return VK_SUCCESS;
    VkDescriptorSetLayoutBinding bindings[2]={
      {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo set={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    TRY(R(CreateDescriptorSetLayout)(d->device,&set,NULL,&d->fused_set_layout));
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=16};
    VkPipelineLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&d->fused_set_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    TRY(R(CreatePipelineLayout)(d->device,&layout,NULL,&d->fused_layout));
    VkShaderModuleCreateInfo shader={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(fused_spirv),.pCode=fused_spirv};VkShaderModule module;
    TRY(R(CreateShaderModule)(d->device,&shader,NULL,&module));
    VkComputePipelineCreateInfo pipeline={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=d->fused_layout,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VkResult result=R(CreateComputePipelines)(d->device,VK_NULL_HANDLE,1,&pipeline,NULL,&d->fused_pipeline);R(DestroyShaderModule)(d->device,module,NULL);
    fprintf(stderr,"[NVK-NATIVE] fused sample/BGRA/opaque output pipeline result %d\n",result);return result;
}
static VkResult fused_descriptors_init(Swapchain*s){
    Device*d=s->owner;TRY(fused_pipeline_init(d));
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
      .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    TRY(R(CreateSampler)(d->device,&sampler,NULL,&s->fused_sampler));
    for(unsigned i=0;i<s->count;i++){
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=s->images[i],.viewType=VK_IMAGE_VIEW_TYPE_2D,
          .format=s->source_format,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        TRY(R(CreateImageView)(d->device,&view,NULL,&s->fused_views[i]));
    }
    unsigned n=s->output_count*s->count;
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,n},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,n}};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=n,.poolSizeCount=2,.pPoolSizes=sizes};
    TRY(R(CreateDescriptorPool)(d->device,&pool,NULL,&s->fused_pool));
    VkDescriptorSetLayout layouts[OUTPUT_COUNT*4];VkDescriptorSet sets[OUTPUT_COUNT*4];for(unsigned i=0;i<n;i++)layouts[i]=d->fused_set_layout;
    VkDescriptorSetAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s->fused_pool,.descriptorSetCount=n,.pSetLayouts=layouts};
    TRY(R(AllocateDescriptorSets)(d->device,&alloc,sets));
    for(unsigned o=0;o<s->output_count;o++)for(unsigned i=0;i<s->count;i++){
        VkDescriptorSet set=sets[o*s->count+i];s->output[o].fused_descriptors[i]=set;
        VkDescriptorImageInfo image={.sampler=s->fused_sampler,.imageView=s->fused_views[i],.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo buffer={.buffer=s->output[o].buffer,.range=VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]={
          {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image},
          {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffer}};
        R(UpdateDescriptorSets)(d->device,2,writes,0,NULL);
    }
    return VK_SUCCESS;
}
