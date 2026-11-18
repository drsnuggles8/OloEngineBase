"""Validate the glTF and publish a finished Stacks Light build."""

import filecmp
import json
import os
import shutil
import stat
import tempfile
from pathlib import Path


def staging_parent(destination):
    """Stay on the output filesystem, outside the editor's watched asset tree."""
    parent = destination.parent
    parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.gettempdir()).resolve()
    if scratch.stat().st_dev == parent.stat().st_dev:
        return scratch
    return next(
        (p.parent for p in destination.parents if p.name.casefold() == "assets"), parent
    )


def finalize_descriptor(staged, finishes):
    descriptor = staged / "StacksLight.gltf"
    doc = json.loads(descriptor.read_text(encoding="utf-8"))
    images = doc["images"]
    textures = doc["textures"]
    seen = set()
    for mat in doc["materials"]:
        name = mat["name"]
        if name not in finishes:
            continue
        pbr = mat["pbrMetallicRoughness"]
        slots = (
            pbr.get("baseColorTexture"),
            mat.get("normalTexture"),
            pbr.get("metallicRoughnessTexture"),
        )
        for slot, image_name in zip(slots, finishes[name], strict=True):
            expected = image_name + ".png"
            uri = (
                None
                if slot is None
                else images[textures[slot["index"]]["source"]]["uri"]
            )
            if uri != expected:
                raise ValueError(
                    f"{name}: expected {expected}, exporter referenced {uri!r}"
                )
        # The normal and colour samplers are independent; AO is the R
        # channel of the same image used for roughness and metallic.
        mat["occlusionTexture"] = {"index": pbr["metallicRoughnessTexture"]["index"]}
        seen.add(name)
    if seen != finishes.keys():
        raise ValueError(f"Missing exported finishes: {sorted(finishes.keys() - seen)}")

    names = []
    for resource in [*images, *doc["buffers"]]:
        uri = resource["uri"]
        if Path(uri).name != uri:
            raise ValueError(f"Expected a sibling resource, got {uri!r}")
        size = (staged / uri).stat().st_size
        if not size or size != resource.get("byteLength", size):
            raise ValueError(f"Incomplete glTF resource: {uri}")
        if uri not in names:
            names.append(uri)
    descriptor.write_text(
        json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    return [*names, descriptor.name]


def publish_package(staged, destination, names):
    """Publish payloads then the descriptor, retaining originals for rollback."""
    destination.mkdir(parents=True, exist_ok=True)
    # Atomic directory creation serializes publishers targeting this package.
    # If interrupted, the recovery directory is deliberately left inspectable.
    # Use the same lock across users and temporary directories. Backup files
    # end in .previous so editor asset watchers cannot import them.
    recovery = destination / ".stacks-light-recovery"
    recovery.mkdir()
    replaced = []
    remove_recovery = True
    try:
        changed = []
        for name in names:
            old = destination / name
            if old.is_symlink() or (old.exists() and not old.is_file()):
                raise ValueError(f"Expected a regular output file: {old}")
            if old.exists():
                if filecmp.cmp(staged / name, old, shallow=False):
                    continue
                if not old.stat().st_mode & stat.S_IWRITE:
                    raise PermissionError(f"Read-only output: {old}")
                shutil.copy2(old, recovery / (name + ".previous"))
            changed.append(name)
        for name in changed:
            os.replace(staged / name, destination / name)
            replaced.append(name)
    except BaseException:
        # Keep backups if the filesystem itself prevents restoration.
        remove_recovery = False
        for name in reversed(replaced):
            backup = recovery / (name + ".previous")
            if backup.exists():
                os.replace(backup, destination / name)
            else:
                (destination / name).unlink()
        remove_recovery = True
        raise
    finally:
        if remove_recovery:
            shutil.rmtree(recovery)
