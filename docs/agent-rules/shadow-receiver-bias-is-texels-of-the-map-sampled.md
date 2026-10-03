# A shadow receiver's bias is the light's, in texels of the map it samples

**Rule.** Bias a receiver by the light's authored pair: depth in texels, normal offset in metres,
each converted through the level, cascade or mip the lookup actually samples. Never give one shadow
technique its own bias constant in metres. A constant sized for its coarse levels erases contact
shadows in its fine ones.

## What happened (#1533)

The Virtual Shadow Map biased every receiver by its own settings: `DepthBiasMeters` (5 cm) and
`NormalBias` (2 cm, plus 1.5 texels of the level). A lamp's layers took `LocalDepthBiasMeters`
(2 cm) plus the same 2 cm. Near the camera a clip level's texel is 1-2 mm, so every occluder within
about 7 cm of a receiver let its light through, 4 cm under a lamp. In the same scene the cascades
use the light's 2 texels of depth plus 1 cm of offset (#1119 made their bias texel-relative for this
reason). The dog's coat read 5-7% brighter under the VSM than under the cascades in the front views,
and 9-16% brighter on the chest and the belly, which the head and body shadow at close range.

Now the sun's clip levels take `ShadowSettings::DepthBiasTexels` and `NormalBias`, which the scene
sets from the light exactly as it does for the cascades. A lamp's layers take `AtlasDepthBiasTexels`
and the atlas's 1.5-texel offset. Each is converted through the level or mip sampled
(`vsmLevelDepthBias` and `vsmLocalDepthBias` in `VirtualShadowResources.glsl`), and
`VirtualShadowMap::SetSamplingParams` carries them in. The three VSM bias settings are gone.

## What stayed green

`VirtualShadowMapCastsTheSameFloorShadowAsCSM` compares the shadow of a 5 m cube, whose footprint a
7 cm bias does not change, with a mass band of 0.5-2x. Nothing compared the two techniques where an
occluder sits a few centimetres above its receiver.

## How it was found, and the wrong turn

1. **Separate the receiver from the map.** The light-by-region record
   (`DogShowcaseEvidenceTest.TheLightTheBodyCannotStopIsMeasuredRegionByRegion`) gained an arm that
   samples the cascades at the coat's light-exit point, where the VSM samples
   (`OLO_FAULT_GROOM_SHADOW_AT_COAT_EXIT`). `exit/key` read 0.99 and `vsm/exit` 1.05, so the excess
   was in the VSM's map, not in where the coat sampled it.
2. **Override the suspect.** A scratch override of the VSM's two biases took `vsm/exit` to
   0.99-1.00 in every view. So the bias was the cause.
3. **A fix whose effect is exactly zero did not reach the term.** The first fix gave only the
   strand's own VSM lookup the cascades' bias. A near-occluder case proved it on a test patch, but
   the dog's numbers did not move in the third decimal. On the dog the coat's exit point lies outside
   the whole animal, so the strand's lookup reads lit under any bias. The excess was the skin's
   VSM shadow, seen through sparse fur: a "coat pixel" holds whatever its fur lets through.

## Evidence

- `VirtualShadowMapVisualEvidenceTest.TheVsmKeepsAContactShadowTheCascadesKeep` puts a plate 3 cm
  over the floor. The VSM's contact strip must be within 2x of the cascades'. The negative control
  sets the light's normal bias to the old 7 cm reach and must lose the strip. With the plate not
  casting, the two techniques must agree, so the smaller bias adds no acne.
- `GroomSceneShadowVisualEvidenceTest.UnderTheVsmAnOccluderJustPastTheCoatShadowsIt*` (sun and spot)
  checks the same for a casting coat's exit-point receiver.
- `Dog_Lighting.txt` records `vsm/exit` per region. The test expects it within 3% over the coat.
