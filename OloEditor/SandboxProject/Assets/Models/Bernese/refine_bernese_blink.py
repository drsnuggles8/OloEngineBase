"""Author Bernese corner-pinned blinking with a following furred orbital surface."""
from pathlib import Path
import argparse,json,shutil
import numpy as np
from build_bernese import Gltf,ramp,normalize

def frame(p,eye,side):
 c=np.array(eye['centre']);f=np.array(eye['gaze']);u=np.array(eye['up']);a=np.cross(u,f);a/=np.linalg.norm(a)
 rel=p-c;r=np.linalg.norm(rel,axis=1);psi=np.arcsin(np.clip(rel@a/np.maximum(r,1e-12),-1,1));phi=np.arctan2(rel@u,rel@f)
 fissure=np.radians(np.where(psi*side>0,54.,58.));t=np.minimum(abs(psi)/fissure,1);outer=np.clip(psi/fissure,-1,1)*side
 line=np.radians(-4.-4.*outer);meet=line+np.radians(-8.)*(1-t*t)
 hi=line+np.radians(33.)*(1-t**2.2)**.8;lo=line+np.radians(-19.)*(1-t**2)
 return c,f,u,a,rel,r,psi,phi,t,meet,hi,lo

def shape(p,eye,side,b,upper=None):
 c,f,u,a,rel,r,psi,phi,t,meet,hi,lo=frame(p,eye,side)
 if upper is not None:
  margin=hi if upper else lo
  attachment=1-ramp(np.radians(22),np.radians(92),abs(phi-margin))
  delta=(meet-margin)*b*attachment
 else:
  margin=np.clip(phi,lo,hi)
  distance=np.hypot(r*abs(phi-margin),np.maximum(r-eye['radius']-.0012,0))
  w=(1-ramp(0,.023,distance))*ramp(0,.005,rel@f)*(t<1)*ramp(-.026,-.018,rel@u)
  delta=(meet-margin)*b*w*.92
 newphi=phi+delta
 direction=np.cos(psi)[:,None]*(np.cos(newphi)[:,None]*f+np.sin(newphi)[:,None]*u)+np.sin(psi)[:,None]*a
 return c+r[:,None]*direction

def normals(p,n,eye,side,b,upper=None):
 e=1e-5;j=np.stack([(shape(p+a*e,eye,side,b,upper)-shape(p-a*e,eye,side,b,upper))/(2*e) for a in np.eye(3)],axis=2)
 det=np.linalg.det(j);assert det.min()>.035,(side,b,upper,det.min())
 return normalize(np.linalg.solve(j.transpose(0,2,1),n[...,None])[...,0]),float(det.min())

def build(source,output):
 assert not output.exists()
 s=Gltf(source/'Bernese.gltf');mesh=s.doc['meshes'][0];count=len(mesh['weights']);assert count==4
 rig=json.loads((source/'Bernese.rig.json').read_text());names=[s.doc['nodes'][i]['name'] for i in s.doc['skins'][0]['joints']]
 lids=[i for i,n in enumerate(names) if n.startswith('lid_')];lidnodes={s.doc['skins'][0]['joints'][i] for i in lids}
 rows=[]
 for prim in mesh['primitives']:
  attr=prim['attributes'];p=s.read(attr['POSITION']).astype(float);n=s.read(attr['NORMAL']).astype(float)
  for t in prim['targets'][:3]:p+=s.read(t['POSITION']);n+=s.read(t['NORMAL'])
  n=normalize(n);expression=s.read(prim['targets'][3]['POSITION']);joints=s.read(attr['JOINTS_0']).astype(int);weights=s.read(attr['WEIGHTS_0']);dominant=joints[np.arange(len(p)),weights.argmax(1)]
  d=np.zeros((3,len(p),3));dn=d.copy();minimum=1.;clearance=1.
  for side,label in ((1,'L'),(-1,'R')):
   eye=dict(rig['eyes'][label],radius=rig['eyeRadius']);near=(np.linalg.norm(p-np.array(eye['centre']),axis=1)<.050)&~np.isin(dominant,lids)
   for step,b in enumerate((1/3,2/3,1.)):
    if near.any():
     q=shape(p[near],eye,side,b);nn,det=normals(p[near],n[near],eye,side,b);minimum=min(minimum,det);d[step,near]=q-p[near];dn[step,near]=nn-n[near]
   for upper in (True,False):
    selection=dominant==names.index('lid_'+('upper_' if upper else 'lower_')+label)
    if not selection.any():continue
    assert np.all(weights[selection].max(1)>.999)
    for step,b in enumerate((1/3,2/3,1.)):
     q=shape(p[selection],eye,side,b,upper);nn,det=normals(p[selection],n[selection],eye,side,b,upper);minimum=min(minimum,det);d[step,selection]=q-p[selection];dn[step,selection]=nn-n[selection]
    for b in np.linspace(0,1,31):
     w=np.maximum(1-abs(b-np.array([1/3,2/3,1.]))*3,0)
     q=p[selection]+np.einsum('i,ijk->jk',w,d[:,selection])+(1-b)*expression[selection]
     clearance=min(clearance,float((np.linalg.norm(q-np.array(eye['centre']),axis=1)-eye['radius']).min()))
    assert clearance>.00005,(label,upper,clearance)
  assert np.allclose(d[:,p[:,1]<.61],0,atol=1e-12)
  for step in range(3):prim['targets'].append(dict(POSITION=s.append(d[step],'VEC3'),NORMAL=s.append(dn[step],'VEC3')))
  rows.append(dict(material=s.doc['materials'][prim['material']]['name'],minimumJacobian=minimum,minimumLidClearanceMM=clearance*1000))
 mesh['weights'] += [0.,0.,0.];mesh['extras']['targetNames'] += ['BerneseBlinkOneThird','BerneseBlinkTwoThirds','BerneseBlinkClosed']
 for animation in s.doc['animations']:
  for ch in animation['channels']:
   if ch['target']['path']=='weights':
    sampler=animation['samplers'][ch['sampler']];old=s.read(sampler['output']).reshape(-1,count);b=1-old[:,3];w=np.maximum(1-abs(b[:,None]-np.array([1/3,2/3,1.]))*3,0)
    sampler['output']=s.append(np.c_[old,w].ravel(),'SCALAR')
  animation['channels']=[ch for ch in animation['channels'] if ch['target']['node'] not in lidnodes]
 shutil.copytree(source,output);s.doc['buffers'][0].update(uri='Bernese.bin',byteLength=len(s.data));(output/'Bernese.gltf').write_text(json.dumps(s.doc,indent=2)+'\n');(output/'Bernese.bin').write_bytes(s.data)
 report=dict(rows=rows,sourceSkinWeightsAndBindingExact=True,originalNonLidAnimationExact=True,blinkDrivenByMorphTargets=True)
 (output/'bernese-blink-refinement.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();build(a.source,a.output)
