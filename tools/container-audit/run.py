#!/usr/bin/env python3
"""Reproduce the lexical map-borrow audit; candidates never authorize TMap adoption.

Run AST work through the repository build-lock wrapper. Each --case invocation
processes at most one real TU; --case fixture is an independent matcher check.
The inventory is exhaustive textual type usage, not a declaration AST or proof.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
AUDIT = Path(__file__).resolve().parent
CASES = {
    "Renderer": "OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualGeometryPageStore.cpp",
    "Terrain": "OloEngine/src/OloEngine/Terrain/TerrainStreamer.cpp",
    "Dialogue": "OloEngine/src/OloEngine/Dialogue/DialogueSystem.cpp",
    "Scene": "OloEngine/src/OloEngine/Scene/Scene.cpp",
    "UI": "OloEngine/src/OloEngine/UI/UINavigationSystem.cpp",
    "fixture": "tools/container-audit/map-borrows-fixture.cpp",
}
EXPECTED = {
    "SafeLocalBorrow": False,
    "UnsafeAcrossInsert": True,
    # Required negative controls: all three are known matcher false negatives.
    "EscapingBorrow": False,
    "StoreBorrow": False,
    "UnsafeAcrossHelper": False,
    # Lexical scope alone also produces false positives.
    "SafeMutationBeforeBorrow": True,
    "SafeOtherMapMutation": True,
}


def inventory(output, results):
    rows = []
    counts = {}
    for subsystem in CASES.keys() - {"fixture"}:
        directory = f"OloEngine/src/OloEngine/{subsystem}"
        run = subprocess.run(
            ["rg", "--json", r"std::(?:unordered_)?(?:multi)?map\s*<", directory],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=False,
        )
        if run.returncode not in (0, 1):
            raise RuntimeError(run.stderr)
        counts[subsystem] = 0
        for line in run.stdout.splitlines():
            record = json.loads(line)
            if record["type"] != "match":
                continue
            data = record["data"]
            path = data["path"]["text"].replace("\\", "/")
            status = "not-scanned-main-file"
            if path == CASES[subsystem]:
                result = results.get(subsystem)
                status = "selected-not-run" if result is None else (
                    "parse-failed" if result["parse_failed"] else "scanned-main-file"
                )
            rows.append((subsystem, path, data["line_number"], status,
                         data["lines"]["text"].strip().replace("\t", " ")))
            counts[subsystem] += 1
    rows.sort(key=lambda row: (row[0], row[1], row[2]))
    with (output / "inventory.tsv").open("w", encoding="utf-8", newline="") as stream:
        stream.write("subsystem\tpath\tline\tmain_file_coverage\ttype_usage\n")
        for row in rows:
            stream.write("\t".join(map(str, row)) + "\n")
    return dict(sorted(counts.items()))


def run_case(case, database, output, executable):
    source = ROOT / CASES[case]
    source_sha = hashlib.sha256(source.read_bytes()).hexdigest()
    entry_path = CASES["Terrain"] if case == "fixture" else CASES[case]
    entry = next((e for e in database
                  if Path(e["file"]).resolve() == (ROOT / entry_path).resolve()), None)
    if entry is None:
        return {"source": CASES[case], "parse_failed": True,
                "reason": "Source absent from compilation database"}
    entry = dict(entry)
    if case == "fixture":
        entry["file"] = str(source)
        # CMake's clang-cl commands use `-- source`; preserve all actual flags.
        if " -- " not in entry["command"]:
            raise RuntimeError("Fixture requires a clang-cl compile command with -- source")
        entry["command"] = entry["command"].rsplit(" -- ", 1)[0] + f' -- "{source}"'
    with tempfile.TemporaryDirectory(prefix="olo-map-audit-") as temporary:
        Path(temporary, "compile_commands.json").write_text(json.dumps([entry]), encoding="utf-8")
        command = [executable, f"-p={temporary}", f"-f={AUDIT / 'map-borrows.query'}", str(source)]
        run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", check=False)
    text = run.stdout + run.stderr
    (output / f"{case}.log").write_text(text, encoding="utf-8")
    source_changed = hashlib.sha256(source.read_bytes()).hexdigest() != source_sha
    parse_failed = run.returncode != 0 or bool(re.search(r"\berror:", text)) or source_changed
    match_count = re.findall(r"(\d+) matches?\.", text)
    result = {"source": CASES[case], "exit_code": run.returncode,
              "source_sha256": source_sha,
              "source_changed_during_parse": source_changed,
              "parse_failed": parse_failed,
              "matches": int(match_count[-1]) if match_count else None,
              "log": f"{case}.log"}
    if case == "fixture":
        observed = {name: bool(re.search(rf"\b{name}\(", text)) for name in EXPECTED}
        result["expected_matches"] = EXPECTED
        result["observed_matches"] = observed
        result["fixture_passed"] = not parse_failed and observed == EXPECTED
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, default=ROOT / "build-cached/compile_commands.json")
    parser.add_argument("--output", type=Path, default=ROOT / "build-cached/container-audit")
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--clang-query", default=shutil.which("clang-query"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    summary_path = args.output / "summary.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8")) if summary_path.exists() else {}
    results = summary.setdefault("cases", {})
    if args.case:
        if not args.clang_query:
            parser.error("clang-query was not found; supply --clang-query")
        database = json.loads(args.database.read_text(encoding="utf-8"))
        summary["database_sha256"] = hashlib.sha256(args.database.read_bytes()).hexdigest()
        summary["query_sha256"] = hashlib.sha256((AUDIT / "map-borrows.query").read_bytes()).hexdigest()
        summary["git_revision"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, encoding="utf-8"
        ).strip()
        summary["clang_query_version"] = subprocess.check_output(
            [args.clang_query, "--version"], text=True, encoding="utf-8"
        ).strip()
        results[args.case] = run_case(args.case, database, args.output, args.clang_query)
        for key in ("database_sha256", "query_sha256", "git_revision", "clang_query_version"):
            results[args.case][key] = summary[key]
        print(json.dumps(results[args.case], indent=2))
    summary["map_type_usage_lines"] = inventory(args.output, results)
    summary["decision"] = "TMap gate closed: lexical matcher misses escaped and helper-mediated borrows"
    summary["coverage_note"] = "Only listed main files are AST-scanned; headers and all other files remain unproved."
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Inventory and results: {args.output}")
    if args.case:
        result = results[args.case]
        return int(result["parse_failed"] or not result.get("fixture_passed", True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
