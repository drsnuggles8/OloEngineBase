"""Round the lower face and refine Bernese leg shape and tricolour markings.

Run on the soft-bridge appearance checkpoint, using a new output directory.
The original body/rig/binding surface and groom stay intact; shape changes are
one additional static morph. The leg pigment is baked onto the existing UVs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import sys

import numpy as np
from PIL import Image
from build_bernese import Gltf, ramp, normalize


def sculpt(p, material='DogSkin'):
    q = p.copy()
    x, y, z = abs(p[:, 0]), p[:, 1], p[:, 2]
    lower = (ramp(.420, .450, z) * ramp(.505, .540, y) *
             (1-ramp(.588, .619, y)) * (1-ramp(.060, .078, x)))
    rear = 1-ramp(.495, .584, z)
    # Sweep the rear jaw up toward the cheek, with a softly rounded front chin.
    q[:, 1] += .012 * lower * rear
    chin = ramp(.526, .568, z) * (1-ramp(.018, .044, x))
    q[:, 1] -= .0035 * lower * chin
    q[:, 0] *= 1-.08 * lower * rear
    q[:, 2] -= .003 * lower * rear
    # Keep paw contacts and leg length. Give the pastern a clearer taper and
    # broaden the paw slightly above its planted underside.
    front = ramp(.105, .135, z)
    hind = 1-ramp(-.125, -.085, z)
    leg = (front+hind) * (1-ramp(.275, .335, y)) * ramp(.025, .045, x)
    ankle = ramp(.055, .095, y) * (1-ramp(.175, .245, y)) * leg
    paw = ramp(.003, .018, y) * (1-ramp(.050, .072, y)) * leg
    centre_x = np.sign(p[:, 0]) * .089
    q[:, 0] = centre_x + (q[:, 0]-centre_x)*(1-.11*ankle+.08*paw)
    centre_z = np.where(front>.5, .202, -.253 + .12*ramp(.085, .245, y))
    q[:, 2] -= .08*ankle*(p[:, 2]-centre_z)
    return q


def pigment_mask(p, n):
    x, y, z = p[:, 0], p[:, 1], p[:, 2]
    front = z > 0
    inner = np.clip(-n[:, 0]*np.sign(x), 0, 1)
    anterior = np.clip(n[:, 2], 0, 1)
    top = np.where(front, .106+.050*anterior+.065*inner, .080+.045*anterior+.065*inner)
    # Irregular boundaries follow the leg surface instead of a horizontal cuff.
    edge = .006*np.sin(abs(x)*280+z*120) + .003*np.sin(y*240-z*170)
    legs = (1-ramp(.315, .365, y))*ramp(.033, .049, abs(x))
    legs *= np.maximum(ramp(.09, .14, z), 1-ramp(-.13, -.08, z))
    return ramp(top-.012, top+.012, y+edge)*ramp(.061, .077, y)*legs


def build(source, output):
    if output.exists() or source.resolve() == output.resolve():
        raise ValueError('Output must be a new directory.')
    asset = Gltf(source/'Bernese.gltf')
    mesh = asset.doc['meshes'][0]
    count = len(mesh['weights'])
    assert mesh['extras']['targetNames'] == ['BerneseSoftNasalBridge']
    rows = []
    uv_triangles, mask_triangles = [], []
    for primitive in mesh['primitives']:
        attr = primitive['attributes']
        p = asset.read(attr['POSITION']).astype(float)
        n = asset.read(attr['NORMAL']).astype(float)
        for weight, target in zip(mesh['weights'], primitive['targets']):
            p += weight*asset.read(target['POSITION'])
            n += weight*asset.read(target['NORMAL'])
        n = normalize(n)
        material = asset.doc['materials'][primitive['material']]['name']
        delta = sculpt(p, material)-p
        eps = 1e-5
        jac = np.stack([(sculpt(p+axis*eps, material)-sculpt(p-axis*eps, material))/(2*eps) for axis in np.eye(3)], axis=2)
        determinant = np.linalg.det(jac)
        assert determinant.min() > .30, (material, determinant.min())
        nn = normalize(np.linalg.solve(jac.transpose(0,2,1), n[...,None])[...,0])
        dn = nn-n
        tri = asset.read(primitive['indices']).reshape(-1,3).astype(int)
        if material == 'DogTeeth':
            # Carry each tooth with the gum without bending its enamel surface.
            _, inverse = np.unique(np.round(p, 6), axis=0, return_inverse=True)
            parent = np.arange(inverse.max()+1)
            def root(i):
                while parent[i] != i:
                    parent[i] = parent[parent[i]]
                    i = parent[i]
                return i
            for face in inverse[tri]:
                for vertex in face[1:]:
                    parent[root(vertex)] = root(face[0])
            groups = np.array([root(i) for i in inverse])
            for group in np.unique(groups):
                selected = groups == group
                centre = p[selected].mean(0, keepdims=True)
                delta[selected] = sculpt(centre, material)-centre
            dn[:] = 0
        old = np.cross(p[tri[:,1]]-p[tri[:,0]],p[tri[:,2]]-p[tri[:,0]])
        q = p+delta
        new = np.cross(q[tri[:,1]]-q[tri[:,0]],q[tri[:,2]]-q[tri[:,0]])
        changed = (np.linalg.norm(delta[tri],axis=2).max(1)>1e-8)&(np.linalg.norm(old,axis=1)>1e-12)
        minimum_alignment = float(np.sum(normalize(old)*normalize(new),axis=1)[changed].min()) if changed.any() else 1.
        assert minimum_alignment > .65, (material, minimum_alignment)
        body = (p[:,1]>.335)&(p[:,1]<.505)
        assert np.allclose(delta[body],0,atol=1e-12)
        assert np.allclose(delta[p[:,1]>.619],0,atol=1e-12)
        assert np.allclose(delta[p[:,1]<.003],0,atol=1e-12)
        primitive['targets'].append(dict(POSITION=asset.append(delta,'VEC3'),NORMAL=asset.append(dn,'VEC3')))
        rows.append(dict(material=material,maximumMM=float(np.linalg.norm(delta,axis=1).max()*1000),minimumJacobian=float(determinant.min()),minimumTriangleAlignment=minimum_alignment))
        if material in ('DogSkin','DogLid'):
            uv = asset.read(attr['TEXCOORD_0']).astype(float)
            uv_triangles.append(uv[tri])
            mask_triangles.append(pigment_mask(p,n)[tri][...,None])
    mesh['weights'].append(1.)
    mesh['extras']['targetNames'].append('BerneseRoundedJawAndPasterns')
    for animation in asset.doc['animations']:
        for channel in animation['channels']:
            if channel['target']['path']=='weights':
                sampler=animation['samplers'][channel['sampler']]
                old=asset.read(sampler['output']).reshape(-1,count)
                sampler['output']=asset.append(np.c_[old,np.ones(len(old))].ravel(),'SCALAR')
    shutil.copytree(source,output)
    asset.doc['buffers'][0].update(uri='Bernese.bin',byteLength=len(asset.data))
    (output/'Bernese.gltf').write_text(json.dumps(asset.doc,indent=2)+'\n')
    (output/'Bernese.bin').write_bytes(asset.data)
    # Reuse the established UV rasterizer without altering the shared generator.
    sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'Dog'))
    import build_dog
    base=np.asarray(Image.open(source/'BerneseCoatColor.png').convert('RGB'),dtype=float)/255.
    mask,covered=build_dog.raster_uv(np.concatenate(uv_triangles),np.concatenate(mask_triangles),base.shape[0])
    rust=ramp(.030,.095,base[:,:,0]-base[:,:,1])
    alpha=np.clip(mask[:,:,0],0,1)*rust
    black=np.array(build_dog.COAT_BLACK)[None,None,:]
    pixels=base*(1-alpha[:,:,None])+black*alpha[:,:,None]
    Image.fromarray(np.uint8(np.clip(pixels*255+.5,0,255))).save(output/'BerneseCoatColor.png')
    report=dict(parent=source.name,rows=rows,coatCurvesExact=True,baseBindingSurfaceExact=True,
                bodyAndEyePositionsExact=True,groundContactsExact=True,legColourChangedTexels=int(np.count_nonzero(alpha>.01)),
                sourceColourSHA256=hashlib.sha256((source/'BerneseCoatColor.png').read_bytes()).hexdigest())
    (output/'bernese-refinement.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    build(args.source,args.output)
