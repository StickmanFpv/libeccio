#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# fork: measure every factory sound's loudness with the host simulator (tools/gen_level_trim.py uses it):
#   tools/measure_levels.py HOSTSIM TAG  ->  build/levels/TAG.json  (needs numpy, pyloudnorm)
import subprocess, os, json, wave, sys
import numpy as np, pyloudnorm as pyln
from concurrent.futures import ThreadPoolExecutor
D=os.environ.get('LEVELS_DIR','build/levels'); os.makedirs(D,exist_ok=True)
HS=sys.argv[1] if len(sys.argv)>1 else '/root/src/melodee/build/hostsim'
TAG=sys.argv[2] if len(sys.argv)>2 else 'before'
ENG={'SID':(3,32),'DRUM':(10,4),'FM6':(12,25),'CZ':(15,65),'PROPHET':(19,201),'SEM':(22,8),'HADES':(23,8),'MOOG':(24,12),'PULSAR':(25,14),'VOICE':(26,10)}
KIT=','.join(str(x) for x in [36,42,38,42,36,42,38,46,36,42,38,42,36,35,38,49])
meter=pyln.Meter(44100)
def m90(path):
    w=wave.open(path); n=w.getnframes(); a=np.frombuffer(w.readframes(n),dtype='<i2').reshape(-1,2).astype(float)/32768
    # momentary loudness via K-weighting from pyloudnorm filters
    b=a.copy()
    for f in meter._filters.values(): b=f.apply_filter(b)
    win=int(0.4*44100); hop=int(0.1*44100); L=[]
    for s in range(0,len(b)-win,hop):
        z=np.mean(b[s:s+win]**2,axis=0).sum()
        if z>0: L.append(-0.691+10*np.log10(z))
    L=[x for x in L if x>-60]
    pk=20*np.log10(np.abs(a).max()+1e-9)
    return (float(np.percentile(L,90)) if L else -99, float(pk))
def job(args):
    nm,e,p,mode=args
    f='%s/%s_%s_%d_%s.wav'%(D,TAG,nm,p,mode)
    env=dict(os.environ,SECS='6',PRESET='1',MASTER='1024')   # well under the master limiter: the sounds' own levels
    if mode=='kit': env['STEPS']=KIT
    if mode=='chord': env['CHORD']='3'
    subprocess.run([HS,str(e),str(p),'0',f],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    v=m90(f); os.remove(f); return (nm,p,mode,v)
jobs=[]
for nm,(e,n) in ENG.items():
    for p in range(n):
        if nm in('DRUM','PULSAR'): jobs.append((nm,e,p,'kit'))
        else: jobs+= [(nm,e,p,'seq'),(nm,e,p,'chord')]
with ThreadPoolExecutor(os.cpu_count()) as ex: out=list(ex.map(job,jobs))
res={}
for nm,p,mode,v in out: res.setdefault(nm,{}).setdefault(p,{})[mode]=v
L={nm:{p:max(v[0] for v in d.values()) for p,d in ps.items()} for nm,ps in res.items()}
PK={nm:{p:max(v[1] for v in d.values()) for p,d in ps.items()} for nm,ps in res.items()}
json.dump(PK,open(D+'/%s_peak.json'%TAG,'w'),indent=0)
json.dump(L,open(D+'/%s.json'%TAG,'w'),indent=0)
for nm in ENG:
    v=list(L[nm].values()); print('%-8s median %6.1f  min %6.1f  max %6.1f  spread %.1f'%(nm,np.median(v),min(v),max(v),np.std(v)))
