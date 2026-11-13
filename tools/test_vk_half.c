// Headless arithmetic probe using the existing Vulkan pipeline test's device.
// Exhaust all 65536 half encodings, preserving exceptional values in software,
// and check downstream float32 arithmetic, not just conversion.
#define TEST_VK_PIPELINES_NO_MAIN
#define TEST_VK_PIPELINES_MINIMAL_DEVICE
#include "test_vk_pipelines.c"
void vk_device_fault_report(void) { fprintf(stderr, "Vulkan device lost during arithmetic probe\n"); }

static const char source[] =
    "#version 450\n"
    "layout(local_size_x=64) in;\n"
    "layout(set=0,binding=0,std430) buffer Out { uint result[]; };\n"
    "float soft(uint h) { uint e=h&0x7C00u; float f=uintBitsToFloat(((h&0x7FFFu)+0x1C000u)<<13);"
    " f=e==0u?float(h&0x3FFu)*5.9604644775390625e-8:f;"
    " f=e==0x7C00u?uintBitsToFloat(0x7F800000u|((h&0x3FFu)<<13)):f;"
    " return uintBitsToFloat(floatBitsToUint(f)|((h&0x8000u)<<16)); }\n"
    "float hw(uint h) { uint e=h&0x7C00u; return e==0u||e==0x7C00u?soft(h):unpackHalf2x16(h).x; }\n"
    "vec2 hw2(uint v) { uint a=v&0x7C00u,b=v&0x7C000000u;"
    " if(a!=0u&&a!=0x7C00u&&b!=0u&&b!=0x7C000000u) return unpackHalf2x16(v);"
    " return vec2(hw(v&0xFFFFu),hw(v>>16)); }\n"
    "float light(float a,float b) { precise float x=a*1.013671875;"
    " precise float y=x+b*0.3125; precise float z=y*0.10009765625+1.0009765625; return z; }\n"
    "void main() { uint h=gl_GlobalInvocationID.x;"
    " result[4*h]=floatBitsToUint(soft(h)); result[4*h+1]=floatBitsToUint(hw(h));"
    " result[4*h+2]=floatBitsToUint(light(soft(h),soft(h^0x1234u)));"
    " vec2 n=hw2(h|((h^0x1234u)<<16)); result[4*h+3]=floatBitsToUint(light(n.x,n.y)); }\n";

static bool nan_bits(uint32_t v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu); }

int main(void) {
    init_device();
    vk_shaders_init();
    VkQueue queue;
    vkGetDeviceQueue(g_vk.dev, 0, 0, &queue);
    size_t words;
    uint32_t *spirv = vk_glsl_to_spirv(source, STAGE_COMPUTE, "half_probe", &words);
    if (!spirv) return 1;
    VkShaderModule module = vk_module(spirv, words);
    free(spirv);
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo sli = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout sl;
    if (vkCreateDescriptorSetLayout(g_vk.dev,&sli,NULL,&sl)) return 2;
    VkPipelineLayoutCreateInfo pli = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl};
    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(g_vk.dev,&pli,NULL,&layout)) return 2;
    VkComputePipelineCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
    VkPipeline pipeline;
    if (vkCreateComputePipelines(g_vk.dev,VK_NULL_HANDLE,1,&pi,NULL,&pipeline)) return 2;
    VkDeviceSize bytes = 65536 * 4 * sizeof(uint32_t);
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    if (vkCreateBuffer(g_vk.dev,&bi,NULL,&buffer)) return 2;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev,buffer,&req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys,&mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i=0;i<mp.memoryTypeCount;i++)
        if ((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mt=i; if(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
        }
    if (mt==UINT32_MAX) return 2;
    VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
    VkDeviceMemory memory;
    if (vkAllocateMemory(g_vk.dev,&ai,NULL,&memory) || vkBindBufferMemory(g_vk.dev,buffer,memory,0)) return 2;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dpi = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&pool_size};
    VkDescriptorPool pool;
    if(vkCreateDescriptorPool(g_vk.dev,&dpi,NULL,&pool)) return 2;
    VkDescriptorSetAllocateInfo si = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&sl};
    VkDescriptorSet set;
    if(vkAllocateDescriptorSets(g_vk.dev,&si,&set)) return 2;
    VkDescriptorBufferInfo db = {buffer,0,bytes};
    VkWriteDescriptorSet write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
    vkUpdateDescriptorSets(g_vk.dev,1,&write,0,NULL);
    VkCommandPoolCreateInfo cpi = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0};
    VkCommandPool cp;
    if(vkCreateCommandPool(g_vk.dev,&cpi,NULL,&cp)) return 2;
    VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cp,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb;
    if(vkAllocateCommandBuffers(g_vk.dev,&cai,&cb)) return 2;
    VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if(vkBeginCommandBuffer(cb,&begin)) return 2;
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);
    vkCmdDispatch(cb,1024,1,1);
    VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    if(vkEndCommandBuffer(cb)) return 2;
    VkSubmitInfo submit = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    if(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) return 2;
    void *mapped;
    if(vkMapMemory(g_vk.dev,memory,0,VK_WHOLE_SIZE,0,&mapped)) return 2;
    VkMappedMemoryRange mr = {.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
    if(vkInvalidateMappedMemoryRanges(g_vk.dev,1,&mr)) return 2;
    uint32_t *out = mapped;
    unsigned conversion=0,math=0;
    for (unsigned h=0;h<65536;h++) {
        if(out[4*h]!=out[4*h+1]) conversion++;
        uint32_t a=out[4*h+2],b=out[4*h+3];
        if(a!=b && !(nan_bits(a)&&nan_bits(b))) {
            if(math<8) fprintf(stderr,"arithmetic mismatch half %04x: %08x != %08x\n",h,a,b);
            math++;
        }
    }
    printf("65536 encodings: conversion mismatches %u, float32 arithmetic mismatches %u, validation messages %ld\n",conversion,math,g_messages);
    vkUnmapMemory(g_vk.dev,memory);
    vkDestroyCommandPool(g_vk.dev,cp,NULL); vkDestroyDescriptorPool(g_vk.dev,pool,NULL);
    vkDestroyBuffer(g_vk.dev,buffer,NULL); vkFreeMemory(g_vk.dev,memory,NULL);
    vkDestroyPipeline(g_vk.dev,pipeline,NULL); vkDestroyPipelineLayout(g_vk.dev,layout,NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev,sl,NULL); vkDestroyShaderModule(g_vk.dev,module,NULL);
    vkDestroyDevice(g_vk.dev,NULL); vkDestroyInstance(g_vk.inst,NULL);
    return conversion || math || g_messages;
}
