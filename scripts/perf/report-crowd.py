"""Summarize native 1080p crowd captures. Pass result directories in run order."""

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import median


def summarize(directory):
    manifest = json.loads((directory / "manifest.yaml").read_text(encoding="utf-8"))
    kind, count_text = Path(manifest["Scene"]).stem.rsplit("_", 1)
    count = int(count_text)
    if kind not in ("crowd_reference", "crowd_shared_trial", "crowd_lod"):
        raise ValueError("expected a generated crowd capture manifest")
    result = json.loads((directory / "result.json").read_text(encoding="utf-8"))
    actual = result["output"]["actual"]
    if not isinstance(actual, dict):
        raise ValueError("capture has no observed resolution")
    if [actual[key] for key in ("renderWidth", "renderHeight", "displayWidth", "displayHeight")] != [1920, 1080] * 2:
        raise ValueError("expected native 1920 x 1080")
    with (directory / "measurement.csv").open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != manifest["Measurement"]["Frames"]:
        raise ValueError("incomplete measurement")
    valid = [row for row in rows if row["gpuStatus"] == "valid" and row["gpuMs"]]
    if len(valid) < 100 or len({row["gpuFrameId"] for row in valid}) != len(valid):
        raise ValueError("fewer than 100 unique valid GPU samples or repeated frame IDs")
    if kind == "crowd_lod" and count:
        # The fixture has one submesh and up to three nonempty clip batches.
        # Every successful GPU batch submits exactly three tier packets.
        expected_packets = 3 * min(count, 3)
        if any(int(row["animatedSubmeshPackets"]) != expected_packets or
               int(row["crowdMembersSubmitted"]) != count for row in rows):
            raise ValueError("timed frames did not submit the complete GPU crowd")
        visibility = json.loads((directory / "crowd-visibility.json").read_text(encoding="utf-8"))
        tiers = [visibility[key] for key in ("near", "mid", "far")]
        if visibility["members"] != count or sum(tiers) != count or (count == 5000 and min(tiers) == 0):
            raise ValueError(f"incomplete mixed-LOD population: {visibility}")
        tier_text = "/".join(map(str, tiers))
    else:
        if any(int(row["animatedSubmeshPackets"]) != count for row in rows):
            raise ValueError("dropped reference packets")
        tier_text = "n/a"

    def p50(samples, key):
        values = [float(sample[key]) for sample in samples]
        if any(not math.isfinite(v) or v < 0 for v in values):
            raise ValueError(f"invalid {key} measurement")
        return median(values)

    packets = p50(rows, "animatedSubmeshPackets")
    collapsed = p50(rows, "skinnedBatchedCommands")
    if kind == "crowd_reference" and any(int(row["skinnedBatchedCommands"]) for row in rows):
        raise ValueError("independent reference unexpectedly batched")
    return (f"| {directory.name} | {kind} | {count} | {tier_text} | {len(valid)} | "
            f"{p50(valid, 'gpuMs'):.3f} | {p50(rows, 'animatedSubmissionMs'):.3f} | "
            f"{packets:.0f} | {collapsed:.0f} | {p50(rows, 'drawCalls'):.0f} |")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    args = parser.parse_args()
    print("| Run | Workload | Actors | Near/mid/far | GPU samples | GPU ms | "
          "CPU submission ms | Geometry packets | Collapsed packets | Renderer draw counter |")
    print("|---|---|---:|---|---:|---:|---:|---:|---:|---:|")
    for capture in args.captures:
        try:
            print(summarize(capture))
        except (KeyError, ValueError, OSError) as exc:
            parser.exit(1, f"{capture}: {exc}\n")
