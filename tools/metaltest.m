// Metal through NVMetal.bundle: finds the NVIDIA device Metal lists (the process must be let in: run with
// NVMETAL_ALLOW=1 or list it in /Library/Preferences/io.github.kvarun-p.nvmetal.allow), then runs a compute
// kernel, draws a triangle and reads the command buffer's GPU times. One line per check: "ok:", "FAIL:", or
// "WARN:" for features newer bundles have (an older installed bundle lacks them).
//
//   clang -fobjc-arc -framework Metal -framework Foundation tools/metaltest.m -o metaltest && NVMETAL_ALLOW=1 ./metaltest
#import <Metal/Metal.h>
#include <stdio.h>

static int fails;
static void check(bool ok, const char *what)
{
  printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
  fails += !ok;
}

static const char *src =
  "#include <metal_stdlib>\nusing namespace metal;\n"
  "kernel void add(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],\n"
  "                device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }\n"
  "struct V { float4 p [[position]]; };\n"
  "vertex V vs(uint i [[vertex_id]]) { float2 q[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };\n"
  "  V v; v.p = float4(q[i], 0, 1); return v; }\n"
  "fragment float4 fs(V v [[stage_in]]) { return float4(1, 0.5, 0.25, 1); }\n";

int main(void)
{
  @autoreleasepool {
    id<MTLDevice> d = nil;
    for (id<MTLDevice> x in MTLCopyAllDevices())
      if ([x.name containsString:@"(nvmetal)"])
        d = x;
    if (!d) {
      printf("  FAIL: Metal lists no NVIDIA (nvmetal) device\n");
      return 1;
    }
    printf("  ok: device %s\n", d.name.UTF8String);
    NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) {
      printf("  FAIL: shader library: %s\n", e.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLCommandQueue> q = [d newCommandQueue];

    // compute: 1M additions
    enum { N = 1 << 20 };
    id<MTLComputePipelineState> add = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"add"] error:&e];
    id<MTLBuffer> a = [d newBufferWithLength:4 * N options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [d newBufferWithLength:4 * N options:MTLResourceStorageModeShared];
    id<MTLBuffer> c = [d newBufferWithLength:4 * N options:MTLResourceStorageModeShared];
    float *pa = a.contents, *pb = b.contents, *pc = c.contents;
    for (int i = 0; i < N; i++) {
      pa[i] = i;
      pb[i] = 2.0f * i;
    }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:add];
    [ce setBuffer:a offset:0 atIndex:0];
    [ce setBuffer:b offset:0 atIndex:1];
    [ce setBuffer:c offset:0 atIndex:2];
    [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [ce endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    int bad = 0;
    for (int i = 0; i < N; i++)
      bad += pc[i] != 3.0f * i;
    check(add && cb.status == MTLCommandBufferStatusCompleted && !bad, "compute kernel (1M additions)");
    if (cb.GPUStartTime > 0 && cb.GPUEndTime > cb.GPUStartTime)
      printf("  ok: command buffer GPU times\n");
    else
      printf("  WARN: command buffer GPU times are 0 (bundle older than 2026-10-05)\n");

    // render: one triangle over a 64x64 target
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&e];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [d newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl synchronizeResource:t];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t px[4] = { 0 };
    [t getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(32, 32, 1, 1) mipmapLevel:0];
    check(ps && px[0] == 255 && px[1] >= 127 && px[1] <= 128 && px[2] >= 63 && px[2] <= 64, "render pipeline (a triangle)");
    return fails != 0;
  }
}
