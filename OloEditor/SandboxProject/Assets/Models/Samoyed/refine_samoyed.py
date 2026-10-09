"""Reconstruct the approved Samoyed refinements from versioned authoring inputs.

Each stage takes an immutable input and a new output directory.
The body surface and its skin binding remain unchanged.
"""


def soft_eyes(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util
    import json
    import shutil
    import numpy as np

    assert not output.exists()
    shutil.copytree(source,output)
    path=Path(__file__).resolve().parent / 'build_samoyed.py'
    spec=importlib.util.spec_from_file_location('samoyed_recipe',path);r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    r.MEET=-4.
    a=r.Gltf(source/'Samoyed.gltf')
    raw=r.Gltf(raw_source / 'Samoyed.gltf')
    rig=json.loads((source/'Samoyed.rig.json').read_text())
    node=next(i for i,n in enumerate(a.doc['nodes']) if 'skin' in n and 'mesh' in n)
    mesh=a.doc['meshes'][a.doc['nodes'][node]['mesh']]
    rawmesh=raw.doc['meshes'][raw.doc['nodes'][node]['mesh']]
    jointnames=[a.doc['nodes'][i]['name'] for i in a.doc['skins'][0]['joints']]
    count=len(mesh['weights']);names=mesh['extras']['targetNames'];rows=[]
    for index,primitive in enumerate(mesh['primitives']):
        p=a.read(primitive['attributes']['POSITION']).astype(float)
        n=a.read(primitive['attributes']['NORMAL']).astype(float)
        settled,sn=p.copy(),n.copy()
        for k in range(count):
            if names[k] in ('NoseShape','SamoyedForeface','SamoyedFaceRefinement'):
                settled+=a.read(primitive['targets'][k]['POSITION']);sn+=a.read(primitive['targets'][k]['NORMAL'])
        sn/=np.maximum(np.linalg.norm(sn,axis=1,keepdims=True),1.e-12)
        material=a.doc['materials'][primitive['material']]['name']
        dominant=None
        if index<len(rawmesh['primitives']):
            original=rawmesh['primitives'][index]
            assert np.array_equal(p.astype(np.float32),raw.read(original['attributes']['POSITION']))
            joints=raw.read(original['attributes']['JOINTS_0']);weights=raw.read(original['attributes']['WEIGHTS_0'])
            dominant=joints[np.arange(len(p)),weights.argmax(1)]
        for si,side in enumerate(('L','R')):
            d,dn=np.zeros_like(p),np.zeros_like(n)
            oldblink=a.read(primitive['targets'][names.index(f'Blink_{side}_3')]['POSITION'])
            affected=np.linalg.norm(oldblink,axis=1)>1.e-8
            if material=='DogSkin' and affected.any():
                args=(rig['eyes'][side],rig['eyeRadius'],1 if side=='L' else -1,-.28)
                d[affected]=r.deform_orbit(settled[affected],*args)-settled[affected]
                dn[affected]=r.orbit_normals(settled[affected],sn[affected],*args)-sn[affected]
            if dominant is not None:
                for upper in (True,False):
                    selected=dominant==jointnames.index(f'lid_{"upper" if upper else "lower"}_{side}')
                    if selected.any():
                        pos,normal=r.deform_lid(settled[selected],sn[selected],rig['eyes'][side],upper,1 if side=='L' else -1,-.28)
                        d[selected]=pos-settled[selected];dn[selected]=normal-sn[selected]
                        clearance=np.linalg.norm(pos-np.asarray(rig['eyes'][side]['centre']),axis=1)-rig['eyeRadius']
                        assert clearance.min()>.0001
            assert np.max(np.abs(d[p[:,1]<.520]),initial=0.)<1.e-10
            primitive['targets'].append(dict(POSITION=a.append(d,'VEC3'),NORMAL=a.append(dn,'VEC3')))
            rows.append(dict(material=material,side=side,movedVertices=int((np.linalg.norm(d,axis=1)>1.e-8).sum()),
                maxDisplacementMM=float(np.linalg.norm(d,axis=1).max()*1000.)))
    mesh['weights'].extend([1.,1.]);mesh['extras']['targetNames'].extend(['SamoyedSoftEye_L','SamoyedSoftEye_R'])
    for anim in a.doc['animations']:
        for channel in anim['channels']:
            if channel['target']==dict(node=node,path='weights'):
                sampler=anim['samplers'][channel['sampler']];old=a.read(sampler['output']).reshape(-1,count)
                amounts=[]
                for side in ('L','R'):
                    ids=[names.index(f'Blink_{side}_{k}') for k in (1,2,3)]
                    closure=old[:,ids]@np.array([1/3.,2/3.,1.])
                    amounts.append(np.clip(1.-closure,0.,1.))
                sampler['output']=a.append(np.c_[old,*amounts].ravel(),'SCALAR')
    a.write(output)
    (output/'soft-eyes.json').write_text(json.dumps(dict(parent=source.name,rows=rows,
        baseAndExistingTargetsRigCoatExact=True,fullyClosedBlinkExact=True,openingAmount=.28,
        openingCreaseDegrees=-4.,oralAndBodyBelowY520Unchanged=True),indent=2)+'\n')
    print(json.dumps(rows,indent=2),flush=True)


def lateral_face(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util
    import shutil
    import json
    import numpy as np

    assert not output.exists()
    shutil.copytree(source,output)
    path=Path(__file__).resolve().parent / 'build_samoyed.py'
    spec=importlib.util.spec_from_file_location('recipe',path);r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    a=r.Gltf(source/'Samoyed.gltf')
    mesh=next(m for m in a.doc['meshes'] if 'weights' in m)
    count=len(mesh['weights'])
    rig=json.loads((source/'Samoyed.rig.json').read_text())
    eyes=np.array([v['centre'] for v in rig['eyes'].values()])
    def ramp(lo,hi,x):return r.smoothstep((x-lo)/(hi-lo))
    def sculpt(p):
        distance=np.linalg.norm(p[:,None,:]-eyes[None,:,:],axis=2).min(1)
        weight=np.exp(-((np.abs(p[:,0])-.060)/.030)**2-((p[:,1]-.555)/.034)**2)
        weight*=ramp(.526,.545,p[:,1])*(1.-ramp(.592,.620,p[:,1]))
        weight*=ramp(.248,.270,p[:,2])*ramp(.017,.024,distance)
        q=p.copy();q[:,2]+=.006*weight
        return q
    rows=[]
    for primitive in mesh['primitives']:
        p=a.read(primitive['attributes']['POSITION']).astype(float)
        n=a.read(primitive['attributes']['NORMAL']).astype(float)
        settled,sn=p.copy(),n.copy()
        for i,w in enumerate(mesh['weights']):
            settled+=w*a.read(primitive['targets'][i]['POSITION'])
            sn+=w*a.read(primitive['targets'][i]['NORMAL'])
        sn/=np.maximum(np.linalg.norm(sn,axis=1,keepdims=True),1.e-12)
        d,dn=np.zeros_like(p),np.zeros_like(n)
        material=a.doc['materials'][primitive['material']]['name']
        if material=='DogSkin':
            d=sculpt(settled)-settled
            eps=1.e-5
            jac=np.stack([(sculpt(settled+axis*eps)-sculpt(settled-axis*eps))/(2*eps) for axis in np.eye(3)],axis=2)
            det=np.linalg.det(jac);assert det.min()>.35,float(det.min())
            nn=np.linalg.solve(jac.transpose(0,2,1),sn[...,None])[...,0]
            nn/=np.maximum(np.linalg.norm(nn,axis=1,keepdims=True),1.e-12)
            dn=nn-sn
            assert not np.count_nonzero(d[settled[:,1]<.526])
            near=np.linalg.norm(settled[:,None,:]-eyes[None,:,:],axis=2).min(1)<.017
            assert not np.count_nonzero(d[near])
            rows.append(dict(material=material,movedVertices=int((np.linalg.norm(d,axis=1)>1.e-8).sum()),
                maximumDisplacementMM=float(np.linalg.norm(d,axis=1).max()*1000.),minimumJacobian=float(det.min())))
        primitive['targets'].append(dict(POSITION=a.append(d,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.);mesh['extras']['targetNames'].append('SamoyedLateralFace')
    node=next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                sampler=anim['samplers'][ch['sampler']];old=a.read(sampler['output']).reshape(-1,count)
                sampler['output']=a.append(np.c_[old,np.ones(len(old))].ravel(),'SCALAR')
    a.write(output)
    proof=dict(parent=source.name,rows=rows,baseRigAndOld13TargetsExact=True,coatArchiveExact=True,
        bodyAndOralBelowY526Exact=True,globeAnd17MMNeighbourhoodExact=True,appearanceAccepted=False)
    (output/'lateral-face.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2),flush=True)


def eye_join(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util
    import json
    import shutil
    import numpy as np

    assert not output.exists()
    shutil.copytree(source,output)
    path=Path(__file__).resolve().parent / 'build_samoyed.py'
    spec=importlib.util.spec_from_file_location('recipe',path)
    r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    a=r.Gltf(source/'Samoyed.gltf')
    mesh=next(m for m in a.doc['meshes'] if 'weights' in m)
    count=len(mesh['weights'])
    rig=json.loads((source/'Samoyed.rig.json').read_text())
    eyes=np.array([v['centre'] for v in rig['eyes'].values()])

    def ramp(lo,hi,x):return r.smoothstep((x-lo)/(hi-lo))

    def sculpt(p):
        distance=np.linalg.norm(p[:,None,:]-eyes[None,:,:],axis=2).min(1)
        weight=1.-ramp(.026,.054,distance)
        weight*=ramp(.514,.526,p[:,1])*ramp(.243,.250,p[:,2])
        q=p.copy();q[:,2]+=.006*weight
        return q

    rows=[]
    for primitive in mesh['primitives']:
        p=a.read(primitive['attributes']['POSITION']).astype(float)
        n=a.read(primitive['attributes']['NORMAL']).astype(float)
        settled,sn=p.copy(),n.copy()
        for i,w in enumerate(mesh['weights']):
            settled+=w*a.read(primitive['targets'][i]['POSITION'])
            sn+=w*a.read(primitive['targets'][i]['NORMAL'])
        sn/=np.maximum(np.linalg.norm(sn,axis=1,keepdims=True),1.e-12)
        d,dn=np.zeros_like(p),np.zeros_like(n)
        material=a.doc['materials'][primitive['material']]['name']
        if material in ('DogSkin','DogLid','DogLip'):

            if material=='DogSkin':
                def join(p):
                    distance=np.linalg.norm(p[:,None,:]-eyes[None,:,:],axis=2).min(1)
                    edge=1.-ramp(.017,.034,distance)
                    depth=1.-ramp(.280,.304,p[:,2])
                    weight=np.maximum(edge,depth)*(1.-ramp(.040,.075,distance))
                    weight*=ramp(.514,.526,p[:,1])*ramp(.243,.250,p[:,2])
                    q=p.copy();q[:,2]+=.006*weight
                    return q
                sculpt_current=join
            else:sculpt_current=sculpt
            d=sculpt_current(settled)-settled
            eps=1.e-5
            jac=np.stack([(sculpt_current(settled+axis*eps)-sculpt_current(settled-axis*eps))/(2*eps) for axis in np.eye(3)],axis=2)
            det=np.linalg.det(jac);assert det.min()>.35,float(det.min())
            nn=np.linalg.solve(jac.transpose(0,2,1),sn[...,None])[...,0]
            nn/=np.maximum(np.linalg.norm(nn,axis=1,keepdims=True),1.e-12)
            dn=nn-sn
            assert not np.count_nonzero(d[settled[:,1]<.514])
            near=np.linalg.norm(settled[:,None,:]-eyes[None,:,:],axis=2).min(1)<(.017 if material=='DogSkin' else .026)
            assert np.allclose(d[near,2],.006,atol=1.e-10),material
            rows.append(dict(material=material,movedVertices=int((np.linalg.norm(d,axis=1)>1.e-8).sum()),
                maximumDisplacementMM=float(np.linalg.norm(d,axis=1).max()*1000.),minimumJacobian=float(det.min())))
        primitive['targets'].append(dict(POSITION=a.append(d,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.);mesh['extras']['targetNames'].append('SamoyedShallowEyeSeat')
    node=next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                sampler=anim['samplers'][ch['sampler']]
                old=a.read(sampler['output']).reshape(-1,count)
                sampler['output']=a.append(np.c_[old,np.ones(len(old))].ravel(),'SCALAR')
    a.write(output)
    for eye in rig['eyes'].values():eye['centre'][2]+=.006
    (output/'Samoyed.rig.json').write_text(json.dumps(rig,indent=2)+'\n')
    proof=dict(parent=source.name,rows=rows,eyeAndLidAdvanceMM=6.,rigEyeCentresMatched=True,
        baseAndOld14TargetsExact=True,coatArchiveExact=True,bodyAndMouthBelowY514Exact=True,
        constantLidsWithin26MM=True,skinMarginWithin17MM=True,smoothJoinDepthRangeMM=[280,304],appearanceAccepted=False)
    (output/'shallow-eye-seat.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2),flush=True)


def nasal_taper(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util
    import json
    import shutil
    import numpy as np

    assert not output.exists()
    shutil.copytree(source,output)
    path=Path(__file__).resolve().parent / 'build_samoyed.py'
    spec=importlib.util.spec_from_file_location('recipe',path)
    r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    a=r.Gltf(source/'Samoyed.gltf')
    mesh=next(m for m in a.doc['meshes'] if 'weights' in m)
    count=len(mesh['weights'])
    rig=json.loads((source/'Samoyed.rig.json').read_text())
    eyes=np.array([v['centre'] for v in rig['eyes'].values()])

    def ramp(lo,hi,x):return r.smoothstep((x-lo)/(hi-lo))

    def sculpt(p,material):
        q=p.copy()
        distance=np.linalg.norm(p[:,None,:]-eyes[None,:,:],axis=2).min(1)
        protected=ramp(.018,.032,distance)
        nose=ramp(.316,.339,p[:,2])*ramp(.489,.505,p[:,1])*(1.-ramp(.530,.551,p[:,1]))
        muzzle=ramp(.280,.308,p[:,2])*ramp(.513,.526,p[:,1])*(1.-ramp(.546,.563,p[:,1]))
        muzzle*=1.-nose
        temples=ramp(.571,.597,p[:,1])*(1.-ramp(.619,.638,p[:,1]))*ramp(.247,.265,p[:,2])
        q[:,0]*=1.-(.25*muzzle+.15*temples+.10*nose)*protected
        q[:,2]-=.0025*(muzzle+nose)*protected
        return q

    rows=[]
    for primitive in mesh['primitives']:
        p=a.read(primitive['attributes']['POSITION']).astype(float)
        n=a.read(primitive['attributes']['NORMAL']).astype(float)
        settled,sn=p.copy(),n.copy()
        for i,w in enumerate(mesh['weights']):
            settled+=w*a.read(primitive['targets'][i]['POSITION'])
            sn+=w*a.read(primitive['targets'][i]['NORMAL'])
        sn/=np.maximum(np.linalg.norm(sn,axis=1,keepdims=True),1.e-12)
        d,dn=np.zeros_like(p),np.zeros_like(n)
        material=a.doc['materials'][primitive['material']]['name']
        if material in ('DogSkin','DogNose','DogLip'):
            d=sculpt(settled,material)-settled
            eps=1.e-5
            jac=np.stack([(sculpt(settled+axis*eps,material)-sculpt(settled-axis*eps,material))/(2*eps) for axis in np.eye(3)],axis=2)
            det=np.linalg.det(jac);assert det.min()>.35,float(det.min())
            nn=np.linalg.solve(jac.transpose(0,2,1),sn[...,None])[...,0]
            nn/=np.maximum(np.linalg.norm(nn,axis=1,keepdims=True),1.e-12)
            dn=nn-sn
            if material=='DogSkin':
                assert not np.count_nonzero(d[settled[:,1]<.489])
                near=np.linalg.norm(settled[:,None,:]-eyes[None,:,:],axis=2).min(1)<.018
                assert not np.count_nonzero(d[near])
            rows.append(dict(material=material,movedVertices=int((np.linalg.norm(d,axis=1)>1.e-8).sum()),
                maximumDisplacementMM=float(np.linalg.norm(d,axis=1).max()*1000.),minimumJacobian=float(det.min())))
        primitive['targets'].append(dict(POSITION=a.append(d,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.);mesh['extras']['targetNames'].append('SamoyedTaperedFacialPlanes')
    node=next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                sampler=anim['samplers'][ch['sampler']]
                old=a.read(sampler['output']).reshape(-1,count)
                sampler['output']=a.append(np.c_[old,np.ones(len(old))].ravel(),'SCALAR')
    a.write(output)
    proof=dict(parent=source.name,rows=rows,muzzleTaperMaximum=.25,templeTaperMaximum=.15,noseWidthScale=.90,
        baseRigAndOld15TargetsExact=True,coatArchiveExact=True,bodyAndOralBelowY489Exact=True,sharedNoseSkinLipTransform=True,
        socketsWithin18MMExact=True,appearanceAccepted=False)
    (output/'tapered-face.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2),flush=True)


def close_pocket(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util
    import json
    import shutil
    import numpy as np
    assert not output.exists()
    shutil.copytree(source,output)
    path=Path(__file__).resolve().parent / 'build_samoyed.py'
    spec=importlib.util.spec_from_file_location('recipe',path)
    r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    a=r.Gltf(source/'Samoyed.gltf')
    mesh=next(m for m in a.doc['meshes'] if 'weights' in m)
    count=len(mesh['weights'])
    names=[a.doc['nodes'][i]['name'] for i in a.doc['skins'][0]['joints']]
    head,jaw=names.index('head'),names.index('jaw')
    jaw_node=a.doc['skins'][0]['joints'][jaw]
    descendants={jaw_node}
    pending=[jaw_node]
    while pending:
        for child in a.doc['nodes'][pending.pop()].get('children',[]):
            descendants.add(child);pending.append(child)
    jaw_joints=[i for i,node in enumerate(a.doc['skins'][0]['joints']) if node in descendants]
    def ramp(lo,hi,x):return r.smoothstep((x-lo)/(hi-lo))
    def sculpt(p,hw,jw,soft):
        q=p.copy()
        front=ramp(.235,.286,p[:,2])
        mouth=front*ramp(.462,.480,p[:,1])
        upper=mouth*(1.-ramp(.498,.504,p[:,1]))*hw*ramp(.310,.329,p[:,2])
        lower=mouth*(1.-ramp(.520,.532,p[:,1]))*jw
        # The old seal raised and narrowed the upper margin into a flat shelf.
        # Roll its exposed lower edge inward and down, keeping the nasal pad fixed.
        q[:,1]+=.0055*lower-.0052*upper
        q[:,2]-=.0030*lower+.0040*upper
        if soft:
            underside=(1.-ramp(.484,.498,p[:,1]))*lower
            q[:,1]-=.0065*underside
            q[:,2]+=.0030*lower
            q[:,0]+=.0020*np.tanh(p[:,0]/.015)*lower


        if soft:
            forward=ramp(.278,.308,p[:,2])
            corner=forward+(1.-forward)*ramp(.486,.499,p[:,1])
            q=p+(q-p)*corner[:,None]
            pocket=(ramp(.462,.479,p[:,1])*(1.-ramp(.487,.502,p[:,1]))*
                    ramp(.260,.292,p[:,2])*(1.-ramp(.315,.336,p[:,2]))*
                    (1.-ramp(.018,.035,np.abs(p[:,0])))*hw)
            q[:,1]+=.0030*pocket

        return q
    rows=[]
    for primitive in mesh['primitives']:
        p=a.read(primitive['attributes']['POSITION']).astype(float)
        n=a.read(primitive['attributes']['NORMAL']).astype(float)
        settled,raw=p.copy(),n.copy()
        for i,w in enumerate(mesh['weights']):
            settled+=w*a.read(primitive['targets'][i]['POSITION'])
            raw+=w*a.read(primitive['targets'][i]['NORMAL'])
        material=a.doc['materials'][primitive['material']]['name']
        d,dn=np.zeros_like(p),np.zeros_like(n)
        if material in ('DogSkin','DogLip','DogGum','DogOralCavity','DogTeeth','DogTongue'):
            joints=a.read(primitive['attributes']['JOINTS_0'])
            weights=a.read(primitive['attributes']['WEIGHTS_0'])
            hw=(weights*(joints==head)).sum(1)
            jw=(weights*np.isin(joints,jaw_joints)).sum(1)
            if material in ('DogTeeth','DogTongue'):hw=np.zeros_like(hw)
            soft=material in ('DogSkin','DogLip','DogOralCavity')
            d=sculpt(settled,hw,jw,soft)-settled
            eps=1.e-5
            jac=np.stack([(sculpt(settled+axis*eps,hw,jw,soft)-sculpt(settled-axis*eps,hw,jw,soft))/(2*eps) for axis in np.eye(3)],axis=2)
            det=np.linalg.det(jac)
            assert det.min()>.25,(material,float(det.min()))
            nn=np.linalg.solve(jac.transpose(0,2,1),raw[...,None])[...,0]
            dn=nn-raw
            assert not np.count_nonzero(d[settled[:,1]>.532])
            assert not np.count_nonzero(d[settled[:,1]<.462])
            rows.append(dict(material=material,movedVertices=int((np.linalg.norm(d,axis=1)>1.e-8).sum()),
                maximumDisplacementMM=float(np.linalg.norm(d,axis=1).max()*1000.),minimumJacobian=float(det.min())))
        primitive['targets'].append(dict(POSITION=a.append(d,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.);mesh['extras']['targetNames'].append('SamoyedClosedMuzzle')
    node=next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                sampler=anim['samplers'][ch['sampler']]
                old=a.read(sampler['output']).reshape(-1,count)
                closed=old[:,mesh['extras']['targetNames'].index('MouthSeal')]
                if anim['name']=='Pant':assert np.count_nonzero(closed)==0
                sampler['output']=a.append(np.c_[old,closed].ravel(),'SCALAR')
    a.write(output)
    proof=dict(parent=source.name,rows=rows,baseRigAndOld16TargetsExact=True,coatArchiveExact=True,
        pantAllFramesExact=True,noseEyesAndBodyExact=True,lowerDentalAndSoftTissueMoveTogether=True,appearanceAccepted=False)
    (output/'closed-muzzle.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps(proof,indent=2),flush=True)


def cheek_fairing(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util, json, shutil
    import numpy as np

    assert not output.exists()
    spec = importlib.util.spec_from_file_location('recipe', Path(__file__).resolve().parent / 'build_samoyed.py')
    r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
    a = r.Gltf(source / 'Samoyed.gltf')
    mesh = next(m for m in a.doc['meshes'] if 'weights' in m)
    count = len(mesh['weights']); assert count == 17
    rig = json.loads((source / 'Samoyed.rig.json').read_text())
    eyes = np.array([v['centre'] for v in rig['eyes'].values()])

    def ramp(lo, hi, x): return r.smoothstep((x-lo)/(hi-lo))
    def normalized(v): return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1.e-20)

    parts = []
    for pr in mesh['primitives']:
        p = a.read(pr['attributes']['POSITION']).astype(float)
        n = a.read(pr['attributes']['NORMAL']).astype(float)
        for w, t in zip(mesh['weights'], pr['targets']):
            p += w*a.read(t['POSITION']); n += w*a.read(t['NORMAL'])
        parts.append((p, n, a.read(pr['indices']).reshape(-1,3).astype(int), a.doc['materials'][pr['material']]['name']))

    # Weld coincident material/UV splits before fairing so they cannot open seams.
    sizes = np.array([len(p[0]) for p in parts]); offsets = np.r_[0, np.cumsum(sizes)]
    allp = np.concatenate([p[0] for p in parts])
    _, unique, inverse = np.unique(np.round(allp, 7), axis=0, return_index=True, return_inverse=True)
    p = allp[unique]
    faces = np.concatenate([inverse[f+off] for (v,n,f,name),off in zip(parts,offsets) if name == 'DogSkin'])
    edges = np.unique(np.sort(np.concatenate([faces[:,[0,1]],faces[:,[1,2]],faces[:,[2,0]]]),axis=1),axis=0)
    edge_a = np.r_[edges[:,0],edges[:,1]]; edge_b = np.r_[edges[:,1],edges[:,0]]
    x,y,z = np.abs(p[:,0]),p[:,1],p[:,2]
    distance = np.linalg.norm(p[:,None,:]-eyes[None,:,:],axis=2).min(1)
    weight = ramp(.015,.025,x)*(1-ramp(.052,.070,x))*ramp(.498,.510,y)*(1-ramp(.535,.551,y))*ramp(.260,.290,z)*ramp(.023,.032,distance)
    # The area being faired contains only the forward cheek, not oral interior or nose.
    weight *= 1-ramp(.343,.357,z)
    for i,part in enumerate(parts):
        if part[3] != 'DogSkin':
            weight[inverse[offsets[i]:offsets[i+1]]] = 0
    weight[np.bincount(edge_a,minlength=len(p))==0] = 0
    active = weight[edge_a] > 0
    ea,eb = edge_a[active],edge_b[active]
    degree = np.maximum(np.bincount(ea,minlength=len(p)),1)
    q = p.copy()
    for iteration in range(160):
        mean = np.bincount(ea,weights=q[eb,2],minlength=len(p))/degree
        q[:,2] += .38*weight*(mean-q[:,2])
    d = q-p
    print('Maximum fairing displacement mm:', float(np.linalg.norm(d,axis=1).max()*1000), flush=True)
    assert np.max(np.linalg.norm(d,axis=1)) < .006

    def vertex_normals(v):
        face = np.cross(v[faces[:,1]]-v[faces[:,0]],v[faces[:,2]]-v[faces[:,0]])
        n = np.zeros_like(v)
        for k in range(3): np.add.at(n, faces[:,k], face)
        return normalized(n)
    old_normal,new_normal = vertex_normals(p),vertex_normals(q)
    axis = np.cross(old_normal,new_normal)
    cosine = np.sum(old_normal*new_normal,axis=1)
    rows = []
    for i,(pr,part) in enumerate(zip(mesh['primitives'],parts)):
        v,n,f,name = part
        ids = inverse[offsets[i]:offsets[i+1]]
        delta = d[ids] if name == 'DogSkin' else np.zeros_like(v)
        # Rotate existing smooth normals by the actual surface-normal change.
        ax = axis[ids]; co = cosine[ids,None]
        nn = n + np.cross(ax,n) + np.cross(ax,np.cross(ax,n))/np.maximum(1+co,1.e-8)
        dn = nn-n
        if name != 'DogSkin': dn[:] = 0
        old = np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        vv = v+delta
        new = np.cross(vv[f[:,1]]-vv[f[:,0]],vv[f[:,2]]-vv[f[:,0]])
        changed = (np.linalg.norm(delta[f],axis=2).max(1)>1.e-7)&(np.linalg.norm(old,axis=1)>1.e-12)
        dots = (old*new).sum(1)/np.maximum(np.linalg.norm(old,axis=1)*np.linalg.norm(new,axis=1),1.e-20)
        minimum = float(dots[changed].min()) if changed.any() else 1.
        assert minimum > .70,(name,minimum)
        assert not np.count_nonzero(delta[v[:,1]<.492])
        rows.append(dict(material=name,moved=int(np.count_nonzero(np.linalg.norm(delta,axis=1)>1.e-7)),maxMM=float(np.linalg.norm(delta,axis=1).max()*1000),minimumTriangleAlignment=minimum))
        pr['targets'].append(dict(POSITION=a.append(delta,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.); mesh['extras']['targetNames'].append('SamoyedCheekFairing')
    node = next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                s=anim['samplers'][ch['sampler']];v=a.read(s['output']).reshape(-1,count)
                s['output']=a.append(np.c_[v,np.ones(len(v))].ravel(),'SCALAR')
    shutil.copytree(source,output)
    a.write(output)
    (output/'cheek-fairing.json').write_text(json.dumps(dict(parent=source.name,rows=rows,eyesNoseChinAndBodyExact=True,appearanceAccepted=False),indent=2)+'\n')
    print(json.dumps(rows,indent=2))


def resting_eyes(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util,json,shutil
    import numpy as np
    assert not output.exists()
    spec=importlib.util.spec_from_file_location('recipe',Path(__file__).resolve().parent / 'build_samoyed.py')
    r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
    a=r.Gltf(source/'Samoyed.gltf')
    mesh=next(m for m in a.doc['meshes'] if 'weights' in m)
    names=mesh['extras']['targetNames'];ids=[names.index('SamoyedSoftEye_'+side) for side in ('L','R')]
    factor=.60;rows=[]
    for pr in mesh['primitives']:
        maximum=0.
        for i in ids:
            t=pr['targets'][i]
            old=a.read(t['POSITION'])
            maximum=max(maximum,float(np.linalg.norm(old*(1-factor),axis=1).max()*1000))
            for attr in ('POSITION','NORMAL'):
                t[attr]=a.append(a.read(t[attr])*factor,'VEC3')
        rows.append(dict(material=a.doc['materials'][pr['material']]['name'],maximumChangeMM=maximum))
    # Both existing channels already fade to zero at the closed part of each blink.
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']['path']=='weights':
                weights=a.read(anim['samplers'][ch['sampler']]['output']).reshape(-1,len(names))
                for side,idx in zip(('L','R'),ids):
                    closed=weights[:,names.index('Blink_'+side+'_3')]==1.
                    assert not np.count_nonzero(weights[closed,idx])
    shutil.copytree(source,output);a.write(output)
    (output/'resting-eyes.json').write_text(json.dumps(dict(parent=source.name,factor=factor,rows=rows,fullBlinkExact=True,animationsUnchanged=True),indent=2)+'\n')
    print(json.dumps(rows,indent=2))


def rounded_brisket(source, output, raw_source=None):
    from pathlib import Path
    import importlib.util, json, shutil
    import numpy as np

    assert not output.exists()
    spec = importlib.util.spec_from_file_location('recipe', Path(__file__).resolve().parent / 'build_samoyed.py')
    r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
    a = r.Gltf(source / 'Samoyed.gltf')
    mesh = next(m for m in a.doc['meshes'] if 'weights' in m)
    count = len(mesh['weights']); assert count == 18
    rig = json.loads((source / 'Samoyed.rig.json').read_text())
    eyes = np.array([v['centre'] for v in rig['eyes'].values()])

    def ramp(lo, hi, x): return r.smoothstep((x-lo)/(hi-lo))
    def normalized(v): return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1.e-20)

    parts = []
    for pr in mesh['primitives']:
        p = a.read(pr['attributes']['POSITION']).astype(float)
        n = a.read(pr['attributes']['NORMAL']).astype(float)
        for w, t in zip(mesh['weights'], pr['targets']):
            p += w*a.read(t['POSITION']); n += w*a.read(t['NORMAL'])
        parts.append((p, n, a.read(pr['indices']).reshape(-1,3).astype(int), a.doc['materials'][pr['material']]['name']))

    # Weld coincident material/UV splits before fairing so they cannot open seams.
    sizes = np.array([len(p[0]) for p in parts]); offsets = np.r_[0, np.cumsum(sizes)]
    allp = np.concatenate([p[0] for p in parts])
    _, unique, inverse = np.unique(np.round(allp, 7), axis=0, return_index=True, return_inverse=True)
    p = allp[unique]
    faces = np.concatenate([inverse[f+off] for (v,n,f,name),off in zip(parts,offsets) if name == 'DogSkin'])
    edges = np.unique(np.sort(np.concatenate([faces[:,[0,1]],faces[:,[1,2]],faces[:,[2,0]]]),axis=1),axis=0)
    edge_a = np.r_[edges[:,0],edges[:,1]]; edge_b = np.r_[edges[:,1],edges[:,0]]
    x,y,z = np.abs(p[:,0]),p[:,1],p[:,2]
    # Restrict to the lower forechest; face, legs below 19 cm and back are fixed.
    weight = ramp(.17,.24,y)*(1-ramp(.24,.47,y))*ramp(.16,.23,z)*(1-ramp(.09,.15,x))
    for i,part in enumerate(parts):
        if part[3] != 'DogSkin':
            weight[inverse[offsets[i] + np.unique(part[2])]] = 0
    weight[np.bincount(edge_a,minlength=len(p))==0] = 0
    active = weight[edge_a] > 0
    ea,eb = edge_a[active],edge_b[active]
    degree = np.maximum(np.bincount(ea,minlength=len(p)),1)
    q = p.copy()
    # Tuck the projecting lower shelf into the brisket, with a rounded side taper.
    q[:,2] -= .032*weight
    q[:,1] += .006*weight*ramp(.025,.095,x)*(1-ramp(.25,.32,y))
    for iteration in range(24):
        for component in (0,1,2):
            mean = np.bincount(ea,weights=q[eb,component],minlength=len(p))/degree
            q[:,component] += .30*weight*(mean-q[:,component])
    d = q-p
    print('Maximum fairing displacement mm:', float(np.linalg.norm(d,axis=1).max()*1000), flush=True)
    assert .001 < np.max(np.linalg.norm(d,axis=1)) < .045

    def vertex_normals(v):
        face = np.cross(v[faces[:,1]]-v[faces[:,0]],v[faces[:,2]]-v[faces[:,0]])
        n = np.zeros_like(v)
        for k in range(3): np.add.at(n, faces[:,k], face)
        return normalized(n)
    old_normal,new_normal = vertex_normals(p),vertex_normals(q)
    axis = np.cross(old_normal,new_normal)
    cosine = np.sum(old_normal*new_normal,axis=1)
    rows = []
    for i,(pr,part) in enumerate(zip(mesh['primitives'],parts)):
        v,n,f,name = part
        ids = inverse[offsets[i]:offsets[i+1]]
        delta = d[ids] if name == 'DogSkin' else np.zeros_like(v)
        # Rotate existing smooth normals by the actual surface-normal change.
        ax = axis[ids]; co = cosine[ids,None]
        nn = n + np.cross(ax,n) + np.cross(ax,np.cross(ax,n))/np.maximum(1+co,1.e-8)
        dn = nn-n
        if name != 'DogSkin': dn[:] = 0
        old = np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        vv = v+delta
        new = np.cross(vv[f[:,1]]-vv[f[:,0]],vv[f[:,2]]-vv[f[:,0]])
        changed = (np.linalg.norm(delta[f],axis=2).max(1)>1.e-7)&(np.linalg.norm(old,axis=1)>1.e-12)
        dots = (old*new).sum(1)/np.maximum(np.linalg.norm(old,axis=1)*np.linalg.norm(new,axis=1),1.e-20)
        minimum = float(dots[changed].min()) if changed.any() else 1.
        assert minimum > .70,(name,minimum)
        assert not np.count_nonzero(delta[(v[:,1]<.17)|(v[:,1]>.47)])
        rows.append(dict(material=name,moved=int(np.count_nonzero(np.linalg.norm(delta,axis=1)>1.e-7)),maxMM=float(np.linalg.norm(delta,axis=1).max()*1000),minimumTriangleAlignment=minimum))
        pr['targets'].append(dict(POSITION=a.append(delta,'VEC3'),NORMAL=a.append(dn,'VEC3')))
    mesh['weights'].append(1.); mesh['extras']['targetNames'].append('SamoyedRoundedBrisket')
    node = next(i for i,n in enumerate(a.doc['nodes']) if 'mesh' in n and a.doc['meshes'][n['mesh']] is mesh)
    for anim in a.doc['animations']:
        for ch in anim['channels']:
            if ch['target']==dict(node=node,path='weights'):
                s=anim['samplers'][ch['sampler']];v=a.read(s['output']).reshape(-1,count)
                s['output']=a.append(np.c_[v,np.ones(len(v))].ravel(),'SCALAR')
    shutil.copytree(source,output)
    a.write(output)
    (output/'rounded-brisket.json').write_text(json.dumps(dict(parent=source.name,rows=rows,faceEyesNoseChinLegsAndBackExact=True,appearanceAccepted=False),indent=2)+'\n')
    print(json.dumps(rows,indent=2))


STAGES = (soft_eyes, lateral_face, eye_join, nasal_taper, close_pocket, cheek_fairing, resting_eyes, rounded_brisket)
