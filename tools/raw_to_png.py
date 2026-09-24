import struct,sys
from PIL import Image
def load(n,scale=1.5):
    d=open(n,'rb').read(); px=struct.unpack('<%dI'%(len(d)//4),d)
    im=Image.new('RGB',(240,160))
    im.putdata([(p&255,(p>>8)&255,(p>>16)&255) for p in px])   # R,G,B  (verified)
    return im.resize((int(240*scale),int(160*scale)),Image.NEAREST)
outs=[load(a) for a in sys.argv[1:-1]]
w=sum(i.width for i in outs); h=max(i.height for i in outs)
o=Image.new('RGB',(w,h)); x=0
for i in outs: o.paste(i,(x,0)); x+=i.width
o.save(sys.argv[-1]); print('saved',sys.argv[-1])
