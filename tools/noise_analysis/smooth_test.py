import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0]
bands=12; low,high=120.0,9000.0
coefs,modes=anr_bank(bands,low,high); ratio=(high/low)**(1.0/(bands-1))
f=[low*ratio**i for i in range(bands)]
n=len(x)//BLK*BLK
B=np.stack([svf_fast(coefs[k],modes[k],x[:n]) for k in range(bands)])
MAG=np.mean(np.abs(B.reshape(bands,-1,BLK)),axis=2)
blk_s=BLK/SR
print("effect of smoothing the per-band magnitude before the minimum is taken")
print("(spread = median - min over the whole 10.5 s, on stationary noise;")
print(" a perfect estimator would have spread 0 and need no bias at all)\n")
print(" band   freq |  raw   tau=20ms  50ms  100ms  200ms  400ms")
for k in range(bands):
    row=[]
    for tau in [None,0.020,0.050,0.100,0.200,0.400]:
        m=MAG[k]
        if tau is not None:
            al=1.0-np.exp(-blk_s/tau); s=np.zeros_like(m); acc=m[0]
            for i in range(len(m)): acc+=al*(m[i]-acc); s[i]=acc
            m=s[int(1.0/blk_s):]   # drop 1 s of settling
        row.append(20*np.log10(np.percentile(m,50)/m.min()))
    print(f"  {k:2d} {f[k]:7.0f} | " + "  ".join(f"{v:5.1f}" for v in row))
