#!/usr/bin/env python3
"""Ask a shader toolchain whether it knows every `#extension` production shaders declare.

This is the PR-level guard for issue #1139 (ADR 0011 amendment (97)). It exists
because the hole that let #1139 land is structural, not a one-off: same-repo PRs
route the Linux sanitizer jobs to the self-hosted box, which carries the LunarG
SDK, so the HOSTED arm -- the one PR CI would otherwise never exercise -- is
reached only by the nightly and by `force_hosted` dispatches. Any shader feature
newer than the toolchain that arm resolves was therefore invisible until the next
nightly, two hours into a sanitizer build, in a glslang diagnostic that named a
line in an include file.

WHY THE UNIT IS AN EXTENSION AND NOT A SHADER. Compiling the real shaders here
would mean re-deriving the per-stage tier rules ShaderCompilationTest.cpp owns
(which stages take vulkan_1_2 without OLO_VULKAN, which take vulkan_1_4 with it,
which go through the OpenGL target env at all) -- two copies of a rule that has
already been edited three times. The question this guard needs to answer is
narrower and has no tiers in it: *does the toolchain know this extension name?*
That is drift-free, it runs in about a second, and it is exactly the failure
class -- `error: '#extension' : extension not supported`.

It also compiles ShaderToolchainFloor.h's own layout probe, read out of the
header rather than copied, so the qualifiers half of #1139's diagnostic is
covered by the same run.

Usage:
    python scripts/check_shader_extension_floor.py [--glslc PATH] [--shaders DIR]

`--glslc` defaults to $GLSLC, then to the SDK's binary via $VULKAN_SDK, then to
whatever is on PATH. Exit status is 1 if any declared extension is rejected.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SHADER_ROOT = REPO_ROOT / "OloEditor" / "assets" / "shaders"
FLOOR_HEADER = REPO_ROOT / "OloEngine" / "src" / "OloEngine" / "Renderer" / "ShaderToolchainFloor.h"

# The Vulkan tier's env and dialect, matching VulkanShader.cpp and
# ShaderToolchainFloor.h. A lower env answers a different question: glslang gates
# some extensions on the SPIR-V version, so a sweep at vulkan1.0 would pass
# extensions the real compile rejects.
TARGET_ARGS = ["--target-env=vulkan1.4", "--target-spv=spv1.6"]

# `#extension NAME : behaviour`, with the directive at the start of a line. GLSL
# allows whitespace between `#` and the keyword.
EXTENSION_RE = re.compile(r"^\s*#\s*extension\s+([A-Za-z_][A-Za-z0-9_]*)\s*:", re.MULTILINE)

# Comment-stripping before the scan, for the reason ShaderSourceScan.h documents
# on the C++ side: DescriptorHeapTextures.glsl's header comment names
# GL_EXT_descriptor_heap four times, and a shader that merely explains an
# extension has not declared one.
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text):
    return LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", text))


def find_glslc(explicit):
    if explicit:
        return explicit
    from_env = os.environ.get("GLSLC")
    if from_env:
        return from_env
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for candidate in (Path(sdk) / "Bin" / "glslc.exe", Path(sdk) / "bin" / "glslc"):
            if candidate.exists():
                return str(candidate)
    return "glslc"


def collect_declarations(shader_root):
    """Map extension name -> sorted list of repo-relative files declaring it."""
    declarations = {}
    for path in sorted(shader_root.rglob("*")):
        if path.suffix.lower() not in (".glsl", ".comp", ".vert", ".frag"):
            continue
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for name in EXTENSION_RE.findall(text):
            if name == "all":
                # `#extension all : ...` names no extension; it sets the default
                # behaviour for every one of them and cannot be probed.
                continue
            try:
                rel = path.relative_to(REPO_ROOT).as_posix()
            except ValueError:
                # --shaders pointed outside the repo; report the path as given.
                rel = path.as_posix()
            declarations.setdefault(name, set()).add(rel)
    return {name: sorted(files) for name, files in sorted(declarations.items())}


def read_layout_probe():
    """Lift kLayoutProbeSource out of ShaderToolchainFloor.h.

    Read rather than copied so the engine's runtime probe, the configure-time
    probe and this guard cannot disagree about what "the floor" means.
    """
    text = FLOOR_HEADER.read_text(encoding="utf-8")
    start = text.find('kLayoutProbeSource = R"GLSL(')
    if start < 0:
        raise SystemExit(
            f"{FLOOR_HEADER}: no kLayoutProbeSource raw-string literal found. "
            "This guard reads the probe out of the header on purpose; if the "
            "constant was renamed, update this script rather than copying it."
        )
    start = text.index("(", start) + 1
    end = text.index(')GLSL"', start)
    return text[start:end]


def compile_source(glslc, source, label):
    """Run glslc on `source` as a fragment shader. Returns (ok, diagnostic)."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.frag"
        src.write_text(source, encoding="utf-8")
        proc = subprocess.run(
            [glslc, *TARGET_ARGS, "-fshader-stage=fragment", str(src), "-o", os.devnull],
            capture_output=True,
            text=True,
        )
    if proc.returncode == 0:
        return True, ""
    diagnostic = (proc.stderr or proc.stdout).strip()
    return False, diagnostic.replace(str(src), label) if diagnostic else "glslc failed with no output"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--glslc", default=None, help="glslc to probe with (default: $GLSLC, $VULKAN_SDK, PATH)")
    parser.add_argument("--shaders", default=str(DEFAULT_SHADER_ROOT), help="production shader root")
    args = parser.parse_args()

    glslc = find_glslc(args.glslc)
    try:
        version = subprocess.run([glslc, "--version"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        # Loud, not skipped: a guard that quietly passes when it cannot find its
        # compiler is worse than no guard, because it reports green.
        print(f"error: cannot run '{glslc}': {exc}", file=sys.stderr)
        return 2
    print(f"toolchain under test: {glslc}\n{version}\n")

    shader_root = Path(args.shaders)
    if not shader_root.is_dir():
        print(f"error: shader root '{shader_root}' does not exist", file=sys.stderr)
        return 2

    declarations = collect_declarations(shader_root)
    if not declarations:
        print(f"error: no `#extension` directives found under {shader_root} — the scan is broken", file=sys.stderr)
        return 2

    # BASELINE FIRST, and this is not defensive padding — it is the whole point
    # of the exercise applied to this script. Run against Ubuntu 24.04's glslc
    # (shaderc 2023.8), every one of the 13 extensions below "REJECTED", and
    # every diagnostic read `glslc: error: invalid value 'vulkan1.4' in
    # '--target-env=vulkan1.4'`: ONE toolchain-level failure, reported thirteen
    # times as if each extension were the problem. That is the same shape of
    # misleading diagnostic #1139 was, so it gets caught here and named once.
    ok, diagnostic = compile_source(glslc, "#version 460 core\nvoid main() {}\n", "baseline")
    if not ok:
        print("\n" + "=" * 78, file=sys.stderr)
        print(
            f"This toolchain cannot compile a TRIVIAL shader at {' '.join(TARGET_ARGS)}, so it cannot\n"
            "be asked about extensions at all — the engine's Vulkan tier compiles every production\n"
            f"shader at that target.\n\n  {glslc} says:\n    {diagnostic}\n\n"
            "Ubuntu 24.04's glslc (shaderc 2023.8) fails exactly here: it predates Vulkan 1.4.\n"
            "Install Vulkan SDK 1.4.357.0 or newer, or point --glslc at the prefix\n"
            ".github/actions/setup-shaderc-linux builds. See ADR 0011 amendment (97).",
            file=sys.stderr,
        )
        return 1

    failures = []
    width = max(len(name) for name in declarations)
    for name, files in declarations.items():
        source = f"#version 460 core\n#extension {name} : require\nvoid main() {{}}\n"
        ok, diagnostic = compile_source(glslc, source, name)
        print(f"  {name:<{width}}  {'ok' if ok else 'REJECTED'}  ({len(files)} shader(s))")
        if not ok:
            failures.append((name, files, diagnostic))

    # The qualifiers, not just the names — #1139's diagnostic had both halves.
    ok, diagnostic = compile_source(glslc, read_layout_probe(), "ShaderToolchainFloor.h:kLayoutProbeSource")
    print(f"  {'descriptor_heap layout qualifiers':<{width}}  {'ok' if ok else 'REJECTED'}")
    if not ok:
        failures.append(("the descriptor_heap / descriptor_stride layout qualifiers", [str(FLOOR_HEADER)], diagnostic))

    if not failures:
        print(f"\nAll {len(declarations)} declared extensions compile on this toolchain.")
        return 0

    print("\n" + "=" * 78, file=sys.stderr)
    print("This toolchain is BELOW the floor these shaders declare.", file=sys.stderr)
    for name, files, diagnostic in failures:
        print(f"\n  {name}\n    declared by:", file=sys.stderr)
        for f in files:
            print(f"      {f}", file=sys.stderr)
        print(f"    {glslc} says:\n      {diagnostic}", file=sys.stderr)
    print(
        "\nEither the toolchain is too old (install Vulkan SDK 1.4.357.0 or newer;\n"
        "CI's hosted Linux arm gets it from .github/actions/setup-shaderc-linux), or a\n"
        "shader has just started declaring an extension newer than the pinned floor —\n"
        "in which case raise the pin in setup-vulkan / setup-shaderc-linux and\n"
        "ShaderToolchainFloor.h together. See ADR 0011 amendment (97).",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
