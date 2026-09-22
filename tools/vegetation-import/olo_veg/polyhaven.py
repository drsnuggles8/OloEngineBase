"""Poly Haven asset fetch — catalogue metadata plus the files the importer needs.

Poly Haven is CC0 1.0 (https://polyhaven.com/license): commercial use permitted,
attribution NOT required, redistribution and modification permitted. That last
clause is what makes this tool legitimate: it downloads a scan, modifies it
heavily, and the repository commits the RESULT. Attribution is still recorded per
asset (see licence.py) because it costs nothing and keeps the audit trail.

THE SIZE TRAP. A resolution tier ("1k", "2k", "4k") selects TEXTURES ONLY. The
mesh .bin is byte-identical at every tier — pine_tree_01.bin is 949 MB whichever
you ask for. Nothing here should suggest that picking 1k makes the download
small; `asset_files` reports the real per-file sizes so the caller can say so.

No API key is needed and the endpoints take no authentication:
    https://api.polyhaven.com/info/<id>      polycount, authors, categories
    https://api.polyhaven.com/files/<id>     per-format, per-resolution URLs
"""

import json
import os
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.polyhaven.com"
LICENCE_NAME = "CC0 1.0 Universal (public domain dedication)"
LICENCE_URL = "https://polyhaven.com/license"

_USER_AGENT = "OloEngine-vegetation-import/1.0 (+https://github.com/drsnuggles8/OloEngineBase)"

# Every URL this module opens comes from a REMOTE response: asset_files() returns
# the download URLs, and the importer hands them straight to download(). That is
# the whole shape of an SSRF - a changed or spoofed metadata response could point
# the fetch at localhost, at a cloud metadata endpoint, or at file:// - so the
# host is pinned here rather than trusted.
#
# The redirect check is the half that is easy to miss: urlopen follows redirects
# itself, so inspecting response.geturl() afterwards is too late. The handler
# below validates each hop BEFORE it is followed.
_ALLOWED_HOSTS = frozenset({
    "api.polyhaven.com",
    "dl.polyhaven.org",
    "cdn.polyhaven.com",
})


def _check_url(url, what="url"):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https":
        raise ValueError(f"refusing a non-https {what}: {url!r}")
    if parts.hostname not in _ALLOWED_HOSTS:
        raise ValueError(f"refusing a {what} outside Poly Haven ({parts.hostname!r}): {url!r}")
    if parts.port not in (None, 443):
        raise ValueError(f"refusing a {what} on port {parts.port}: {url!r}")
    return url


class _PinnedRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _check_url(newurl, "redirect target")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_OPENER = urllib.request.build_opener(_PinnedRedirectHandler)


def _open(url, timeout):
    _check_url(url)
    request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    return _OPENER.open(request, timeout=timeout)


def _get(url, timeout=120):
    with _open(url, timeout) as response:
        return response.read()


def asset_info(asset_id):
    return json.loads(_get(f"{API}/info/{asset_id}"))


def asset_files(asset_id):
    return json.loads(_get(f"{API}/files/{asset_id}"))


def gltf_package(files, resolution):
    """Return (gltf_url, {relative_path: (url, size)}) for the glTF at `resolution`."""
    try:
        entry = files["gltf"][resolution]["gltf"]
    except KeyError as exc:
        available = sorted(files.get("gltf", {}))
        raise KeyError(f"no glTF at resolution {resolution!r}; have {available}") from exc
    includes = {name: (spec["url"], spec.get("size", 0)) for name, spec in entry.get("include", {}).items()}
    return entry["url"], includes


def map_url(files, map_name, resolution, fmt="png"):
    """URL of a standalone map (e.g. 'twig_alpha', 'Alpha') at a resolution.

    The maps the glTF package ships are jpg, which cannot carry an alpha channel,
    so every alpha mask has to come from this standalone set as png.
    """
    entry = files.get(map_name)
    if entry is None:
        raise KeyError(f"asset has no map named {map_name!r}; have {sorted(k for k in files if k not in ('blend', 'fbx', 'gltf', 'usd'))}")
    tier = entry.get(resolution)
    if tier is None:
        raise KeyError(f"map {map_name!r} has no resolution {resolution!r}; have {sorted(entry)}")
    if fmt not in tier:
        fmt = "png" if "png" in tier else sorted(tier)[0]
    return tier[fmt]["url"]


def download(url, dest, expected_size=None, log=print):
    """Download `url` to `dest` unless it is already there at the expected size.

    Writes to a sibling .part first: a half-written file at `dest` would look
    complete to the next run and then fail to parse somewhere far from the cause.
    """
    if os.path.exists(dest) and (expected_size is None or os.path.getsize(dest) == expected_size):
        return dest
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    part = dest + ".part"
    size_note = f" ({expected_size / 1e6:.1f} MB)" if expected_size else ""
    log(f"    fetch {os.path.basename(dest)}{size_note}")
    with _open(url, 900) as response, open(part, "wb") as handle:
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            handle.write(chunk)
    os.replace(part, dest)
    return dest


def fetch_gltf(asset_id, resolution, cache_dir, want_maps=(), log=print):
    """Fetch the glTF package plus any standalone maps into cache_dir/<asset_id>/.

    Returns (gltf_path, {map_name: path}). Textures that ship inside the glTF
    package land beside it under their own relative paths, which is what the
    .gltf's image URIs already reference.
    """
    files = asset_files(asset_id)
    root = os.path.join(cache_dir, asset_id)
    gltf_url, includes = gltf_package(files, resolution)
    gltf_path = os.path.join(root, os.path.basename(gltf_url))
    download(gltf_url, gltf_path, log=log)
    for relative, (url, size) in includes.items():
        # `relative` is a REMOTE key. Check containment on the RESOLVED path, not
        # by scanning the key for "..": an absolute key, a drive letter or a
        # symlinked cache dir all escape without one appearing anywhere.
        dest = os.path.realpath(os.path.join(root, relative.replace("/", os.sep)))
        if os.path.commonpath([dest, os.path.realpath(root)]) != os.path.realpath(root):
            raise ValueError(f"refusing to write outside the cache: {relative!r} -> {dest}")
        download(url, dest, expected_size=size, log=log)
    maps = {}
    for name in want_maps:
        url = map_url(files, name, resolution)
        path = os.path.join(root, "maps", os.path.basename(url))
        download(url, path, log=log)
        maps[name] = path
    return gltf_path, maps
