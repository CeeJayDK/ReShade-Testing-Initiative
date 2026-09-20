import numpy as np, sys
from PIL import Image
def bilinear(img, x, y):
    # img HxWx3 float; x,y pixel-space sample positions (texel centers at i+0.5), clamp addressing
    H,W,_=img.shape
    fx=x-0.5; fy=y-0.5
    x0=np.floor(fx).astype(int); y0=np.floor(fy).astype(int)
    ax=(fx-x0)[...,None]; ay=(fy-y0)[...,None]
    def g(yy,xx): return img[np.clip(yy,0,H-1),np.clip(xx,0,W-1)]
    return (g(y0,x0)*(1-ax)*(1-ay)+g(y0,x0+1)*ax*(1-ay)+g(y0+1,x0)*(1-ax)*ay+g(y0+1,x0+1)*ax*ay)
def lumasharpen(img, strength=0.65, clamp=0.035, pattern=1, bias=1.0):
    H,W,_=img.shape
    yy,xx=np.mgrid[0:H,0:W].astype(np.float64); xx+=0.5; yy+=0.5
    coef=np.array([0.2126,0.7152,0.0722])*strength
    # note: texcoord y down; BUFFER_PIXEL_SIZE*(dx,dy) -> pixel offsets (dx,dy)
    if pattern==1:
        offs=[(0.5,-0.5),(-0.5,-0.5),(0.5,0.5),(-0.5,0.5)]; offs=[(a*bias,b*bias) for a,b in offs]
    elif pattern==2:
        offs=[(0.4,-1.2),(-1.2,-0.4),(1.2,0.4),(-0.4,1.2)]; offs=[(a*bias,b*bias) for a,b in offs]; coef=coef*0.51
    elif pattern==0:
        offs=[(bias/3,bias/3),(-bias/3,-bias/3)]; coef=coef*1.5
    blur=sum(bilinear(img,xx+dx,yy+dy) for dx,dy in offs)/len(offs)
    sharp=img-blur
    s=np.clip((sharp*coef).sum(2)*(0.5/clamp)+0.5,0,1)
    s=clamp*2*s-clamp
    return np.clip(img+s[...,None],0,1)
if __name__=='__main__':
    src,out=sys.argv[1],sys.argv[2]; kw=dict(a.split('=') for a in sys.argv[3:])
    img=np.asarray(Image.open(src).convert('RGB')).astype(np.float64)/255
    ref=lumasharpen(img, float(kw.get('sharp_strength',0.65)), float(kw.get('sharp_clamp',0.035)), int(kw.get('pattern',1)), float(kw.get('offset_bias',1.0)))
    got=np.asarray(Image.open(out).convert('RGB')).astype(np.float64)/255
    d=np.abs(ref-got)*255
    print('vs reference: mean %.3f  p99 %.2f  max %.1f  (8-bit levels)'%(d.mean(),np.percentile(d,99),d.max()))
    unproc=np.abs(img-got)*255; print("vs input:     mean %.3f  max %.1f"%(unproc.mean(),unproc.max()), flush=True)
