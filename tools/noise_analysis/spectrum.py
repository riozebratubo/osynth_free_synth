import numpy as np
a = np.load('tools/noise_analysis/data.npy')
x = a[:,0]; sr = 48000
# short-term RMS envelope, 20ms
w = int(0.020*sr)
n = len(x)//w
env = np.sqrt(np.mean(x[:n*w].reshape(n,w)**2, axis=1))
db = 20*np.log10(np.maximum(env,1e-9))
print("20ms-RMS envelope percentiles (dBFS):")
for p in [1,5,10,25,50,75,90,95,99,100]:
    print(f"  p{p:>3}: {np.percentile(db,p):7.1f}")
print("first 25 frames dB:", np.round(db[:25],1))
# Where is the quietest 10%? contiguous?
idx = np.argsort(db)[:max(1,n//10)]
print("quietest-10 pct frame indices span: %d..%d of %d"%(idx.min(), idx.max(), n))
# Welch spectrum over whole file and over quietest 20%
from numpy.fft import rfft, rfftfreq
def welch(sig, nfft=8192):
    hop=nfft//2; win=np.hanning(nfft); acc=np.zeros(nfft//2+1); c=0
    for s in range(0,len(sig)-nfft,hop):
        acc += np.abs(rfft(sig[s:s+nfft]*win))**2; c+=1
    return rfftfreq(nfft,1/sr), 10*np.log10(np.maximum(acc/max(c,1),1e-30))
f, P = welch(x)
# quiet-only spectrum: concatenate quietest frames
qi = np.sort(idx); quiet = np.concatenate([x[i*w:(i+1)*w] for i in qi])
fq, Pq = welch(quiet, 2048)
print("\nfull-file spectrum (dB rel, 1/3-oct-ish samples):")
for hz in [30,50,60,80,100,120,150,180,200,250,300,400,500,700,1000,1500,2000,3000,4000,6000,8000,10000,12000,16000,20000,23000]:
    i=np.argmin(np.abs(f-hz)); j=np.argmin(np.abs(fq-hz))
    print(f"  {hz:>6} Hz: full {P[i]:7.1f}   quiet {Pq[j]:7.1f}")
np.save('tools/noise_analysis/spec.npy', np.vstack([f,P]))
