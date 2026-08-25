import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); noise=a[:,0]
n=len(noise)//BLK*BLK; noise=noise[:n]
f0=110.0; ph=np.cumsum(np.full(n,f0/SR)); src=np.zeros(n)
for h in range(1,64):
    if h*f0>7000: break
    src+=np.sin(2*np.pi*h*ph)/h
for fc,q,g in [(700,8,1.0),(1200,10,0.6),(2600,12,0.35)]:
    src=src+g*svf_fast(svf_coef_k(fc,1.0/q),'BpN',src)
env=np.zeros(n)
for k in range(11):
    s0=int(k*1.0*SR); s1=s0+int(0.6*SR)
    if s0>=n: break
    L=min(s1,n)-s0; env[s0:s0+L]=np.hanning(max(L,2))[:L]**0.5
speech=src*env; speech*=10**(-20/20)/np.sqrt(np.mean(speech[env>0.5]**2))
mix=speech+noise; voiced=env>0.5; pause=env<1e-9
print(f"{'config':44s} {'noise-only':>11s} {'pause cut':>10s} {'damage':>8s} {'1st cut':>8s}")
def ev(tag,**kw):
    yo=anr_run_v4(noise,**kw)
    y=anr_run_v4(mix,**kw); ys=anr_run_v4(speech,**kw)
    _,gt=anr_run_v4(noise,trace=True,**kw)
    gd=20*np.log10(np.maximum(gt.mean(axis=0),1e-6)); i=np.argmax(gd<-1.0)
    dmg,_=spectral_damage(speech,ys,voiced)
    print(f"  {tag:42s} {rms_db(noise)-rms_db(yo):8.1f} dB {rms_db(noise[pause])-rms_db(y[pause]):7.1f} dB "
          f"{dmg:6.1f} dB {i*BLK/SR:6.2f} s")
for ad in [2.0,3.0,4.0,6.0,8.0]: ev(f"adapt={ad}s",adapt_s=ad)
print()
for tau in [0.050,0.100,0.150,0.250]: ev(f"est_tau={tau*1000:.0f}ms (adapt 4s)",adapt_s=4.0,est_tau=tau)
print()
for sw in [2,4,6]: ev(f"subwins={sw} (adapt 4s)",adapt_s=4.0,subwins=sw)
print()
for rel in [150.,300.,500.]: ev(f"release={rel:.0f}ms (adapt 4s)",adapt_s=4.0,release_ms=rel)
print()
for bd in [8,12,16]: ev(f"bands={bd} (adapt 4s)",adapt_s=4.0,bands=bd)
