import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0]
base=rms_db(x); print("input rms %.1f dBFS\n"%base)
def report(tag,y,gt=None):
    n=min(len(x),len(y)); 
    late=slice(int(9.0*SR),n)   # after everything has converged
    print(f"{tag:52s} rms {rms_db(y):6.1f}  reduction {base-rms_db(y):5.1f} dB   "
          f"(last 1.5 s: {rms_db(x[late])-rms_db(y[late]):5.1f} dB)")
    return y
y0=anr_run_fast(x); report("ANR as shipped (defaults)",y0)
for tau in [0.050,0.150,0.300]:
    for gfs in [False,True]:
        y=anr_run_v2(x,est_tau=tau,gain_from_smoothed=gfs)
        report(f"ANR v2 tau={tau*1000:.0f}ms gain_from_smoothed={gfs}",y)
print()
for amt in [0.6,1.0]:
    for fl in [-20.0,-30.0]:
        y=anr_run_v2(x,est_tau=0.150,amount=amt,floor_db=fl)
        report(f"ANR v2 tau=150ms amount={amt} floor={fl}",y)
