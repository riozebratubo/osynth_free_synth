"""Offline, sample-faithful reimplementation of osynth's fx.cpp NR + ANR units.

Mirrors components/fx/fx.cpp:anr_process() and nr_process() exactly, including
the block-rate structure (SYNTH_BLOCK_SIZE = 64 @ 48 kHz), the TPT SVF from
components/synth_core/synth_dsp.cpp, and every constant. Used to measure what
the units actually do to a real recording before changing them.
"""
import numpy as np

SR = 48000.0
BLK = 64
SVF_MIN_K = 0.04

# ---- svf ------------------------------------------------------------------
def svf_coef_k(fc, k, sr=SR):
    fc = min(max(fc, 20.0), 0.45*sr)
    k = max(k, SVF_MIN_K)
    g = np.tan(np.pi*fc/sr)
    a1 = 1.0/(1.0+g*(g+k)); a2 = g*a1; a3 = g*a2
    return (k, a1, a2, a3)

class Svf:
    __slots__=('ic1','ic2')
    def __init__(self): self.ic1=0.0; self.ic2=0.0

def svf_block(st, c, mode, x):
    """Vectorised-in-time is impossible (recursive); this is the scalar loop."""
    k,a1,a2,a3 = c
    ic1, ic2 = st.ic1, st.ic2
    out = np.empty_like(x)
    for i in range(x.shape[0]):
        v0 = x[i]
        v3 = v0 - ic2
        v1 = a1*ic1 + a2*v3
        v2 = ic2 + a2*ic1 + a3*v3
        ic1 = 2.0*v1 - ic1
        ic2 = 2.0*v2 - ic2
        if   mode=='Lp':   out[i]=v2
        elif mode=='Bp':   out[i]=v1
        elif mode=='Hp':   out[i]=v0-k*v1-v2
        elif mode=='Notch':out[i]=v0-k*v1
        else:              out[i]=k*v1     # BpN
    st.ic1, st.ic2 = ic1, ic2
    return out

# ---- ANR ------------------------------------------------------------------
ANR_SIGNAL_RATIO=4.0; ANR_BIAS=1.5; ANR_CREEP=2.0; ANR_OVERSUB=3.0
ANR_END_K=1.41421356; ANR_HUGE=1e30; ANR_SEED=1e-6; ANR_EPS=1e-7

def anr_bank(bands, low, high):
    if high < low*4.0: high = low*4.0
    ratio = (high/low)**(1.0/(bands-1))
    k = (ratio-1.0)/np.sqrt(ratio)
    coefs=[]; modes=[]; f=low
    for i in range(bands):
        end = (i==0) or (i==bands-1)
        coefs.append(svf_coef_k(f, ANR_END_K if end else k))
        modes.append('Lp' if i==0 else ('Hp' if i==bands-1 else 'BpN'))
        f*=ratio
    return coefs, modes

def anr_run(x, bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
            adapt_s=8.0, attack_ms=5.0, release_ms=150.0, learn=False,
            oversub=ANR_OVERSUB, bias=ANR_BIAS, sig_ratio=ANR_SIGNAL_RATIO,
            trace=False):
    coefs, modes = anr_bank(bands, low, high)
    st=[Svf() for _ in range(bands)]
    noise=np.zeros(bands); cur=np.full(bands,ANR_HUGE); prev=np.full(bands,ANR_HUGE)
    raw=np.full(bands,ANR_HUGE); g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    blk_s=BLK/SR
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    win=max(int((0.080 if learn else adapt_s)/blk_s),1)
    win_cnt=0
    n=len(x)//BLK*BLK
    y=x[:n].astype(np.float64).copy()
    tr=[]
    for s in range(0,n,BLK):
        xb=x[s:s+BLK].astype(np.float64)
        corr=np.zeros(BLK)
        ramp=np.arange(BLK)
        for kb in range(bands):
            yb=svf_block(st[kb],coefs[kb],modes[kb],xb)
            # acc over both channels; file is mono-identical so 2*sum|y|
            acc=2.0*np.sum(np.abs(yb))
            dloc=d[kb]+dstep[kb]*ramp
            corr+=dloc*yb
            d[kb]=d[kb]+dstep[kb]*BLK
            mag=acc*(1.0/(2.0*BLK))
            if not primed[kb]:
                if mag>ANR_EPS:
                    noise[kb]=mag*bias; cur[kb]=prev[kb]=raw[kb]=mag; primed[kb]=True
            else:
                if mag<raw[kb]: raw[kb]=mag
                if learn or mag < noise[kb]*sig_ratio+ANR_SEED:
                    if mag<cur[kb]: cur[kb]=mag
            gg=1.0
            if settled[kb] and mag>ANR_EPS:
                gg=(mag-sub*noise[kb])/mag
                gg=min(max(gg,gmin),1.0)
            g[kb]+= (ka if gg>g[kb] else kr)*(gg-g[kb])
            dstep[kb]=(g[kb]-1.0-d[kb])/BLK
            if trace: tr.append((s,kb,mag,noise[kb],gg,g[kb]))
        y[s:s+BLK]=xb+corr
        win_cnt+=1
        boundary = win_cnt>=win
        if boundary:
            win_cnt=0
            for kb in range(bands):
                if not primed[kb]: continue
                if cur[kb]<ANR_HUGE or prev[kb]<ANR_HUGE:
                    noise[kb]=min(cur[kb],prev[kb])*bias
                else:
                    noise[kb]=min(raw[kb]*bias, noise[kb]*ANR_CREEP)
                prev[kb]=cur[kb]; cur[kb]=ANR_HUGE; raw[kb]=ANR_HUGE
                if noise[kb]<=ANR_EPS: primed[kb]=False; settled[kb]=False
                else: settled[kb]=True
    return (y, tr) if trace else y

# ---- NR -------------------------------------------------------------------
NR_HUM_HARM=3; NR_HUM_K=0.05; NR_HPF_OFF=20.0; NR_HP_K=1.41421356
NR_KNEE=6.0; NR_FLOOR_EPS=1e-6

def nr_run(x, hpf=80.0, hum=0, thresh=-45.0, ratio=4.0, floor_db=-24.0,
           attack_ms=3.0, hold_ms=150.0, release_ms=200.0, knee=NR_KNEE,
           trace=False):
    use_hp = hpf > NR_HPF_OFF*1.02
    hp_c = svf_coef_k(hpf, NR_HP_K); hp = Svf()
    hum_c=[]; hum_st=[]
    if hum>0:
        f0 = 50.0 if hum==1 else 60.0
        for h in range(NR_HUM_HARM):
            hum_c.append(svf_coef_k(f0*(h+1), NR_HUM_K)); hum_st.append(Svf())
    ka=1.0-np.exp(-1.0/(attack_ms*1e-3*SR))
    kr=1.0-np.exp(-1.0/(release_ms*1e-3*SR))
    env=0.0; gain=1.0; hold=0
    hold_blocks=int(hold_ms*1e-3*SR/BLK)
    n=len(x)//BLK*BLK
    y=np.empty(n); tr=[]
    for s in range(0,n,BLK):
        xb=x[s:s+BLK].astype(np.float64)
        f=xb
        if use_hp: f=svf_block(hp,hp_c,'Hp',f)
        for h in range(len(hum_c)): f=svf_block(hum_st[h],hum_c[h],'Notch',f)
        # per-sample envelope
        peak=0.0
        for i in range(BLK):
            t=abs(f[i])
            env += (ka if t>env else kr)*(t-env)
            if env>peak: peak=env
        db=20.0*np.log10(max(peak,NR_FLOOR_EPS))
        over=db-thresh
        if over>0.0: hold=hold_blocks
        elif hold>0: hold-=1
        gr=0.0
        if hold==0:
            slope=max(ratio,1.0)-1.0
            if over>=0.5*knee: gr=0.0
            elif over<=-0.5*knee: gr=slope*over
            else:
                t=over-0.5*knee; gr=-slope*t*t/(2.0*knee)
            gr=max(gr,floor_db)
        target=10.0**(gr/20.0)
        g0=gain; gstep=(target-g0)/BLK
        gg=g0+gstep*(np.arange(BLK)+1)
        y[s:s+BLK]=f*gg
        if trace: tr.append((s,db,over,gr,target))
        gain=target
    return (y,tr) if trace else y

def rms_db(v): return 20*np.log10(max(np.sqrt(np.mean(v*v)),1e-12))

# ---- fast path: the TPT SVF is LTI here, so it has a closed-form biquad ----
from scipy.signal import lfilter

def svf_biquad(c):
    """(b,a) for the TPT SVF of svf_coef_k, per mode. Verified against the
    scalar recursion in verify_biquad()."""
    k,a1,a2,a3 = c
    g = a2/a1                      # a2 = g*a1
    D0 = 1.0+g*k+g*g; D1 = 2.0*g*g-2.0; D2 = 1.0-g*k+g*g
    a = [D0, D1, D2]
    return {
      'Lp':    ([g*g, 2*g*g, g*g], a),
      'Bp':    ([g, 0.0, -g], a),
      'Hp':    ([1.0, -2.0, 1.0], a),
      'BpN':   ([k*g, 0.0, -k*g], a),
      'Notch': ([D0-k*g, D1, D2+k*g], a),
    }

def svf_fast(c, mode, x):
    b,a = svf_biquad(c)[mode]
    return lfilter(np.array(b)/a[0], np.array(a)/a[0], x)

def verify_biquad():
    rng=np.random.default_rng(0); x=rng.standard_normal(4096)
    ok=True
    for fc,k in [(120.0,1.414),(1000.0,0.5),(9000.0,1.414),(50.0,0.05)]:
        c=svf_coef_k(fc,k)
        for m in ['Lp','Bp','Hp','BpN','Notch']:
            ref=svf_block(Svf(),c,m,x.copy()); fast=svf_fast(c,m,x)
            e=np.max(np.abs(ref-fast))/max(np.max(np.abs(ref)),1e-9)
            if e>1e-6: ok=False; print("MISMATCH",fc,k,m,e)
    print("biquad verification:", "OK" if ok else "FAILED")
    return ok

def anr_run_fast(x, bands=12, low=120.0, high=9000.0, amount=0.6,
                 floor_db=-20.0, adapt_s=8.0, attack_ms=5.0, release_ms=150.0,
                 learn=False, oversub=ANR_OVERSUB, bias=ANR_BIAS,
                 sig_ratio=ANR_SIGNAL_RATIO, creep=ANR_CREEP, trace=False):
    coefs,modes = anr_bank(bands,low,high)
    x=np.asarray(x,dtype=np.float64)
    n=len(x)//BLK*BLK; x=x[:n]
    B=np.stack([svf_fast(coefs[k],modes[k],x) for k in range(bands)])  # bands x n
    nb=n//BLK
    Bb=B.reshape(bands,nb,BLK)
    MAG=np.mean(np.abs(Bb),axis=2)                     # bands x nb (== acc/(2*BLK))
    noise=np.zeros(bands); cur=np.full(bands,ANR_HUGE); prev=np.full(bands,ANR_HUGE)
    raw=np.full(bands,ANR_HUGE); g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    blk_s=BLK/SR
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    win=max(int((0.080 if learn else adapt_s)/blk_s),1); win_cnt=0
    ramp=np.arange(BLK); y=x.copy(); gtrace=np.zeros((bands,nb))
    for bi in range(nb):
        mag=MAG[:,bi]
        dloc=d[:,None]+dstep[:,None]*ramp[None,:]
        y[bi*BLK:(bi+1)*BLK]+=np.sum(dloc*Bb[:,bi,:],axis=0)
        d+=dstep*BLK
        new=(~primed)&(mag>ANR_EPS)
        noise[new]=mag[new]*bias; cur[new]=prev[new]=raw[new]=mag[new]; primed|=new
        old=primed&(~new)
        raw[old]=np.minimum(raw[old],mag[old])
        offer=old&(learn|(mag<noise*sig_ratio+ANR_SEED))
        cur[offer]=np.minimum(cur[offer],mag[offer])
        gg=np.ones(bands)
        act=settled&(mag>ANR_EPS)
        gg[act]=np.clip((mag[act]-sub*noise[act])/mag[act],gmin,1.0)
        g+=np.where(gg>g,ka,kr)*(gg-g)
        dstep=(g-1.0-d)/BLK
        gtrace[:,bi]=g
        win_cnt+=1
        if win_cnt>=win:
            win_cnt=0
            have=(cur<ANR_HUGE)|(prev<ANR_HUGE)
            nn=np.where(have,np.minimum(cur,prev)*bias,np.minimum(raw*bias,noise*creep))
            noise=np.where(primed,nn,noise)
            prev=np.where(primed,cur,prev)
            cur=np.where(primed,ANR_HUGE,cur); raw=np.where(primed,ANR_HUGE,raw)
            dead=primed&(noise<=ANR_EPS)
            primed&=~dead; settled=np.where(dead,False,np.where(primed,True,settled))
    return (y,gtrace,MAG) if trace else y

def nr_run_fast(x, hpf=80.0, hum=0, thresh=-45.0, ratio=4.0, floor_db=-24.0,
                attack_ms=3.0, hold_ms=150.0, release_ms=200.0, knee=NR_KNEE,
                trace=False):
    x=np.asarray(x,dtype=np.float64); n=len(x)//BLK*BLK; x=x[:n]
    f=x
    if hpf>NR_HPF_OFF*1.02: f=svf_fast(svf_coef_k(hpf,NR_HP_K),'Hp',f)
    if hum>0:
        f0=50.0 if hum==1 else 60.0
        for h in range(NR_HUM_HARM): f=svf_fast(svf_coef_k(f0*(h+1),NR_HUM_K),'Notch',f)
    ka=1.0-np.exp(-1.0/(attack_ms*1e-3*SR)); kr=1.0-np.exp(-1.0/(release_ms*1e-3*SR))
    af=np.abs(f); env=np.empty(n); e=0.0
    for i in range(n):
        t=af[i]; e+=(ka if t>e else kr)*(t-e); env[i]=e
    nb=n//BLK
    peak=env.reshape(nb,BLK).max(axis=1)
    db=20*np.log10(np.maximum(peak,NR_FLOOR_EPS)); over=db-thresh
    hold_blocks=int(hold_ms*1e-3*SR/BLK)
    slope=max(ratio,1.0)-1.0
    y=np.empty(n); gain=1.0; hold=0; grs=np.zeros(nb)
    ramp=np.arange(1,BLK+1)
    for bi in range(nb):
        o=over[bi]
        if o>0.0: hold=hold_blocks
        elif hold>0: hold-=1
        gr=0.0
        if hold==0:
            if o>=0.5*knee: gr=0.0
            elif o<=-0.5*knee: gr=slope*o
            else:
                t=o-0.5*knee; gr=-slope*t*t/(2.0*knee)
            gr=max(gr,floor_db)
        grs[bi]=gr
        target=10.0**(gr/20.0)
        y[bi*BLK:(bi+1)*BLK]=f[bi*BLK:(bi+1)*BLK]*(gain+(target-gain)/BLK*ramp)
        gain=target
    return (y,db,grs) if trace else y

def anr_run_v2(x, bands=12, low=120.0, high=9000.0, amount=0.6,
               floor_db=-20.0, adapt_s=8.0, attack_ms=5.0, release_ms=150.0,
               learn=False, oversub=ANR_OVERSUB, bias=ANR_BIAS,
               sig_ratio=ANR_SIGNAL_RATIO, creep=ANR_CREEP,
               est_tau=0.150, gain_from_smoothed=True, subwins=4, trace=False):
    """Candidate fix: a one-pole on the per-band block magnitude ahead of the
    minimum tracker (est_tau), and the window split into `subwins` buckets so
    the first estimate lands in adapt_s/subwins instead of adapt_s."""
    coefs,modes=anr_bank(bands,low,high)
    x=np.asarray(x,dtype=np.float64); n=len(x)//BLK*BLK; x=x[:n]
    B=np.stack([svf_fast(coefs[k],modes[k],x) for k in range(bands)])
    nb=n//BLK; Bb=B.reshape(bands,nb,BLK)
    MAG=np.mean(np.abs(Bb),axis=2)
    blk_s=BLK/SR
    al=1.0-np.exp(-blk_s/max(est_tau,1e-4))
    noise=np.zeros(bands); sm=np.zeros(bands)
    buckets=np.full((subwins,bands),ANR_HUGE)
    raw=np.full(bands,ANR_HUGE)
    g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    win=max(int((0.080 if learn else adapt_s)/subwins/blk_s),1); win_cnt=0
    ramp=np.arange(BLK); y=x.copy(); gtr=np.zeros((bands,nb)); ntr=np.zeros((bands,nb))
    for bi in range(nb):
        mag=MAG[:,bi]
        y[bi*BLK:(bi+1)*BLK]+=np.sum((d[:,None]+dstep[:,None]*ramp[None,:])*Bb[:,bi,:],axis=0)
        d+=dstep*BLK
        new=(~primed)&(mag>ANR_EPS)
        sm[new]=mag[new]; noise[new]=mag[new]*bias
        buckets[:,new]=mag[new]; raw[new]=mag[new]; primed|=new
        old=primed&(~new)
        sm[old]+=al*(mag[old]-sm[old])
        raw[old]=np.minimum(raw[old],sm[old])
        offer=old&(learn|(sm<noise*sig_ratio+ANR_SEED))
        buckets[0,offer]=np.minimum(buckets[0,offer],sm[offer])
        det = sm if gain_from_smoothed else mag
        gg=np.ones(bands); act=settled&(det>ANR_EPS)
        gg[act]=np.clip((det[act]-sub*noise[act])/det[act],gmin,1.0)
        g+=np.where(gg>g,ka,kr)*(gg-g)
        dstep=(g-1.0-d)/BLK
        gtr[:,bi]=g; ntr[:,bi]=noise
        win_cnt+=1
        if win_cnt>=win:
            win_cnt=0
            best=buckets.min(axis=0)
            have=best<ANR_HUGE
            nn=np.where(have,best*bias,np.minimum(raw*bias,noise*creep))
            noise=np.where(primed,nn,noise)
            buckets[1:]=buckets[:-1]; buckets[0]=ANR_HUGE
            raw=np.where(primed,ANR_HUGE,raw)
            dead=primed&(noise<=ANR_EPS)
            primed&=~dead
            settled=np.where(dead,False,np.where(primed,True,settled))
    return (y,gtr,ntr,MAG) if trace else y

# ---- v3: difference bank (perfect reconstruction) + smoothed estimator ----
def anr_bank_diff_coefs(bands, low, high):
    """bands-1 crossovers spanning low..high inclusive."""
    if high < low*4.0: high = low*4.0
    if bands < 3: bands = 3
    r = (high/low)**(1.0/(bands-2))
    return [low*r**i for i in range(bands-1)]

def bank_diff_run(x, bands, low, high, order=2):
    fc = anr_bank_diff_coefs(bands, low, high)
    c  = [svf_coef_k(f,1.41421356) for f in fc]
    if order==4: L=[svf_fast(cc,'Lp',svf_fast(cc,'Lp',x)) for cc in c]
    else:        L=[svf_fast(cc,'Lp',x) for cc in c]
    return np.stack([L[0]]+[L[i]-L[i-1] for i in range(1,len(L))]+[x-L[-1]])

def anr_run_v3(x, bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
               adapt_s=8.0, attack_ms=5.0, release_ms=150.0, learn=False,
               oversub=ANR_OVERSUB, bias=ANR_BIAS, sig_ratio=ANR_SIGNAL_RATIO,
               creep=ANR_CREEP, est_tau=0.150, subwins=4, order=2, trace=False):
    x=np.asarray(x,dtype=np.float64); n=len(x)//BLK*BLK; x=x[:n]
    B=bank_diff_run(x,bands,low,high,order)
    nb=n//BLK; Bb=B.reshape(bands,nb,BLK)
    MAG=np.mean(np.abs(Bb),axis=2)
    blk_s=BLK/SR; al=1.0-np.exp(-blk_s/max(est_tau,1e-4))
    noise=np.zeros(bands); sm=np.zeros(bands)
    buckets=np.full((subwins,bands),ANR_HUGE); raw=np.full(bands,ANR_HUGE)
    g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    win=max(int((0.080 if learn else adapt_s)/subwins/blk_s),1); win_cnt=0
    ramp=np.arange(BLK); y=x.copy(); gtr=np.zeros((bands,nb))
    for bi in range(nb):
        mag=MAG[:,bi]
        y[bi*BLK:(bi+1)*BLK]+=np.sum((d[:,None]+dstep[:,None]*ramp[None,:])*Bb[:,bi,:],axis=0)
        d+=dstep*BLK
        new=(~primed)&(mag>ANR_EPS)
        sm[new]=mag[new]; noise[new]=mag[new]*bias; buckets[:,new]=mag[new]
        raw[new]=mag[new]; primed|=new
        old=primed&(~new)
        sm[old]+=al*(mag[old]-sm[old])
        raw[old]=np.minimum(raw[old],sm[old])
        offer=old&(learn|(sm<noise*sig_ratio+ANR_SEED))
        buckets[0,offer]=np.minimum(buckets[0,offer],sm[offer])
        gg=np.ones(bands); act=settled&(sm>ANR_EPS)
        gg[act]=np.clip((sm[act]-sub*noise[act])/sm[act],gmin,1.0)
        g+=np.where(gg>g,ka,kr)*(gg-g)
        dstep=(g-1.0-d)/BLK; gtr[:,bi]=g
        win_cnt+=1
        if win_cnt>=win:
            win_cnt=0
            best=buckets.min(axis=0); have=best<ANR_HUGE
            nn=np.where(have,best*bias,np.minimum(raw*bias,noise*creep))
            noise=np.where(primed,nn,noise)
            buckets[1:]=buckets[:-1]; buckets[0]=ANR_HUGE
            raw=np.where(primed,ANR_HUGE,raw)
            dead=primed&(noise<=ANR_EPS)
            primed&=~dead
            settled=np.where(dead,False,np.where(primed,True,settled))
    return (y,gtr) if trace else y

def anr_run_v4(x, bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
               adapt_s=3.0, attack_ms=5.0, release_ms=150.0, learn=False,
               oversub=ANR_OVERSUB, bias=ANR_BIAS, sig_ratio=ANR_SIGNAL_RATIO,
               creep=ANR_CREEP, est_tau=0.150, subwins=4, order=2,
               gain_from='raw', trace=False):
    """v3 but the gain computer may read the raw block magnitude (fast, keeps
    transients) while the estimator reads the smoothed one (stable)."""
    x=np.asarray(x,dtype=np.float64); n=len(x)//BLK*BLK; x=x[:n]
    B=bank_diff_run(x,bands,low,high,order)
    nb=n//BLK; Bb=B.reshape(bands,nb,BLK); MAG=np.mean(np.abs(Bb),axis=2)
    blk_s=BLK/SR; al=1.0-np.exp(-blk_s/max(est_tau,1e-4))
    noise=np.zeros(bands); sm=np.zeros(bands)
    buckets=np.full((subwins,bands),ANR_HUGE); raw=np.full(bands,ANR_HUGE)
    g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    win=max(int((0.080 if learn else adapt_s)/subwins/blk_s),1); win_cnt=0
    ramp=np.arange(BLK); y=x.copy(); gtr=np.zeros((bands,nb))
    for bi in range(nb):
        mag=MAG[:,bi]
        y[bi*BLK:(bi+1)*BLK]+=np.sum((d[:,None]+dstep[:,None]*ramp[None,:])*Bb[:,bi,:],axis=0)
        d+=dstep*BLK
        new=(~primed)&(mag>ANR_EPS)
        sm[new]=mag[new]; noise[new]=mag[new]*bias; buckets[:,new]=mag[new]
        raw[new]=mag[new]; primed|=new
        old=primed&(~new)
        sm[old]+=al*(mag[old]-sm[old])
        raw[old]=np.minimum(raw[old],sm[old])
        offer=old&(learn|(sm<noise*sig_ratio+ANR_SEED))
        buckets[0,offer]=np.minimum(buckets[0,offer],sm[offer])
        det = mag if gain_from=='raw' else sm
        gg=np.ones(bands); act=settled&(det>ANR_EPS)
        gg[act]=np.clip((det[act]-sub*noise[act])/det[act],gmin,1.0)
        g+=np.where(gg>g,ka,kr)*(gg-g)
        dstep=(g-1.0-d)/BLK; gtr[:,bi]=g
        win_cnt+=1
        if win_cnt>=win:
            win_cnt=0
            best=buckets.min(axis=0); have=best<ANR_HUGE
            nn=np.where(have,best*bias,np.minimum(raw*bias,noise*creep))
            noise=np.where(primed,nn,noise)
            buckets[1:]=buckets[:-1]; buckets[0]=ANR_HUGE
            raw=np.where(primed,ANR_HUGE,raw)
            dead=primed&(noise<=ANR_EPS)
            primed&=~dead
            settled=np.where(dead,False,np.where(primed,True,settled))
    return (y,gtr) if trace else y

def spectral_damage(clean, processed, mask, sr=SR, nfft=2048):
    """Phase-insensitive: mean |dB| difference of the two magnitude spectra over
    the frames selected by `mask`, 150 Hz..8 kHz."""
    from numpy.fft import rfft, rfftfreq
    f=rfftfreq(nfft,1/sr); band=(f>150)&(f<8000)
    hop=nfft//2; errs=[]; lev=[]
    w=np.hanning(nfft)
    for s in range(0,len(clean)-nfft,hop):
        if not mask[s:s+nfft].all(): continue
        A=np.abs(rfft(clean[s:s+nfft]*w))[band]
        Bm=np.abs(rfft(processed[s:s+nfft]*w))[band]
        keep=A>np.max(A)*1e-3
        if keep.sum()<5: continue
        e=20*np.log10(np.maximum(Bm[keep],1e-12))-20*np.log10(A[keep])
        errs.append(np.mean(np.abs(e))); lev.append(np.mean(e))
    return (np.mean(errs) if errs else 0.0, np.mean(lev) if lev else 0.0)

ANR_CREEP_REF_S = 8.0   # kAnrCreep is "per 8 s", not "per bucket rotation"

def anr_run_v5(x, bands=12, low=120.0, high=9000.0, amount=0.6, floor_db=-20.0,
               adapt_s=3.0, attack_ms=5.0, release_ms=150.0, learn=False,
               oversub=ANR_OVERSUB, bias=ANR_BIAS, sig_ratio=ANR_SIGNAL_RATIO,
               creep=ANR_CREEP, est_tau=0.150, subwins=4, order=2, trace=False):
    """v4 + the creep escape re-anchored to wall clock, so shortening `adapt`
    no longer speeds up how fast a held pad is mistaken for noise."""
    x=np.asarray(x,dtype=np.float64); n=len(x)//BLK*BLK; x=x[:n]
    B=bank_diff_run(x,bands,low,high,order)
    nb=n//BLK; Bb=B.reshape(bands,nb,BLK); MAG=np.mean(np.abs(Bb),axis=2)
    blk_s=BLK/SR; al=1.0-np.exp(-blk_s/max(est_tau,1e-4))
    noise=np.zeros(bands); sm=np.zeros(bands)
    buckets=np.full((subwins,bands),ANR_HUGE); raw=np.full(bands,ANR_HUGE)
    g=np.ones(bands); d=np.zeros(bands); dstep=np.zeros(bands)
    primed=np.zeros(bands,bool); settled=np.zeros(bands,bool)
    gmin=10.0**(floor_db/20.0); sub=amount*oversub
    ka=1.0-np.exp(-blk_s/max(attack_ms*1e-3,1e-4))
    kr=1.0-np.exp(-blk_s/max(release_ms*1e-3,1e-4))
    sub_s=(0.080 if learn else adapt_s)/subwins
    win=max(int(sub_s/blk_s),1); win_cnt=0
    creep_sub=creep**(max(win,1)*blk_s/ANR_CREEP_REF_S)
    ramp=np.arange(BLK); y=x.copy(); gtr=np.zeros((bands,nb))
    for bi in range(nb):
        mag=MAG[:,bi]
        y[bi*BLK:(bi+1)*BLK]+=np.sum((d[:,None]+dstep[:,None]*ramp[None,:])*Bb[:,bi,:],axis=0)
        d+=dstep*BLK
        new=(~primed)&(mag>ANR_EPS)
        sm[new]=mag[new]; noise[new]=mag[new]*bias; buckets[:,new]=mag[new]
        raw[new]=mag[new]; primed|=new
        old=primed&(~new)
        sm[old]+=al*(mag[old]-sm[old])
        raw[old]=np.minimum(raw[old],sm[old])
        offer=old&(learn|(sm<noise*sig_ratio+ANR_SEED))
        buckets[0,offer]=np.minimum(buckets[0,offer],sm[offer])
        gg=np.ones(bands); act=settled&(mag>ANR_EPS)
        gg[act]=np.clip((mag[act]-sub*noise[act])/mag[act],gmin,1.0)
        g+=np.where(gg>g,ka,kr)*(gg-g)
        dstep=(g-1.0-d)/BLK; gtr[:,bi]=g
        win_cnt+=1
        if win_cnt>=win:
            win_cnt=0
            best=buckets.min(axis=0); have=best<ANR_HUGE
            nn=np.where(have,best*bias,np.minimum(raw*bias,noise*creep_sub))
            noise=np.where(primed,nn,noise)
            buckets[1:]=buckets[:-1]; buckets[0]=ANR_HUGE
            raw=np.where(primed,ANR_HUGE,raw)
            dead=primed&(noise<=ANR_EPS)
            primed&=~dead
            settled=np.where(dead,False,np.where(primed,True,settled))
    return (y,gtr) if trace else y
