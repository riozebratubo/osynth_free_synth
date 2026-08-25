import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0]
bands=12; low,high=120.0,9000.0
coefs,modes=anr_bank(bands,low,high)
ratio=(high/low)**(1.0/(bands-1)); f=[low*ratio**i for i in range(bands)]
n=len(x)//BLK*BLK
B=np.stack([svf_fast(coefs[k],modes[k],x[:n]) for k in range(bands)])
MAG=np.mean(np.abs(B.reshape(bands,-1,BLK)),axis=2)
print("band   freq   mean|b|   min over 8s win   mean/min   what the estimator calls 'noise'")
blocks_per_win=int(8.0/(BLK/SR))
for k in range(bands):
    m=MAG[k]
    mean=m.mean()
    wmin=np.min(m[:blocks_per_win]) if len(m)>blocks_per_win else m.min()
    p5=np.percentile(m,5); p1=np.percentile(m,1)
    est=wmin*1.5
    g=np.clip((mean-1.8*est)/mean,10**(-20/20),1.0)
    print(f"  {k:2d} {f[k]:7.0f}  {20*np.log10(mean):7.1f}  {20*np.log10(max(wmin,1e-12)):9.1f}  "
          f"{20*np.log10(mean/max(wmin,1e-12)):7.1f} dB   est {20*np.log10(max(est,1e-12)):6.1f} -> g {20*np.log10(g):6.1f} dB")
print("\nblock-magnitude fluctuation per band (how much a 1.33 ms window wobbles on *stationary* noise):")
for k in range(bands):
    m=MAG[k]; 
    print(f"  {k:2d} {f[k]:7.0f} Hz: p50 {20*np.log10(np.percentile(m,50)):6.1f}  p5 {20*np.log10(np.percentile(m,5)):6.1f}"
          f"  p0.1 {20*np.log10(np.percentile(m,0.1)):6.1f}  min {20*np.log10(m.min()):6.1f}   (spread p50-min {20*np.log10(np.percentile(m,50)/m.min()):5.1f} dB)")
