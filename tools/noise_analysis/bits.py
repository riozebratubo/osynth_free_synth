import numpy as np, wave
w=wave.open('noise.wav','rb'); raw=w.readframes(w.getnframes())
s=np.frombuffer(raw,dtype='<i2').reshape(-1,2)
L=s[:,0].astype(np.int64)
print("int16 min/max:",L.min(),L.max())
print("unique values:",len(np.unique(L)))
for b in range(0,8):
    frac=np.mean((L>>b)&1)
    print(f"  bit{b} set fraction: {frac:.4f}")
# gcd of values -> detects left-shifted / decimated resolution
from math import gcd
g=0
for v in np.unique(L)[:5000]: g=gcd(g,int(abs(v)))
print("gcd of sample values:",g)
# alternating-sample (Nyquist) component
d=L[:-1]-L[1:]
print("mean of x[n]*(-1)^n :",np.mean(L*((-1)**np.arange(len(L)))))
# autocorrelation for periodicity
x=L.astype(np.float64); x-=x.mean()
n=1<<18
X=np.fft.rfft(x[:n]); ac=np.fft.irfft(np.abs(X)**2)[:5000]; ac/=ac[0]
top=np.argsort(ac[20:3000])[::-1][:15]+20
print("\ntop autocorrelation lags (samples, ms, r):")
for t in sorted(top): print(f"  lag {t:5d}  {t/48.0:7.3f} ms  r={ac[t]:.3f}")
