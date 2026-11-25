"""Generate the isolated CAP-05 trial shader from the textured mirror baseline."""

import argparse
from pathlib import Path


RAY = '''    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const uint seed = oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), {seed}u);
    const vec2 xi = OloSobolOwen2D(u_TlasAddress.w, seed, 0u);
    const float receiverRoughness = clamp(roughness, 0.04, 1.0);
    const vec3 H = sampleGGXVNDF(N, -V, receiverRoughness, xi);
    const vec3 R = reflect(V, H);
    const float NoV = dot(N, -V);
    const float NoL = dot(N, R);
    if (!(NoV > 0.0 && NoL > 0.0))
        return;'''

THROUGHPUT = '''    const vec3 receiverF0 = vec3({f0});
    const vec3 fresnel = fresnelSchlick(clamp(dot(-V, H), 0.0, 1.0), receiverF0);
    float throughput = ggxVNDFWeight(NoV, NoL, receiverRoughness);
'''

WRONG_PDF = '    throughput /= (1.0 + ggxSmithLambda(NoV, receiverRoughness * receiverRoughness));\n'

RAW = '''    // Zero for below-horizon directions and misses in the raw geometry-only integral.
    o_Specular = vec4(fresnel * throughput * hitRadiance, 1.0);
    o_Color = vec4(hitT, throughput, confidence, 1.0);
'''

# The normalizer is the single-scatter lobe integral. It converts the sample
# into the units of the live pass's W for the composition control.
COMPOSITION = '''    const vec2 energy = ggxEnergy(NoV, roughness);
    const vec3 singleScatter = receiverF0 * (1.0 - energy.x - energy.y) + energy.y;
    const vec3 normalizedWeight = fresnel * throughput / max(singleScatter, vec3(1.0e-4));
    const vec3 delta = confidence * normalizedWeight * (weight * hitRadiance - baseSpecular);
    o_Color = vec4(max(baseColor + delta, vec3(0.0)), 1.0);
    o_Specular = vec4(max(baseSpecular + delta, vec3(0.0)), 1.0);
'''


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"Unexpected baseline shader around {before!r}")
    return source.replace(before, after, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='CAP-04 RayTracedReflection.glsl')
    parser.add_argument('output', type=Path, help='shader in an isolated editor copy')
    parser.add_argument('--f0', type=float, nargs=3, default=[0.9, 0.9, 0.92])
    parser.add_argument('--seed', type=int, default=1356)
    parser.add_argument('--view', choices=('raw', 'composition'), default='raw')
    parser.add_argument('--wrong-pdf', action='store_true')
    args = parser.parse_args()

    if args.source.resolve() == args.output.resolve():
        parser.error('source and output must be different files')
    if not all(0.0 <= channel <= 1.0 for channel in args.f0) or not 0 <= args.seed < 2**32:
        parser.error('F0 must use three channels in [0,1] and seed must fit uint32')

    text = args.source.read_text(encoding='utf-8')
    text = replace_once(text, '#include "include/PBRCommon.glsl"',
                        '#include "include/PBRCommon.glsl"\n#include "include/PathTracerSampler.glsl"')
    text = replace_once(text, '    const vec3 R = reflect(V, N);', RAY.format(seed=args.seed))
    text = replace_once(text, '    const float gateStart = u_RoughnessGate.x;',
                        '    const float gateStart = 0.55;')
    text = replace_once(text, '    const float gateEnd = max(u_RoughnessGate.y, gateStart + 1e-4);',
                        '    const float gateEnd = 0.9;')

    if args.view == 'raw':
        text = replace_once(text, '    o_Specular = vec4(baseSpecular, 1.0);', '    o_Specular = vec4(0.0);')
        text = replace_once(text, '    o_Color = vec4(baseColor, (u_Flags.x > 0.5) ? 0.0 : 1.0);',
                            '    o_Color = vec4(0.0, 0.0, 0.0, 1.0);')

    f0 = ', '.join(f'{channel:.9f}' for channel in args.f0)
    body = THROUGHPUT.format(f0=f0)
    if args.wrong_pdf:
        body += WRONG_PDF
    body += RAW if args.view == 'raw' else COMPOSITION
    start = text.index('    const vec3 delta = oloSpecularTierDelta(')
    args.output.write_text(text[:start] + body + '}\n', encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
