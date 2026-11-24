// SPDX-License-Identifier: MIT-0
// The camera's preview (docs/ios27/CAMERA.md, "The display side"), part of NativeBridgeMetalFrontend.mm (included there; a file
// of its own for the size rule): the whole state of a draw that samples a converted two-plane YCbCr texture (formats 500 to 510)
// in its fragment function, said once for each pipeline (24 lines a process at most). Off unless SWEET_MTL2_DRAWNOTE=1
// (mtl2-env). Said: `native-metal: draw note N: PIPELINE | t<slot> <format> <w>x<h> [surface ID] ... | s<slot> SAMPLER ... |
// fb<slot> <bytes> B: FLOATS | vb<slot> +<offset>: FLOATS | vbytes<slot> ...`. A pipeline is its fragment function's name, its
// color attachments (format, blending on or off with the factors and operations, write mask) and its sample count; a sampler its
// filters (min, mag, mip), address modes (s, t), whether its coordinates are normalized, and its level clamps. Reading only.
static int gDrawNote=-1;
static bool sweetDrawNoteOn() {
 if(gDrawNote<0){const char *e=getenv("SWEET_MTL2_DRAWNOTE");gDrawNote=(e && e[0]=='1')?1:0;}
 return gDrawNote==1;
}
static char gDrawNoteKey;
static NSMutableDictionary *sweetDrawNotes(id encoder) {
 NSMutableDictionary *d=objc_getAssociatedObject(encoder,&gDrawNoteKey);
 if(!d){d=[NSMutableDictionary dictionary];objc_setAssociatedObject(encoder,&gDrawNoteKey,d,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
 return d;
}
static char gDrawNoteSummaryKey;
static void sweetDrawNoteSummary(id object,NSString *summary) {
 if(sweetDrawNoteOn() && object)objc_setAssociatedObject(object,&gDrawNoteSummaryKey,summary,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static NSString *sweetDrawNoteSummaryOf(id object) {return object?objc_getAssociatedObject(object,&gDrawNoteSummaryKey):nil;}
static NSString *sweetDrawNoteSampler(MTLSamplerDescriptor *d) {
 return [NSString stringWithFormat:@"min %lu mag %lu mip %lu addr %lu,%lu norm %d lod %.1f-%.1f aniso %lu cmp %lu",(unsigned long)d.minFilter,
  (unsigned long)d.magFilter,(unsigned long)d.mipFilter,(unsigned long)d.sAddressMode,(unsigned long)d.tAddressMode,(int)d.normalizedCoordinates,
  d.lodMinClamp,d.lodMaxClamp,(unsigned long)d.maxAnisotropy,(unsigned long)d.compareFunction];
}
static NSString *sweetDrawNotePipeline(MTLRenderPipelineDescriptor *d) {
 NSMutableString *s=[NSMutableString stringWithFormat:@"frag %@ vert %@ samples %lu",d.fragmentFunction.name,d.vertexFunction.name,(unsigned long)d.rasterSampleCount];
 for(unsigned i=0;i<8;i++) {
  MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[i];
  if(a.pixelFormat==MTLPixelFormatInvalid)continue;
  [s appendFormat:@" c%u f%lu mask %lu",i,(unsigned long)a.pixelFormat,(unsigned long)a.writeMask];
  if(a.blendingEnabled)[s appendFormat:@" blend rgb %lu*%lu op%lu a %lu*%lu op%lu",(unsigned long)a.sourceRGBBlendFactor,(unsigned long)a.destinationRGBBlendFactor,
   (unsigned long)a.rgbBlendOperation,(unsigned long)a.sourceAlphaBlendFactor,(unsigned long)a.destinationAlphaBlendFactor,(unsigned long)a.alphaBlendOperation];
  else [s appendString:@" noblend"];
 }
 return s;
}
static NSString *sweetDrawNoteFloats(const void *bytes,size_t length) {
 NSMutableString *s=[NSMutableString string];const float *f=(const float *)bytes;
 for(size_t k=0;k<length/4 && k<24;k++)[s appendFormat:@"%s%.4g",k?" ":"",f[k]];
 return s;
}
// From the encoders' binding calls: what each slot holds now (stage 1 vertex, 2 fragment).
static void sweetDrawNoteTexture(id encoder,uint32_t stage,NSUInteger index,id<MTLTexture> t) {
 if(!sweetDrawNoteOn() || stage!=2)return;
 NSString *key=[NSString stringWithFormat:@"t%lu",(unsigned long)index];
 if(!t){[sweetDrawNotes(encoder) removeObjectForKey:key];return;}
 IOSurfaceRef surface=t.iosurface;
 sweetDrawNotes(encoder)[key]=[NSString stringWithFormat:@"%lu %lux%lu%@",(unsigned long)t.pixelFormat,(unsigned long)t.width,(unsigned long)t.height,
  surface?[NSString stringWithFormat:@" surface %u",IOSurfaceGetID(surface)]:@""];
}
static void sweetDrawNoteSamplerBound(id encoder,uint32_t stage,NSUInteger index,id sampler) {
 if(!sweetDrawNoteOn() || stage!=2)return;
 NSString *key=[NSString stringWithFormat:@"s%lu",(unsigned long)index];
 NSString *v=sweetDrawNoteSummaryOf(sampler);
 if(v)sweetDrawNotes(encoder)[key]=v;else [sweetDrawNotes(encoder) removeObjectForKey:key];
}
static void sweetDrawNoteBuffer(id encoder,uint32_t stage,NSUInteger index,id<MTLBuffer> buffer,NSUInteger offset) {
 if(!sweetDrawNoteOn() || stage!=1)return;
 NSString *key=[NSString stringWithFormat:@"vb%lu",(unsigned long)index];
 if(buffer)sweetDrawNotes(encoder)[key]=@[buffer,@(offset)];else [sweetDrawNotes(encoder) removeObjectForKey:key];
}
static void sweetDrawNoteBufferOffset(id encoder,uint32_t stage,NSUInteger index,NSUInteger offset) {
 if(!sweetDrawNoteOn() || stage!=1)return;
 NSString *key=[NSString stringWithFormat:@"vb%lu",(unsigned long)index];
 NSArray *was=sweetDrawNotes(encoder)[key];
 if([was isKindOfClass:[NSArray class]] && was.count==2)sweetDrawNotes(encoder)[key]=@[was[0],@(offset)];
}
static void sweetDrawNoteBytes(id encoder,uint32_t stage,NSUInteger index,const void *bytes,NSUInteger length) {
 if(!sweetDrawNoteOn() || !bytes)return;
 NSString *key=[NSString stringWithFormat:@"%@%lu",stage==2?@"fb":@"vbytes",(unsigned long)index];
 sweetDrawNotes(encoder)[key]=[NSString stringWithFormat:@"%lu B: %@",(unsigned long)length,sweetDrawNoteFloats(bytes,length)];
}
static void sweetDrawNotePipelineBound(id encoder,id pipeline) {
 if(!sweetDrawNoteOn())return;
 NSString *v=sweetDrawNoteSummaryOf(pipeline);
 sweetDrawNotes(encoder)[@"p"]=v?v:@"(unknown pipeline)";
}
// At a draw: when a fragment slot holds a texture of formats 500 to 510, say the state (once a pipeline, 24 a process).
static void sweetDrawNoteDraw(id encoder,const char *how,NSUInteger count) {
 if(!sweetDrawNoteOn())return;
 NSMutableDictionary *d=objc_getAssociatedObject(encoder,&gDrawNoteKey);
 if(!d)return;
 bool ycc=false;
 for(NSString *key in d) {
  if(![key hasPrefix:@"t"])continue;
  const long f=[(NSString *)d[key] integerValue];ycc=ycc || (f>=500 && f<=510);
 }
 if(!ycc)return;
 static std::mutex lock;static NSMutableSet *said;static unsigned lines;
 NSString *pipeline=d[@"p"]?:@"(no pipeline)";
 {
  std::lock_guard<std::mutex> hold(lock);
  if(!said)said=[NSMutableSet set];
  if(lines>=24 || [said containsObject:pipeline])return;
  [said addObject:pipeline];lines++;
 }
 NSMutableString *s=[NSMutableString stringWithFormat:@"native-metal: draw note %u: %s %lu | %@ |",lines,how,(unsigned long)count,pipeline];
 for(NSString *key in [d.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
  if([key isEqualToString:@"p"])continue;
  id v=d[key];
  if([v isKindOfClass:[NSArray class]]) {
   id<MTLBuffer> b=((NSArray *)v)[0];const NSUInteger off=[((NSArray *)v)[1] unsignedIntegerValue];
   const uint8_t *c=(const uint8_t *)b.contents;
   if(c && off+16<=b.length)[s appendFormat:@" %@ +%lu: %@ |",key,(unsigned long)off,sweetDrawNoteFloats(c+off,MIN((NSUInteger)96,b.length-off))];
   else [s appendFormat:@" %@ +%lu: (no contents) |",key,(unsigned long)off];
  } else [s appendFormat:@" %@ %@ |",key,v];
 }
 const char *line=s.UTF8String;
 for(size_t at=0,n=strlen(line);at<n;at+=900) {   // the console takes long lines in pieces
  char piece[920];snprintf(piece,sizeof piece,"%s%.900s",at?"native-metal: draw note ... ":"",line+at);sm2_logs(piece);
 }
}
// The pipeline side, wrapped at load (the frontend's own methods, always; the wrappers do nothing more unless the note is on):
// a pipeline made keeps its summary, a pipeline set is noted on the encoder.
__attribute__((constructor)) static void sweetDrawNoteInstall() {
 Class methods=NSClassFromString(@"NBDeviceMethods"),encoder=NSClassFromString(@"NBRenderEncoder");
 const SEL make=@selector(newRenderPipelineStateWithDescriptor:options:reflection:error:),set=@selector(setRenderPipelineState:);
 if(Method m=methods?class_getInstanceMethod(methods,make):nullptr) {
  typedef id (*Make)(id,SEL,MTLRenderPipelineDescriptor *,MTLPipelineOption,MTLAutoreleasedRenderPipelineReflection *,NSError **);
  const Make old=(Make)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^id(id self,MTLRenderPipelineDescriptor *d,MTLPipelineOption o,MTLAutoreleasedRenderPipelineReflection *r,NSError **e) {
   id state=old(self,make,d,o,r,e);
   if(state && sweetDrawNoteOn())sweetDrawNoteSummary(state,sweetDrawNotePipeline(d));
   return state;
  }));
 }
Class base=NSClassFromString(@"NBEncoder");
 if(Method m=base?class_getInstanceMethod(base,@selector(sampler:index:stage:)):nullptr) {
  typedef void (*F)(id,SEL,id,NSUInteger,uint32_t);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,id x,NSUInteger i,uint32_t st){old(self,@selector(sampler:index:stage:),x,i,st);sweetDrawNoteSamplerBound(self,st,i,x);}));
 }
 if(Method m=base?class_getInstanceMethod(base,@selector(bytes:length:index:stage:)):nullptr) {
  typedef void (*F)(id,SEL,const void *,NSUInteger,NSUInteger,uint32_t);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,const void *p,NSUInteger n,NSUInteger i,uint32_t st){old(self,@selector(bytes:length:index:stage:),p,n,i,st);sweetDrawNoteBytes(self,st,i,p,n);}));
 }
 if(Method m=base?class_getInstanceMethod(base,@selector(buffer:offset:index:stage:)):nullptr) {
  typedef void (*F)(id,SEL,id,NSUInteger,NSUInteger,uint32_t);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,id b,NSUInteger o,NSUInteger i,uint32_t st){old(self,@selector(buffer:offset:index:stage:),b,o,i,st);sweetDrawNoteBuffer(self,st,i,b,o);}));
 }
 if(Method m=base?class_getInstanceMethod(base,@selector(offset:index:stage:)):nullptr) {
  typedef void (*F)(id,SEL,NSUInteger,NSUInteger,uint32_t);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,NSUInteger o,NSUInteger i,uint32_t st){old(self,@selector(offset:index:stage:),o,i,st);sweetDrawNoteBufferOffset(self,st,i,o);}));
 }
 const SEL draw=@selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:);
 if(Method m=encoder?class_getInstanceMethod(encoder,draw):nullptr) {
  typedef void (*F)(id,SEL,MTLPrimitiveType,NSUInteger,NSUInteger,NSUInteger,NSUInteger);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,MTLPrimitiveType t,NSUInteger s,NSUInteger n,NSUInteger i,NSUInteger b){sweetDrawNoteDraw(self,"draw",n);old(self,draw,t,s,n,i,b);}));
 }
 const SEL indexed=@selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:);
 if(Method m=encoder?class_getInstanceMethod(encoder,indexed):nullptr) {
  typedef void (*F)(id,SEL,MTLPrimitiveType,NSUInteger,MTLIndexType,id,NSUInteger,NSUInteger,NSInteger,NSUInteger);const F old=(F)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,MTLPrimitiveType t,NSUInteger n,MTLIndexType k,id b,NSUInteger o,NSUInteger i,NSInteger v,NSUInteger bi){
   sweetDrawNoteDraw(self,"indexed",n);old(self,indexed,t,n,k,b,o,i,v,bi);}));
 }
 if(Method m=encoder?class_getInstanceMethod(encoder,set):nullptr) {
  typedef void (*Set)(id,SEL,id);
  const Set old=(Set)method_getImplementation(m);
  method_setImplementation(m,imp_implementationWithBlock(^(id self,id state){old(self,set,state);sweetDrawNotePipelineBound(self,state);}));
 }
}
