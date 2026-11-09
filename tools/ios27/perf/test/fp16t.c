/*
 * fp16t.c: FCCMP H under ten conditions and FABS/FNEG .4H/.8H, run natively on the phone and checked against C copies of the
 * emulator's fp16_cond_holds() and fp16_compare_nzcv() (xnu/files-12377/osfmk/arm64/sweet_a64e.c, which emulates them on a
 * Mongoose M3 under sweetspeed&2). On an A55 it tests the copies against the hardware; on an M3 with sweetspeed&2 the kernel's
 * emulation is what answers, so it tests that. A mismatch prints. Build for the phone and run it there:
 *   xcrun --sdk iphoneos clang -arch arm64 -O1 -o fp16t tools/ios27/perf/test/fp16t.c && codesign -f -s - fp16t
 * The run of 9 November 2026 (kernel without the gate, every form emulated): 21.6 million checks, no mismatch.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
static bool fp16_cond_holds(uint32_t cond, uint32_t cpsr){bool n=(cpsr>>31)&1,z=(cpsr>>30)&1,c=(cpsr>>29)&1,v=(cpsr>>28)&1,r;
switch(cond>>1){case 0:r=z;break;case 1:r=c;break;case 2:r=n;break;case 3:r=v;break;case 4:r=c&&!z;break;case 5:r=n==v;break;case 6:r=n==v&&!z;break;default:r=true;break;}
return ((cond&1)&&cond!=15)?!r:r;}
static uint32_t cmpz(uint16_t a,uint16_t b){bool an=(a&0x7c00)==0x7c00&&(a&0x3ff),bn=(b&0x7c00)==0x7c00&&(b&0x3ff);if(an||bn)return 3;
int ka=(a&0x8000)?-(int)(a&0x7fff):(int)(a&0x7fff);int kb=(b&0x8000)?-(int)(b&0x7fff):(int)(b&0x7fff);return ka==kb?6:ka<kb?8:2;}
#define T(cond,imm) static uint32_t nat_##cond##_##imm(uint16_t a,uint16_t b,uint32_t in){uint64_t out; __asm__ volatile( \
 "msr nzcv, %x3\n fmov h0, %w1\n fmov h1, %w2\n fccmp h0, h1, #" #imm ", " #cond "\n mrs %0, nzcv" : "=r"(out) : "r"(a),"r"(b),"r"((uint64_t)in) : "v0","v1","cc"); return (uint32_t)out;}
T(eq,0) T(ne,4) T(le,0) T(gt,9) T(lt,2) T(hi,7) T(ls,15) T(ge,1) T(mi,3) T(vs,5)
static const struct { const char*n; uint32_t c, imm; uint32_t (*f)(uint16_t,uint16_t,uint32_t);} tc[]={
 {"eq",0,0,nat_eq_0},{"ne",1,4,nat_ne_4},{"le",13,0,nat_le_0},{"gt",12,9,nat_gt_9},{"lt",11,2,nat_lt_2},{"hi",8,7,nat_hi_7},{"ls",9,15,nat_ls_15},{"ge",10,1,nat_ge_1},{"mi",4,3,nat_mi_3},{"vs",6,5,nat_vs_5}};
static uint16_t rh(void){ static const uint16_t sp[]={0,0x8000,0x3c00,0xbc00,0x7c00,0xfc00,0x7e00,0x7c01,0xfe00,0x0001,0x8001,0x03ff,0x7bff}; int r=rand(); return (r&3)==0? sp[(r>>2)%13] : (uint16_t)(rand()); }
int main(void){ long bad=0,n=0;
 for(int i=0;i<2000000;i++){ uint16_t a=rh(),b=rh(); uint32_t in=((uint32_t)rand()&0xf)<<28;
  for(unsigned k=0;k<sizeof tc/sizeof tc[0];k++){ uint32_t e = fp16_cond_holds(tc[k].c,in)? cmpz(a,b) : tc[k].imm; uint32_t g=tc[k].f(a,b,in)>>28; n++;
   if(e!=g){ if(bad++<10) printf("MISMATCH %s a=%04x b=%04x in=%x emu=%x nat=%x\n",tc[k].n,a,b,in>>28,e,g);} } }
 /* FABS/FNEG vector and FCSEL scalar */
 for(int i=0;i<200000;i++){ uint16_t v[8]; for(int j=0;j<8;j++) v[j]=rh(); uint16_t o1[8],o2[8];
  __asm__ volatile("ldr q0,[%0]\n fneg v1.8h, v0.8h\n str q1,[%1]\n fabs v2.4h, v0.4h\n str q2,[%2]" :: "r"(v),"r"(o1),"r"(o2) : "v0","v1","v2","memory");
  for(int j=0;j<8;j++){ n++; if(o1[j]!=(uint16_t)(v[j]^0x8000)) bad++; if(o2[j]!=(j<4?(uint16_t)(v[j]&0x7fff):0)) bad++; } }
 printf("%ld checks, %ld mismatches\n", n, bad); return bad!=0; }
