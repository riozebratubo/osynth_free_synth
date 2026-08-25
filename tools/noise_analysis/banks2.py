import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
from numpy.fft import rfft, rfftfreq

def lp4(x,fc):
    """Linkwitz-Riley 4th order = two cascaded Butterworth 2-poles."""
    c=svf_coef_k(fc,1.41421356)
    return svf_fast(c,'Lp',svf_fast(c,'Lp',x))
def lp2(x,fc):
    return svf_fast(svf_coef_k(fc,1.41421356),'Lp',x)

def bank_diff(x,bands,low,high,order=4):
    ratio=(high/low)**(1.0/(bands-1))
    fc=[low*ratio**i for i in range(bands-1)]
    f=lp4 if order==4 else lp2
    L=[f(x,c) for c in fc]
    out=[L[0]]+[L[i]-L[i-1] for i in range(1,len(L))]+[x-L[-1]]
    return np.stack(out), fc

N=32768; imp=np.zeros(N); imp[0]=1.0; fr=rfftfreq(N,1/SR)
print(f"{'config':28s} {'recon':>8s} {'peak dB':>9s} {'adj leak':>9s} {'all->-20dB':>11s} {'all->-30dB':>11s}")
for order in [2,4]:
    for bands in [6,8,10,12,16]:
        Bk,fc=bank_diff(imp,bands,120.,9000.,order)
        S=Bk.sum(axis=0)
        rec=20*np.log10(max(np.sqrt(np.mean((S-imp)**2)),1e-16))
        H=np.array([np.abs(rfft(Bk[k])) for k in range(bands)])
        Hd=20*np.log10(np.maximum(H,1e-9))
        pk=Hd.max(axis=1); pki=Hd.argmax(axis=1)
        # adjacent leakage: band k's response at band k+1's peak, rel. to band k+1's peak
        leak=[Hd[k][pki[k+1]]-pk[k+1] for k in range(bands-1)]
        out=[]
        for fl in [-20.,-30.]:
            gm=10**(fl/20.); Y=np.abs(rfft(imp+(gm-1.0)*S))
            m=(fr>150)&(fr<15000)
            out.append(20*np.log10(np.maximum(Y[m],1e-12)).mean())
        print(f"  order={order} bands={bands:<3d}            {rec:7.0f}  {pk.mean():8.1f}  {np.mean(leak):8.1f}  {out[0]:10.1f}  {out[1]:10.1f}")
