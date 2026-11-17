// SPDX-License-Identifier: MIT
// The pass log (SWEET_MTL2_PASSLOG=S in /var/local/mtl2-env; off by default, NOT run on a phone yet): what a frame asks of the GPU,
// one console line per encoder, for a few command buffers at a time while the process is animating.
//
// A window opens at a command buffer's creation when S seconds have passed since the last one and the process committed at least
// SWEET_MTL2_PASSLOG_RATE command buffers (default 6) in the last 250 ms, i.e. while something moves; it covers the next
// SWEET_MTL2_PASSLOG_CBS command buffers (default 4). At most SWEET_MTL2_PASSLOG_LINES lines a process (default 3000).
// Lines (MTL2[pid name]: PL ...), numbers as Metal gives them (pixel formats are MTLPixelFormat values):
//   PL w<window> c<cb> e<n> R <rt w>x<h> s<samples> | c<i> <w>x<h> f<fmt> <flags> L<load>S<store> ... | dr<draws> vx<vertices> sc<kpx> |
//      p <fragment function>*<draws> ... | t <w>x<h> f<fmt> m<mips> <flags>*<draws> ... | <us>
//   PL w c e C | ds<dispatches> th<k threads> | p <kernel>*<dispatches> ... | t ... | us
//   PL w c e B | o <operation>*<count> ... | t ... | us
//   PL w c commit e<encoders> enc<us from creation to commit>   and   PL w c done gpu<us from commit to completion>
// Texture flags: s IOSurface, v view, b over a buffer, P private, S shared, M managed, m memoryless, r render target, w shader write.
// Load/store: L0 dontcare 1 load 2 clear; S0 dontcare 1 store 2 resolve 3 store+resolve 4 unknown.
// The scissor column (sc) sums, over the draws, the scissor's area (or the target's when none is set), in thousands of pixels: an upper
// bound of the pixels drawn, i.e. of the overdraw.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void sm2_logs(const char *);
extern "C" uint64_t native_bridge_metal_recent_commits(uint64_t windowUs);

struct NBPassTex {const void *key;uint32_t w,h,fmt,mips,draws;char flags[8];};
struct NBPassName {char name[48];uint32_t count;};
struct NBPassLog {
 uint32_t window,cb,enc;char kind;               // kind R render, C compute, B blit
 std::string head;
 uint32_t draws,dispatches;uint64_t vertices,threads,scissorPx,targetPx,currentScissor;bool scissorSet;
 int current;NBPassName names[10];unsigned nnames,namesOver;
 NBPassTex tex[12];unsigned ntex,texOver;
 const void *bound[32];                          // the texture at each index of the logged stage (fragment, or compute), now
 uint64_t began;
};

static int gPassLogEvery=-1;   // seconds between windows; 0 off
static unsigned gPassLogRate=6,gPassLogCbs=4,gPassLogLines=3000;
static std::atomic<uint64_t> gPassLogLast{0};
static std::atomic<unsigned> gPassLogLeft{0},gPassLogWindow{0},gPassLogPrinted{0};
static std::atomic<uint32_t> gPassLogCb{0};

static uint64_t passLogNowUs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000;}
static bool passLogOn() {
 if(gPassLogEvery<0) {
  const char *e=getenv("SWEET_MTL2_PASSLOG");int v=e?atoi(e):0;gPassLogEvery=v>0?(v>600?600:v):0;
  if((e=getenv("SWEET_MTL2_PASSLOG_RATE")) && atoi(e)>0)gPassLogRate=(unsigned)atoi(e);
  if((e=getenv("SWEET_MTL2_PASSLOG_CBS")) && atoi(e)>0)gPassLogCbs=(unsigned)atoi(e)>32?32:(unsigned)atoi(e);
  if((e=getenv("SWEET_MTL2_PASSLOG_LINES")) && atoi(e)>0)gPassLogLines=(unsigned)atoi(e);
  if(gPassLogEvery){char t[160];snprintf(t,sizeof t,"PL on: a window every %d s at %u commits in 250 ms, %u command buffers each, %u lines at most",
   gPassLogEvery,gPassLogRate,gPassLogCbs,gPassLogLines);sm2_logs(t);}
 }
 return gPassLogEvery>0;
}
static void passLogSay(const std::string &line) {
 if(gPassLogPrinted.fetch_add(1)>=gPassLogLines)return;
 sm2_logs(line.c_str());
}
// At a command buffer's creation: its number in the open window (window<<16 | cb, from 1), or 0 when it is not logged.
static uint32_t passLogCommandBuffer() {
 if(!passLogOn() || gPassLogPrinted.load()>=gPassLogLines)return 0;
 unsigned left=gPassLogLeft.load();
 while(left) {
  if(gPassLogLeft.compare_exchange_weak(left,left-1))return (gPassLogWindow.load()<<16)|(gPassLogCbs-left+1);
 }
 const uint64_t now=passLogNowUs(),last=gPassLogLast.load();
 if(last && now-last<(uint64_t)gPassLogEvery*1000000)return 0;
 if(native_bridge_metal_recent_commits(250000)<gPassLogRate)return 0;
 uint64_t expected=last;
 if(!gPassLogLast.compare_exchange_strong(expected,now))return 0;
 const unsigned window=gPassLogWindow.fetch_add(1)+1;
 gPassLogLeft.store(gPassLogCbs-1);
 return (window<<16)|1;
}
static NBPassLog *passLogNew(uint32_t id,uint32_t enc,char kind) {
 if(!id)return nullptr;
 NBPassLog *l=new NBPassLog();l->window=id>>16;l->cb=id&0xffff;l->enc=enc;l->kind=kind;l->current=-1;l->began=passLogNowUs();
 return l;
}
static void passLogName(NBPassLog *l,const char *name) {
 if(!l)return;
 if(!name || !*name)name="?";
 for(unsigned i=0;i<l->nnames;i++)if(!strncmp(l->names[i].name,name,sizeof l->names[i].name-1)){l->current=(int)i;return;}
 if(l->nnames==sizeof l->names/sizeof l->names[0]){l->namesOver++;l->current=-1;return;}
 strlcpy(l->names[l->nnames].name,name,sizeof l->names[0].name);l->current=(int)l->nnames++;
}
static void passLogCount(NBPassLog *l) {if(l && l->current>=0)l->names[l->current].count++;}
static void passLogTexture(NBPassLog *l,unsigned index,const void *key,uint32_t w,uint32_t h,uint32_t fmt,uint32_t mips,const char *flags) {
 if(!l)return;
 if(index<32)l->bound[index]=key;
 if(!key)return;
 for(unsigned i=0;i<l->ntex;i++)if(l->tex[i].key==key)return;
 if(l->ntex==sizeof l->tex/sizeof l->tex[0]){l->texOver++;return;}
 NBPassTex &t=l->tex[l->ntex++];t={key,w,h,fmt,mips,0,{0}};strlcpy(t.flags,flags,sizeof t.flags);
}
// A draw (or dispatch) counts once for each texture bound at it (a texture at two indices counts twice).
static void passLogBound(NBPassLog *l) {
 for(unsigned k=0;k<32;k++)if(l->bound[k])for(unsigned i=0;i<l->ntex;i++)if(l->tex[i].key==l->bound[k]){l->tex[i].draws++;break;}
}
static void passLogDraw(NBPassLog *l,uint64_t vertices) {
 if(!l)return;
 l->draws++;l->vertices+=vertices;l->scissorPx+=l->scissorSet?l->currentScissor:l->targetPx;passLogCount(l);passLogBound(l);
}
static void passLogDispatch(NBPassLog *l,uint64_t threads) {
 if(!l)return;
 l->dispatches++;l->threads+=threads;passLogCount(l);passLogBound(l);
}
static void passLogEnd(NBPassLog *l) {
 if(!l)return;
 char t[200];std::string s;
 snprintf(t,sizeof t,"PL w%u c%u e%u %c",l->window,l->cb,l->enc,l->kind);s=t;
 if(!l->head.empty())s+=" "+l->head;
 if(l->kind=='R')snprintf(t,sizeof t," | dr%u vx%llu sc%lluk |",l->draws,(unsigned long long)l->vertices,(unsigned long long)(l->scissorPx/1000));
 else if(l->kind=='C')snprintf(t,sizeof t," | ds%u th%lluk |",l->dispatches,(unsigned long long)(l->threads/1000));
 else snprintf(t,sizeof t," |");
 s+=t;s+=l->kind=='B'?" o":" p";
 for(unsigned i=0;i<l->nnames;i++){snprintf(t,sizeof t," %s*%u",l->names[i].name,l->names[i].count);s+=t;}
 if(l->namesOver){snprintf(t,sizeof t," +%u",l->namesOver);s+=t;}
 s+=" | t";
 for(unsigned i=0;i<l->ntex;i++){const NBPassTex &x=l->tex[i];snprintf(t,sizeof t," %ux%u f%u m%u %s*%u",x.w,x.h,x.fmt,x.mips,x.flags,x.draws);s+=t;}
 if(l->texOver){snprintf(t,sizeof t," +%u",l->texOver);s+=t;}
 snprintf(t,sizeof t," | %lluus",(unsigned long long)(passLogNowUs()-l->began));s+=t;
 passLogSay(s);
 delete l;
}
