"""The guard the original comment worries about: a held pad on the bus looks
exactly like a fan. Shortening `adapt` makes it get learned sooner, so measure."""
import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
n=int(20.0*SR)//BLK*BLK; t=np.arange(n)/SR
pad=np.zeros(n)
for f in [110.,138.6,164.8,220.,277.2,329.6]:      # Am chord, 2 octaves
    for h in [1,2,3]:
        pad+=np.sin(2*np.pi*f*h*t+f)/ (h*h)
pad*=10**(-18/20)/np.sqrt(np.mean(pad**2))
pad[:int(0.05*SR)]*=np.linspace(0,1,int(0.05*SR))   # 50 ms fade-in
def survive(tag,**kw):
    y=anr_run_v4(pad,**kw)
    seg=lambda s,e: slice(int(s*SR),int(e*SR))
    print(f"  {tag:34s} " + "  ".join(
        f"{a}-{b}s {rms_db(y[seg(a,b)])-rms_db(pad[seg(a,b)]):+5.1f}dB"
        for a,b in [(1,2),(4,5),(9,10),(19,20)]))
print("how much of a SUSTAINED PAD the unit eats over 20 s (0 dB = untouched):")
survive("shipped (adapt 8s, old bank)",**{})
y=anr_run_fast(pad)
seg=lambda s,e: slice(int(s*SR),int(e*SR))
print("  shipped (anr_run_fast)             " + "  ".join(
    f"{a}-{b}s {rms_db(y[seg(a,b)])-rms_db(pad[seg(a,b)]):+5.1f}dB" for a,b in [(1,2),(4,5),(9,10),(19,20)]))
for ad in [2.0,3.0,4.0,8.0]: survive(f"fixed, adapt={ad}s",adapt_s=ad)
print("\nsame, but the pad arrives 5 s into a noisy room (the realistic case):")
a=np.load('tools/noise_analysis/data.npy'); nz=np.tile(a[:,0],3)[:n]
mix=nz.copy(); mix[int(5*SR):]+=pad[:n-int(5*SR)]
for ad in [3.0,8.0]:
    y=anr_run_v4(mix,adapt_s=ad)
    print(f"  adapt={ad}s: pad+noise level change  " + "  ".join(
        f"{x}-{x+1}s {rms_db(y[seg(x,x+1)])-rms_db(mix[seg(x,x+1)]):+5.1f}dB" for x in [2,6,10,15,19]))
