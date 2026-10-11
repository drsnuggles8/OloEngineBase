"""Refine Bernese orbital proportions and muzzle from the rounded-jaw checkpoint."""
from pathlib import Path
import argparse,json,shutil,re
import numpy as np
from build_bernese import Gltf,ramp,normalize

EYE_SCALE=1.18

def sculpt(p,material,eyes):
    q=p.copy()
    for eye in eyes:
        centre=np.array(eye['centre']); gaze=np.array(eye['gaze'])
        rel=p-centre; radius=np.linalg.norm(rel,axis=1)
        w=1-ramp(.023,.043,radius)
        q+=(EYE_SCALE-1)*rel*w[:,None]
        if material=='DogSkin':
            # Ease the cheek away from the lower lid; the original cheek covers
            # the lower margin and pinches the visible opening into a triangle.
            lower=1-ramp(-.001,.008,rel[:,1])
            front=ramp(.002,.012,rel@gaze)
            orbit=(1-ramp(.024,.034,radius))*lower*front
            q-=.0018*orbit[:,None]*gaze
            q[:,1]-=.0015*orbit
    # Preserve the leather nose and closed mouth line; taper just the fleshy
    # muzzle sides instead of scaling the whole head or shortening the nose.
    if material=='DogSkin':
        x,y,z=abs(p[:,0]),p[:,1],p[:,2]
        cheek=ramp(.487,.536,z)*(1-ramp(.570,.599,z))*ramp(.570,.595,y)*(1-ramp(.629,.655,y))*ramp(.020,.044,x)
        q[:,0]*=1-.075*cheek
    return q

def build(source,output):
    assert not output.exists()
    asset=Gltf(source/'Bernese.gltf'); mesh=asset.doc['meshes'][0]
    assert mesh['extras']['targetNames']==['BerneseSoftNasalBridge','BerneseRoundedJawAndPasterns']
    count=len(mesh['weights'])
    rig=json.loads((source/'Bernese.rig.json').read_text()); eyes=list(rig['eyes'].values())
    rows=[]
    for primitive in mesh['primitives']:
        p=asset.read(primitive['attributes']['POSITION']).astype(float)
        n=asset.read(primitive['attributes']['NORMAL']).astype(float)
        for w,t in zip(mesh['weights'],primitive['targets']):
            p+=w*asset.read(t['POSITION']); n+=w*asset.read(t['NORMAL'])
        n=normalize(n)
        material=asset.doc['materials'][primitive['material']]['name']
        q=sculpt(p,material,eyes); delta=q-p
        eps=1e-5
        jac=np.stack([(sculpt(p+a*eps,material,eyes)-sculpt(p-a*eps,material,eyes))/(2*eps) for a in np.eye(3)],axis=2)
        determinant=np.linalg.det(jac)
        assert determinant.min()>.35,(material,determinant.min())
        nn=normalize(np.linalg.solve(jac.transpose(0,2,1),n[...,None])[...,0])
        tri=asset.read(primitive['indices']).reshape(-1,3).astype(int)
        old=np.cross(p[tri[:,1]]-p[tri[:,0]],p[tri[:,2]]-p[tri[:,0]])
        new=np.cross(q[tri[:,1]]-q[tri[:,0]],q[tri[:,2]]-q[tri[:,0]])
        valid=np.linalg.norm(old,axis=1)>1e-12
        alignment=np.sum(normalize(old)*normalize(new),axis=1)[valid].min()
        assert alignment>.6,(material,alignment)
        assert np.allclose(delta[p[:,1]<.57],0,atol=1e-12)
        primitive['targets'].append(dict(POSITION=asset.append(delta,'VEC3'),NORMAL=asset.append(nn-n,'VEC3')))
        rows.append(dict(material=material,maxMM=float(np.linalg.norm(delta,axis=1).max()*1000),minimumJacobian=float(determinant.min()),minimumTriangleAlignment=float(alignment)))
    mesh['weights'].append(1.)
    mesh['extras']['targetNames'].append('BerneseSoftEyeAndMuzzle')
    for animation in asset.doc['animations']:
        for channel in animation['channels']:
            if channel['target']['path']=='weights':
                sampler=animation['samplers'][channel['sampler']]
                old=asset.read(sampler['output']).reshape(-1,count)
                sampler['output']=asset.append(np.c_[old,np.ones(len(old))].ravel(),'SCALAR')
    for material in asset.doc['materials']:
        if material['name']=='DogNose':material['normalTexture']['scale']=1.05
    shutil.copytree(source,output)
    asset.doc['buffers'][0].update(uri='Bernese.bin',byteLength=len(asset.data))
    (output/'Bernese.gltf').write_text(json.dumps(asset.doc,indent=2)+'\n')
    (output/'Bernese.bin').write_bytes(asset.data)
    scene=(source/'Bernese.olo').read_text()
    value=rig['eyeRadius']*EYE_SCALE
    scene,n=re.subn(r'Scale: \[0\.0157696, 0\.0157696, 0\.0157696\]',f'Scale: [{value}, {value}, {value}]',scene)
    assert n==2
    (output/'Bernese.olo').write_text(scene)
    profile=(source/'BerneseEye.oloskin').read_text()
    profile=profile.replace('    - 0.25\n    - 0.34\n    - 0.43','    - 0.46\n    - 0.48\n    - 0.50')
    (output/'BerneseEye.oloskin').write_text(profile)
    report=dict(parent=source.name,eyeScale=EYE_SCALE,rows=rows,bodyLegsAndJawBelow570mmExact=True)
    (output/'bernese-face-refinement.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();build(args.source,args.output)
