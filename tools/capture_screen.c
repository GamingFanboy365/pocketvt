#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* cap <rom> <screen> <dma0buff-addr> <out-prefix>
   screens: sa150 sa700 vgtitle vgcat                                       */
static void hold(struct mCore*c,int k,int n){ for(int i=0;i<n;i++){c->setKeys(c,k);c->runFrame(c);} }
int main(int argc,char**argv){
  struct mCore* core=GBACoreCreate(); core->init(core); mCoreInitConfig(core,NULL);
  mCoreLoadFile(core,argv[1]);
  unsigned w,h; core->desiredVideoDimensions(core,&w,&h);
  uint32_t*v=malloc(w*h*4); core->setVideoBuffer(core,v,w); core->reset(core);
  const char*s=argv[2]; unsigned DP=strtoul(argv[3],0,16);
  if(!strcmp(s,"sa150")){ hold(core,0,150); }
  else if(!strcmp(s,"sa700")){ for(int f=0;f<700;f++){int k=0;if(f>=200&&f<=205)k=8;if(f>=400&&f<=620)k=0x40;if(f>=630&&f<=690)k=0x10;core->setKeys(core,k);core->runFrame(core);} }
  else if(!strcmp(s,"vgtitle")){ hold(core,0,300); }
  else if(!strcmp(s,"vgcat")){ hold(core,0,180); hold(core,1,8); hold(core,0,112); }
  else if(!strcmp(s,"vglist")){ hold(core,0,180); hold(core,1,8); hold(core,0,80);
                                hold(core,1,8); hold(core,0,90); }
  else { /* vggame: three A taps -> first game of category 0 */
         hold(core,0,200);
         for(int i=0;i<3;i++){ hold(core,1,8); hold(core,0,22); hold(core,0,80); }
         hold(core,0,520); }
  char p[256];
  snprintf(p,sizeof p,"%s_fb.raw",argv[4]); FILE*f=fopen(p,"wb"); fwrite(v,4,w*h,f); fclose(f);
  if(argc>5){ unsigned P=strtoul(argv[5],0,16);
    snprintf(p,sizeof p,"%s_pram.txt",argv[4]); FILE*q=fopen(p,"w");
    for(int i=0;i<128;i++) fprintf(q,"%d\n",core->busRead8(core,P+i)); fclose(q); }
  unsigned dma=core->busRead32(core,DP);
  snprintf(p,sizeof p,"%s_geom.txt",argv[4]); f=fopen(p,"w");
  for(int y=0;y<160;y++){ uint32_t e=core->busRead32(core,dma+y*4);
    fprintf(f,"%d %d\n",(int)(short)(e&0xFFFF),(int)(short)(e>>16)); }
  fclose(f);
  printf("%-8s $2010=%02x dma0buff=%08x\n",s,core->busRead8(core,0x02000b8c),dma);
  return 0;
}
