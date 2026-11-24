// SPDX-License-Identifier: MIT-0
// Framebuffer fetch through a memoryless color attachment (docs/ios27/CAMERA.md, "The display side"), part of
// NativeBridgeMetalFrontend.mm (included there; a file of its own for the size rule). Off unless SWEET_MTL2_FBPROBE=1
// (mtl2-env). Once per process (the window server only), 25 s after the device is made, on a queue and a thread of its own,
// a few passes of the shape of CoreAnimation's camera layer draw run on a 64 x 64 target (FetchProbe.metal): color 0 BGRA8
// cleared to magenta and stored, color 1 RGBA16Float and color 2 R32Float. Each case is said as
// `native-metal: fetch probe CASE: left B G R A, right B G R A (status S)`, the mean bytes of each half of color 0.
// A draw that wrote color 1 = (0.25, 0.5, 0.75, 1) and a draw that copied color 1 into color 0 leave B G R = 191 128 64.
//   mem    color 1 and 2 memoryless (load and store don't care), write1 then read1 over the whole target
//   priv   the same with color 1 a private texture (cleared, stored)
//   half   memoryless; write1 over the left half only (scissor), read1 over the whole target
//   clear  memoryless color 1 cleared to green (load clear), read1 only
//   self   color 0 alone: write0, then read0 (reads color 0, writes half of it back): 96 64 32
//   two    color 0 and a memoryless color 1 only (no color 2), write1 then read1
#if __has_include("FetchProbe.inc")
#include "FetchProbe.inc" // sweet_fetch_probe_metallib[]: FetchProbe.metal, built by the bundle's script
#define SWEET_FETCH_PROBE 1
#endif
static bool sweetFetchProbeOn() {
 static int on=-1;
 if(on<0){const char *e=getenv("SWEET_MTL2_FBPROBE");on=(e && e[0]=='1')?1:0;}
 return on==1;
}
#ifdef SWEET_FETCH_PROBE
static void sweetFetchProbeRun() {
 id<MTLDevice> device=(id<MTLDevice>)gDeviceObject;
 char said[400];
 NSError *error=nil;
 dispatch_data_t data=dispatch_data_create(sweet_fetch_probe_metallib,sizeof sweet_fetch_probe_metallib,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
 if(!library){snprintf(said,sizeof said,"native-metal: fetch probe: no library: %s",error?error.localizedDescription.UTF8String:"?");sm2_logs(said);return;}
 // A pipeline of the three formats (color 2 left out where `three` is false), writing only the attachments of `writes`.
 auto pipeline=[&](NSString *fragment,unsigned writes,bool three)->id<MTLRenderPipelineState> {
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
  pd.vertexFunction=[library newFunctionWithName:@"sweet_fetch_vert"];pd.fragmentFunction=[library newFunctionWithName:fragment];
  pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;pd.colorAttachments[1].pixelFormat=MTLPixelFormatRGBA16Float;
  if(three)pd.colorAttachments[2].pixelFormat=MTLPixelFormatR32Float;
  for(unsigned i=0;i<3;i++)pd.colorAttachments[i].writeMask=(writes>>i)&1?MTLColorWriteMaskAll:MTLColorWriteMaskNone;
  NSError *e=nil;
  id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:pd error:&e];
  if(!state){char line[300];snprintf(line,sizeof line,"native-metal: fetch probe: pipeline %s (%s) refused: %s",fragment.UTF8String,three?"three":"two",
                                    e?e.localizedDescription.UTF8String:"?");sm2_logs(line);}
  return state;
 };
 id<MTLRenderPipelineState> w1=pipeline(@"sweet_fetch_write1",2,true),r1=pipeline(@"sweet_fetch_read1",1,true);
 id<MTLRenderPipelineState> w0=pipeline(@"sweet_fetch_write0",1,true),r0=pipeline(@"sweet_fetch_read0",1,true);
 id<MTLRenderPipelineState> w1b=pipeline(@"sweet_fetch_write1",2,false),r1b=pipeline(@"sweet_fetch_read1",1,false);
 id<MTLCommandQueue> queue=[device newCommandQueue];
 if(!queue)return;
 auto texture=[&](MTLPixelFormat format,MTLStorageMode mode)->id<MTLTexture> {
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
  td.usage=MTLTextureUsageRenderTarget|(mode==MTLStorageModeMemoryless?0:MTLTextureUsageShaderRead);td.storageMode=mode;
  return [device newTextureWithDescriptor:td];
 };
 enum {Mem,Priv,Half,Clear,Self,Two};
 const char *names[]={"mem","priv","half","clear","self","two"};
 for(int c=Mem;c<=Two;c++) {
  id<MTLTexture> c0=texture(MTLPixelFormatBGRA8Unorm,MTLStorageModeShared);
  id<MTLTexture> c1=c==Self?nil:texture(MTLPixelFormatRGBA16Float,c==Priv?MTLStorageModePrivate:MTLStorageModeMemoryless);
  id<MTLTexture> c2=(c==Self || c==Two)?nil:texture(MTLPixelFormatR32Float,MTLStorageModeMemoryless);
  if(!c0 || (c!=Self && !c1)){snprintf(said,sizeof said,"native-metal: fetch probe %s: no texture",names[c]);sm2_logs(said);continue;}
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture=c0;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
  pass.colorAttachments[0].clearColor=MTLClearColorMake(1,0,1,1);pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  if(c1) {
   pass.colorAttachments[1].texture=c1;
   pass.colorAttachments[1].loadAction=c==Clear || c==Priv?MTLLoadActionClear:MTLLoadActionDontCare;
   pass.colorAttachments[1].clearColor=c==Clear?MTLClearColorMake(0,1,0,1):MTLClearColorMake(0,0,0,0);
   pass.colorAttachments[1].storeAction=c==Priv?MTLStoreActionStore:MTLStoreActionDontCare;
  }
  if(c2){pass.colorAttachments[2].texture=c2;pass.colorAttachments[2].loadAction=MTLLoadActionDontCare;pass.colorAttachments[2].storeAction=MTLStoreActionDontCare;}
  id<MTLCommandBuffer> commands=[queue commandBuffer];
  id<MTLRenderCommandEncoder> re=[commands renderCommandEncoderWithDescriptor:pass];
  auto draw=[&](id<MTLRenderPipelineState> state){if(state){[re setRenderPipelineState:state];[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}};
  switch(c) {
   case Mem:case Priv:draw(w1);draw(r1);break;
   case Half:{MTLScissorRect left={0,0,32,64},all={0,0,64,64};[re setScissorRect:left];draw(w1);[re setScissorRect:all];draw(r1);break;}
   case Clear:draw(r1);break;
   case Self:{
    // color 0 alone: pipelines of one attachment
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[library newFunctionWithName:@"sweet_fetch_vert"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    pd.fragmentFunction=[library newFunctionWithName:@"sweet_fetch_write0"];id<MTLRenderPipelineState> a=[device newRenderPipelineStateWithDescriptor:pd error:nil];
    pd.fragmentFunction=[library newFunctionWithName:@"sweet_fetch_read0"];id<MTLRenderPipelineState> b=[device newRenderPipelineStateWithDescriptor:pd error:nil];
    (void)w0;(void)r0;draw(a);draw(b);break;}
   case Two:draw(w1b);draw(r1b);break;
  }
  [re endEncoding];
  [commands commit];[commands waitUntilCompleted];
  uint8_t px[64*64*4]={};
  [c0 getBytes:px bytesPerRow:64*4 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
  unsigned long s[2][4]={};
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)for(unsigned k=0;k<4;k++)s[x>=32][k]+=px[(y*64+x)*4+k];
  snprintf(said,sizeof said,"native-metal: fetch probe %s: left %lu %lu %lu %lu, right %lu %lu %lu %lu (status %ld%s%s)",names[c],
           s[0][0]/2048,s[0][1]/2048,s[0][2]/2048,s[0][3]/2048,s[1][0]/2048,s[1][1]/2048,s[1][2]/2048,s[1][3]/2048,(long)commands.status,
           commands.error?": ":"",commands.error?commands.error.localizedDescription.UTF8String:"");
  sm2_logs(said);
 }
}
#endif
// From the device's making: in the window server only, start the probe 25 s later, off every caller's thread.
static void sweetFetchProbeStart() {
#ifdef SWEET_FETCH_PROBE
 if(!sweetFetchProbeOn() || strcmp(getprogname(),"backboardd"))return;
 static std::atomic<bool> started{false};
 if(started.exchange(true))return;
 static dispatch_queue_t lane=dispatch_queue_create("scion.fetch-probe",DISPATCH_QUEUE_SERIAL);
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,25*NSEC_PER_SEC),lane,^{@autoreleasepool{sweetFetchProbeRun();}});
#endif
}
// SWEET_MTL2_FBPROBE=2 (window server only), the main pass's color 1: while a converted two-plane YCbCr texture (formats 500 to 510, the
// camera preview) was bound in the last 2 s, a render pass whose color 1 is a memoryless RGBA16Float texture of color 0's size (at
// least 1000 high) gets instead a private texture of that size (one, kept), cleared at its load and stored, the shadow. Every 2 s from
// 4 s after the first such pass (20 probes at most), after its command buffer: a compute pass of the same queue reads an 8 x 8 grid of
// the shadow over the viewfinder's rows (178 to 1284 of 1702; the whole height of another target), and the processor reads the same
// grid of color 0's surface when the command buffer completes. Said: `native-metal: fetch shadow N: c1 R G B A, K of 64 lit` and
// `... c0 R G B A, K of 64 lit` (0 to 255; lit: luma above 24). With SWEET_MTL2_DRAWNOTE=2 the draws of probes 2, 5 and 8's passes
// are said too (DrawNote.h). Writes nothing the window server reads but the shadow. Run on phone 1 on 24 November (F3).
static std::atomic<uint64_t> gFetchYccUs{0};
static std::atomic<unsigned> gFetchShadowPasses{0},gFetchShadowProbes{0};
static std::mutex gFetchShadowLock;
static id<MTLTexture> gFetchShadow;
static id<MTLComputePipelineState> gFetchGrid;
static char gFetchProbeKey,gFetchEncoderKey;
// SWEET_MTL2_FBPROBE=3 (window server only): a draw of CoreAnimation's direct camera layer function (TimgA3Xhf_Ixrg..., the steady
// preview draw into color 0) asks, at most once a second and 30 times a process, that its command buffer's completion read the
// same 8 x 8 viewfinder grid of that pass's color 0 surface on the processor (`fetch shadow N: c0 ...`, N from 1001), and says the
// draw: `native-metal: camera draw N: PIPELINE | t3 ... | vb1 ...`.
static char gFetchC0Key;
static bool sweetFetchDrawProbeOn() {
 static int on=-1;
 if(on<0){const char *e=getenv("SWEET_MTL2_FBPROBE");on=(e && e[0]=='3' && !strcmp(getprogname(),"backboardd"))?1:0;}
 return on==1;
}
static bool sweetFetchShadowOn() {
 static int on=-1;
 if(on<0){const char *e=getenv("SWEET_MTL2_FBPROBE");on=(e && e[0]=='2' && !strcmp(getprogname(),"backboardd"))?1:0;}
 return on==1;
}
static void sweetFetchSawYcc(uint64_t format) {
 if(format>=500 && format<=510)gFetchYccUs.store(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000,std::memory_order_relaxed);
}
// The probe number of the shadow pass `encoder` draws (0 for every other encoder).
static unsigned sweetFetchEncoderPass(id encoder) {
 NSNumber *n=encoder?objc_getAssociatedObject(encoder,&gFetchEncoderKey):nil;return n?n.unsignedIntValue:0;
}
static void sweetFetchRect(id<MTLTexture> t,uint32_t r[4]) {
 const uint32_t w=(uint32_t)t.width,h=(uint32_t)t.height;
 r[0]=0;r[2]=w;
 if(h==1702){r[1]=178;r[3]=1106;}else{r[1]=0;r[3]=h;}
}
// From renderCommandEncoderWithDescriptor: the descriptor to use, and the probe number of a probed shadow pass (else 0).
static MTLRenderPassDescriptor *sweetFetchShadowPass(MTLRenderPassDescriptor *d,id commandBuffer,unsigned *number) {
 *number=0;
#ifdef SWEET_FETCH_PROBE
 if(!sweetFetchShadowOn() || !gDeviceObject)return d;
 id<MTLTexture> c1=d.colorAttachments[1].texture;
 id<MTLTexture> c0=d.colorAttachments[0].texture;
 if(!c1 || !c0 || c1.storageMode!=MTLStorageModeMemoryless || c1.pixelFormat!=MTLPixelFormatRGBA16Float || c1.width!=c0.width || c1.height!=c0.height || c1.height<1000)return d;
 const uint64_t seen=gFetchYccUs.load(std::memory_order_relaxed),now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;
 if(!seen || now-seen>2000000)return d;
 id<MTLTexture> shadow;
 {
  std::lock_guard<std::mutex> hold(gFetchShadowLock);
  if(!gFetchShadow || gFetchShadow.width!=c1.width || gFetchShadow.height!=c1.height) {
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:c1.width height:c1.height mipmapped:NO];
   td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;td.storageMode=MTLStorageModePrivate;
   gFetchShadow=[(id<MTLDevice>)gDeviceObject newTextureWithDescriptor:td];
   char said[160];snprintf(said,sizeof said,"native-metal: fetch shadow: %s %lux%lu",gFetchShadow?"made":"NOT made",(unsigned long)c1.width,(unsigned long)c1.height);sm2_logs(said);
  }
  if(!gFetchGrid) {
   dispatch_data_t data=dispatch_data_create(sweet_fetch_probe_metallib,sizeof sweet_fetch_probe_metallib,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
   id<MTLLibrary> library=[(id<MTLDevice>)gDeviceObject newLibraryWithData:data error:nil];
   id<MTLFunction> f=[library newFunctionWithName:@"sweet_fetch_grid"];
   gFetchGrid=f?[(id<MTLDevice>)gDeviceObject newComputePipelineStateWithFunction:f error:nil]:nil;
  }
  shadow=gFetchShadow;
 }
 if(!shadow)return d;
 MTLRenderPassDescriptor *copy=[d copy];
 copy.colorAttachments[1].texture=shadow;copy.colorAttachments[1].loadAction=MTLLoadActionClear;
 copy.colorAttachments[1].clearColor=MTLClearColorMake(0,0,0,0);copy.colorAttachments[1].storeAction=MTLStoreActionStore;
 // a probe every 2 s from 4 s after the first such pass (20 at most); its pass is the one the draw notes describe (probes 2, 5 and 8)
 static std::atomic<uint64_t> first{0},last{0};
 gFetchShadowPasses.fetch_add(1);
 uint64_t f=first.load();if(!f){first.compare_exchange_strong(f,now);f=first.load();}
 uint64_t l=last.load();
 if(now-f>=4000000 && now-l>=2000000 && last.compare_exchange_strong(l,now)) {
  const unsigned k=gFetchShadowProbes.fetch_add(1)+1;
  if(k<=20){*number=k;objc_setAssociatedObject(commandBuffer,&gFetchProbeKey,@[c0,@(k)],OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 }
 return copy;
#else
 (void)commandBuffer;return d;
#endif
}
#ifdef SWEET_FETCH_PROBE
static void sweetFetchSay(unsigned n,const char *what,const float *c,bool bytes) {
 double r=0,g=0,b=0,a=0;unsigned lit=0;
 for(unsigned k=0;k<64;k++){const float *p=c+4*k;r+=p[0];g+=p[1];b+=p[2];a+=p[3];lit+=(0.299*p[0]+0.587*p[1]+0.114*p[2])*(bytes?1.0:255.0)>24.0;}
 const double s=bytes?1.0/64:255.0/64;
 char said[200];snprintf(said,sizeof said,"native-metal: fetch shadow %u: %s %.0f %.0f %.0f %.0f, %u of 64 lit",n,what,r*s,g*s,b*s,a*s,lit);sm2_logs(said);
}
// Wrapped at load: a render encoder's making takes the shadow pass's descriptor, and its encoder is marked with the probe number;
// with SWEET_MTL2_FBPROBE=3 every encoder keeps its color 0 texture (for the camera draw probe in DrawNote.h).
__attribute__((constructor)) static void sweetFetchShadowEncoderInstall() {
 Class cls=NSClassFromString(@"NBCommandBuffer");const SEL make=@selector(renderCommandEncoderWithDescriptor:);
 Method m=cls?class_getInstanceMethod(cls,make):nullptr;
 if(!m)return;
 typedef id (*F)(id,SEL,MTLRenderPassDescriptor *);const F old=(F)method_getImplementation(m);
 method_setImplementation(m,imp_implementationWithBlock(^id(id self,MTLRenderPassDescriptor *d) {
  sweetFetchProbeStart();
  unsigned n=0;MTLRenderPassDescriptor *use=sweetFetchShadowPass(d,self,&n);
  id encoder=old(self,make,use);
  if(encoder && sweetFetchDrawProbeOn() && d.colorAttachments[0].texture)objc_setAssociatedObject(encoder,&gFetchC0Key,d.colorAttachments[0].texture,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  if(n && encoder)objc_setAssociatedObject(encoder,&gFetchEncoderKey,@(n),OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  return encoder;
 }));
}
// Wrapped at load: a command buffer with a probe request commits, then a probe of the shadow follows it on its queue.
__attribute__((constructor)) static void sweetFetchShadowInstall() {
 Class cls=NSClassFromString(@"NBCommandBuffer");const SEL commit=@selector(commit);
 Method m=cls?class_getInstanceMethod(cls,commit):nullptr;
 if(!m)return;
 typedef void (*F)(id,SEL);const F old=(F)method_getImplementation(m);
 method_setImplementation(m,imp_implementationWithBlock(^(id self) {
  NSArray *req=objc_getAssociatedObject(self,&gFetchProbeKey);
  if(!req){old(self,commit);return;}
  objc_setAssociatedObject(self,&gFetchProbeKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  id<MTLTexture> c0=req[0];const unsigned n=[req[1] unsignedIntValue];
  [(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer>) {
   IOSurfaceRef s=c0.iosurface;
   if(!s || IOSurfaceGetBytesPerElement(s)!=4 || IOSurfaceLock(s,kIOSurfaceLockReadOnly,nullptr)){sm2_logs("native-metal: fetch shadow: c0 not read");return;}
   const uint8_t *base=(const uint8_t *)IOSurfaceGetBaseAddress(s);const size_t bpr=IOSurfaceGetBytesPerRow(s);
   uint32_t r[4];sweetFetchRect(c0,r);float px[256];
   sys_dcache_flush((void *)base,bpr*IOSurfaceGetHeight(s));
   for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){const uint8_t *p=base+(size_t)(r[1]+r[3]*(2*y+1)/16)*bpr+(size_t)(r[0]+r[2]*(2*x+1)/16)*4;
    float *q=px+4*(y*8+x);q[0]=p[2];q[1]=p[1];q[2]=p[0];q[3]=p[3];}
   IOSurfaceUnlock(s,kIOSurfaceLockReadOnly,nullptr);
   sweetFetchSay(n,"c0 (surface, RGBA bytes)",px,true);
  }];
  old(self,commit);
  id<MTLTexture> shadow;{std::lock_guard<std::mutex> hold(gFetchShadowLock);shadow=gFetchShadow;}
  if(!shadow || !gFetchGrid)return;
  id<MTLBuffer> out=[(id<MTLDevice>)gDeviceObject newBufferWithLength:64*4*sizeof(float) options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> probe=[[(id<MTLCommandBuffer>)self commandQueue] commandBuffer];
  if(!out || !probe)return;
  uint32_t r[4];sweetFetchRect(shadow,r);
  id<MTLComputeCommandEncoder> e=[probe computeCommandEncoder];
  [e setComputePipelineState:gFetchGrid];[e setTexture:shadow atIndex:0];[e setBuffer:out offset:0 atIndex:0];[e setBytes:r length:sizeof r atIndex:1];
  [e dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[e endEncoding];
  [probe addCompletedHandler:^(id<MTLCommandBuffer> done) {
   if(done.status!=MTLCommandBufferStatusCompleted){char said[120];snprintf(said,sizeof said,"native-metal: fetch shadow %u: c1 probe status %ld",n,(long)done.status);sm2_logs(said);return;}
   sweetFetchSay(n,"c1 (shadow, RGBA)",(const float *)out.contents,false);
  }];
  [probe commit];
 }));
}
#endif
