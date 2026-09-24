#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#define KA 0x01
#define KD 0x80
static unsigned nth(struct mCore*c){unsigned h=0;for(int i=0;i<960;i++)h=h*31u+c->busRead8(c,0x0203e18c+i);return h;}
static unsigned settle(struct mCore*c,int need,int cap){
  unsigned prev=nth(c); int run=0;
  for(int f=0;f<cap;f++){c->setKeys(c,0);c->runFrame(c);unsigned x=nth(c);
    run=(x==prev)?run+1:0; prev=x; if(run>=need)break;}
  return prev;
}
static unsigned press(struct mCore*c,int k){
  for(int i=0;i<8;i++){c->setKeys(c,k);c->runFrame(c);}
  return settle(c,20,240);
}
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  int cat=atoi(argv[2]), N=atoi(argv[3]);
  for(int f=0;f<200;f++){c->setKeys(c,0);c->runFrame(c);} settle(c,20,240);
  press(c,KA);
  for(int i=0;i<cat;i++) press(c,KD);
  unsigned seen[64]; int n=0;
  seen[n++]=press(c,KA);
  for(int i=1;i<N;i++){
    unsigned x=press(c,KD);
    for(int j=0;j<n;j++) if(seen[j]==x){ printf("category %d: entry %d repeats entry %d  => %d ENTRIES\n",cat,i,j,i-j); return 0; }
    seen[n++]=x;
  }
  printf("category %d: no repeat within %d\n",cat,N);
  return 0;
}
