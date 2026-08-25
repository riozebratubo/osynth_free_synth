"""Does the fixed ANR still pass speech? Synthesises a formant-shaped voiced
signal with pauses, mixes it into the REAL noise from noise.wav, and measures
(a) how much noise goes away in the pauses and (b) how intact the signal is."""
import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
rng=np.random.default_rng(7)
a=np.load('tools/noise_analysis/data.npy'); noise=a[:,0]
n=len(noise)//BLK*BLK; noise=noise[:n]; t=np.arange(n)/SR

# voiced bursts: 110 Hz pulse train through three formants, 0.6 s on / 0.4 s off
f0=110.0
ph=np.cumsum(np.full(n,f0/SR))
src=np.zeros(n)
for h in range(1,40):
    if h*f0>7000: break
    src+=np.sin(2*np.pi*h*ph)/h
for fc,q,g in [(700,8,1.0),(1200,10,0.6),(2600,12,0.35)]:
    c=svf_coef_k(fc,1.0/q); src=src+g*svf_fast(c,'BpN',src)
env=np.zeros(n)
for k in range(11):
    s0=int(k*1.0*SR); s1=s0+int(0.6*SR)
    if s0>=n: break
    seg=slice(s0,min(s1,n)); L=min(s1,n)-s0
    env[seg]=np.hanning(max(L,2))[:L]**0.5
speech=src*env
speech*=10**(-20/20)/np.sqrt(np.mean(speech[env>0.5]**2))   # -20 dBFS while voiced
mix=speech+noise
voiced=env>0.5; pause=env<1e-6
print("test mix: speech %.1f dBFS (voiced), noise %.1f dBFS -> SNR %.1f dB"%(
    rms_db(speech[voiced]),rms_db(noise),rms_db(speech[voiced])-rms_db(noise)))
print()
def evaluate(tag,fn):
    y=fn(mix)[:n]
    ys=fn(speech)[:n]     # the same processing applied to the clean signal alone
    noise_cut = rms_db(noise[pause])-rms_db(y[pause])
    # signal damage: level change + waveform mismatch during voiced parts
    sl = rms_db(y[voiced])-rms_db(mix[voiced])
    d  = ys[voiced]-speech[voiced]
    dist = rms_db(d)-rms_db(speech[voiced])
    print(f"  {tag:44s} noise in pauses {noise_cut:5.1f} dB down | voiced level {sl:+5.1f} dB | speech distortion {dist:6.1f} dB")
print("ANR:")
evaluate("shipped defaults", lambda s: anr_run_fast(s))
evaluate("fixed (v3, order2, tau150, subwins4, adapt8)", lambda s: anr_run_v3(s))
evaluate("fixed, adapt=3s", lambda s: anr_run_v3(s,adapt_s=3.0))
evaluate("fixed, adapt=3s amount=1.0", lambda s: anr_run_v3(s,adapt_s=3.0,amount=1.0))
evaluate("fixed, adapt=3s floor=-30", lambda s: anr_run_v3(s,adapt_s=3.0,floor_db=-30.))
print("\nNR (expander) at several thresholds:")
for th in [-45.,-35.,-30.,-25.,-20.]:
    evaluate(f"thresh={th:.0f} (default is -45)", lambda s,th=th: nr_run_fast(s,thresh=th))
