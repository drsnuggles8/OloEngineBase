"""Independent emitter-surface quadrature and temporal replay for a raw RT sequence."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


ROOT = Path('OloEditor/assets/tests/visual/HybridReflectionBaseline')
LUMA = np.array([.2126, .7152, .0722])
F0 = np.array([.9, .9, .92])
BOX_CENTRE = np.array([-2.5, 9.0, -9.0])
BOX_HALF_SIZE = np.array([1.25, 1.5, 1.25])
ROI = np.s_[115:173, 75:225]


def read_hdr(path):
    with path.open('rb') as stream:
        if stream.readline().strip() != b'#?RADIANCE':
            raise ValueError(f'Expected Radiance HDR: {path}')
        for line in stream:
            if not line.strip():
                break
        layout = stream.readline().split()
        if layout[0::2] != [b'-Y', b'+X']:
            raise ValueError(f'Unexpected scanline order: {path}')
        height, width = int(layout[1]), int(layout[3])
        pixels = np.empty((height, width, 4), dtype=np.uint8)
        marker = bytes((2, 2, width // 256, width % 256))
        for row in range(height):
            if stream.read(4) != marker:
                raise ValueError(f'Unexpected scanline {row}: {path}')
            for channel in range(4):
                col = 0
                while col < width:
                    code = stream.read(1)
                    if not code:
                        raise ValueError(f'Truncated scanline {row}: {path}')
                    repeated = code[0] > 128
                    count = code[0] - 128 if repeated else code[0]
                    if count <= 0 or count > width - col:
                        raise ValueError(f'Invalid scanline run {row}: {path}')
                    data = stream.read(1 if repeated else count)
                    if len(data) != (1 if repeated else count):
                        raise ValueError(f'Truncated scanline data {row}: {path}')
                    pixels[row, col:col + count, channel] = data[0] if repeated else np.frombuffer(data, dtype=np.uint8)
                    col += count
    exponent = pixels[..., 3:4].astype(np.int32) - 136
    return np.ldexp(pixels[..., :3].astype(np.float32), exponent)


def read_sequence(arm, name='RTSpecular'):
    return np.stack([read_hdr(p) for p in sorted((arm/'sequence').glob(f'{name}_*.hdr'))])


def basis(yaw, pitch):
    yaw, pitch = np.deg2rad((yaw, pitch))
    forward = np.array([np.cos(pitch)*np.sin(yaw), -np.sin(pitch), -np.cos(pitch)*np.cos(yaw)])
    side = np.array([np.cos(yaw), 0., np.sin(yaw)])
    return forward, side, np.cross(side, forward)


def floor_points(width, height, eye, yaw, pitch):
    u, v = np.meshgrid((np.arange(width)+.5)/width, (np.arange(height)+.5)/height)
    tan = np.tan(np.deg2rad(22.5))
    forward, side, up = basis(yaw, pitch)
    view = forward + ((2*u-1)*tan*width/height)[...,None]*side + ((1-2*v)*tan)[...,None]*up
    view /= np.linalg.norm(view, axis=2, keepdims=True)
    point = eye - eye[1] * view / view[...,1,None]
    valid = (view[...,1] < 0) & (np.abs(point[...,0]) <= 20) & (np.abs(point[...,2]) <= 20)
    return point, -view, valid


def emitter_surface(grid):
    points, normals, areas = [], [], []
    samples = 2*(np.arange(grid)+.5)/grid - 1
    uv = np.column_stack([a.ravel() for a in np.meshgrid(samples, samples)])
    for axis in range(3):
        remaining = [i for i in range(3) if i != axis]
        for sign in (-1,1):
            p = np.repeat(BOX_CENTRE[None], grid*grid, axis=0)
            p[:,axis] += sign*BOX_HALF_SIZE[axis]
            p[:,remaining] += uv*BOX_HALF_SIZE[remaining]
            normal = np.zeros_like(p)
            normal[:,axis] = sign
            points.append(p)
            normals.append(normal)
            areas.append(np.full(grid*grid, 4*np.prod(BOX_HALF_SIZE[remaining])/grid**2))
    return np.concatenate(points), np.concatenate(normals), np.concatenate(areas)


def integrate(work, view, roughness, emitter, grid=24):
    points, normals, areas = emitter_surface(grid)
    result = np.empty(work.shape[:-1]+(3,))
    out = result.reshape(-1,3)
    alpha = roughness**2
    for start in range(0,len(out),96):
        stop = start+96
        x = work.reshape(-1,3)[start:stop,None,:]
        v = view.reshape(-1,3)[start:stop,None,:]
        offset = points[None] - x
        distance2 = np.sum(offset**2, axis=2)
        light = offset / np.sqrt(distance2)[...,None]
        half = v + light
        half /= np.linalg.norm(half,axis=2,keepdims=True)
        nl = np.maximum(light[...,1],0)
        nv = np.maximum(v[...,1],0)
        nh = np.maximum(half[...,1],0)
        vh = np.clip(np.sum(v*half,axis=2),0,1)
        d = alpha**2 / (np.pi*(1-nh**2+alpha**2*nh**2)**2)
        vl = np.sqrt(nl**2+alpha**2*(1-nl**2))
        vv = np.sqrt(nv**2+alpha**2*(1-nv**2))
        visibility = .5/np.maximum(nl*vv+nv*vl,1e-7)
        emitter_cosine = np.maximum(-np.sum(light*normals[None],axis=2),0)
        solid_angle = emitter_cosine*areas[None]/distance2
        contribution = emitter * (d*visibility*nl*solid_angle)
        base = contribution.sum(axis=1)
        a = np.sum(contribution*(1-vh)**5,axis=1)
        out[start:stop] = base[:,None]*F0 + a[:,None]*(1-F0)
    return result


def neighborhood(sample):
    pad = np.pad(sample,((1,1),(1,1)),mode='edge')
    return np.stack([pad[y:y+sample.shape[0],x:x+sample.shape[1]] for x in range(3) for y in range(3)])


def reconstruct(sequence, maximum=8):
    # SurfaceHistory.glsl moments, capped to eight accepted samples. Static
    # fixture reprojection is identity. A neighborhood clamp prevents a bright
    # history from exceeding the range of the current 3x3 observation.
    first = np.zeros_like(sequence[0])
    second = np.zeros_like(first)
    history = np.zeros_like(first)
    output, variances, clamped = [], [], []
    for index,sample in enumerate(sequence):
        samples = neighborhood(sample)
        spatial = np.average(samples,axis=0,weights=[1,2,1,2,4,2,1,2,1])
        length = min(index+1,maximum)
        first += (sample-first)/length
        second += (sample*sample-second)/length
        variances.append(np.maximum(second-first*first,0))
        clipped = np.clip(history,samples.min(axis=0),samples.max(axis=0))
        clamped.append(np.abs(history-clipped) > .0001)
        history = clipped*(1-1/length) + spatial/length
        output.append(history.copy())
    return np.stack(output), np.stack(variances), np.stack(clamped)


def project(world, width, height, eye, yaw, pitch):
    forward, side, up = basis(yaw, pitch)
    offset = world-eye
    depth = offset@forward
    tan = np.tan(np.deg2rad(22.5))
    u = .5*(1+(offset@side)/(depth*tan*width/height))
    v = .5*(1-(offset@up)/(depth*tan))
    return u, v, depth


def motion_report(occlusion_fixture=False):
    root = 'rough-disocclusion' if occlusion_fixture else 'rough-motion'
    directory = arm(root,'r65')
    sequence = read_sequence(directory)@LUMA
    distance = read_sequence(directory,'RTColour')[...,0]
    normals = read_sequence(directory,'RasterNormal') if occlusion_fixture else None
    poses = [frame['pose'] for frame in json.loads((directory/'result.json').read_text())['cameras'][0]['sequence']]
    height,width = sequence.shape[1:]
    previous = None
    temporal = []
    measured = []
    history_valid = []
    clamp_masks = []
    rejected_geometry = 0
    accepted_history = 0
    distance_rejected = 0
    history_candidates = 0
    foreground_reveals = 0
    reveal_fallback_max_error = 0.
    for index,(sample,pose) in enumerate(zip(sequence,poses)):
        camera = (np.array(pose['position']), pose['yawDegrees'], pose['pitchDegrees'])
        world,view,floor_extent = floor_points(width,height,*camera)
        valid = floor_extent if normals is None else floor_extent & (np.abs(normals[index,...,2]-.65)<.025)
        rays = neighborhood(sample)
        spatial = np.average(rays,axis=0,weights=[1,2,1,2,4,2,1,2,1])
        if previous is None:
            history = spatial
            history_valid.append(np.zeros_like(valid))
            clamp_masks.append(np.zeros_like(valid))
            age = valid.astype(np.int32)
        else:
            previous_camera,previous_world,previous_valid,previous_extent,previous_history,previous_age = previous
            u,v,depth = project(world,width,height,*previous_camera)
            px = np.clip((u*width).astype(np.int64),0,width-1)
            py = np.clip((v*height).astype(np.int64),0,height-1)
            target_depth = (previous_world[py,px]-previous_camera[0]) @ basis(*previous_camera[1:])[0]
            error = np.abs(depth-target_depth)/np.maximum(np.maximum(np.abs(depth),np.abs(target_depth)),1e-4)
            in_bounds = (depth>0) & (u>=0) & (u<1) & (v>=0) & (v<1)
            available = valid & previous_valid[py,px] & in_bounds & (error<.02)
            old_distance = distance[index-1,py,px]
            new_distance = distance[index]
            got_hit = new_distance>0
            prior_hit = old_distance>0
            distance_error = np.abs(old_distance-new_distance)/np.maximum(np.maximum(np.abs(old_distance),np.abs(new_distance)),1e-4)
            disagrees = (got_hit != prior_hit) | (got_hit & prior_hit & (distance_error>.1))
            history_candidates += int(available.sum())
            distance_rejected += int((available & disagrees).sum())
            rejected_geometry += int((valid & ~available).sum())
            accepted_history += int(available.sum())
            age = np.where(available,np.minimum(previous_age[py,px]+1,8),1)
            prior = np.clip(previous_history[py,px],rays.min(axis=0),rays.max(axis=0))
            history_valid.append(available)
            clamp_masks.append(available & (np.abs(previous_history[py, px]-prior) > .0001))
            # Raw fallback when reprojection has no valid geometric history.
            history = np.where(available,prior*(1-1/age)+spatial/age,sample)
            if occlusion_fixture:
                revealed = valid & in_bounds & previous_extent[py,px] & ~previous_valid[py,px]
                foreground_reveals += int(revealed.sum())
                if revealed.any():
                    reveal_fallback_max_error = max(reveal_fallback_max_error,float(np.abs(history[revealed]-sample[revealed]).max()))
        history = np.where(valid,history,0)
        previous = camera,world,valid,floor_extent,history,age
        temporal.append(history)
        if not occlusion_fixture and index in (9,19,29,39,47):
            expected = integrate(world[ROI],view[ROI],.65,2.,grid=12)@LUMA
            measured.append({'sample':index, 'raw_rmse':rmse(sample[ROI],expected),
                             'reprojection_rmse':rmse(history[ROI],expected), 'zero_fallback_rmse':rmse(0,expected)})
    data = {'geometric_history_accepted':accepted_history,'geometric_history_rejected':rejected_geometry,
            'hit_distance_rejected_fraction':distance_rejected/history_candidates,'distance_min':float(distance[distance>0].min()),
            'distance_max':float(distance.max()),'motion_frames':measured}
    if occlusion_fixture:
        data.update(foreground_reveals=foreground_reveals,reveal_fallback_max_error=reveal_fallback_max_error)
    print(root,json.dumps(data),flush=True)
    save(ROOT/root/'rough65-reconstructed-linear.png',temporal[-1][ROI])
    np.savez_compressed(ROOT/root/'rough65-motion-diagnostics.npz', history_valid=history_valid, clamped=clamp_masks,
                        reconstructed=temporal, hit_distance=distance)
    return data


def rmse(images, target):
    return float(np.sqrt(np.mean((images-target)**2)))


def save(path, img):
    rgb = np.round(np.clip(img*350,0,255)).astype(np.uint8)
    Image.fromarray(rgb).save(path)


def arm(root,name):
    return next((ROOT/root).glob(f'*-{name}'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture-root', type=Path, default=ROOT)
    args = parser.parse_args()
    ROOT = args.capture_root
    point, view, valid = floor_points(320, 180, np.array([1.1,1.85,8.6]), -4, 15)
    report = {}
    for case,r in [('r20',.2),('r45',.45),('r65',.65),('r80',.8)]:
        correct = read_sequence(arm('rough-raw',case))[:,ROI[0],ROI[1],:]@LUMA
        mirror = read_sequence(arm('rough-mirror',case))[:,ROI[0],ROI[1],:]@LUMA
        expected = integrate(point[ROI],view[ROI],r,2.0)@LUMA
        refinement = integrate(point[ROI],view[ROI],r,2.0,grid=16)@LUMA
        temporal,var,clamped = reconstruct(correct)
        group_means = correct.reshape(6,8,*correct.shape[1:]).mean(axis=(1,2,3))
        data = {'reference_mean':float(expected.mean()),'refinement_abs_mean':float(abs(refinement.mean()-expected.mean())),
                'raw_mean':float(correct.mean()),'raw_group_mean_min':float(group_means.min()),
                'raw_group_mean_max':float(group_means.max()), 'raw_rmse':rmse(correct,expected),
                'one_spp_temporal_spatial_rmse':rmse(temporal[8:],expected), 'mirror_rmse':rmse(mirror,expected),
                'mean48_rmse':rmse(correct.mean(axis=0),expected), 'variance':float(var[8:].mean()),
                'clamped_fraction':float(clamped[8:].mean())}
        if case!='r20':
            wrong = read_sequence(arm('rough-wrong-pdf',case))[:,ROI[0],ROI[1],:]@LUMA
            data['wrong_pdf_mean'] = float(wrong.mean())
        report[case]=data
        print(case,json.dumps(data),flush=True)
        if case=='r65':
            dim = read_sequence(arm('rough-raw','r65-dim'))[:,ROI[0],ROI[1],:]@LUMA
            changed = np.concatenate([correct[:24],dim[:24]])
            replay,variance,clamp = reconstruct(changed)
            np.savez_compressed(ROOT/'rough-raw/rough65-static-diagnostics.npz',
                                variance=var, clamped=clamped, reconstructed=temporal,
                                changed_light_reconstruction=replay)
            dim_target = expected*.125
            print('relight', json.dumps({'raw_mean_1':float(changed[24].mean()),'replayed_mean_1':float(replay[24].mean()),
                  'reference_mean':float(dim_target.mean()),'replayed_mean_4':float(replay[27].mean()),
                  'replayed_rmse_1':rmse(replay[24],dim_target),'replayed_rmse_4':rmse(replay[27],dim_target)}),flush=True)
            save(ROOT/'rough-raw/rough65-raw-linear.png',correct[10])
            save(ROOT/'rough-raw/rough65-reconstructed-linear.png',temporal[10])
            save(ROOT/'rough-raw/rough65-reference-linear.png',expected)
    report['motion'] = motion_report()
    report['disocclusion'] = motion_report(occlusion_fixture=True)
    (ROOT/'rough-raw/analysis.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
