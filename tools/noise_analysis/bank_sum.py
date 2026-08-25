import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0][:480000]
for bands,low,high in [(12,120.,9000.),(8,120.,9000.),(16,120.,9000.),(12,60.,12000.),(12,120.,9000.)]:
    coefs,modes=anr_bank(bands,low,high)
    S=sum(svf_fast(coefs[k],modes[k],x) for k in range(bands))
    r=x-S
    print(f"bands={bands:2d} low={low:6.0f} high={high:6.0f}: "
          f"sum-of-bands vs input -> residual {rms_db(r)-rms_db(x):6.1f} dB, "
          f"bank sum level {rms_db(S)-rms_db(x):+5.1f} dB")
    # deepest achievable cut with every band at gmin: y = x + (gmin-1)*S
    for fl in [-10.,-20.,-30.,-40.,-60.]:
        gm=10**(fl/20.)
        y=x+(gm-1.0)*S
        print(f"      all bands at floor {fl:5.0f} dB -> actual output {rms_db(y)-rms_db(x):6.1f} dB")
    break
print()
# frequency response of the bank sum
from numpy.fft import rfft, rfftfreq
imp=np.zeros(16384); imp[0]=1.0
for bands,low,high in [(12,120.,9000.),(8,120.,9000.),(16,120.,9000.)]:
    coefs,modes=anr_bank(bands,low,high)
    S=sum(svf_fast(coefs[k],modes[k],imp) for k in range(bands))
    H=rfft(S); f=rfftfreq(16384,1/SR); mag=20*np.log10(np.maximum(np.abs(H),1e-9))
    print(f"bank sum |H| dB, bands={bands} ({low:.0f}..{high:.0f}):")
    for hz in [50,100,200,500,1000,2000,4000,8000,12000,16000,20000]:
        i=np.argmin(np.abs(f-hz)); print(f"    {hz:>6} Hz: {mag[i]:6.2f}")
