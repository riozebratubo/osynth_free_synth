import sys, numpy as np; sys.path.insert(0,'tools/noise_analysis')
from sim_fx import *
n=int(60.0*SR)//BLK*BLK; t=np.arange(n)/SR
pad=np.zeros(n)
for f in [110.,138.6,164.8,220.,277.2,329.6]:
    for h in [1,2,3]: pad+=np.sin(2*np.pi*f*h*t+f)/(h*h)
pad*=10**(-18/20)/np.sqrt(np.mean(pad**2))
pad[:int(0.05*SR)]*=np.linspace(0,1,int(0.05*SR))
seg=lambda s,e: slice(int(s*SR),int(e*SR))
marks=[2,5,10,20,30,45,59]
print("sustained pad, level change vs time (the 'held pad looks like a fan' guard):")
print("       config            " + "".join(f"{m:>7d}s" for m in marks))
def row(tag,y):
    print(f"  {tag:22s}" + "".join(f"{rms_db(y[seg(m-1,m)])-rms_db(pad[seg(m-1,m)]):+7.1f}" for m in marks))
row("shipped (does nothing)",anr_run_fast(pad))
row("v4 adapt=3s (broken)",anr_run_v4(pad,adapt_s=3.0))
row("v5 adapt=3s (fixed)",anr_run_v5(pad,adapt_s=3.0))
row("v5 adapt=8s",anr_run_v5(pad,adapt_s=8.0))
print("\n(the original comment's promise: a pad 40 dB up takes 'the better part")
print(" of a minute' to be mistaken for noise)\n")
a=np.load('tools/noise_analysis/data.npy'); noise=np.tile(a[:,0],2)
nn=len(noise)//BLK*BLK; noise=noise[:nn]
print("and the noise file itself, v5 defaults:")
for ad in [2.0,3.0,4.0]:
    y=anr_run_v5(noise,adapt_s=ad)
    la=slice(int(6*SR),nn)
    print(f"  adapt={ad}s: overall {rms_db(noise)-rms_db(y):5.1f} dB   converged {rms_db(noise[la])-rms_db(y[la]):5.1f} dB")
