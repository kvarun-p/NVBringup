// vktest: first Vulkan test of NVK on macOS. Loads the ICD directly (no Vulkan loader):
//   build/vktest /path/to/mesa/build/src/nouveau/vulkan/libvulkan_nouveau.dylib
// 1. instance, physical device, properties, memory types, queue families;
// 2. device + queue; a compute shader (hand-assembled SPIR-V) writes out[i] = 2*i + 1
//    into a host-visible buffer, checked by the CPU;
// 3. the same into a device-local buffer, copied back with vkCmdCopyBuffer;
// 4. timestamps around the dispatch.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { printf("  %s: ", (cond) ? "ok" : "FAIL"); printf(__VA_ARGS__); printf("\n"); \
                              if (!(cond)) fails++; } while (0)
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
        printf("  FAIL: %s -> %d\n", #call, r_); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

// ---- A minimal SPIR-V 1.3 compute shader, assembled word by word -----------------------------
static uint32_t spv[256];
static uint32_t nspv;

static void op(uint32_t opcode, uint32_t n, ...)
{
    spv[nspv++] = ((n + 1) << 16) | opcode;
    va_list ap;
    va_start(ap, n);
    for (uint32_t i = 0; i < n; i++)
        spv[nspv++] = va_arg(ap, uint32_t);
    va_end(ap);
}

static void op_str(uint32_t opcode, uint32_t a, uint32_t b, const char *s, uint32_t tail)
{
    uint32_t len = (uint32_t)strlen(s) / 4 + 1, start = nspv;
    spv[nspv++] = 0;
    spv[nspv++] = a;
    spv[nspv++] = b;
    memset(&spv[nspv], 0, len * 4);
    memcpy(&spv[nspv], s, strlen(s));
    nspv += len;
    spv[nspv++] = tail;
    spv[start] = ((nspv - start) << 16) | opcode;
}

// out[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x * 2 + 1, local size 64
static void build_shader(void)
{
    enum { void_ = 1, fn, uint_, v3, pin, gid, rta, bufT, psb, buf, int_, c0, c2, c1, psbu, main_, lbl,
           g, x, m, v, ptr, BOUND };
    spv[nspv++] = 0x07230203;
    spv[nspv++] = 0x00010300;
    spv[nspv++] = 0;
    spv[nspv++] = BOUND;
    spv[nspv++] = 0;
    op(17, 1, 1);                                   // OpCapability Shader
    op(14, 2, 0, 1);                                // OpMemoryModel Logical GLSL450
    op_str(15, 5, main_, "main", gid);              // OpEntryPoint GLCompute %main "main" %gid
    op(16, 5, main_, 17, 64, 1, 1);                 // OpExecutionMode LocalSize 64 1 1
    op(71, 3, gid, 11, 28);                         // OpDecorate %gid BuiltIn GlobalInvocationId
    op(71, 3, rta, 6, 4);                           // OpDecorate %rta ArrayStride 4
    op(72, 4, bufT, 0, 35, 0);                      // OpMemberDecorate %Buf 0 Offset 0
    op(71, 2, bufT, 2);                             // OpDecorate %Buf Block
    op(71, 3, buf, 34, 0);                          // DescriptorSet 0
    op(71, 3, buf, 33, 0);                          // Binding 0
    op(19, 1, void_);                               // OpTypeVoid
    op(33, 2, fn, void_);                           // OpTypeFunction
    op(21, 3, uint_, 32, 0);                        // OpTypeInt 32 unsigned
    op(23, 3, v3, uint_, 3);                        // OpTypeVector
    op(32, 3, pin, 1, v3);                          // OpTypePointer Input
    op(59, 3, pin, gid, 1);                         // OpVariable Input
    op(29, 2, rta, uint_);                          // OpTypeRuntimeArray
    op(30, 2, bufT, rta);                           // OpTypeStruct
    op(32, 3, psb, 12, bufT);                       // OpTypePointer StorageBuffer
    op(59, 3, psb, buf, 12);                        // OpVariable StorageBuffer
    op(21, 3, int_, 32, 1);                         // OpTypeInt 32 signed
    op(43, 3, int_, c0, 0);                         // OpConstant
    op(43, 3, uint_, c2, 2);
    op(43, 3, uint_, c1, 1);
    op(32, 3, psbu, 12, uint_);                     // OpTypePointer StorageBuffer uint
    op(54, 4, void_, main_, 0, fn);                 // OpFunction
    op(248, 1, lbl);                                // OpLabel
    op(61, 3, v3, g, gid);                          // OpLoad
    op(81, 4, uint_, x, g, 0);                      // OpCompositeExtract
    op(132, 4, uint_, m, x, c2);                    // OpIMul
    op(128, 4, uint_, v, m, c1);                    // OpIAdd
    op(65, 5, psbu, ptr, buf, c0, x);               // OpAccessChain
    op(62, 2, ptr, v);                              // OpStore
    op(253, 0);                                     // OpReturn
    op(56, 0);                                      // OpFunctionEnd
}

// ---- Helpers ----------------------------------------------------------------------------------
static VkPhysicalDeviceMemoryProperties memProps;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want &&
            !(memProps.memoryTypes[i].propertyFlags & avoid))
            return i;
    return UINT32_MAX;
}

struct buf { VkBuffer b; VkDeviceMemory m; void *map; };

static struct buf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                              VkMemoryPropertyFlags avoid)
{
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkBindBufferMemory); DFN(vkMapMemory);
    struct buf r = {0};
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VKC(vkCreateBuffer(dev, &bi, NULL, &r.b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, r.b, &req);
    uint32_t t = find_type(req.memoryTypeBits, want, avoid);
    if (t == UINT32_MAX) {
        printf("  FAIL: no memory type with flags 0x%x\n", want);
        exit(1);
    }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t };
    VKC(vkAllocateMemory(dev, &ai, NULL, &r.m));
    VKC(vkBindBufferMemory(dev, r.b, r.m, 0));
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VKC(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.map));
    return r;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("NVB_NVK_LIB");
    if (!path) {
        printf("usage: %s <libvulkan_nouveau.dylib>   (or set NVB_NVK_LIB)\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) {
        printf("no vk_icdGetInstanceProcAddr\n");
        return 1;
    }
    uint32_t iface = 5;
    if (neg)
        neg(&iface);
    printf("[1] instance and physical device (ICD interface %u)\n", iface);

    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VKC(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkCreateDevice); IFN(vkDestroyInstance);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

    uint32_t n = 0;
    VKC(vkEnumeratePhysicalDevices(inst, &n, NULL));
    CHECK(n == 1, "%u physical device(s)", n);
    if (!n)
        return 1;
    VkPhysicalDevice pd;
    n = 1;
    VKC(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    printf("  %s, Vulkan %u.%u.%u, driver 0x%x, vendor %04x device %04x, timestampPeriod %.2f ns\n",
           p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
           VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID, p.limits.timestampPeriod);
    vkGetPhysicalDeviceMemoryProperties(pd, &memProps);
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
        printf("  heap %u: %llu MiB flags 0x%x\n", i, (unsigned long long)(memProps.memoryHeaps[i].size >> 20),
               memProps.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        printf("  type %u: heap %u flags 0x%x\n", i, memProps.memoryTypes[i].heapIndex,
               memProps.memoryTypes[i].propertyFlags);
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    for (uint32_t i = 0; i < nq; i++)
        printf("  queue family %u: flags 0x%x count %u timestampValidBits %u\n", i, qf[i].queueFlags,
               qf[i].queueCount, qf[i].timestampValidBits);

    printf("[2] device, compute shader into host-visible memory\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
    VKC(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateShaderModule); DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout);
    DFN(vkCreateComputePipelines); DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets);
    DFN(vkUpdateDescriptorSets); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
    DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline); DFN(vkCmdBindDescriptorSets); DFN(vkCmdDispatch);
    DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyBuffer); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit);
    DFN(vkCreateFence); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkResetCommandBuffer);
    DFN(vkCreateQueryPool); DFN(vkCmdResetQueryPool); DFN(vkCmdWriteTimestamp); DFN(vkGetQueryPoolResults);
    DFN(vkDeviceWaitIdle); DFN(vkDestroyDevice);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    build_shader();
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nspv * 4, spv };
    VkShaderModule sm;
    VKC(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb };
    VkDescriptorSetLayout dsl;
    VKC(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
    VkPipelineLayout pl;
    VKC(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
        pl, VK_NULL_HANDLE, 0 };
    VkPipeline pipe;
    VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    CHECK(1, "compute pipeline compiled by NAK");

    const uint32_t N = 1 << 20;
    const VkDeviceSize size = N * 4;
    struct buf host = make_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    struct buf local = make_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps };
    VkDescriptorPool dp;
    VKC(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetLayout layouts[2] = { dsl, dsl };
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 2, layouts };
    VkDescriptorSet ds[2];
    VKC(vkAllocateDescriptorSets(dev, &dsai, ds));
    VkDescriptorBufferInfo dbi[2] = { { host.b, 0, VK_WHOLE_SIZE }, { local.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++)
        w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds[i], 0, 0, 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi[i], NULL };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    VkCommandPool pool;
    VKC(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    VKC(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    VkFence fence;
    VKC(vkCreateFence(dev, &fci, NULL, &fence));
    VkQueryPoolCreateInfo qpci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0 };
    VkQueryPool qp;
    VKC(vkCreateQueryPool(dev, &qpci, NULL, &qp));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };

    memset(host.map, 0, size);
    VKC(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdResetQueryPool(cb, qp, 0, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds[0], 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
    VKC(vkEndCommandBuffer(cb));
    VKC(vkQueueSubmit(q, 1, &si, fence));
    VkResult fr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    CHECK(fr == VK_SUCCESS, "fence signaled (%d)", fr);
    uint32_t bad = 0, first = UINT32_MAX;
    for (uint32_t i = 0; i < N; i++)
        if (((uint32_t *)host.map)[i] != 2 * i + 1 && !bad++)
            first = i;
    CHECK(!bad, "%u of %u values wrong%s", bad, N, bad ? "" : " (out[i] = 2i+1 for 1M invocations)");
    if (bad)
        printf("  first wrong: out[%u] = 0x%x\n", first, ((uint32_t *)host.map)[first]);
    uint64_t ts[2] = {0};
    VkResult qr = vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT);
    CHECK(qr == VK_SUCCESS && ts[1] > ts[0], "timestamps: dispatch took %.1f us",
          (double)(ts[1] - ts[0]) * p.limits.timestampPeriod / 1000.0);

    printf("[3] compute into device-local memory, copied back with vkCmdCopyBuffer\n");
    memset(host.map, 0, size);
    VKC(vkResetFences(dev, 1, &fence));
    VKC(vkResetCommandBuffer(cb, 0));
    VKC(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds[1], 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
    VkBufferCopy region = { 0, 0, size };
    vkCmdCopyBuffer(cb, local.b, host.b, 1, &region);
    VKC(vkEndCommandBuffer(cb));
    VKC(vkQueueSubmit(q, 1, &si, fence));
    fr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    CHECK(fr == VK_SUCCESS, "fence signaled (%d)", fr);
    bad = 0;
    for (uint32_t i = 0; i < N; i++)
        bad += ((uint32_t *)host.map)[i] != 2 * i + 1;
    CHECK(!bad, "%u of %u values wrong after the copy", bad, N);

    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("\nvktest: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
