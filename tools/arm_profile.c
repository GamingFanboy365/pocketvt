/* ARM PC profiler: steps the core and histograms PC by 64-byte bin.
 * Resolve bins against `arm-none-eabi-nm -n pocketvt.elf`.
 * Usage: prof <rom.gba> <warmup-frames> <steps>    (guide section 57a) */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#define HN (1<<20)
static uint32_t key[HN]; static unsigned cnt[HN];
static void bump(uint32_t k){
  uint32_t h=(k*2654435761u)&(HN-1);
  for(;;){ if(cnt[h]&&key[h]==k){cnt[h]++;return;}
           if(!cnt[h]){key[h]=k;cnt[h]=1;return;} h=(h+1)&(HN-1); }
}
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  int warm=atoi(argv[2]); long N=atol(argv[3]);
  for(int f=0;f<warm;f++){int k=(f>60&&f<70)?1:0;c->setKeys(c,k);c->runFrame(c);}
  c->setKeys(c,0);
  uint32_t pc=0; long taken=0; unsigned long reg[4]={0,0,0,0};
  for(long i=0;i<N;i++){
    c->step(c);
    if(!c->readRegister(c,"pc",&pc)) break;
    bump(pc & ~0x3F); taken++;
    unsigned r=pc>>24;
    if(r==2)reg[0]++; else if(r==3)reg[1]++; else if(r==8||r==9)reg[2]++; else reg[3]++;
  }
  printf("sampled %ld  EWRAM=%.1f%% IWRAM=%.1f%% ROM=%.1f%% other=%.1f%%\n",
    taken,100.0*reg[0]/taken,100.0*reg[1]/taken,100.0*reg[2]/taken,100.0*reg[3]/taken);
  for(int t=0;t<400;t++){
    int b=-1; for(int i=0;i<HN;i++) if(cnt[i]&&(b<0||cnt[i]>cnt[b])) b=i;
    if(b<0)break;
    printf("%08x %7.3f%%\n",key[b],100.0*cnt[b]/taken); cnt[b]=0;
  }
  return 0;
}
