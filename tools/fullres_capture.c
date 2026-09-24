#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* fullres <rom> <out-prefix>
   Captures the VG category menu twice:
     - normal SCALED output + its per-line NES-row table
     - the same screen in UNSCALED mode at several windowtop values, so the
       full-vertical-resolution NES frame can be reassembled offline.
   emuflags / windowtop are poked directly; the display path re-reads both
   every vblank, so no rebuild is needed.                                   */
#define GB        0x0300729C
#define EMUFLAGS  (GB+0x50c)
#define WINDOWTOP (GB+0x477)
#define DMA0PTR   0x0300795c
static void hold(struct mCore*c,int k,int n){for(int i=0;i<n;i++){c->setKeys(c,k);c->runFrame(c);}}
int main(int argc,char**argv){
  struct mCore*c=GBACoreCreate();c->init(c);mCoreInitConfig(c,NULL);
  mCoreLoadFile(c,argv[1]);
  unsigned w,h;c->desiredVideoDimensions(c,&w,&h);
  uint32_t*v=malloc(w*h*4);c->setVideoBuffer(c,v,w);c->reset(c);
  hold(c,0,180); hold(c,1,8); hold(c,0,112);          /* -> category menu */
  char p[256];
  /* 1. scaled reference output + geometry */
  snprintf(p,sizeof p,"%s_scaled.raw",argv[2]);
  FILE*f=fopen(p,"wb");fwrite(v,4,38400,f);fclose(f);
  unsigned dp=c->busRead32(c,DMA0PTR);
  snprintf(p,sizeof p,"%s_geom.txt",argv[2]);f=fopen(p,"w");
  for(int y=0;y<160;y++){uint32_t e=c->busRead32(c,dp+y*4);
    fprintf(f,"%d\n",y+(int)(short)(e>>16));}
  fclose(f);
  uint32_t ef=c->busRead32(c,EMUFLAGS);
  printf("emuflags=%08x windowtop=%u\n",ef,c->busRead8(c,WINDOWTOP));
  /* 2. unscaled strips */
  c->busWrite32(c,EMUFLAGS,ef&~0x0000FF00);           /* scaling mode -> 0 */
  int tops[3]={16,96,176};
  for(int i=0;i<3;i++){
    c->busWrite8(c,WINDOWTOP,tops[i]);
    hold(c,0,3);
    snprintf(p,sizeof p,"%s_full%d.raw",argv[2],tops[i]);
    f=fopen(p,"wb");fwrite(v,4,38400,f);fclose(f);
    unsigned d2=c->busRead32(c,DMA0PTR);
    printf("  windowtop=%3d -> first line NES row %d, last %d\n",tops[i],
      0+(int)(short)(c->busRead32(c,d2)>>16),
      159+(int)(short)(c->busRead32(c,d2+159*4)>>16));
  }
  return 0;
}
