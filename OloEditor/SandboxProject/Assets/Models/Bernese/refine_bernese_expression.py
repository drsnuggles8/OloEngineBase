"""Soften Bernese lid aperture and keep its authored blink fully closed."""
from pathlib import Path
import argparse,json,shutil,sys
import numpy as np
from PIL import Image
from build_bernese import Gltf,ramp,normalize

def aperture(p,eyes):
    q=p.copy()
    for eye in eyes:
        c=np.array(eye['centre']); f=np.array(eye['gaze']); u=np.array(eye['up']); a=np.cross(u,f)
        rel=p-c; r=np.linalg.norm(rel,axis=1)
        lateral=rel@a; forward=rel@f; vertical=rel@u
        psi=np.arctan2(lateral,np.sqrt(forward**2+vertical**2)); phi=np.arctan2(vertical,forward)
        centre=1-ramp(.30,.95,abs(psi))
        shoulders=np.exp(-((abs(psi)-.43)/.22)**2)
        upper=np.radians(-2.0)*centre+np.radians(4.5)*shoulders
        lower=np.radians(-8.0)*centre
        blend=ramp(np.radians(-9),np.radians(8),phi)
        angle=lower*(1-blend)+upper*blend
        weight=(1-ramp(.025,.039,r))*ramp(.15,.60,forward/np.maximum(r,1e-12))
        angle*=weight
        newphi=phi+angle; horizontal=np.sqrt(forward**2+vertical**2)
        q+=(horizontal*(np.cos(newphi)-np.cos(phi)))[:,None]*f+(horizontal*(np.sin(newphi)-np.sin(phi)))[:,None]*u
    return q

def build(source,output):
    assert not output.exists()
    asset=Gltf(source/'Bernese.gltf'); mesh=asset.doc['meshes'][0]; count=len(mesh['weights'])
    assert mesh['extras']['targetNames'][-1]=='BerneseSoftEyeAndMuzzle'
    rig=json.loads((source/'Bernese.rig.json').read_text());eyes=list(rig['eyes'].values());rows=[]
    for prim in mesh['primitives']:
        p=asset.read(prim['attributes']['POSITION']).astype(float); n=asset.read(prim['attributes']['NORMAL']).astype(float)
        for w,t in zip(mesh['weights'],prim['targets']):p+=w*asset.read(t['POSITION']);n+=w*asset.read(t['NORMAL'])
        material=asset.doc['materials'][prim['material']]['name']
        strength=.4 if material=='DogSkin' else 1.
        def shape(v):return v+strength*(aperture(v,eyes)-v)
        n=normalize(n);delta=shape(p)-p;eps=1e-5
        jac=np.stack([(shape(p+a*eps)-shape(p-a*eps))/(2*eps) for a in np.eye(3)],axis=2)
        det=np.linalg.det(jac);assert det.min()>.45
        nn=normalize(np.linalg.solve(jac.transpose(0,2,1),n[...,None])[...,0])
        assert np.allclose(delta[p[:,1]<.62],0,atol=1e-12)
        prim['targets'].append(dict(POSITION=asset.append(delta,'VEC3'),NORMAL=asset.append(nn-n,'VEC3')))
        rows.append(dict(material=asset.doc['materials'][prim['material']]['name'],maxMM=float(np.linalg.norm(delta,axis=1).max()*1000),minimumJacobian=float(det.min())))
    mesh['weights'].append(1.);mesh['extras']['targetNames'].append('BerneseGentleLidAperture')
    upper=next(i for i,n in enumerate(asset.doc['nodes']) if n.get('name')=='lid_upper_L')
    body=next(i for i,n in enumerate(asset.doc['nodes']) if n.get('mesh')==0)
    rest=np.array(asset.doc['nodes'][upper]['rotation']);animation_report=[]
    for animation in asset.doc['animations']:
        channel=next(c for c in animation['channels'] if c['target']==dict(node=upper,path='rotation'))
        sampler=animation['samplers'][channel['sampler']]
        times=asset.read(sampler['input']).ravel();quats=asset.read(sampler['output'])
        amount=np.clip(2*np.arccos(np.clip(abs(quats@rest),0,1))/np.radians(41),0,1)
        authored=False
        for channel in animation['channels']:
            if channel['target']['path']=='weights':
                authored=True
                sampler=animation['samplers'][channel['sampler']]
                old_times=asset.read(sampler['input']).ravel()
                old=asset.read(sampler['output']).reshape(-1,count)
                assert len(old)==len(old_times)
                wt=np.union1d(old_times,times)
                expression=1-np.interp(wt,times,amount)
                previous=np.stack([np.interp(wt,old_times,old[:,i]) for i in range(count)],axis=1)
                sampler['input']=asset.append(wt,'SCALAR')
                sampler['output']=asset.append(np.c_[previous,expression].ravel(),'SCALAR')
        if not authored:
            values=np.c_[np.broadcast_to(mesh['weights'][:count],(len(times),count)),1-amount]
            index=len(animation['samplers'])
            animation['samplers'].append(dict(input=asset.append(times,'SCALAR'),output=asset.append(values.ravel(),'SCALAR'),interpolation='LINEAR'))
            animation['channels'].append(dict(sampler=index,target=dict(node=body,path='weights')))
        animation_report.append(dict(name=animation['name'],minBlink=float(amount.min()),maxBlink=float(amount.max()),blinkAt1p10=float(np.interp(1.10,times,amount))))
    shutil.copytree(source,output)
    asset.doc['buffers'][0].update(uri='Bernese.bin',byteLength=len(asset.data))
    (output/'Bernese.gltf').write_text(json.dumps(asset.doc,indent=2)+'\n');(output/'Bernese.bin').write_bytes(asset.data)
    scene=(source/'Bernese.olo').read_text().replace('BaseColor: [0.2, 0.18, 0.17, 1]','BaseColor: [0.55, 0.52, 0.5, 1]')
    (output/'Bernese.olo').write_text(scene)
    profile=(source/'BerneseEye.oloskin').read_text().replace('    - 0.46\n    - 0.48\n    - 0.50','    - 0.36\n    - 0.42\n    - 0.47')
    profile=profile.replace('IrisRadiusMM: 9.6','IrisRadiusMM: 8.4').replace('PupilRadiusMM: 3.5','PupilRadiusMM: 2.8').replace('IrisPlaneDepthMM: 5.0','IrisPlaneDepthMM: 3.4')
    (output/'BerneseEye.oloskin').write_text(profile)
    import authoring_maps as build_dog
    build_dog.IRIS_LIMBUS=8.4/12.
    build_dog.IRIS_PUPIL=2.8/12.
    build_dog.IRIS_SCLERA_LINEAR=(.12,.08,.07)
    build_dog.IRIS_PUPILLARY=(.36,.19,.078)
    build_dog.IRIS_CILIARY=(.26,.135,.052)
    build_dog.IRIS_OUTER=(.11,.055,.022)
    pixels=build_dog.iris_albedo()
    Image.fromarray(np.uint8(np.clip(pixels*255+.5,0,255))).save(output/'BerneseIrisColor.png')
    for key in ('eyeRadius','lidInnerRadius','socketRadius'):rig[key]*=1.18
    rig['assembly']['lid']=rig['assembly']['pelt']
    rig['eyeProfile']='Models/Bernese/BerneseEye.oloskin'
    (output/'Bernese.rig.json').write_text(json.dumps(rig,indent=2)+'\n')
    report=dict(rows=rows,animations=animation_report,closedBlinkUsesOriginalAperture=True)
    (output/'bernese-expression-refinement.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();build(a.source,a.output)
