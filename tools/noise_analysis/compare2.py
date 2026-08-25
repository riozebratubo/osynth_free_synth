import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
a=np.load('tools/noise_analysis/data.npy'); x=a[:,0]
base=rms_db(x); n=len(x)//BLK*BLK
late=slice(int(6.0*SR),n)
def rep(tag,y):
    print(f"  {tag:46s} overall {base-rms_db(y):5.1f} dB   converged {rms_db(x[late])-rms_db(y[late]):5.1f} dB")
print("input rms %.1f dBFS\n"%base)
print("shipped:"); rep("current bank + raw min-statistics",anr_run_fast(x))
print("\nfix 1 only (smoothed estimator, current bank):"); rep("v2 tau=150ms",anr_run_v2(x,est_tau=0.150))
print("\nfix 2 only (difference bank, raw estimator):")
rep("v3 tau=1ms (no smoothing)",anr_run_v3(x,est_tau=0.0013))
print("\nboth fixes (v3):")
for order in [2,4]:
    for fl in [-12.,-20.,-30.]:
        rep(f"order={order} floor={fl:.0f} amount=0.6",anr_run_v3(x,est_tau=0.150,floor_db=fl,order=order))
print("\nboth fixes, amount sweep (order=2, floor=-20):")
for amt in [0.3,0.6,0.8,1.0]:
    rep(f"amount={amt}",anr_run_v3(x,est_tau=0.150,amount=amt))
print("\nadapt/subwin: time to first cut (order=2, floor=-20, amount=0.6)")
for aw,sw in [(8.0,1),(8.0,4),(3.0,4),(2.0,2)]:
    y,gt=anr_run_v3(x,est_tau=0.150,adapt_s=aw,subwins=sw,trace=True)
    gd=20*np.log10(np.maximum(gt.mean(axis=0),1e-6))
    idx=np.argmax(gd< -1.0)
    print(f"  adapt={aw}s subwins={sw}: first >1 dB of cut at {idx*BLK/SR:5.2f} s   final {gd[-1]:5.1f} dB   overall {base-rms_db(y):5.1f} dB")
