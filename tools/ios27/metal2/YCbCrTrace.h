// SPDX-License-Identifier: MIT
// The camera's preview (docs/ios27/CAMERA.md, "Dylib -30 on the phone, and where the picture is lost"), part of
// NativeBridgeMetalFrontend.mm (included there; a file of its own for the size rule): what a two-plane YCbCr texture over an
// IOSurface holds as this process's processor reads the surface (a 16 x 16 grid of 8-bit luma, an 8 x 8 grid of chroma, after
// cleaning and invalidating the surface's lines), said when the texture is made and when an encoder first binds a texture over
// each surface: 12 lines of each a process, always on. A picture is in the surface where the luma's grid is lit and Cb Cr are not
// 0; a black one reads Y 0 or 16 and Cb Cr 128. Reading only: nothing is written and no surface is held. NOT RUN ON A PHONE.
static bool sweetYCbCrFormat(uint64_t f) {return (f>=500 && f<=510) || (f>=520 && f<=530) || (f>=540 && f<=545);}
static void sweetYCbCrSay(const char *what,uint64_t format,IOSurfaceRef surface,unsigned long plane) {
 char held[160]="not read (not two planes of 8 bits)";
 const OSType os=IOSurfaceGetPixelFormat(surface);
 if(IOSurfaceGetPlaneCount(surface)==2 && IOSurfaceGetBytesPerElementOfPlane(surface,0)==1 && IOSurfaceGetBytesPerElementOfPlane(surface,1)==2 &&
    IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr)==0) {
  const uint8_t *l=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,0),*c=(const uint8_t *)IOSurfaceGetBaseAddressOfPlane(surface,1);
  const size_t w=IOSurfaceGetWidthOfPlane(surface,0),h=IOSurfaceGetHeightOfPlane(surface,0),lr=IOSurfaceGetBytesPerRowOfPlane(surface,0);
  const size_t cw=IOSurfaceGetWidthOfPlane(surface,1),ch=IOSurfaceGetHeightOfPlane(surface,1),cr=IOSurfaceGetBytesPerRowOfPlane(surface,1);
  if(l && c && w && h && cw && ch) {
   sys_dcache_flush((void *)l,lr*h);sys_dcache_flush((void *)c,cr*ch);
   unsigned long sum=0,cbs=0,crs=0;unsigned least=255,most=0,lit=0;
   for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
    const unsigned v=l[(h*(2*y+1)/32)*lr+w*(2*x+1)/32];sum+=v;least=v<least?v:least;most=v>most?v:most;lit+=v>24;
   }
   for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
    const uint8_t *p=c+(ch*(2*y+1)/16)*cr+(cw*(2*x+1)/16)*2;cbs+=p[0];crs+=p[1];
   }
   snprintf(held,sizeof held,"Y mean %lu least %u most %u, %u of 256 above black; Cb %lu Cr %lu",sum/256,least,most,lit,cbs/64,crs/64);
  }
  IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr);
 }
 char line[400];
 snprintf(line,sizeof line,"native-metal: ycbcr %s: fmt %llu over surface %u '%c%c%c%c' %zux%zu plane %lu: %s",what,(unsigned long long)format,
          IOSurfaceGetID(surface),(char)(os>>24),(char)(os>>16),(char)(os>>8),(char)os,IOSurfaceGetWidth(surface),IOSurfaceGetHeight(surface),plane,held);
 sm2_logs(line);
}
static void sweetYCbCrMade(uint64_t format,IOSurfaceRef surface,NSUInteger plane) {
 static std::atomic<unsigned> said{0};
 if(said.fetch_add(1)<12)sweetYCbCrSay("texture made",format,surface,(unsigned long)plane);
}
// The first binding of a YCbCr texture over each surface (12 surfaces a process): the camera's preview, drawn or not.
static void sweetYCbCrBound(const char *encoder,uint32_t stage,NSUInteger index,uint64_t format,IOSurfaceRef surface,NSUInteger plane) {
 static std::mutex lock;static uint32_t bound[12];static unsigned n;
 bool fresh=false;
 {
  std::lock_guard<std::mutex> hold(lock);const uint32_t id=IOSurfaceGetID(surface);bool seen=false;
  for(unsigned k=0;k<n;k++)seen=seen || bound[k]==id;
  if(!seen && n<12){bound[n++]=id;fresh=true;}
 }
 if(!fresh)return;
 char what[96];snprintf(what,sizeof what,"texture bound (%s, stage %u, slot %lu)",encoder,stage,(unsigned long)index);
 sweetYCbCrSay(what,format,surface,(unsigned long)plane);
}
