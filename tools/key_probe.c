#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
static void hold(struct mCore*c,int k,int n){for(int i=0;i<n;i++){c->setKeys(c,k);c->runFrame(c);}}
static unsigned nthash(struct mCore*c){unsigned h=0;for(int i=0;i<960;i++)h=h*31u+c->busRead8(c,0x0203e18c+i);return h;}
int main(int argc,char**argv){
  const char*nm[8]={"A(0x01)","B(0x02)","Sel(0x04)","Sta(0x08)","Rt(0x10)","Lf(0x20)","Up(0x40)","Dn(0x80)"};
  for(int b=0;b<8;b++){
    struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
    mCoreLoadFile(c,argv[1]);
    unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
    uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
    hold(c,0,200); hold(c,1,8); hold(c,0,40);      /* -> category menu */
    unsigned before=nthash(c);
    hold(c,1<<b,8); hold(c,0,30);
    unsigned after=nthash(c);
    printf("  %-10s category-menu hash %08x -> %08x  %s\n",nm[b],before,after,
      before!=after?"*** CHANGED ***":"");
    free(v);
  }
  return 0;
}
