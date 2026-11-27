#!/usr/bin/env python3
"""Fail on mechanical documentation drift: dead links, dead anchors, unindexed docs.

This catches MECHANICAL drift only. It proves that a link resolves, that a heading an
anchor names still exists, that every doc is reachable from the index, and that the
file and `File::Symbol` names a renderer contract doc cites still exist. It does not
prove that a sentence is true. A claim about what the renderer draws, how a term is
composed or what a benchmark measured still needs a behavioural test or a capture;
passing this check is not evidence for any of that (issue #1357).

Six checks. "Tracked" below also covers an untracked file that .gitignore does not
exclude, so a new doc is checked before it is staged. A project documentation tree
(PROJECT_DOC_TREES, e.g. KESTREL's game design under OloEditor/KestrelProject/docs/)
gets checks 1 and 3 against its own index, and checks 5 and 6.

1. Every relative link and reference definition in a tracked Markdown file under
   docs/ or a project doc tree, and in the root CLAUDE.md, AGENTS.md, README.md and
   CONTRIBUTING.md, resolves to a tracked path. A `.md#anchor` link must name a heading of the target
   (GitHub's slug rules) or an explicit `<a id="...">`.
2. Every `docs/<path>.md` named in a tracked file outside docs/ (a source comment, a
   CMake option, a script, a tool's README) exists; a relative `../docs/x.md` is
   resolved against the file that cites it. The generated, git-ignored test
   catalogue is the one exception.
3. Every tracked Markdown file under docs/ is reachable from docs/README.md by
   following relative links, and every one under a project doc tree from that tree's
   README.md. "Adding a doc? Add it here too." is the rule this enforces; an evidence
   bundle linked from its analysis doc counts as reachable.
4. In the docs listed in CONTRACT_DOCS, every backticked source file name exists in
   the repository, and every `File.ext::Symbol` names a symbol that occurs in that
   file. These docs state current entry points, so a renamed test or deleted file
   there is a stale claim. Elsewhere the same check is mostly noise: dated reports
   name deleted files on purpose, and many docs cite vendor files that are not
   tracked.
5. In a design decision index (DECISION_INDEXES), every row `| ID | ... | [ID](f.md#a) |`
   names an ID no other row names, links the anchor that ID owns (`G-18–G-20` owns
   `#g-18-g-20`), and points at the one current document holding both that anchor and
   the `### ID` record; every record has a row; no ID, anchor or member of a grouped
   range (`G-19` inside `G-18–G-20`) is defined twice; and a row or heading that looks
   like a decision but does not parse is reported rather than skipped. The tree's
   archive/ holds historical snapshots and is exempt, since a decision appears in every
   version of a register.
6. Every file in an archive with a manifest (ARCHIVE_MANIFESTS, `sha256sum` format)
   is listed in it and still has the listed SHA-256: archived snapshots are frozen.

Check 2 also covers citations of a project doc tree from anywhere in the repository
(`.claude/rules/x.md` linking `../../OloEditor/KestrelProject/docs/AGENT_GUIDE.md#a`):
the file must exist and, when an anchor is given, define it.

The fixtures in SELF_TEST run first, every time: each plants one defect of one kind
and the run fails if the check does not report it. That is the negative control;
a checker that cannot fail proves nothing.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from urllib.parse import unquote

INDEX = "docs/README.md"
# This script's own fixtures name docs that do not exist, on purpose.
SELF = "scripts/check_docs_consistency.py"
ROOT_DOCS = ("CLAUDE.md", "AGENTS.md", "README.md", "CONTRIBUTING.md")
GENERATED_DOCS = re.compile(r"^docs/test-catalogue\.[a-z]+\.md$")
# Project documentation trees outside docs/, each with its own index (checks 1 and 3).
PROJECT_DOC_TREES = ("OloEditor/KestrelProject/docs/",)
# Design decision indexes (check 5); the archive/ directory beside each is exempt.
DECISION_INDEXES = ("OloEditor/KestrelProject/docs/DECISIONS.md",)
# A decision ID, or a grouped range of them joined by an en dash: C-01, A-18a, G-18–G-20.
DECISION_ID = r"[A-Z]+-\d+[a-z]?(?:–[A-Z]+-\d+[a-z]?)?"
DECISION_ROW_RE = re.compile(r"^\|\s*(" + DECISION_ID + r")\s*\|.*\|\s*\[[^\]]*\]\(([^)#\s]+)#([^)\s]+)\)\s*\|\s*$")
DECISION_HEADING_RE = re.compile(r"^\s{0,3}#{2,6}\s+(" + DECISION_ID + r")\s*$")
# Anything that starts like a decision, so a malformed row or heading is reported, not skipped.
DECISION_LIKE_ROW_RE = re.compile(r"^\|\s*[A-Z]+-\d")
DECISION_LIKE_HEADING_RE = re.compile(r"^\s{0,3}#{2,6}\s+[A-Z]+-\d")
DECISION_RANGE_RE = re.compile(r"([A-Z]+)-(\d+)–\1-(\d+)")
# Frozen archives (check 6): a `sha256sum`-format manifest beside the snapshots.
ARCHIVE_MANIFESTS = ("OloEditor/KestrelProject/docs/archive/SHA256SUMS",)
MANIFEST_LINE_RE = re.compile(r"([0-9a-f]{64}) [ *](\S+)")
# Citations of a project doc tree from any file (check 2), relative or repo-rooted.
PROJECT_PATH_RES = tuple(
    re.compile(r"(?<![\w/.:-])((?:\.\./)*" + re.escape(tree) + r"[A-Za-z0-9_./-]*\.md)(?:#([\w-]+))?")
    for tree in PROJECT_DOC_TREES
)

# Docs that state the renderer's CURRENT contracts and their entry points (check 4).
CONTRACT_DOCS = (
    "docs/guides/renderer-support-matrix.md",
    "docs/analysis/renderer-docs-reconcile-1357.md",
    "docs/agent-rules/vulkan-command-ordered-buffer-writes.md",
    "docs/agent-rules/vulkan-command-ordered-buffer-writes-details.md",
    "docs/adr/0020-reflection-tier-selection-contract.md",
    "docs/adr/0022-reference-tracer-owns-its-sampling-model.md",
    "docs/adr/0023-virtual-geometry-is-ray-traced-through-a-fixed-proxy.md",
    "docs/adr/0024-skinned-virtual-geometry-stays-on-the-hardware-rasterizer.md",
)

FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})(.*)$")
INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
LINK_RE = re.compile(r"(?<!\\)!?\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"")
SCHEME_RE = re.compile(r"^[a-z][a-z0-9+.-]*:", re.IGNORECASE)
# Anchored so a URL or a longer path (`https://…/docs/x.md`, `../docs/x.md`) is not
# read as this repository's docs/ directory.
DOCS_PATH_RE = re.compile(r"(?<![\w/.:-])docs/[A-Za-z0-9_./-]+\.md")
RELATIVE_DOCS_PATH_RE = re.compile(r"(?<![\w/.:-])((?:\.\./)+docs/[A-Za-z0-9_./-]+\.md)")
SOURCE_EXT = r"(?:h|hpp|inl|cpp|glsl|comp|vert|frag|geom|tesc|tese|py|ps1|cmake|cs|lua)"
SOURCE_NAME_RE = re.compile(r"`([A-Za-z0-9_./-]+\." + SOURCE_EXT + r")(?:::([A-Za-z_][A-Za-z0-9_]*))?")


def slugify(heading: str) -> str:
    """GitHub's heading anchor: link text kept, code ticks dropped, punctuation removed."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def prose_lines(text: str):
    """Yield (line number, line) outside fenced code blocks.

    CommonMark rules: a fence closes only on the same character, at least as long
    as the opening run, with nothing after it; a backtick fence's info string may
    not contain a backtick.
    """
    fence: tuple[str, int] | None = None
    for number, line in enumerate(text.splitlines(), 1):
        match = FENCE_RE.match(line)
        if match:
            marker, rest = match.group(1), match.group(2)
            if fence is None:
                if not (marker[0] == "`" and "`" in rest):
                    fence = (marker[0], len(marker))
                    continue
            elif marker[0] == fence[0] and len(marker) >= fence[1] and not rest.strip():
                fence = None
                continue
        if fence is None:
            yield number, line


class Repo:
    def __init__(self, root: Path, tracked: list[str], use_git: bool = False):
        self.root = root
        self.use_git = use_git
        self.tracked = set(tracked)
        self.dirs = {str(Path(t).parent.as_posix()) for t in tracked}
        for d in list(self.dirs):
            parts = d.split("/")
            for i in range(1, len(parts)):
                self.dirs.add("/".join(parts[:i]))
        self.by_name: dict[str, list[str]] = defaultdict(list)
        for t in tracked:
            self.by_name[t.rsplit("/", 1)[-1]].append(t)
        self._anchors: dict[str, set[str]] = {}
        self._text: dict[str, str] = {}

    def text(self, rel: str) -> str:
        if rel not in self._text:
            self._text[rel] = (self.root / rel).read_text(encoding="utf-8", errors="replace")
        return self._text[rel]

    def exists(self, rel: str) -> bool:
        return rel in self.tracked or rel.rstrip("/") in self.dirs

    def anchors(self, rel: str) -> set[str]:
        if rel not in self._anchors:
            found: set[str] = set()
            seen: dict[str, int] = {}
            for _, line in prose_lines(self.text(rel)):
                heading = HEADING_RE.match(line)
                if heading:
                    base = slugify(heading.group(1))
                    count = seen.get(base, 0)
                    seen[base] = count + 1
                    found.add(base if count == 0 else f"{base}-{count}")
                found.update(anchor.lower() for anchor in EXPLICIT_ANCHOR_RE.findall(line))
            self._anchors[rel] = found
        return self._anchors[rel]


def resolve(source: str, target: str) -> tuple[str, str]:
    path, _, fragment = target.partition("#")
    path = unquote(path)
    if not path:
        return source, fragment
    if path.startswith("/"):
        return path.lstrip("/"), fragment
    parts: list[str] = []
    for part in (Path(source).parent.as_posix() + "/" + path).split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return "/".join(parts), fragment


def links_in(repo: Repo, rel: str):
    for number, line in prose_lines(repo.text(rel)):
        line = INLINE_CODE_RE.sub("", line)
        targets = [m.group(1) for m in LINK_RE.finditer(line)]
        ref = REF_DEF_RE.match(line)
        if ref:
            targets.append(ref.group(1))
        for target in targets:
            if not SCHEME_RE.match(target):
                yield number, target


def check_links(repo: Repo, docs: list[str]) -> list[str]:
    errors = []
    for rel in docs:
        for number, target in links_in(repo, rel):
            path, fragment = resolve(rel, target)
            if not repo.exists(path):
                errors.append(f"{rel}:{number}: link target does not exist: {target}")
            elif fragment and path.endswith(".md") and path in repo.tracked:
                if fragment.lower() not in repo.anchors(path):
                    errors.append(f"{rel}:{number}: no heading or anchor '#{fragment}' in {path}")
    return errors


def parse_git_grep(output: bytes):
    # git separates matches with LF; CR and Unicode line separators can be
    # part of a source line and must not become additional match records.
    for hit in output.decode("utf-8", errors="replace").split("\n"):
        if not hit:
            continue
        rel, number, line = hit.split(":", 2)
        yield rel, int(number), line.rstrip("\r")


def lines_citing_docs(repo: Repo):
    """Yield (file, line number, line) for text lines outside docs/ that mention docs/."""
    if repo.use_git:
        grep = subprocess.run(
            ["git", "grep", "--untracked", "-n", "-I", "-F", "docs/", "--", ".", ":!docs", ":!**/vendor/**", f":!{SELF}"],
            cwd=repo.root, capture_output=True,
        )
        # 1 means no match; anything else is git failing, which must not read as "clean".
        if grep.returncode not in (0, 1):
            raise RuntimeError(f"git grep failed ({grep.returncode}): {grep.stderr.decode('utf-8', errors='replace').strip()}")
        yield from parse_git_grep(grep.stdout)
        return
    for rel in sorted(repo.tracked):
        if not rel.startswith("docs/") and rel != SELF:
            for number, line in enumerate(repo.text(rel).splitlines(), 1):
                if "docs/" in line:
                    yield rel, number, line


def check_cited_doc_paths(repo: Repo) -> list[str]:
    errors = []
    try:
        cited_lines = list(lines_citing_docs(repo))
    except RuntimeError as failure:
        return [f"check 2 could not run: {failure}"]
    for rel, number, line in cited_lines:
        for cited in DOCS_PATH_RE.findall(line):
            if cited not in repo.tracked and not GENERATED_DOCS.match(cited):
                errors.append(f"{rel}:{number}: names a doc that does not exist: {cited}")
        # A relative citation (`../../docs/x.md`, e.g. a Markdown link from a
        # tool's README) is resolved against the file that makes it.
        for cited in RELATIVE_DOCS_PATH_RE.findall(line):
            resolved, _ = resolve(rel, cited)
            if resolved.startswith("docs/") and resolved not in repo.tracked:
                errors.append(f"{rel}:{number}: names a doc that does not exist: {cited}")
        # A project doc tree cited from anywhere: `../../OloEditor/KestrelProject/docs/x.md#a`.
        for pattern in PROJECT_PATH_RES:
            for match in pattern.finditer(line):
                cited, fragment = match.group(1), match.group(2)
                resolved = resolve(rel, cited)[0] if cited.startswith("../") else cited
                if resolved not in repo.tracked:
                    errors.append(f"{rel}:{number}: names a doc that does not exist: {cited}")
                elif fragment and fragment.lower() not in repo.anchors(resolved):
                    errors.append(f"{rel}:{number}: no heading or anchor '#{fragment}' in {resolved}")
    return errors


def check_reachable(repo: Repo, docs: list[str], index: str = INDEX) -> list[str]:
    if index not in repo.tracked:
        return [f"{index}: the index is missing"]
    reached = {index}
    queue = [index]
    while queue:
        rel = queue.pop()
        for _, target in links_in(repo, rel):
            path, _ = resolve(rel, target)
            if path.endswith(".md") and path in repo.tracked and path not in reached:
                reached.add(path)
                queue.append(path)
    return [f"{rel}: not reachable from {index}; add it to the index" for rel in docs if rel not in reached]


def decision_anchor(decision: str) -> str:
    """The anchor an ID owns: lower case, a range's en dash as a hyphen (G-18–G-20 -> g-18-g-20)."""
    return decision.lower().replace("–", "-")


def decision_members(decision: str) -> list[str]:
    """The single IDs a record covers: G-18–G-20 covers G-18, G-19 and G-20."""
    match = DECISION_RANGE_RE.fullmatch(decision)
    if not match:
        return [decision]
    prefix, first, last = match.group(1), match.group(2), match.group(3)
    return [f"{prefix}-{n:0{len(first)}d}" for n in range(int(first), int(last) + 1)]


def check_decision_index(repo: Repo, index: str) -> list[str]:
    if index not in repo.tracked:
        return [f"{index}: the decision index is missing"]
    tree = index.rsplit("/", 1)[0] + "/"
    current = sorted(
        t for t in repo.tracked
        if t.startswith(tree) and t.endswith(".md") and t != index and not t.startswith(tree + "archive/")
    )
    errors = []
    # An anchor is an explicit `<a id>` or a decision heading's own slug; each occurrence counts.
    anchor_homes: dict[str, list[str]] = defaultdict(list)
    heading_homes: dict[str, list[str]] = defaultdict(list)
    for rel in current:
        explicit: list[str] = []
        slugs: set[str] = set()
        for number, line in prose_lines(repo.text(rel)):
            explicit += [anchor.lower() for anchor in EXPLICIT_ANCHOR_RE.findall(line)]
            if DECISION_LIKE_HEADING_RE.match(line):
                heading = DECISION_HEADING_RE.match(line)
                if not heading:
                    errors.append(f"{rel}:{number}: heading looks like a decision record but is not 'ID' or 'ID–ID' (en dash): {line.strip()}")
                    continue
                heading_homes[heading.group(1)].append(f"{rel}:{number}")
                slugs.add(slugify(heading.group(1)))
        for anchor in explicit + sorted(slugs - set(explicit)):
            anchor_homes[anchor].append(rel)
    for anchor, homes in sorted(anchor_homes.items()):
        if len(homes) > 1:
            errors.append(f"{index}: anchor '#{anchor}' is defined more than once: {', '.join(homes)}")
    covered: dict[str, list[str]] = defaultdict(list)
    for decision, homes in sorted(heading_homes.items()):
        if len(homes) > 1:
            errors.append(f"{index}: decision {decision} has more than one record: {', '.join(homes)}")
        for member in decision_members(decision):
            covered[member].append(decision)
    for member, records in sorted(covered.items()):
        if len(records) > 1:
            errors.append(f"{index}: {member} is covered by more than one record: {', '.join(records)}")
    rows: dict[str, int] = {}
    for number, line in prose_lines(repo.text(index)):
        if not DECISION_LIKE_ROW_RE.match(line):
            continue
        row = DECISION_ROW_RE.match(line)
        if not row:
            errors.append(f"{index}:{number}: row looks like a decision but is not '| ID | ... | [ID](file.md#anchor) |'")
            continue
        decision, target, anchor = row.groups()
        if decision in rows:
            errors.append(f"{index}:{number}: decision {decision} already has a row (line {rows[decision]})")
        rows.setdefault(decision, number)
        if anchor.lower() != decision_anchor(decision):
            errors.append(f"{index}:{number}: {decision} links #{anchor}; its anchor is #{decision_anchor(decision)}")
        path, _ = resolve(index, target)
        homes = anchor_homes.get(anchor.lower(), [])
        if homes != [path]:
            where = ", ".join(homes) if homes else "no current document"
            errors.append(
                f"{index}:{number}: {decision} links {target}#{anchor}, but that anchor "
                f"(an <a id> or a decision heading) is defined in {where}"
            )
        records = heading_homes.get(decision, [])
        if not records:
            errors.append(f"{index}:{number}: {decision} has no '### {decision}' record in a current document")
        elif not any(record.rsplit(":", 1)[0] == path for record in records):
            errors.append(f"{index}:{number}: {decision} links {path}, but its record is at {records[0]}")
    for decision, homes in sorted(heading_homes.items()):
        if decision not in rows:
            errors.append(f"{homes[0]}: decision {decision} has no row in {index}")
    return errors


def check_archive_manifest(repo: Repo, manifest: str) -> list[str]:
    if manifest not in repo.tracked:
        return [f"{manifest}: the archive manifest is missing"]
    folder = manifest.rsplit("/", 1)[0] + "/"
    errors = []
    listed: dict[str, tuple[str, int]] = {}
    for number, line in enumerate(repo.text(manifest).splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        entry = MANIFEST_LINE_RE.fullmatch(line.rstrip("\r"))
        if not entry:
            errors.append(f"{manifest}:{number}: not '<sha256>  <file>'")
            continue
        listed[entry.group(2)] = (entry.group(1), number)
    for name, (digest, number) in sorted(listed.items()):
        rel = folder + name
        if rel not in repo.tracked:
            errors.append(f"{manifest}:{number}: lists a file that does not exist: {name}")
        elif hashlib.sha256((repo.root / rel).read_bytes()).hexdigest() != digest:
            errors.append(f"{rel}: changed, but archived snapshots are frozen (sha256 listed at {manifest}:{number})")
    for rel in sorted(repo.tracked):
        if rel.startswith(folder) and rel not in (manifest, folder + "README.md") and rel[len(folder):] not in listed:
            errors.append(f"{rel}: in the archive but not listed in {manifest}")
    return errors


def check_contract_names(repo: Repo) -> list[str]:
    errors = []
    for rel in CONTRACT_DOCS:
        if rel not in repo.tracked:
            continue
        for number, line in prose_lines(repo.text(rel)):
            for match in SOURCE_NAME_RE.finditer(line):
                name, symbol = match.group(1), match.group(2)
                if "/" in name:
                    candidates = [t for t in repo.tracked if t == name or t.endswith("/" + name)]
                else:
                    candidates = repo.by_name.get(name, [])
                if not candidates:
                    errors.append(f"{rel}:{number}: names a file that does not exist: {name}")
                elif symbol and not any(re.search(r"\b" + re.escape(symbol) + r"\b", repo.text(c)) for c in candidates):
                    errors.append(f"{rel}:{number}: {name} has no symbol '{symbol}'")
    return errors


def run_checks(repo: Repo) -> list[str]:
    docs = sorted(t for t in repo.tracked if t.startswith("docs/") and t.endswith(".md"))
    trees = {tree: sorted(t for t in repo.tracked if t.startswith(tree) and t.endswith(".md")) for tree in PROJECT_DOC_TREES}
    linked = docs + [d for tree_docs in trees.values() for d in tree_docs] + [d for d in ROOT_DOCS if d in repo.tracked]
    errors = check_links(repo, linked) + check_cited_doc_paths(repo) + check_reachable(repo, docs)
    for tree, tree_docs in trees.items():
        if not tree_docs:
            errors.append(f"{tree}: the project doc tree has no Markdown files; fix PROJECT_DOC_TREES if it moved")
            continue
        errors += check_reachable(repo, tree_docs, tree + "README.md")
    for index in DECISION_INDEXES:
        errors += check_decision_index(repo, index)
    for manifest in ARCHIVE_MANIFESTS:
        errors += check_archive_manifest(repo, manifest)
    return errors + check_contract_names(repo)


# One defect per fixture; `expect` is a substring the report must contain.
_KD = PROJECT_DOC_TREES[0]
_KD_INDEX = "# Decisions\n\n| Decision | Disposition | Record |\n|---|---|---|\n| X-01 | Accepted | [X-01](topic.md#x-01) |\n"
_KD_TOPIC = '# Topic\n\n<a id="x-01"></a>\n\n### X-01\n\nText.\n'
_KD_MANIFEST = "# sha256 of each snapshot\n"
_KD_WITH_OLD = "# Design\n\n- [Decisions](DECISIONS.md)\n- [Topic](topic.md)\n- [Old](archive/old.md)\n"


def _manifest_for(body: str) -> str:
    return _KD_MANIFEST + hashlib.sha256(body.encode("utf-8")).hexdigest() + "  old.md\n"


SELF_TEST = {
    "clean": ({}, None),
    "dead link": ({"docs/guides/a.md": "[x](gone.md)\n"}, "link target does not exist: gone.md"),
    "dead anchor": ({"docs/guides/a.md": "[x](#nowhere)\n"}, "no heading or anchor '#nowhere'"),
    "orphan doc": ({"docs/guides/orphan.md": "# Orphan\n"}, "docs/guides/orphan.md: not reachable"),
    "dead cited doc": ({"src/X.h": "// see docs/guides/missing.md\n"}, "names a doc that does not exist: docs/guides/missing.md"),
    "dead relative citation": ({"tools/t/README.md": "[g](../../docs/guides/gone.md)\n"}, "names a doc that does not exist: ../../docs/guides/gone.md"),
    "a nested fence is still code": (
        {"docs/guides/a.md": "# A\n\n[back](../README.md#guides)\n\n````md\n```js\n[x](gone.md)\n```\n~~~\n[y](also-gone.md)\n````\n"},
        None,
    ),
    "a URL is not a local doc": ({"src/Y.h": "// https://github.com/o/r/blob/main/docs/x.md\n"}, None),
    "explicit anchor, any case": (
        {"docs/guides/a.md": '# A\n\n<a id="Mixed-Case"></a>\n\n[back](../README.md#guides) [m](#Mixed-Case)\n'},
        None,
    ),
    "renamed test": (
        {"docs/guides/renderer-support-matrix.md": "`T.cpp::OldName` pins it. [i](../README.md)\n", "tests/T.cpp": "TEST(S, NewName) {}\n"},
        "T.cpp has no symbol 'OldName'",
    ),
    "project doc dead anchor": (
        {_KD + "README.md": "# Design\n\n- [Decisions](DECISIONS.md)\n- [Topic](topic.md#x-99)\n"},
        "no heading or anchor '#x-99' in " + _KD + "topic.md",
    ),
    "project doc orphan": ({_KD + "orphan.md": "# Orphan\n"}, _KD + "orphan.md: not reachable from " + _KD + "README.md"),
    "decision row with no record": (
        {_KD + "DECISIONS.md": _KD_INDEX + "| X-02 | Accepted | [X-02](topic.md#x-02) |\n"},
        "X-02 has no '### X-02' record in a current document",
    ),
    "decision row links another decision's anchor": (
        {
            _KD + "DECISIONS.md": _KD_INDEX.replace("topic.md#x-01", "topic.md#x-02") + "| X-02 | Accepted | [X-02](topic.md#x-02) |\n",
            _KD + "topic.md": _KD_TOPIC + '\n<a id="x-02"></a>\n\n### X-02\n\nMore.\n',
        },
        "X-01 links #x-02; its anchor is #x-01",
    ),
    "malformed decision row": (
        {_KD + "DECISIONS.md": _KD_INDEX + "| X-02 | Accepted | [X-02](topic.md) |\n"},
        "row looks like a decision but is not",
    ),
    "hyphenated range heading": (
        {_KD + "topic.md": _KD_TOPIC + "\n### X-02-X-03\n\nMore.\n"},
        "heading looks like a decision record but is not",
    ),
    "decision inside a grouped range": (
        {
            _KD + "DECISIONS.md": _KD_INDEX + "| X-02–X-04 | Accepted | [X-02–X-04](topic.md#x-02-x-04) |\n| X-03 | Accepted | [X-03](topic.md#x-03) |\n",
            _KD + "topic.md": _KD_TOPIC + '\n<a id="x-02-x-04"></a>\n\n### X-02–X-04\n\nGroup.\n\n<a id="x-03"></a>\n\n### X-03\n\nSingle.\n',
        },
        "X-03 is covered by more than one record: X-02–X-04, X-03",
    ),
    "a decision heading's own slug is its anchor": (
        {
            _KD + "DECISIONS.md": _KD_INDEX + "| X-02 | Accepted | [X-02](topic.md#x-02) |\n",
            _KD + "topic.md": _KD_TOPIC + "\n### X-02\n\nNo explicit anchor.\n",
        },
        None,
    ),
    "decision index missing": ({_KD + "DECISIONS.md": None, _KD + "README.md": "# Design\n\n- [Topic](topic.md)\n"}, "the decision index is missing"),
    "project doc tree empty": (
        {_KD + "README.md": None, _KD + "DECISIONS.md": None, _KD + "topic.md": None},
        "the project doc tree has no Markdown files",
    ),
    "edited archive snapshot": (
        {_KD + "README.md": _KD_WITH_OLD, _KD + "archive/old.md": "# Old\n\nEdited.\n", _KD + "archive/SHA256SUMS": _manifest_for("# Old\n")},
        "archive/old.md: changed, but archived snapshots are frozen",
    ),
    "unlisted archive file": (
        {_KD + "README.md": _KD_WITH_OLD, _KD + "archive/old.md": "# Old\n"},
        "archive/old.md: in the archive but not listed in",
    ),
    "archive manifest missing": ({_KD + "archive/SHA256SUMS": None}, "the archive manifest is missing"),
    "dead anchor into a project doc from outside it": (
        {".claude/rules/r.md": "[g](../../" + _KD + "topic.md#x-99)\n"},
        "no heading or anchor '#x-99' in " + _KD + "topic.md",
    ),
    "dead project doc named from outside it": (
        {"scripts/s.py": "# see " + _KD + "gone.md\n"},
        "names a doc that does not exist: " + _KD + "gone.md",
    ),
    "decision row listed twice": ({_KD + "DECISIONS.md": _KD_INDEX + "| X-01 | Accepted | [X-01](topic.md#x-01) |\n"}, "decision X-01 already has a row"),
    "decision record with no row": (
        {_KD + "topic.md": _KD_TOPIC + '\n<a id="x-03"></a>\n\n### X-03\n\nMore.\n'},
        "decision X-03 has no row in " + _KD + "DECISIONS.md",
    ),
    "decision recorded twice": (
        {
            _KD + "README.md": "# Design\n\n- [Decisions](DECISIONS.md)\n- [Topic](topic.md)\n- [Other](other.md)\n",
            _KD + "other.md": _KD_TOPIC,
        },
        "anchor '#x-01' is defined more than once",
    ),
    "an archived copy of a decision is history, not a second record": (
        {_KD + "README.md": _KD_WITH_OLD, _KD + "archive/old.md": _KD_TOPIC, _KD + "archive/SHA256SUMS": _manifest_for(_KD_TOPIC)},
        None,
    ),
}
SELF_TEST_BASE = {
    "docs/README.md": "# Docs\n\n## Guides\n\n- [a](guides/a.md)\n",
    "docs/guides/a.md": "# A\n\n[back](../README.md#guides)\n",
    _KD + "README.md": "# Design\n\n- [Decisions](DECISIONS.md)\n- [Topic](topic.md)\n",
    _KD + "DECISIONS.md": _KD_INDEX,
    _KD + "topic.md": _KD_TOPIC,
    _KD + "archive/SHA256SUMS": _KD_MANIFEST,
}


def self_test() -> list[str]:
    failures = []
    for ending in ["\n", "\r\n", "\r\r\n"]:
        record = "src/X.h:3:// docs/guides/a.md\u0085 still the same source line" + ending
        expected = [("src/X.h", 3, "// docs/guides/a.md\u0085 still the same source line")]
        if list(parse_git_grep(record.encode("utf-8"))) != expected:
            failures.append("git grep records split on a source line separator")
    for name, (overrides, expect) in SELF_TEST.items():
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # An override of None removes a base file; bytes are written as given (LF), so a
            # fixture's SHA-256 matches what it hashed.
            files = {rel: body for rel, body in {**SELF_TEST_BASE, **overrides}.items() if body is not None}
            for rel, body in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(body.encode("utf-8"))
            report = run_checks(Repo(root, sorted(files)))
            if expect is None and report:
                failures.append(f"self-test '{name}': a clean tree reported {report}")
            elif expect is not None and not any(expect in line for line in report):
                failures.append(f"self-test '{name}': expected a report containing '{expect}', got {report}")
    return failures


def main() -> int:
    failures = self_test()
    if failures:
        print("check_docs_consistency: the checker failed its own negative controls:", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        return 2
    root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, encoding="utf-8", check=True
        ).stdout.strip()
    )
    # Untracked files count, so a new doc is checked before `git add`; a tracked
    # file deleted from the working tree does not.
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, capture_output=True, text=True, encoding="utf-8", check=True,
    ).stdout.split("\0")
    files = sorted({t for t in listed if t and (root / t).is_file()})
    errors = run_checks(Repo(root, files, use_git=True))
    for line in errors:
        print(line)
    if errors:
        print(f"\n{len(errors)} documentation consistency error(s). This checks links and names only, not whether the prose is true.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
