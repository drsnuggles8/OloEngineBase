#!/usr/bin/env python3
"""Replay renderer state-machine traces against a LIVE editor over MCP (issue #1349).

The in-process harness (OloEngine/tests/Rendering/StateMachine/) runs on OpenGL
only: RendererAttachedTest holds a GL context and no Vulkan scene fixture exists
(docs/agent-rules/testing-architecture.md s9-10). This script is the live-only
half of the manifest's Vulkan rows. It takes the SAME trace files, applies each
operation to a running editor (either backend), and after every operation
renders the same state through each pair that has an MCP lever:

  parallel-recording  OLO_VK_PARALLEL_RECORDING unset vs 0 (Vulkan only; GL never forks)
  alias               olo_render_debug_set disableAliasing false vs true
  serial-submission   OLO_RENDERER_SERIAL_MESH_SUBMISSION 0 vs 1
  cached-vs-rebuild   OLO_RG_VERIFY_DECLARATION_CACHE off vs on for one frame

and at the end compares the sequence-reached frame with the same configuration
reached directly from a freshly reopened scene. Each comparison is held against
a same-state control pair (two consecutive frames), so a live frame's own noise
is measured, not assumed: a difference counts only above twice the control.

Operations with no MCP lever (entity-churn, fence-drain, frames-in-flight,
scene-swap, pool-trim) are reported as NOT APPLIED rather than skipped quietly.

Usage (editor launched with OLO_MCP_AUTOSTART=1 and OLO_MCP_ALLOW_WRITES=1):
  python scripts/renderer-state-machine-live.py --port 7361 --backend vulkan --scene Scenes/MaterialLab.olo \
      OloEngine/tests/Rendering/StateMachine/corpus/*.trace
Exit code 0 when every applied comparison held, 1 otherwise.
"""

import argparse
import base64
import io
import json
import os
import sys
import tempfile
import urllib.request

import numpy as np
from PIL import Image

PATHS = {"forward": "forward", "forward-plus": "forwardplus", "deferred": "deferred"}
FEATURE_FIELDS = {
    "bloom": ["BloomEnabled"],
    "fxaa": ["FXAAEnabled"],
    "gtao": ["GTAOEnabled"],
    "gtao-denoise": ["GTAODenoiseEnabled"],
    "ssr": ["SSREnabled"],
    "vignette-grading": ["VignetteEnabled", "ColorGradingEnabled"],
}
# The trace's three poses, in the olo_camera_set_pose convention (degrees).
# The default frames the Sponza atrium; edit POSES for another scene.
POSES = [((0.0, 2.0, 0.0), 90.0, 5.0), ((0.0, 2.0, 0.0), -90.0, 5.0), ((0.0, 8.0, 0.0), 90.0, 25.0)]
NOT_APPLIED = {"entity-churn", "fence-drain", "frames-in-flight", "scene-swap", "pool-trim"}


class Mcp:
    def __init__(self, port):
        path = os.path.join(tempfile.gettempdir(), "oloengine-mcp-%d.json" % port)
        with open(path, encoding="utf-8") as f:
            disc = json.load(f)
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
        return result

    def frame(self):
        """One freshly rendered frame of the viewport as an RGB array."""
        result = self.call("olo_screenshot", {"forceFrame": True, "settleFrames": 2})
        for block in result.get("content", []):
            if block.get("type") == "image":
                img = Image.open(io.BytesIO(base64.b64decode(block["data"]))).convert("RGB")
                return np.asarray(img, dtype=np.int16)
        raise RuntimeError("olo_screenshot returned no image")

    def settle(self, frames=3):
        for _ in range(frames):
            self.call("olo_screenshot", {"forceFrame": True, "settleFrames": 1, "maxWidth": 64})


def parse_trace(path):
    initial, ops = {}, []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            words = line.split()
            if words[0] == "initial":
                initial = dict(w.split("=", 1) for w in words[1:])
            elif words[0] == "op":
                ops.append(words[1:])
    return initial, ops


def differing(a, b):
    if a.shape != b.shape:
        return a.shape[0] * a.shape[1], 255
    delta = np.abs(a - b).max(axis=2)
    return int((delta > 0).sum()), int(delta.max())


class Session:
    def __init__(self, mcp, scene, backend):
        self.mcp, self.scene, self.backend = mcp, scene, backend
        self.results = []

    def configure(self, cfg):
        m = self.mcp
        m.call("olo_scene_open", {"path": self.scene})
        m.call("olo_renderer_settings_set", {"setting": "renderpath", "value": PATHS[cfg.get("path", "forward")]})
        m.call("olo_renderer_settings_set", {"setting": "msaa", "value": cfg.get("msaa", "1")})
        m.call("olo_renderer_settings_set", {"setting": "upscale", "value": cfg.get("upscale", "off")})
        feats = [x for x in cfg.get("features", "").split(",") if x]
        for name, fs in FEATURE_FIELDS.items():
            for f in fs:
                m.call("olo_postprocess_settings_set", {"field": f, "value": name in feats})
        w, h = (int(v) for v in cfg.get("size", "320x180").split("x"))
        m.call("olo_viewport_set_size", {"width": w, "height": h})
        self.pose(int(cfg.get("pose", "0")))

    def pose(self, index):
        pos, yaw, pitch = POSES[index % len(POSES)]
        self.mcp.call("olo_camera_set_pose", {"position": list(pos), "yaw": yaw, "pitch": pitch})

    def apply(self, op, cfg):
        kind, args = op[0], op[1:]
        m = self.mcp
        if kind in NOT_APPLIED:
            return False
        if kind == "resize":
            w, h = (int(v) for v in args[0].split("x"))
            m.call("olo_viewport_set_size", {"width": w, "height": h})
            cfg["size"] = args[0]
        elif kind == "path":
            m.call("olo_renderer_settings_set", {"setting": "renderpath", "value": PATHS[args[0]]})
            cfg["path"] = args[0]
        elif kind == "feature":
            on = args[1] == "on"
            for f in FEATURE_FIELDS[args[0]]:
                m.call("olo_postprocess_settings_set", {"field": f, "value": on})
            feats = set(x for x in cfg.get("features", "").split(",") if x)
            (feats.add if on else feats.discard)(args[0])
            cfg["features"] = ",".join(sorted(feats))
        elif kind == "msaa":
            m.call("olo_renderer_settings_set", {"setting": "msaa", "value": args[0]})
            cfg["msaa"] = args[0]
        elif kind == "upscale":
            m.call("olo_renderer_settings_set", {"setting": "upscale", "value": args[0]})
            cfg["upscale"] = args[0]
        elif kind == "shader-reload":
            m.call("olo_shader_reload", {})
        elif kind == "scene-reload":
            self.configure(cfg)  # reopens the scene, then puts the trace's settings back over the scene's own
        elif kind == "history-advance":
            here = int(cfg.get("pose", "0"))
            for step in (1, 2):
                self.pose(here + step)
                m.settle(1)
            self.pose(here)
        elif kind == "camera-move":
            cfg["pose"] = args[0]
            self.pose(int(args[0]))
        return True

    def pair(self, name, where, control, flip, unflip):
        flip()
        self.mcp.settle(2)
        b = self.mcp.frame()
        unflip()
        self.mcp.settle(2)
        ctrl_px, _ = differing(control[0], control[1])
        px, mx = differing(control[1], b)
        held = px <= 2 * ctrl_px
        self.results.append((name, where, held, px, mx, ctrl_px))
        print("  %-18s %s  %6d px differ (max %3d), control %d" % (name, "held  " if held else "FAILED", px, mx, ctrl_px))

    def checkpoint(self, where):
        m = self.mcp
        m.settle(3)
        control = (m.frame(), m.frame())
        print(" %s" % where)
        if self.backend == "vulkan":
            self.pair("parallel-recording", where, control,
                      lambda: m.call("olo_cvar_set", {"name": "OLO_VK_PARALLEL_RECORDING", "value": "off"}),
                      lambda: m.call("olo_cvar_set", {"name": "OLO_VK_PARALLEL_RECORDING", "value": "unset"}))
        self.pair("alias", where, control,
                  lambda: m.call("olo_render_debug_set", {"disableAliasing": True}),
                  lambda: m.call("olo_render_debug_set", {"disableAliasing": False}))
        self.pair("serial-submission", where, control,
                  lambda: m.call("olo_cvar_set", {"name": "OLO_RENDERER_SERIAL_MESH_SUBMISSION", "value": "on"}),
                  lambda: m.call("olo_cvar_set", {"name": "OLO_RENDERER_SERIAL_MESH_SUBMISSION", "value": "off"}))
        self.pair("cached-vs-rebuild", where, control,
                  lambda: m.call("olo_cvar_set", {"name": "OLO_RG_VERIFY_DECLARATION_CACHE", "value": "on"}),
                  lambda: m.call("olo_cvar_set", {"name": "OLO_RG_VERIFY_DECLARATION_CACHE", "value": "off"}))

    def run(self, path):
        initial, ops = parse_trace(path)
        cfg = dict(initial)
        print("== %s (%d ops, %s)" % (os.path.basename(path), len(ops), self.backend))
        self.configure(cfg)
        self.checkpoint("initial")
        for i, op in enumerate(ops):
            applied = self.apply(op, cfg)
            where = "after op %d '%s'%s" % (i + 1, " ".join(op), "" if applied else " (NOT APPLIED: no MCP lever)")
            self.checkpoint(where)
        m = self.mcp
        m.settle(3)
        seq = (m.frame(), m.frame())
        self.configure(cfg)
        m.settle(5)
        fresh = (m.frame(), m.frame())
        ctrl = max(differing(*seq)[0], differing(*fresh)[0])
        px, mx = differing(seq[1], fresh[1])
        held = px <= 2 * ctrl
        self.results.append(("fresh-vs-sequence", "end of trace", held, px, mx, ctrl))
        print("  %-18s %s  %6d px differ (max %3d), control %d" % ("fresh-vs-sequence", "held  " if held else "FAILED", px, mx, ctrl))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--scene", required=True, help="Assets-relative scene path, e.g. Scenes/MaterialLab.olo")
    ap.add_argument("--backend", choices=["opengl", "vulkan"], required=True,
                    help="what the editor log's '[RHI] Backend:' line says; the parallel-recording pair is Vulkan-only")
    ap.add_argument("traces", nargs="+")
    args = ap.parse_args()
    session = Session(Mcp(args.port), args.scene, args.backend)
    try:
        for path in args.traces:
            session.run(path)
    finally:
        # Hand the editor back as found: the trace sizes are an MCP viewport
        # OVERRIDE, which otherwise keeps the viewport drawing a 320x180 image
        # in the corner of the panel, and a reopen restores the scene's own
        # render path and post-process settings.
        session.mcp.call("olo_viewport_set_size", {"reset": True})
        session.mcp.call("olo_scene_open", {"path": args.scene})
    failed = [r for r in session.results if not r[2]]
    print("\n%d comparisons, %d held, %d failed" % (len(session.results), len(session.results) - len(failed), len(failed)))
    for name, where, _, px, mx, ctrl in failed:
        print("  FAILED %s %s: %d px (max %d), control %d" % (name, where, px, mx, ctrl))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
