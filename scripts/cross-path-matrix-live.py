#!/usr/bin/env python3
"""Replay the cross-path lighting matrix's rows against a LIVE editor over MCP (issue #1347).

The in-process fixture (OloEngine/tests/Rendering/CrossPath/CrossPathLightingMatrixTest.cpp)
runs the GL arms only: scene-level Vulkan is unreachable in-process (one process holds one
backend; docs/agent-rules/testing-architecture.md s9). This script is the live half of the
matrix's Vulkan cells. It takes the rows the fixture EXPORTS from its reference arm (GL
Forward native) and, for every rendering path, renders each probe's OFF and ON scene in a
running editor, reads the linear HDR colour the GL reference read (the tone map's input,
the probe's "target") at exactly the exported pixels, and holds
the separated term (ON - OFF, averaged per region) against

  * the GL reference arm's value over the same pixels  (crossArmTolerance: the probe's
    cross-arm bound, here a cross-BACKEND and cross-path comparison), and
  * the independent analytic model, where the row has one  (analyticTolerance),

each as |measured - expected| <= relative * |expected| + absolute, per channel.

Every capture carries its own settle control: the first region is read again one rendered
frame later, and a change of its MEAN beyond the probe's cross-arm tolerance marks the cell
UNSETTLED (a failure), so a still-converging frame cannot pass or fail by accident.

Rows the live editor cannot answer are reported, never dropped:
  * a row whose "requires" names a ray-query estimator (ReSTIR DI/GI/PT, RT reflections)
    runs on --backend vulkan only; a failed attempt there is NOT RUN with the error, since
    whether the device has hardware ray queries is not visible from here;
  * a row that requires SSGI runs on Deferred only (the path that hosts it).

1. Export the rows (reference arm only; from OloEditor/, the test binary's scene paths are
   cwd-relative). SSGIBounce's reference arm is GL Deferred, not Forward, so name it too:
     cd OloEditor
     ..\\build\\OloEngine\\tests\\Debug\\OloEngine-Tests.exe --olo-cross-path-export=assets/tests/crosspath-live ^
       --gtest_filter=AllRowsAllArms/CrossPathLightingMatrix.*gl_forward_native:AllRowsAllArms/CrossPathLightingMatrix.*SSGIBounce_gl_deferred_native
   This writes <Row>_<Probe>_Off.olo, <Row>_<Probe>_On.olo and <Row>.json per row.

2. Launch the editor on the backend under test, with MCP autostart and write consent:
     pwsh -NoProfile -File .claude/skills/run-oloengine/driver.ps1 -Action attach -Rhi vulkan -AllowWrites
   (the driver sets OLO_MCP_AUTOSTART=1, OLO_MCP_ALLOW_WRITES=1 and the per-worktree
   OLO_MCP_PORT, and prints "MCP port <n>").

3. Replay:
     python scripts/cross-path-matrix-live.py --port <n> --backend vulkan ^
       --export-dir OloEditor/assets/tests/crosspath-live

olo_scene_open resolves a relative path against the ACTIVE PROJECT's asset directory
(EditorLayer.cpp OpenSceneFromMcp: Project::GetAssetFileSystemPath), which for the
Sandbox project is OloEditor/SandboxProject/Assets, NOT OloEditor/assets. It also takes
an absolute path. So an export under --project-assets is opened by its Assets-relative
path, and any other export directory by its absolute path; the reply's resolved path is
checked against the file either way.

The run leaves the editor as it found it (the viewport override is reset, Play is
stopped, the scene open at the start is reopened, then the render path and MSAA are put
back). olo_scene_summary reports a scene NAME, not a file, so the original scene is found
as <name>.olo under --project-assets; pass --restore-scene when that is ambiguous. Unsaved
edits in the scene open at the start are lost when the first row is opened.

Exit code 0 when every compared region held, 1 when any did not (or a cell failed to
capture), 2 on a setup error (no editor, wrong backend, no manifests, writes refused).
"""

import argparse
import glob
import json
import os
import sys
import tempfile
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATHS = ("forward", "forwardplus", "deferred")  # McpRendererSettings.h kRenderPathValues tokens

# CrossPathMatrix.h NeedsRayQueries(): ReSTIRDI / ReSTIRGI / ReSTIRPT / RayTracedReflection.
# The manifest carries the row's EstimatorToggle.Name ("ReSTIR DI" in
# CrossPathLightingMatrixTest.cpp ReSTIRDIToggle()).
RAY_QUERY_REQUIREMENTS = ("ReSTIR DI", "ReSTIR GI", "ReSTIR PT", "RT reflections", "RTReflection")

# The path that hosts a required estimator. SSGI: "Deferred is required for SSR / SSGI"
# (McpRendererSettings.h kSettings "renderpath"); the fixture asks
# ResolveLightingSignalOwnership, which this script cannot call.
PATH_REQUIREMENTS = {"SSGI": ("deferred",), "ReSTIR DI": ("deferred",)}

# Fields the editor's scene load OVERWRITES from the project's quality tier after copying
# the scene's post-process settings (EditorLayer.cpp LoadEditorSceneFile ->
# ApplyTieringToSettings -> QualityTiering.cpp CopyTierPPFields). The scene file holds the
# exported values; this script writes them back with olo_postprocess_settings_set.
TIER_OWNED_FIELDS = ("SSAOEnabled", "SSAORadius", "SSAOBias", "SSAOSamples", "BloomEnabled", "BloomIterations",
                     "FXAAEnabled", "DOFEnabled", "MotionBlurEnabled", "VignetteEnabled", "ChromaticAberrationEnabled")
AO_TECHNIQUE_TOKENS = {0: "none", 1: "ssao", 2: "gtao"}  # PostProcessSettings.h AOTechnique

# The AO state when the export omits it. SceneSerializer.cpp writes ActiveAOTechnique /
# GTAOEnabled only when pp.m_AOTechniqueOverride is set, and the fixture's ApplyArm writes
# GTAOEnabled without it, so an export can arrive without its AO state; the tier then
# decides. The fixture's AO sources: ApplyArm (AO off) and ScreenSpaceAO/AOOnAmbient's
# SetSource (GTAO on in the ON scene). Used with a loud warning, never silently.
AO_FALLBACK_ON = {("ScreenSpaceAO", "AOOnAmbient", True)}


class SetupError(Exception):
    pass


class Mcp:
    def __init__(self, port):
        path = os.path.join(tempfile.gettempdir(), "oloengine-mcp-%d.json" % port)
        try:
            with open(path, encoding="utf-8") as f:
                disc = json.load(f)
        except OSError as e:
            raise SetupError("no MCP discovery file %s (%s): is the editor running with OLO_MCP_AUTOSTART=1?" % (path, e))
        self.url = disc["url"]
        self.token = disc["token"]
        self.next_id = 1

    def call(self, name, arguments=None):
        body = json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": "tools/call",
                           "params": {"name": name, "arguments": arguments or {}}}).encode()
        self.next_id += 1
        req = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Authorization": "Bearer " + self.token, "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(req, timeout=300) as resp:
            text = resp.read().decode("utf-8")
        for line in text.splitlines():
            if line.startswith("data:"):
                text = line[5:].strip()
                break
        reply = json.loads(text)
        if "error" in reply:
            raise RuntimeError("%s: %s" % (name, reply["error"]))
        result = reply["result"]
        if result.get("isError"):
            raise RuntimeError("%s refused: %s" % (name, result.get("content")))
        return payload(result)


def payload(result):
    """A tool call's JSON reply: structuredContent, or the first text block."""
    if isinstance(result.get("structuredContent"), dict):
        return result["structuredContent"]
    for block in result.get("content", []):
        if block.get("type") == "text":
            return json.loads(block["text"])
    raise RuntimeError("tool reply carries no JSON payload")


def same_file(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def read_postprocess_block(path):
    """The top-level keys of the .olo's PostProcessSettings map (SceneSerializer.cpp
    Serialize: `PostProcessSettings:` then a two-space-indented block)."""
    values = {}
    inside = False
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line.strip():
                continue
            if not line.startswith(" "):
                inside = line.startswith("PostProcessSettings:")
                continue
            if inside and line.startswith("  ") and not line.startswith("   ") and ":" in line:
                key, _, value = line.strip().partition(":")
                values[key.strip()] = parse_scalar(value.strip())
    return values


def parse_scalar(text):
    if text in ("true", "false"):
        return text == "true"
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def within(measured, expected, tol):
    return all(abs(m - e) <= tol["relative"] * abs(e) + tol["absolute"] for m, e in zip(measured, expected))


def fmt(v):
    return "(%s)" % ", ".join("%.4g" % c for c in v) if v is not None else "-"


def mean(vectors):
    n = float(len(vectors))
    return [sum(v[c] for v in vectors) / n for c in range(3)]


def log_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return None


def log_backend(path):
    """The last `[RHI] Backend: <X> (source: ...)` line (Application.cpp)."""
    backend = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            at = line.find("[RHI] Backend: ")
            if at >= 0:
                backend = line[at + len("[RHI] Backend: "):].split()[0].lower()
    return backend


class Session:
    def __init__(self, mcp, args):
        self.target = "SceneColor"
        self.estimator_field = ""
        self.cross_tolerance = {"relative": 0.0, "absolute": 1.0e-3}
        self.target_pass = ""
        self.targets = (("SceneColor", ""), ("SceneColor", ""))
        self.mcp = mcp
        self.args = args
        self.results = []    # (label, held, detail)
        self.not_run = []    # (label, reason)
        self.ao_warned = set()
        self.project_assets = os.path.abspath(args.project_assets)

    # ---- editor state ---------------------------------------------------

    def renderer_settings(self):
        # olo_renderer_settings_set with NO arguments = introspection: every setting with
        # its currentValue (McpToolsRender.cpp olo_renderer_settings_set, Handle_RendererSettingsSet).
        reply = self.mcp.call("olo_renderer_settings_set", {})
        return {s["setting"]: s.get("currentValue") for s in reply.get("settings", [])}

    def set_renderer(self, setting, value):
        # McpToolsRender.cpp olo_renderer_settings_set {setting, value}; reply 'value' is read back.
        reply = self.mcp.call("olo_renderer_settings_set", {"setting": setting, "value": value})
        if reply.get("value") != value:
            raise RuntimeError("olo_renderer_settings_set %s=%s read back %r" % (setting, value, reply.get("value")))
        if self.renderer_settings().get(setting) != value:
            raise RuntimeError("renderer setting %s did not stick at %s" % (setting, value))

    def set_pp(self, field, value):
        # McpToolsRender.cpp olo_postprocess_settings_set {field, value}; reply 'value' is post-clamp.
        reply = self.mcp.call("olo_postprocess_settings_set", {"field": field, "value": value})
        if reply.get("clamped"):
            raise RuntimeError("olo_postprocess_settings_set %s=%r was clamped to %r" % (field, value, reply.get("value")))
        return reply

    def scene_arg(self, path):
        # EditorLayer.cpp OpenSceneFromMcp: relative -> Project::GetAssetFileSystemPath;
        # absolute passes through. McpSceneControl.h ValidateScenePath rejects '..'.
        rel = os.path.relpath(path, self.project_assets)
        if not rel.startswith("..") and not os.path.isabs(rel):
            return rel.replace("\\", "/")
        return os.path.abspath(path)

    def open_scene(self, path):
        # McpToolsScene.cpp olo_scene_open {path}; reply {ok, path, sceneName, entityCount, message}.
        reply = self.mcp.call("olo_scene_open", {"path": self.scene_arg(path)})
        if not reply.get("ok"):
            raise RuntimeError("olo_scene_open %s: %s" % (path, reply.get("message")))
        if not same_file(reply.get("path", ""), path):
            raise RuntimeError("olo_scene_open resolved %s to %s: the editor's project is not the one "
                               "--project-assets names" % (path, reply.get("path")))
        # McpToolsScene.cpp olo_scene_summary {} -> {hasActiveScene, isPlaying, name, entityCount}.
        summary = self.mcp.call("olo_scene_summary", {})
        if summary.get("name") != reply.get("sceneName") or summary.get("entityCount") != reply.get("entityCount"):
            raise RuntimeError("olo_scene_summary reports %r (%s entities) after opening %r (%s entities)" % (
                summary.get("name"), summary.get("entityCount"), reply.get("sceneName"), reply.get("entityCount")))

    def restore_postprocess(self, row, probe, state, scene_path):
        """Write back what the quality tier overwrote on load (see TIER_OWNED_FIELDS)."""
        pp = read_postprocess_block(scene_path)
        if "ActiveAOTechnique" in pp:
            self.set_pp("ActiveAOTechnique", AO_TECHNIQUE_TOKENS[int(pp["ActiveAOTechnique"])])
            if "GTAOEnabled" in pp:
                self.set_pp("GTAOEnabled", bool(pp["GTAOEnabled"]))
        else:
            gtao = (row, probe, state) in AO_FALLBACK_ON
            if scene_path not in self.ao_warned:
                self.ao_warned.add(scene_path)
                print("  WARNING %s carries no ActiveAOTechnique (exported without m_AOTechniqueOverride), so the "
                      "project tier would pick its AO; forcing GTAO %s from the fixture's source table" % (
                          os.path.basename(scene_path), "ON" if gtao else "off"))
            if gtao:
                self.set_pp("ActiveAOTechnique", "gtao")
            self.set_pp("GTAOEnabled", gtao)
        for field in TIER_OWNED_FIELDS:
            if field in pp:
                self.set_pp(field, pp[field])
        self.set_renderer("upscale", "off")  # every exported arm is native resolution
        # A ray-query row (ReSTIR DI) was exported from GL with the raster loop as its reference;
        # the live cell turns the estimator on (manifest "estimatorField") and reads it back.
        if self.estimator_field:
            self.set_pp(self.estimator_field, True)

    def play(self):
        # McpToolsScene.cpp olo_scene_play {} -> SceneStateOutputSchema {ok, playing, mode, message}.
        reply = self.mcp.call("olo_scene_play", {})
        if not reply.get("ok") or not reply.get("playing"):
            raise RuntimeError("olo_scene_play: %s" % reply.get("message"))
        if not self.mcp.call("olo_scene_summary", {}).get("isPlaying"):
            raise RuntimeError("olo_scene_summary says the editor is not playing after olo_scene_play")

    def stop(self):
        # McpToolsScene.cpp olo_scene_stop {} (idempotent: already stopped -> changed:false).
        self.mcp.call("olo_scene_stop", {})

    def probe(self, x, y, width, height, force_frame=False):
        # McpToolsRender.cpp olo_render_probe_pixel {x, y, target, space, forceFrame}:
        # single-target mode returns {available, value[], width, height, mappedCoord{texel}, meta{stale}}.
        # The target is the one the GL reference read (the manifest's probe "target": the
        # tone-map pass's input, e.g. SSGIColor or PostProcessColor, not always SceneColor);
        # ResolveTargetHandle takes its colour attachment 0.
        target = self.target
        args = {"x": x, "y": y, "target": target, "space": "texel"}
        if force_frame:
            args["forceFrame"] = True
        # A target downstream of SceneColor (SSGIColor, SSRColor, ...) is a transient whose
        # memory later passes reuse: read it AS OF the pass the fixture read it after.
        if self.target_pass and target != "SceneColor":
            args["afterPass"] = self.target_pass
        reply = self.mcp.call("olo_render_probe_pixel", args)
        if not reply.get("available"):
            raise RuntimeError("%s (%d,%d) unavailable: %s" % (target, x, y, reply.get("reason")))
        if (reply.get("width"), reply.get("height")) != (width, height):
            raise RuntimeError("%s is %sx%s, the manifest's pixels address %dx%d (viewport override or "
                               "upscale not in effect)" % (target, reply.get("width"), reply.get("height"), width, height))
        texel = reply.get("mappedCoord", {}).get("texel")
        if texel != [x, y]:
            raise RuntimeError("probe of (%d,%d) read texel %r" % (x, y, texel))
        if reply.get("meta", {}).get("stale"):
            raise RuntimeError("the editor's frame loop is parked (meta.stale): values are from an old frame")
        value = reply.get("value") or []
        if len(value) < 3:
            raise RuntimeError("%s probe returned %r, not an RGB value" % (target, value))
        return [float(c) for c in value[:3]]

    # ---- one capture: open, play, settle, probe every pixel, stop ----------

    def capture(self, row, probe, state, scene_path, regions, width, height):
        # The fixture read each state from its own scene-band end (SSGI off has no SSGIColor).
        self.target, self.target_pass = (self.targets[1] if state else self.targets[0])
        self.open_scene(scene_path)
        self.restore_postprocess(row, probe, state, scene_path)
        self.play()
        try:
            first = regions[0]["pixels"][0]
            for _ in range(self.args.settle_calls):
                self.probe(first[0], first[1], width, height, force_frame=True)
            # An estimator row compares the estimator, not a silent fallback: olo_restir_stats
            # (McpToolsRender.cpp) must say the tier is ACTIVE this frame, or the cell fails.
            if state and self.estimator_field == "ReSTIRDIEnabled":
                stats = self.mcp.call("olo_restir_stats", {})
                availability = stats.get("availability", {})
                if not availability.get("active"):
                    raise RuntimeError("ReSTIR DI did not engage (%s): %s" % (
                        availability.get("status"), availability.get("fallbackReason")))
            values = {}
            for region in regions:
                values[region["name"]] = [self.probe(x, y, width, height) for x, y in region["pixels"]]
            # Settle control: the first region's MEAN, read again one rendered frame later,
            # held to the probe's own cross-arm tolerance around that mean. A single pixel is
            # the wrong control for a per-frame-noisy estimator (SSGI) and an absolute bound is
            # the wrong one for a large value (a 722-unit mirror highlight): both failed a
            # comparison that held.
            self.probe(first[0], first[1], width, height, force_frame=True)
            again = mean([self.probe(x, y, width, height) for x, y in regions[0]["pixels"]])
            before = mean(values[regions[0]["name"]])
            tol = self.cross_tolerance
            drift = max(abs(a - b) / (tol["relative"] * abs(b) + tol["absolute"]) for a, b in zip(again, before))
            return values, drift
        finally:
            self.stop()

    # ---- one row ---------------------------------------------------------

    def run_row(self, manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        row = manifest["row"]
        requires = manifest.get("requires", "")
        width, height = int(manifest["width"]), int(manifest["height"])
        base = os.path.dirname(manifest_path)
        print("== %s (%s)%s" % (row, manifest.get("motivation", ""), " requires " + requires if requires else ""))

        ray_query = requires in RAY_QUERY_REQUIREMENTS or requires.startswith("ReSTIR")
        if ray_query and self.args.backend != "vulkan":
            self.not_run.append((row, "needs %s: ray queries run on a Vulkan device only; this is %s" % (
                requires, self.args.backend)))
            print("  NOT RUN: needs %s (Vulkan ray queries)" % requires)
            return

        # McpToolsCamera.cpp olo_viewport_set_size {width, height} -> {override, width, height}.
        vp = self.mcp.call("olo_viewport_set_size", {"width": width, "height": height})
        if not vp.get("override") or (vp.get("width"), vp.get("height")) != (width, height):
            raise RuntimeError("olo_viewport_set_size applied %r, not %dx%d" % (vp, width, height))

        for path in self.args.paths:
            hosts = PATH_REQUIREMENTS.get(requires)
            if hosts and path not in hosts:
                reason = "%s runs on %s only" % (requires, "/".join(hosts))
                self.not_run.append(("%s %s" % (row, path), reason))
                print("  %-12s NOT RUN: %s" % (path, reason))
                continue
            self.set_renderer("renderpath", path)
            self.set_renderer("msaa", "1")  # the exported reference is single-sample
            for probe_name, probe in manifest.get("probes", {}).items():
                label = "%s/%s %s" % (row, probe_name, path)
                regions = [r for r in probe.get("regions", []) if r.get("pixels")]
                if not regions:
                    self.not_run.append((label, "the manifest lists no probe pixels"))
                    continue
                try:
                    on_target = (probe.get("target", "SceneColor"), probe.get("targetPass", ""))
                    self.targets = ((probe.get("targetOff", on_target[0]), probe.get("targetPassOff", on_target[1])),
                                    on_target)
                    self.estimator_field = manifest.get("estimatorField", "")
                    self.cross_tolerance = probe["crossArmTolerance"]
                    off, drift_off = self.capture(row, probe_name, False, os.path.join(base, probe["off"]),
                                                  regions, width, height)
                    on, drift_on = self.capture(row, probe_name, True, os.path.join(base, probe["on"]),
                                                regions, width, height)
                except RuntimeError as e:
                    if ray_query:
                        self.not_run.append((label, "attempted on %s and failed (the device may lack ray "
                                                    "queries): %s" % (self.args.backend, e)))
                        print("  %s NOT RUN: %s" % (label, e))
                    else:
                        self.results.append((label, False, "capture failed: %s" % e))
                        print("  %s CAPTURE FAILED: %s" % (label, e))
                    continue
                self.compare(label, probe, regions, off, on, max(drift_off, drift_on))

    def compare(self, label, probe, regions, off, on, drift):
        cross = probe["crossArmTolerance"]
        model_tol = probe["analyticTolerance"]
        settled = drift <= 1.0  # capture() normalises the drift by the cross-arm bound
        for region in regions:
            name = region["name"]
            term = mean([[a - b for a, b in zip(o, f)] for o, f in zip(on[name], off[name])])
            finite = all(c == c and abs(c) != float("inf") for c in term)
            gl = region.get("glReference")
            model = region.get("analytic")
            verdicts = []
            held = finite and settled
            if not finite:
                verdicts.append("NON-FINITE")
            if not settled:
                verdicts.append("UNSETTLED: the first region's mean moved %.3gx its cross-arm bound in one frame" % drift)
            if gl is not None:
                ok = within(term, gl, cross)
                held = held and ok
                verdicts.append("gl " + ("held" if ok else "FAILED"))
            if model is not None:
                ok = within(term, model, model_tol)
                held = held and ok
                verdicts.append("model " + ("held" if ok else "FAILED"))
            where = "%s %s" % (label, name)
            self.results.append((where, held, "%s live %s gl %s model %s" % (
                ", ".join(verdicts), fmt(term), fmt(gl), fmt(model))))
            print("  %-44s %-6s live %-30s gl %-30s model %-30s %s" % (
                where, "held" if held else "FAILED", fmt(term), fmt(gl), fmt(model), ", ".join(verdicts)))


def find_restore_scene(args, name):
    if args.restore_scene:
        return args.restore_scene
    if not name:
        return None
    hits = glob.glob(os.path.join(os.path.abspath(args.project_assets), "**", name + ".olo"), recursive=True)
    return hits[0] if len(hits) == 1 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, required=True, help="the editor's MCP port (driver.ps1 attach prints it)")
    ap.add_argument("--backend", choices=["opengl", "vulkan"], required=True,
                    help="the editor's backend; checked against the log's '[RHI] Backend:' line")
    ap.add_argument("--export-dir", required=True, help="the --olo-cross-path-export directory (<Row>.json + scenes)")
    ap.add_argument("--paths", default=",".join(PATHS), help="comma list of forward,forwardplus,deferred")
    ap.add_argument("--rows", default="", help="comma list of row names to replay (default: every <Row>.json)")
    ap.add_argument("--log", default=os.path.join(REPO, "OloEditor", "OloEngine.log"), help="the editor's log")
    ap.add_argument("--project-assets", default=os.path.join(REPO, "OloEditor", "SandboxProject", "Assets"),
                    help="the editor project's asset directory (olo_scene_open's relative-path root)")
    ap.add_argument("--restore-scene", default="", help="scene to reopen at the end (default: <name>.olo of the "
                                                         "scene open at the start, found under --project-assets)")
    ap.add_argument("--settle-calls", type=int, default=4,
                    help="forceFrame probes before reading (each renders >= 2 frames; the fixture renders 8)")
    args = ap.parse_args()
    args.paths = [p.strip() for p in args.paths.split(",") if p.strip()]
    bad = [p for p in args.paths if p not in PATHS]
    if bad:
        ap.error("unknown path(s) %s; expected %s" % (", ".join(bad), ",".join(PATHS)))

    try:
        manifests = sorted(glob.glob(os.path.join(args.export_dir, "*.json")))
        if args.rows:
            wanted = set(r.strip() for r in args.rows.split(",") if r.strip())
            manifests = [m for m in manifests if os.path.splitext(os.path.basename(m))[0] in wanted]
        if not manifests:
            raise SetupError("no row manifests (<Row>.json) in %s%s" % (
                args.export_dir, " matching --rows" if args.rows else ""))
        if not os.path.exists(args.log):
            raise SetupError("no editor log at %s (pass --log)" % args.log)
        seen = log_backend(args.log)
        if seen != args.backend:
            raise SetupError("--backend %s, but %s says '[RHI] Backend: %s'" % (args.backend, args.log, seen))
        log_start = log_size(args.log)
        mcp = Mcp(args.port)
        start = mcp.call("olo_scene_summary", {})
        session = Session(mcp, args)
        original = session.renderer_settings()  # also proves write consent: the tool is ProjectWrite
    except (SetupError, RuntimeError, OSError) as e:
        print("SETUP ERROR: %s" % e)
        return 2

    restore_scene = find_restore_scene(args, start.get("name"))
    setup_failed = None
    try:
        for manifest in manifests:
            session.run_row(manifest)
    except (RuntimeError, OSError) as e:
        setup_failed = e
    finally:
        # Hand the editor back as found ("MCP scripts must release their overrides"):
        # the viewport override otherwise keeps drawing a 384x256 image in a corner of
        # the panel. Each step runs even when an earlier one fails.
        steps = [("stop Play", lambda: session.stop()),
                 ("reset the viewport override", lambda: mcp.call("olo_viewport_set_size", {"reset": True}))]
        if restore_scene:
            steps.append(("reopen %s" % restore_scene, lambda: mcp.call("olo_scene_open", {
                "path": session.scene_arg(restore_scene) if os.path.exists(restore_scene) else restore_scene})))
        else:
            print("WARNING: the scene open at the start (%r) was not found as one <name>.olo under %s; it is NOT "
                  "reopened (pass --restore-scene)" % (start.get("name"), args.project_assets))
        for setting in ("renderpath", "msaa"):
            if original.get(setting):
                steps.append(("restore %s=%s" % (setting, original[setting]),
                              lambda s=setting: mcp.call("olo_renderer_settings_set",
                                                         {"setting": s, "value": original[s]})))
        for what, step in steps:
            try:
                step()
            except Exception as e:  # noqa: BLE001 - report every restore failure, keep restoring
                print("WARNING: could not %s: %s" % (what, e))

    errors, vuids = 0, 0
    samples = []
    if log_start is not None and os.path.exists(args.log):
        with open(args.log, "rb") as f:
            if log_size(args.log) >= log_start:
                f.seek(log_start)
            tail = f.read().decode("utf-8", errors="replace")
        for line in tail.splitlines():
            is_error, is_vuid = "[error]" in line, "VUID" in line
            errors += is_error
            vuids += is_vuid
            if (is_error or is_vuid) and len(samples) < 5:
                samples.append(line.strip()[:200])

    failed = [r for r in session.results if not r[1]]
    print("\n%d regions compared, %d held, %d failed; %d not run" % (
        len(session.results), len(session.results) - len(failed), len(failed), len(session.not_run)))
    for label, _, detail in failed:
        print("  FAILED %s: %s" % (label, detail))
    for label, reason in session.not_run:
        print("  NOT RUN %s: %s" % (label, reason))
    print("editor log during the run: %d [error] line(s), %d VUID line(s)" % (errors, vuids))
    for line in samples:
        print("  " + line)
    if setup_failed is not None:
        print("SETUP ERROR: %s" % setup_failed)
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
