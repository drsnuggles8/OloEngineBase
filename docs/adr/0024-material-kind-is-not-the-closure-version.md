# ADR 0024 — Material kind and skin-profile transport

Accepted for #1231 and updated by #1497.

## Decision

A material's `MaterialKind` selects the surface category. For skin,
`SkinEvaluationModel` belongs to the shared profile asset and selects its transport.
ClosureV2 evaluates the standard reflected PBR lobe. The renderer previously carried
a third selector for Legacy shading; #1497 retired that selector and its storage.

| Selector | Owner | Encoding |
|---|---|---|
| `MaterialKind` | Material | Generic, Snow, Skin or Foliage in G-Buffer flags bits 1–2 |
| `SkinEvaluationModel` | Skin profile | Append-only value in the profile asset |

## Why

A surface keeps its kind when an artist updates the scattering model of its profile.
Keeping transport with the asset also gives every material that references the same
profile the same interpretation of its authored parameters.

## Consequences

The material UBO and GPU scene record carry the kind and profile slot. The deferred
lighting pass recovers the kind and four-bit slot from RT2 alpha. Skin and foliage
interpret the slot within their own tables. Four kinds fill the two-bit kind field.
A fifth needs a deliberate layout change and matching GLSL and C++ checks.
Scene and save readers reject unknown kinds to Generic.

## Alternatives considered

The original proposal considered representing skin as a PBR algorithm version.
A skin material needs both its reflected surface lobe and the profile transport
applied to its scattering component.

**Scene alpha as the scattering channel.** The snow overlay already writes its SSS mask into
scene-colour alpha for `SSSRenderPass`. Issue #1231 explicitly rules out overloading it, and
it is right to: two kinds writing one channel means whichever runs last wins, silently.

> **Note (issue #1451).** Snow's mask later moved into skin's own hand-off lane (scene
> attachment 4, `.a`). That lane carries skin slots as `(s + 1) / 8` and snow as `-weight`.
> This is not the channel rejected above, for two reasons. First, each pixel has exactly one
> writer for this quantity: the surface shader decides, per pixel, whether it hands over skin
> or snow. Second, the value range is partitioned, so the decision can be read from the value
> itself: each consumer decodes its own range and reads the other as "not mine"
> (`include/SnowDiffusionCommon.glsl`). Scene alpha had neither property. Every opaque writer
> set it to 1 and every blended writer to its blend alpha, so its value depended on draw order.
> On the deferred path the snow weight crosses the G-Buffer in RT3.a, the #1256 "material
> profile" channel, because a snow blend is a position along a material profile axis.
