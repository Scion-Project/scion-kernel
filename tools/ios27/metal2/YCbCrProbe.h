// SPDX-License-Identifier: MIT-0
// The camera's preview (docs/ios27/CAMERA.md, "The display side"), part of NativeBridgeMetalFrontend.mm (included there; a file
// of its own for the size rule): what a converted two-plane YCbCr texture over an IOSurface (formats 500 to 510, the window
// server's texture over the camera preview's pictures) reads on this process's GPU, beside what the surface holds as this
// process's processor reads it. Off unless SWEET_MTL2_YCCPROBE=1 (mtl2-env). At the 1st, 3rd, 10th, 30th, ... binding of such a
// texture over a surface at least 256 texels wide (16 probes a process at most), passes of its own (YCbCrProbe.metal, on a queue
// of its own, off the caller's thread) read an 8 x 8 grid of the texture in a compute function (read(), a linear and a nearest
// sample) and draw it, sampled in half precision from fragment slot 3 (linear, then nearest), into a 16 x 16 RGBA8 target. Said:
// `native-metal: ycbcr probe N (bind B, ENCODER stage S slot I): fmt F over surface ID seed X WxH: read ...; sample ...; nearest ...;
// cpu Y M, K of 64 lit; frag ...; frag nearest ...; point 0 read ...; status T`. The colors are 0 to 255; a point is lit above 24
// (luma from the colors as BT.601 weights). Reading only: nothing is written and the surface is held only while the probe runs.
// Run on phone 1 on 24 November (D1-D5): every one of these reads the picture the surface holds.
#if __has_include("YCbCrProbe.inc")
#include "YCbCrProbe.inc" // sweet_ycc_probe_metallib[]: YCbCrProbe.metal, built by the bundle's script
#define SWEET_YCC_PROBE 1
#endif
static bool sweetYCbCrProbeOn() {
 static int on=-1;
 if(on<0){const char *e=getenv("SWEET_MTL2_YCCPROBE");on=(e && e[0]=='1')?1:0;}
 return on==1;
}
#ifdef SWEET_YCC_PROBE
// The probe's colors, said: mean of each channel (0..255), and how many of the 64 points are lit.
static void sweetYCbCrProbeSum(const float *c,char *out,size_t size) {
 double r=0,g=0,b=0,a=0;unsigned lit=0;
 for(unsigned k=0;k<64;k++) {
  const float *p=c+4*k;r+=p[0];g+=p[1];b+=p[2];a+=p[3];
  lit+=(0.299*p[0]+0.587*p[1]+0.114*p[2])*255.0>24.0;
 }
 snprintf(out,size,"%.0f %.0f %.0f a %.2f, %u of 64 lit",r*255/64,g*255/64,b*255/64,a/64,lit);
}
static void sweetYCbCrProbeRun(id<MTLTexture> texture,IOSurfaceRef surface,unsigned probe,unsigned bind,const char *where,uint64_t format) {
 static id<MTLComputePipelineState> pipeline;static id<MTLRenderPipelineState> drawing;static id<MTLCommandQueue> queue;static id<MTLSamplerState> sampler,point;static bool failed;
 id<MTLDevice> device=(id<MTLDevice>)gDeviceObject;
 char said[900];
 if(!pipeline && !failed) {
  NSError *error=nil;
  dispatch_data_t data=dispatch_data_create(sweet_ycc_probe_metallib,sizeof sweet_ycc_probe_metallib,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
  id<MTLFunction> function=library?[library newFunctionWithName:@"sweet_ycc_probe"]:nil;
  pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
  if(library) {
   MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
   pd.vertexFunction=[library newFunctionWithName:@"sweet_ycc_probe_vert"];pd.fragmentFunction=[library newFunctionWithName:@"sweet_ycc_probe_frag"];
   pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
   NSError *drawError=nil;
   drawing=(pd.vertexFunction && pd.fragmentFunction)?[device newRenderPipelineStateWithDescriptor:pd error:&drawError]:nil;
   if(!drawing){snprintf(said,sizeof said,"native-metal: ycbcr probe: no render pipeline: %s",drawError?drawError.localizedDescription.UTF8String:"no function");sm2_logs(said);}
  }
  MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];
  sd.minFilter=MTLSamplerMinMagFilterLinear;sd.magFilter=MTLSamplerMinMagFilterLinear;sd.mipFilter=MTLSamplerMipFilterNotMipmapped;
  sd.sAddressMode=MTLSamplerAddressModeClampToEdge;sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
  sampler=[device newSamplerStateWithDescriptor:sd];
  sd.minFilter=MTLSamplerMinMagFilterNearest;sd.magFilter=MTLSamplerMinMagFilterNearest;
  point=[device newSamplerStateWithDescriptor:sd];
  queue=[device newCommandQueue];
  if(!pipeline || !sampler || !point || !queue) {
   failed=true;
   snprintf(said,sizeof said,"native-metal: ycbcr probe: not made (library %s, pipeline %s, sampler %s, queue %s): %s",library?"yes":"no",
            pipeline?"yes":"no",sampler?"yes":"no",queue?"yes":"no",error?error.localizedDescription.UTF8String:"no error");
   sm2_logs(said);
  }
 }
 if(failed)return;
 id<MTLBuffer> out=[device newBufferWithLength:192*4*sizeof(float) options:MTLResourceStorageModeShared];
 if(!out)return;
 id<MTLCommandBuffer> commands=[queue commandBuffer];
 id<MTLComputeCommandEncoder> encoder=[commands computeCommandEncoder];
 [encoder setComputePipelineState:pipeline];
 [encoder setTexture:texture atIndex:0];
 [encoder setSamplerState:sampler atIndex:0];
 [encoder setSamplerState:point atIndex:1];
 [encoder setBuffer:out offset:0 atIndex:0];
 [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
 [encoder endEncoding];
 [commands commit];
 [commands waitUntilCompleted];
 const long status=(long)commands.status;

 char frag[96]="not drawn",fragNearest[96]="not drawn";
 for(int pass=0;pass<2 && drawing;pass++) {
  char *into=pass?fragNearest:frag;
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
  td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;td.storageMode=MTLStorageModeShared;
  id<MTLTexture> target=[device newTextureWithDescriptor:td];
  if(target) {
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
   pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
   pass.colorAttachments[0].clearColor=MTLClearColorMake(1,0,1,1);pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   id<MTLCommandBuffer> drawn=[queue commandBuffer];
   id<MTLRenderCommandEncoder> re=[drawn renderCommandEncoderWithDescriptor:pass];
   [re setRenderPipelineState:drawing];[re setFragmentTexture:texture atIndex:3];[re setFragmentSamplerState:pass?point:sampler atIndex:0];
   [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[re endEncoding];
   [drawn commit];[drawn waitUntilCompleted];
   uint8_t px[16*16*4]={};
   [target getBytes:px bytesPerRow:16*4 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
   unsigned long r=0,g=0,b=0,al=0;unsigned lit=0,magenta=0;
   for(unsigned k=0;k<256;k++){const uint8_t *q=px+4*k;r+=q[0];g+=q[1];b+=q[2];al+=q[3];lit+=(299*q[0]+587*q[1]+114*q[2])/1000>24;magenta+=q[0]==255&&q[1]==0&&q[2]==255;}
   snprintf(into,96,"%lu %lu %lu a %lu, %u of 256 lit, %u untouched, status %ld",r/256,g/256,b/256,al/256,lit,magenta,(long)drawn.status);
  }
 }
 char read[96],sample[96],nearest[96],cpu[96]="not read";
 sweetYCbCrProbeSum((const float *)out.contents,read,sizeof read);
 sweetYCbCrProbeSum((const float *)out.contents+256,sample,sizeof sample);
 sweetYCbCrProbeSum((const float *)out.contents+512,nearest,sizeof nearest);
 if(IOSurfaceGetPlaneCount(surface)==2 && IOSurfaceGetBytesPerElementOfPlane(surface,0)==1 && IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)==0) {
  const uint8_t *l=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0);
  const size_t w=IOSurfaceGetWidthOfPlane(surface,0),h=IOSurfaceGetHeightOfPlane(surface,0),lr=IOSurfaceGetBytesPerRowOfPlane(surface,0);
  if(l && w && h) {
   sys_dcache_flush((void *)l,lr*h);
   unsigned long sum=0;unsigned lit=0;
   for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){const unsigned v=l[(h*(2*y+1)/16)*lr+w*(2*x+1)/16];sum+=v;lit+=v>24;}
   snprintf(cpu,sizeof cpu,"Y %lu, %u of 64 lit",sum/64,lit);
  }
  IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);
 }
 const float *first=(const float *)out.contents;
 snprintf(said,sizeof said,"native-metal: ycbcr probe %u (bind %u, %s): fmt %llu over surface %u seed %u %zux%zu: read %s; sample %s; nearest %s; cpu %s; "
          "frag %s; frag nearest %s; point 0 read %.3f %.3f %.3f %.3f; status %ld%s%s",probe,bind,where,(unsigned long long)format,IOSurfaceGetID(surface),IOSurfaceGetSeed(surface),
          IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),read,sample,nearest,cpu,frag,fragNearest,first[0],first[1],first[2],first[3],status,
          commands.error?": ":"",commands.error?commands.error.localizedDescription.UTF8String:"");
 sm2_logs(said);
}
#endif
// From the encoders' texture binding: count the binding, and at the probe points start a probe of this texture off this thread.
static void sweetYCbCrProbe(id<MTLTexture> texture,IOSurfaceRef surface,const char *encoder,uint32_t stage,NSUInteger index,uint64_t format) {
#ifdef SWEET_YCC_PROBE
 if(!sweetYCbCrProbeOn() || !gDeviceObject || !texture || !surface || format<500 || format>510 || IOSurfaceGetWidth(surface)<256)return;
 static std::atomic<unsigned> binds{0},probes{0};
 static const unsigned at[]={1,3,10,30,60,100,200,300,450,600,900,1200,1800,2400,3600,4800};
 const unsigned bind=binds.fetch_add(1)+1;
 bool due=false;for(unsigned a:at)due=due || a==bind;
 if(!due)return;
 const unsigned probe=probes.fetch_add(1)+1;
 if(probe>16)return;
 static dispatch_queue_t lane=dispatch_queue_create("scion.ycbcr-probe",DISPATCH_QUEUE_SERIAL);
 char where[80];snprintf(where,sizeof where,"%s stage %u slot %lu",encoder,stage,(unsigned long)index);
 std::string at_=where;
 CFRetain(surface);
 dispatch_async(lane,^{
  @autoreleasepool {sweetYCbCrProbeRun(texture,surface,probe,bind,at_.c_str(),format);}
  CFRelease(surface);
 });
#else
 (void)texture;(void)surface;(void)encoder;(void)stage;(void)index;(void)format;
#endif
}
// The flattening (SWEET_MTL2_YCCFLAT=1, a test of the camera preview's display side): a render encoder's fragment binding of a converted
// two-plane YCbCr texture over a surface is given instead a BGRA8 texture of the same size holding what the texture reads (the kernel
// sweet_ycc_flatten). The copy is encoded in a command buffer of the caller's own queue, committed just before the caller's (so the queue
// orders it first and nothing waits on the caller's thread). If the window server's draw shows the picture then, its pipeline's sampling
// of the two-plane texture is what loses it. One flat texture per surface (16 at most), written again for every command buffer that binds it.
static bool sweetYCbCrFlatOn() {
 static int on=-1;
 if(on<0){const char *e=getenv("SWEET_MTL2_YCCFLAT");on=(e && e[0]=='1')?1:0;}
 return on==1;
}
#ifdef SWEET_YCC_PROBE
static std::mutex gFlatLock;static id<MTLComputePipelineState> gFlatPipeline;static NSMutableDictionary *gFlats;static bool gFlatFailed;
static char gFlatPendingKey;
#endif
static id<MTLTexture> sweetYCbCrFlat(id<MTLTexture> texture,IOSurfaceRef surface,uint64_t format,id commandBuffer) {
#ifdef SWEET_YCC_PROBE
 if(!sweetYCbCrFlatOn() || !gDeviceObject || !texture || !surface || !commandBuffer || format<500 || format>510)return nil;
 std::lock_guard<std::mutex> hold(gFlatLock);
 id<MTLDevice> device=(id<MTLDevice>)gDeviceObject;
 if(!gFlatPipeline && !gFlatFailed) {
  NSError *error=nil;
  dispatch_data_t data=dispatch_data_create(sweet_ycc_probe_metallib,sizeof sweet_ycc_probe_metallib,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
  id<MTLFunction> function=library?[library newFunctionWithName:@"sweet_ycc_flatten"]:nil;
  gFlatPipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
  gFlats=[NSMutableDictionary dictionary];gFlatFailed=!gFlatPipeline;
  char said[300];snprintf(said,sizeof said,"native-metal: ycbcr flat: %s%s",gFlatPipeline?"on":"not made: ",
                          gFlatPipeline?"":(error?error.localizedDescription.UTF8String:"no function"));
  sm2_logs(said);
 }
 if(gFlatFailed)return nil;
 NSNumber *key=@(IOSurfaceGetID(surface));
 id<MTLTexture> flat=gFlats[key];
 if(!flat || flat.width!=texture.width || flat.height!=texture.height) {
  if(gFlats.count>=16)[gFlats removeAllObjects];
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:texture.width height:texture.height mipmapped:NO];
  td.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;td.storageMode=MTLStorageModePrivate;
  flat=[device newTextureWithDescriptor:td];
  if(!flat)return nil;
  gFlats[key]=flat;
 }
 NSMutableArray *pending=objc_getAssociatedObject(commandBuffer,&gFlatPendingKey);
 if(!pending){pending=[NSMutableArray array];objc_setAssociatedObject(commandBuffer,&gFlatPendingKey,pending,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 for(NSArray *pair in pending)if(pair[1]==flat)return flat;
 [pending addObject:@[texture,flat]];
 return flat;
#else
 (void)texture;(void)surface;(void)format;(void)commandBuffer;return nil;
#endif
}
#ifdef SWEET_YCC_PROBE
// Wrapped at load: a command buffer's commit first commits, on its own queue, the copies of the flat textures it binds.
__attribute__((constructor)) static void sweetYCbCrFlatInstall() {
 Class cls=NSClassFromString(@"NBCommandBuffer");const SEL commit=@selector(commit);
 Method m=cls?class_getInstanceMethod(cls,commit):nullptr;
 if(!m)return;
 typedef void (*F)(id,SEL);const F old=(F)method_getImplementation(m);
 method_setImplementation(m,imp_implementationWithBlock(^(id self) {
  NSMutableArray *pending=objc_getAssociatedObject(self,&gFlatPendingKey);
  if(pending.count && gFlatPipeline) {
   NSArray *pairs=[pending copy];[pending removeAllObjects];
   id<MTLCommandBuffer> copies=[[(id<MTLCommandBuffer>)self commandQueue] commandBuffer];
   id<MTLComputeCommandEncoder> encoder=[copies computeCommandEncoder];
   [encoder setComputePipelineState:gFlatPipeline];
   for(NSArray *pair in pairs) {
    id<MTLTexture> picture=pair[0];
    [encoder setTexture:picture atIndex:0];[encoder setTexture:pair[1] atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((picture.width+15)/16,(picture.height+15)/16,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
   }
   [encoder endEncoding];[copies commit];
   static std::atomic<unsigned> said{0};
   if(said.fetch_add(1)<6){char line[160];snprintf(line,sizeof line,"native-metal: ycbcr flat: %lu copies committed before a command buffer",(unsigned long)pairs.count);sm2_logs(line);}
  }
  old(self,commit);
 }));
}
#endif
