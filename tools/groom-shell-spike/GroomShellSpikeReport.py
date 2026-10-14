"""Summarise a complete #1611 campaign; raw evidence stays outside Git.

Run from the repo root after GroomShellSpike*Evidence:
    python tools/groom-shell-spike/GroomShellSpikeReport.py .v-build/1611
Requires Pillow only for the optional contact sheets, not for tables/validation.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def validate_frames(samples, label, numeric_fields):
    frames = set()
    for sample in samples:
        frame = int(sample["frame"])
        if frame in frames or str(frame) != sample["frame"]:
            raise ValueError(f"{label}: duplicate or invalid frame")
        frames.add(frame)
        for field in numeric_fields:
            value = float(sample[field])
            if not math.isfinite(value) or value < 0:
                raise ValueError(f"{label}: invalid {field} at frame {frame}")
    if frames != set(range(64)):
        raise ValueError(f"{label}: incomplete frames 0..63")


def read_rows(root, coat):
    with (root / f"{coat}_metrics.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    if len(rows) != 456:
        raise ValueError(f"{coat}: incomplete campaign: {len(rows)} / 456 rows")
    names = set()
    for row in rows:
        if row["name"] in names:
            raise ValueError(f"duplicate row {row['name']}")
        names.add(row["name"])
        for field in row:
            if field != "name":
                row[field] = float(row[field])
                if not math.isfinite(row[field]):
                    raise ValueError(f"non-finite {field}: {row['name']}")
        if row["gpu_ms"] <= 0:
            raise ValueError(f"invalid GPU timing {row['name']}")
        for suffix in ("frame0.png", "mean64.png", "sequence.csv"):
            if not (root / f"{row['name']}_{suffix}").is_file():
                raise ValueError(f"missing evidence {row['name']}_{suffix}")
        with (root / f"{row['name']}_sequence.csv").open(newline="") as source:
            validate_frames(list(csv.DictReader(source)), row["name"],
                            ("area", "energy", "mean_area", "mean_energy", "delta_area"))
    with (root / f"{coat}_timings.csv").open(newline="") as source:
        samples = list(csv.DictReader(source))
    by_name = {name: [] for name in names}
    for sample in samples:
        if sample["name"] not in by_name:
            raise ValueError(f"unexpected GPU sample: {sample['name']}")
        by_name[sample["name"]].append(sample)
    for name, group in by_name.items():
        validate_frames(group, name, ("gpu_ms",))
        values = sorted(float(sample["gpu_ms"]) for sample in group)
        median = (values[31] + values[32]) * 0.5
        recorded = next(row["gpu_ms"] for row in rows if row["name"] == name)
        if median <= 0 or not math.isclose(median, recorded, rel_tol=2e-5, abs_tol=1e-8):
            raise ValueError(f"{name}: invalid or mismatched median GPU timing")
        next(row for row in rows if row["name"] == name)["zero_gpu_samples"] = sum(value == 0 for value in values)
    return rows


def interval(rows, field, decimals=3):
    values = [row[field] for row in rows]
    lo, hi = min(values), max(values)
    return f"{lo:.{decimals}f}–{hi:.{decimals}f}"


def selected(rows, size, arm):
    return [row for row in rows if row["pixels"] == size and row["width"] == 1280
            and row["samples"] == 1 and row["motion"] == 0 and row["name"].endswith("_" + arm)]


def table(rows):
    lines = ["| px | arm | coverage / strands | energy / strands | GPU draw ms | mean abs coverage error |",
             "|---:|---|---:|---:|---:|---:|"]
    for size in (128, 64, 32, 16, 8, 4):
        for arm in ("strands", "cards", "shell8_jitter", "shell12_jitter", "shell16_jitter"):
            group = selected(rows, size, arm)
            if len(group) != 3:
                raise ValueError(f"missing angle: {size}, {arm}")
            lines.append(f"| {size} | {arm.replace('_jitter', '')} | {interval(group, 'coverage_ratio')} | "
                         f"{interval(group, 'energy_ratio')} | {interval(group, 'gpu_ms', 4)} | {interval(group, 'mean_abs', 6)} |")
    return "\n".join(lines)


def contact_sheets(root, coat):
    from PIL import Image, ImageDraw
    arms = ("strands", "cards", "shell8_jitter", "shell12_jitter", "shell16_jitter")
    for size in (128, 32, 8, 4):
        sheet = Image.new("RGB", (5 * 260, 3 * 286), "#303030")
        draw = ImageDraw.Draw(sheet)
        for angle in range(3):
            for col, arm in enumerate(arms):
                name = f"{coat}_c0_p{size}_a{angle}_static_{arm}"
                image = Image.open(root / f"{name}_mean64.png").convert("RGB")
                # Common display gain for inspection of very sparse coverage.
                # Raw PNGs and numerical measurements remain unchanged.
                image = image.point([min(255, value * 8) for value in range(256)] * 3)
                if size <= 32:
                    # Nearest neighbour magnification shows individual samples.
                    image = image.crop((96, 96, 160, 160)).resize((256, 256), Image.Resampling.NEAREST)
                sheet.paste(image, (col * 260, angle * 286 + 25))
                draw.text((col * 260 + 4, angle * 286 + 5), f"{coat} {size}px a{angle} {arm} x8", fill="white")
        sheet.save(root / f"{coat}_{size}px_contact.png")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--contact-sheets", action="store_true", help="generate inspection sheets (requires Pillow)")
    args = parser.parse_args()
    root = args.evidence
    summary = {}
    tables = []
    for coat in ("short", "long"):
        rows = read_rows(root, coat)
        tables.append(f"## {coat.title()} coat\n\nRanges across front, oblique and silhouette views; 720p, 1x, 64 frames.\n\n" + table(rows))
        candidates = [row for row in rows if row["layers"] > 0]
        within_coverage = [row for row in candidates if abs(row["coverage_ratio"] - 1) <= 0.10]
        # The issue's coverage condition uses the existing ±10% area bar.
        # Diagnostic energy and shell/card MAE have no inherited pass bar.
        # A range must hold across the measured views/configurations; a lucky
        # low-pixel stochastic sample is not a representation selection.
        ranges = []
        for size in (128, 64, 32, 16, 8, 4):
            for layers in (8, 12, 16):
                for jitter in (0, 1):
                    group = [row for row in candidates if row["pixels"] == size and row["layers"] == layers
                             and row["jitter"] == jitter and row["motion"] == 0]
                    if len(group) != 9:
                        raise ValueError(f"missing configuration/view for {coat}, {size}, {layers}, {jitter}")
                    if all(abs(row["coverage_ratio"] - 1) <= 0.10 for row in group):
                        ranges.append({"pixels": size, "layers": layers, "jitter": jitter})
        summary[coat] = {
            "rows": len(rows), "coverage_cells_within_10pct": len(within_coverage),
            "zero_gpu_samples": sum(row["zero_gpu_samples"] for row in rows),
            "coverage_groups_with_all_views_configs_within_10pct": ranges,
            "shell_coverage_range": interval(candidates, "coverage_ratio"),
            "shell_energy_range": interval(candidates, "energy_ratio"),
            "jitter_native_error": interval([r for r in candidates if r["jitter"] == 1], "native_error", 6),
            "fixed_native_error": interval([r for r in candidates if r["jitter"] == 0], "native_error", 6),
            "motion_residual": interval([r for r in candidates if r["motion"] == 1], "motion_residual", 6),
        }
        if args.contact_sheets:
            contact_sheets(root, coat)
    (root / "tables.md").write_text("\n\n".join(tables) + "\n", encoding="utf-8")
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    manifest = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(root.glob("*")) if p.is_file() and p.suffix in (".csv", ".bin", ".png", ".txt")}
    (root / "evidence-sha256.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
