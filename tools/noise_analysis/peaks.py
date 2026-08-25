import numpy as np
from numpy.fft import rfft, rfftfreq
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0]; sr=48000
nfft=32768; hop=nfft//2; win=np.hanning(nfft)
cg=win.sum()/nfft
acc=np.zeros(nfft//2+1); c=0
for s in range(0,len(x)-nfft,hop):
    acc+=np.abs(rfft(x[s:s+nfft]*win))**2; c+=1
P=acc/c
# amplitude spectrum in dBFS (coherent gain corrected, peak-normalized per bin)
amp=20*np.log10(np.maximum(np.sqrt(P)/(nfft*cg)*2,1e-12))
f=rfftfreq(nfft,1/sr)
# smooth baseline via median filter over +-40 bins
from scipy.ndimage import median_filter
base=median_filter(amp,size=81)
prom=amp-base
order=np.argsort(prom)[::-1]
seen=[]; out=[]
for i in order:
    if prom[i]<6: break
    if any(abs(f[i]-g)<15 for g in seen): continue
    seen.append(f[i]); out.append((f[i],amp[i],prom[i]))
    if len(out)>=40: break
out.sort()
print("Tonal peaks (freq Hz, level dBFS, prominence dB over local floor):")
for fr,l,p in out: print(f"  {fr:9.1f}  {l:7.1f}  +{p:5.1f}")
print("\nOctave-band energy (dBFS in band):")
edges=[20,40,80,160,315,630,1250,2500,5000,10000,20000,24000]
tot=0
for lo,hi in zip(edges[:-1],edges[1:]):
    m=(f>=lo)&(f<hi)
    # PSD sum -> band power
    bp=np.sum(P[m])/( (nfft*cg)**2 )*2
    tot+=bp
    print(f"  {lo:>6}-{hi:<6}: {10*np.log10(max(bp,1e-20)):7.1f} dBFS")
print(f"  TOTAL          : {10*np.log10(max(tot,1e-20)):7.1f} dBFS")
np.save('tools/noise_analysis/amp.npy',np.vstack([f,amp,base]))
