#!/usr/bin/env python3
"""Fail on mechanical documentation drift: dead links, dead anchors, unindexed docs.

This catches MECHANICAL drift only. It proves that a link resolves, that a heading an
anchor names still exists, that every doc is reachable from the index, and that the
file and `File::Symbol` names a renderer contract doc cites still exist. It does not
prove that a sentence is true. A claim about what the renderer draws, how a term is
composed or what a benchmark measured still needs a behavioural test or a capture;
passing this check is not evidence for any of that (issue #1357).

Four checks. "Tracked" below also covers an untracked file that .gitignore does not
exclude, so a new doc is checked before it is staged.

1. Every relative link and reference definition in a tracked Markdown file under
   docs/, and in the root CLAUDE.md, AGENTS.md, README.md and CONTRIBUTING.md,
   resolves to a tracked path. A `.md#anchor` link must name a heading of the target
   (GitHub's slug rules) or an explicit `<a id="...">`.
2. Every `docs/<path>.md` named in a tracked file outside docs/ (a source comment, a
   CMake option, a script) exists. The generated, git-ignored test catalogue is the
   one exception.
3. Every tracked Markdown file under docs/ is reachable from docs/README.md by
   following relative links. "Adding a doc? Add it here too." is the rule this
   enforces; an evidence bundle linked from its analysis doc counts as reachable.
4. In the docs listed in CONTRACT_DOCS, every backticked source file name exists in
   the repository, and every `File.ext::Symbol` names a symbol that occurs in that
   file. These docs state current entry points, so a renamed test or deleted file
   there is a stale claim. Elsewhere the same check is mostly noise: dated reports
   name deleted files on purpose, and many docs cite vendor files that are not
   tracked.

The fixtures in SELF_TEST run first, every time: each plants one defect of one kind
and the run fails if the check does not report it. That is the negative control;
a checker that cannot fail proves nothing.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from urllib.parse import unquote

INDEX = "docs/README.md"
ROOT_DOCS = ("CLAUDE.md", "AGENTS.md", "README.md", "CONTRIBUTING.md")
GENERATED_DOCS = re.compile(r"^docs/test-catalogue\.[a-z]+\.md$")

# Docs that state the renderer's CURRENT contracts and their entry points (check 4).
CONTRACT_DOCS = (
    "docs/guides/renderer-support-matrix.md",
    "docs/analysis/renderer-docs-reconcile-1357.md",
    "docs/agent-rules/vulkan-command-ordered-buffer-writes.md",
    "docs/adr/0020-reflection-tier-selection-contract.md",
    "docs/adr/0022-reference-tracer-owns-its-sampling-model.md",
    "docs/adr/0023-virtual-geometry-is-ray-traced-through-a-fixed-proxy.md",
    "docs/adr/0024-skinned-virtual-geometry-stays-on-the-hardware-rasterizer.md",
)

FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
LINK_RE = re.compile(r"(?<!\\)!?\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+(.*?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"")
SCHEME_RE = re.compile(r"^[a-z][a-z0-9+.-]*:", re.IGNORECASE)
DOCS_PATH_RE = re.compile(r"docs/[A-Za-z0-9_./-]+\.md")
SOURCE_EXT = r"(?:h|hpp|inl|cpp|glsl|comp|vert|frag|geom|tesc|tese|py|ps1|cmake|cs|lua)"
SOURCE_NAME_RE = re.compile(r"`([A-Za-z0-9_./-]+\." + SOURCE_EXT + r")(?:::([A-Za-z_][A-Za-z0-9_]*))?")


def slugify(heading: str) -> str:
    """GitHub's heading anchor: link text kept, code ticks dropped, punctuation removed."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def prose_lines(text: str):
    """Yield (line number, line) outside fenced code blocks."""
    in_fence = False
    for number, line in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
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
                found.update(EXPLICIT_ANCHOR_RE.findall(line))
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


def lines_citing_docs(repo: Repo):
    """Yield (file, line number, line) for text lines outside docs/ that mention docs/."""
    if repo.use_git:
        grep = subprocess.run(
            ["git", "grep", "-n", "-I", "-F", "docs/", "--", ".", ":!docs", ":!**/vendor/**"],
            cwd=repo.root, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        for hit in grep.stdout.splitlines():
            rel, number, line = hit.split(":", 2)
            yield rel, int(number), line
        return
    for rel in sorted(repo.tracked):
        if not rel.startswith("docs/"):
            for number, line in enumerate(repo.text(rel).splitlines(), 1):
                if "docs/" in line:
                    yield rel, number, line


def check_cited_doc_paths(repo: Repo) -> list[str]:
    errors = []
    for rel, number, line in lines_citing_docs(repo):
        for cited in DOCS_PATH_RE.findall(line):
            if cited not in repo.tracked and not GENERATED_DOCS.match(cited):
                errors.append(f"{rel}:{number}: names a doc that does not exist: {cited}")
    return errors


def check_reachable(repo: Repo, docs: list[str]) -> list[str]:
    if INDEX not in repo.tracked:
        return [f"{INDEX}: the index is missing"]
    reached = {INDEX}
    queue = [INDEX]
    while queue:
        rel = queue.pop()
        for _, target in links_in(repo, rel):
            path, _ = resolve(rel, target)
            if path.endswith(".md") and path in repo.tracked and path not in reached:
                reached.add(path)
                queue.append(path)
    return [f"{rel}: not reachable from {INDEX}; add it to the index" for rel in docs if rel not in reached]


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
    linked = docs + [d for d in ROOT_DOCS if d in repo.tracked]
    return check_links(repo, linked) + check_cited_doc_paths(repo) + check_reachable(repo, docs) + check_contract_names(repo)


# One defect per fixture; `expect` is a substring the report must contain.
SELF_TEST = {
    "clean": ({}, None),
    "dead link": ({"docs/guides/a.md": "[x](gone.md)\n"}, "link target does not exist: gone.md"),
    "dead anchor": ({"docs/guides/a.md": "[x](#nowhere)\n"}, "no heading or anchor '#nowhere'"),
    "orphan doc": ({"docs/guides/orphan.md": "# Orphan\n"}, "docs/guides/orphan.md: not reachable"),
    "dead cited doc": ({"src/X.h": "// see docs/guides/missing.md\n"}, "names a doc that does not exist: docs/guides/missing.md"),
    "renamed test": (
        {"docs/guides/renderer-support-matrix.md": "`T.cpp::OldName` pins it. [i](../README.md)\n", "tests/T.cpp": "TEST(S, NewName) {}\n"},
        "T.cpp has no symbol 'OldName'",
    ),
}
SELF_TEST_BASE = {
    "docs/README.md": "# Docs\n\n## Guides\n\n- [a](guides/a.md)\n",
    "docs/guides/a.md": "# A\n\n[back](../README.md#guides)\n",
}


def self_test() -> list[str]:
    failures = []
    for name, (overrides, expect) in SELF_TEST.items():
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = {**SELF_TEST_BASE, **overrides}
            for rel, body in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text(body, encoding="utf-8")
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
    root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip())
    # Untracked files count, so a new doc is checked before `git add`; a tracked
    # file deleted from the working tree does not.
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root, capture_output=True, text=True, check=True,
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
