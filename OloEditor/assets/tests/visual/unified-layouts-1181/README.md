# Unified image layouts evidence

Compare the same backend, scene, camera and render path across the two builds.
These are original 1280 x 720 editor captures, inspected at capture time; they
are comparison evidence, not replacements for the existing visual-test goldens.
The [validation report](../../../../../docs/guides/unified-image-layouts-validation.md)
records the controls, camera poses, test results and timing limitations.
[Capture metadata and SHA-256 hashes](captures.json) preserve frame/liveness data.
[Image comparisons](image-comparisons.json) record RGB RMSE on the 0-255 scale;
animated scenes are not subject to a pixel-identity threshold.

## Vulkan

The control is clean git commit `433c3e67d`; unified is the optional layout
implementation in `60f51319b`. Forward+ images come from the initial live checks.
The remaining Release images come from timing pilot pair 1: those timings were
excluded because other worktrees started builds/tests, but the fresh rendered
frames remain valid visual evidence.

| Scene/path | Control | Unified |
| --- | --- | --- |
| Drift Forward | [Front](vulkan-control-Drift-forward-front.png), [angle](vulkan-control-Drift-forward-angle.png) | [Front](vulkan-unified-Drift-forward-front.png), [angle](vulkan-unified-Drift-forward-angle.png) |
| Drift Forward+ | [Front](vulkan-control-Drift-forwardplus-front.png), [angle](vulkan-control-Drift-forwardplus-angle.png) | [Front](vulkan-unified-Drift-forwardplus-front.png), [angle](vulkan-unified-Drift-forwardplus-angle.png) |
| Drift Deferred | [Front](vulkan-control-Drift-deferred-front.png), [angle](vulkan-control-Drift-deferred-angle.png) | [Front](vulkan-unified-Drift-deferred-front.png), [angle](vulkan-unified-Drift-deferred-angle.png) |
| VirtualGeometryStress Deferred | [Front](vulkan-control-VirtualGeometry-deferred-front.png), [close](vulkan-control-VirtualGeometry-deferred-close.png) | [Front](vulkan-unified-VirtualGeometry-deferred-front.png), [close](vulkan-unified-VirtualGeometry-deferred-close.png) |

## OpenGL

The unchanged backend was checked on all four scene/path combinations. Selected
frames from those sessions are below; the shader-toolchain comparison covers the
whole shader tree. Animated Drift water/clouds also differ between repeated
captures from the same binary.

| Scene/path | Control | Candidate |
| --- | --- | --- |
| Drift Deferred | [Angle](opengl-control-Drift-deferred-angle.png) | [Angle](opengl-candidate-Drift-deferred-angle.png) |
| VirtualGeometryStress Deferred | [Close](opengl-control-VirtualGeometry-deferred-close.png) | [Close](opengl-candidate-VirtualGeometry-deferred-close.png) |

## Debug validation

The unified Debug session forced a fresh IBL bake and used core/synchronization
validation. Selected frames: [Drift Deferred](debug-unified-Drift-deferred-front.png)
and [VirtualGeometry close view](debug-unified-VirtualGeometry-deferred-close.png).

The dragon/ground intersections and the Forward/Deferred water differences are
also present in the control. They are not new layout regressions.
