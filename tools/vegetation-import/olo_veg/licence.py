"""Per-asset provenance files, following OloEditor/assets/models/InfiniteScanHead/.

That directory is the convention this repository already uses: a LICENSE.md
naming the licence and its URL, the original author, the exact download URL, and
EVERY modification made to the original, plus a README.md describing the asset.

The modification list is not a formality here. What lands in the repository is
not the scan: the canopy has been rebuilt from cards baked out of the scan and
the trunk has been decimated by an order of magnitude. Someone comparing the
committed mesh against Poly Haven's would otherwise be entitled to think the
import was broken.
"""

import os
from datetime import date

from .polyhaven import LICENCE_NAME, LICENCE_URL


def write_licence(directory, asset_id, info, source_urls, modifications, resolution):
    authors = info.get("authors", {})
    credit = ", ".join(f"{name} ({role})" for name, role in authors.items()) or "Poly Haven"
    lines = [
        f"# {info.get('name', asset_id)} - licence and provenance\n",
        "\n",
        f"Source: **Poly Haven** - <https://polyhaven.com/a/{asset_id}>\n",
        "\n",
        f"Licence: **{LICENCE_NAME}** - <{LICENCE_URL}>\n",
        "\n",
        "CC0 places the work in the public domain: commercial use, redistribution and\n",
        "modification are all permitted and **attribution is not required**. It is recorded here\n",
        "anyway, because Poly Haven asks for it as a courtesy and because a provenance trail\n",
        "costs nothing to keep.\n",
        "\n",
        f"Original author(s): **{credit}**\n",
        "\n",
        f"Imported {date.today().isoformat()} at the {resolution} texture tier by\n",
        "`tools/vegetation-import/import_vegetation.py`.\n",
        "\n",
        "## Downloaded from\n",
        "\n",
    ]
    for url in source_urls:
        lines.append(f"- <{url}>\n")
    lines.extend([
        "\n",
        "## Modifications made to the original\n",
        "\n",
        "Everything in this directory is a **derivative** of the files above, not a copy of\n",
        "them. The scan is hundreds of megabytes and millions of triangles; what is committed\n",
        "here is a game-ready plant built from it.\n",
        "\n",
    ])
    for item in modifications:
        lines.append(f"- {item}\n")
    lines.extend([
        "\n",
        "Re-run the import to reproduce this directory exactly:\n",
        "\n",
        "```\n",
        f"python tools/vegetation-import/import_vegetation.py {os.path.basename(directory)}\n",
        "```\n",
    ])
    _write(os.path.join(directory, "LICENSE.md"), lines)


def write_readme(directory, species, info, stats):
    lines = [
        f"# {species} - imported vegetation\n",
        "\n",
        f"{info.get('description', '').strip()}\n" if info.get("description") else "",
        "\n",
        "Licence and full provenance: [LICENSE.md](LICENSE.md).\n",
        "\n",
        "## What is here\n",
        "\n",
        "| file | what it is |\n",
        "|---|---|\n",
        f"| `{species}.obj` | the plant, {stats['total_tris']:,} triangles |\n",
        f"| `{species}.mtl` | one material per submesh, so each gets its own albedo |\n",
        "| `Textures/` | albedo maps; the foliage one carries the alpha cutout |\n",
        "\n",
        "## Numbers that matter\n",
        "\n",
    ]
    for label, value in stats["report"]:
        lines.append(f"- **{label}**: {value}\n")
    lines.extend([
        "\n",
        "`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen\n",
        "deliberately rather than inherited: the fraction of the albedo that survives the\n",
        "alpha test, measured over the texels the mesh actually samples.\n",
    ])
    _write(os.path.join(directory, "README.md"), [line for line in lines if line])


def _write(path, lines):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(lines)
