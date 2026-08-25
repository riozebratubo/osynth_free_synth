"""Three candidate filterbanks for the ANR residual form y = x + sum (g_k-1) b_k.
Only a bank whose bands sum to x can honour `fx.anr.floor` at all."""
import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *

def bank_current(x, bands, low, high):
    coefs,modes=anr_bank(bands,low,high)
    return np.stack([svf_fast(coefs[k],modes[k],x) for k in range(bands)])

def cutoffs(bands, low, high):
    if high < low*4.0: high = low*4.0
    ratio=(high/low)**(1.0/(bands-1))
    # N bands need N-1 internal crossovers; place them geometrically
    return [low*ratio**i for i in range(bands-1)], ratio

def bank_diff(x, bands, low, high, k=1.41421356):
    """B: band_k = LP_{k+1} - LP_k, telescoping. Sums to x by identity."""
    fc,_=cutoffs(bands,low,high)
    L=[svf_fast(svf_coef_k(f,k),'Lp',x) for f in fc]
    out=[L[0]]
    for i in range(1,len(L)): out.append(L[i]-L[i-1])
    out.append(x-L[-1])
    return np.stack(out)

def bank_cascade(x, bands, low, high, k=1.41421356):
    """D: successive complementary splits of the running highpass residual.
    band_k = LP_k(res_{k-1}); res_k = res_{k-1} - band_k. Sums to x by identity."""
    fc,_=cutoffs(bands,low,high)
    res=x; out=[]
    for f in fc:
        lo=svf_fast(svf_coef_k(f,k),'Lp',res)
        out.append(lo); res=res-lo
    out.append(res)
    return np.stack(out)

if __name__=='__main__':
    N=16384; imp=np.zeros(N); imp[0]=1.0
    from numpy.fft import rfft, rfftfreq
    f=rfftfreq(N,1/SR)
    probe=[100,200,500,1000,2000,4000,8000,12000]
    for name,fn in [("current (Lp/BpN/Hp)",bank_current),("B: difference",bank_diff),
                    ("D: cascade",bank_cascade)]:
        Bk=fn(imp,12,120.,9000.)
        S=Bk.sum(axis=0)
        err=20*np.log10(max(np.sqrt(np.mean((S-imp)**2))/np.sqrt(np.mean(imp**2)),1e-16))
        print(f"\n=== {name} ===")
        print(f"  reconstruction error ||sum(b)-x|| = {err:.1f} dB")
        H=[20*np.log10(np.maximum(np.abs(rfft(Bk[k])),1e-9)) for k in range(12)]
        print("  per-band peak gain dB:", np.round([h.max() for h in H],1))
        # notch depth when one band is driven to -20 dB
        print("  notch depth achievable at each band's own peak, driving that band to -20 dB:")
        d=[]
        for k in range(12):
            g=np.ones(12); g[k]=0.1
            Y=rfft(imp + sum((g[j]-1)*Bk[j] for j in range(12)))
            i=np.argmax(H[k])
            d.append(20*np.log10(max(abs(Y[i]),1e-12)))
        print("   ", np.round(d,1))
