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
mix=speech+noise
voiced=env>0.5; pause=env<1e-9
print("mix: speech %.1f dBFS voiced, noise %.1f dBFS, SNR %.1f dB\n"%(
    rms_db(speech[voiced]),rms_db(noise),rms_db(speech[voiced])-rms_db(noise)))
print(f"{'':52s} {'noise cut':>10s} {'spec damage':>12s} {'level bias':>11s}")
def ev(tag,fn):
    y=fn(mix)[:n]; ys=fn(speech)[:n]
    cut=rms_db(noise[pause])-rms_db(y[pause])
    dmg,bias=spectral_damage(speech,ys,voiced)
    print(f"  {tag:50s} {cut:8.1f} dB {dmg:10.1f} dB {bias:+9.1f} dB")
print("ANR:")
ev("shipped defaults", lambda s: anr_run_fast(s))
ev("fixed, gain from SMOOTHED mag, adapt 3s", lambda s: anr_run_v4(s,gain_from='smooth'))
ev("fixed, gain from RAW mag, adapt 3s", lambda s: anr_run_v4(s,gain_from='raw'))
ev("fixed, raw, amount 0.4", lambda s: anr_run_v4(s,amount=0.4))
ev("fixed, raw, amount 1.0", lambda s: anr_run_v4(s,amount=1.0))
ev("fixed, raw, floor -12", lambda s: anr_run_v4(s,floor_db=-12.))
ev("fixed, raw, release 400ms", lambda s: anr_run_v4(s,release_ms=400.))
print("\nNR expander (noise-only file and speech test):")
for th in [-45.,-30.,-25.,-22.,-20.,-16.]:
    y=nr_run_fast(noise,thresh=th)
    ev(f"thresh {th:.0f}  [noise-only file: {rms_db(noise)-rms_db(y):4.1f} dB down]",
       lambda s,th=th: nr_run_fast(s,thresh=th))
