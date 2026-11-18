"""Validate the glTF and publish a finished Stacks Light build."""

import filecmp
import json
import os
import shutil
import stat
import tempfile
from pathlib import Path


def create_staging_directory():
    """Allocate a writable private directory outside the editor's watched projects."""
    # gettempdir() normally supplies a writable directory. A developer can
    # point TMPDIR into the project, so also try its sibling, other configured
    # temp roots, and the user's home. Actually creating the directory checks
    # permissions (including Windows ACLs) without an access-check race.
    try:
        preferred = tempfile.gettempdir()
    except OSError:
        preferred = None
    home = os.path.expanduser("~")
    if os.name == "nt":
        # gettempdir may have succeeded inside a watched project. Its other
        # documented Windows candidates still deserve an actual allocation try.
        local_data = os.getenv("LOCALAPPDATA")
        system = os.getenv("SYSTEMROOT")
        platform_roots = (
            Path(local_data) / "Temp" if local_data else None,
            Path(home) / "AppData/Local/Temp" if home != "~" else None,
            Path(system) / "Temp" if system else None,
            r"C:\Temp",
            r"C:\tmp",
            r"\Temp",
            r"\tmp",
        )
    else:
        platform_roots = ("/tmp", "/var/tmp", "/usr/tmp")
    roots = (
        preferred,
        os.getenv("TMPDIR"),
        os.getenv("TEMP"),
        os.getenv("TMP"),
        *platform_roots,
        None if home == "~" else home,
    )
    tried = set()
    last_error = None
    for root in roots:
        if not root:
            continue
        try:
            candidate = Path(root).resolve()
            for folder in (candidate, *candidate.parents):
                # glob suppresses listing errors. A directory we can traverse
                # but cannot inspect is not proof that staging is unwatched.
                with os.scandir(folder) as entries:
                    is_project = any(
                        entry.name.lower().endswith(".oloproj") and entry.is_file()
                        for entry in entries
                    )
                if is_project:
                    if folder.parent == folder:
                        candidate = None
                        break
                    candidate = folder.parent
            if candidate is None or candidate in tried:
                continue
            tried.add(candidate)
            return tempfile.TemporaryDirectory(dir=candidate, prefix=".stacks-bake-")
        except OSError as error:
            last_error = error
    raise OSError(
        "Set TMPDIR to a writable directory outside the watched project"
    ) from last_error


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
    try:
        recovery.mkdir()
    except FileExistsError as error:
        raise RuntimeError(
            f"Publication already active or interrupted at {recovery}. "
            "Leave it in place until the publisher exits; see README.md for recovery."
        ) from error

    changed = []
    remove_recovery = True
    try:
        existing, added = [], []
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
                existing.append(name)
            else:
                added.append(name)
            # Copy every validated payload onto the destination mount before
            # the first replacement. Its .pending suffix is invisible to import.
            shutil.copyfile(staged / name, recovery / (name + ".pending"))
            changed.append(name)
        # The marker is installed atomically. Without it no replacement started.
        plan = recovery / "transaction.pending"
        plan.write_text(
            json.dumps({"existing": existing, "added": added}), encoding="utf-8"
        )
        os.replace(plan, recovery / "transaction.plan")
        for name in changed:
            os.replace(recovery / (name + ".pending"), destination / name)
    except BaseException as publish_error:
        remove_recovery = False
        errors = []
        # os.replace can finish just before Python receives an interrupt.
        # The consumed .pending file records the rename without a second
        # bookkeeping operation. Preparation errors never install the plan.
        published = (
            reversed(changed) if (recovery / "transaction.plan").exists() else ()
        )
        for name in published:
            if (recovery / (name + ".pending")).exists():
                continue
            try:
                backup = recovery / (name + ".previous")
                if backup.exists():
                    # Retain every original until the whole rollback succeeds.
                    restore = recovery / (name + ".restore")
                    shutil.copy2(backup, restore)
                    os.replace(restore, destination / name)
                else:
                    (destination / name).unlink(missing_ok=True)
            except OSError as rollback_error:
                errors.append(f"{name}: {rollback_error}")
        if errors:
            raise RuntimeError(
                f"Publication failed: {publish_error}. Originals are in {recovery}. "
                f"Could not restore: {'; '.join(errors)}"
            ) from publish_error
        remove_recovery = True
        raise
    finally:
        if remove_recovery:
            # Remove the active journal before deleting its backups. A process
            # killed during cleanup then leaves a stable destination.
            (recovery / "transaction.plan").unlink(missing_ok=True)
            shutil.rmtree(recovery)
