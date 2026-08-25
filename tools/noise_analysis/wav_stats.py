import sys, wave, numpy as np
p = sys.argv[1] if len(sys.argv)>1 else 'noise.wav'
w = wave.open(p,'rb')
print("channels",w.getnchannels(),"sampwidth",w.getsampwidth(),"rate",w.getframerate(),"frames",w.getnframes(),
      "dur %.2fs"%(w.getnframes()/w.getframerate()))
raw = w.readframes(w.getnframes())
sw = w.getsampwidth()
if sw==2: a = np.frombuffer(raw,dtype='<i2').astype(np.float64)/32768.0
elif sw==4: a = np.frombuffer(raw,dtype='<i4').astype(np.float64)/2147483648.0
else: raise SystemExit("sw %d"%sw)
ch = w.getnchannels()
a = a.reshape(-1,ch)
np.save('tools/noise_analysis/data.npy', a)
for c in range(ch):
    x=a[:,c]
    print(f"ch{c}: peak {20*np.log10(max(np.max(np.abs(x)),1e-12)):.1f} dBFS  rms {20*np.log10(max(np.sqrt(np.mean(x*x)),1e-12)):.1f} dBFS  dc {np.mean(x):.6f}")
if ch==2:
    d = a[:,0]-a[:,1]
    print("L-R rms %.1f dBFS (identical if -inf)"%(20*np.log10(max(np.sqrt(np.mean(d*d)),1e-12))))
