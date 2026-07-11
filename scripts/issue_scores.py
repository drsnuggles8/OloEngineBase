#!/usr/bin/env python3
"""Issue scoring picker — see docs/process/issue-scoring.md.

Scores live in each GitHub issue body inside an `olo-score` fenced block
(only the raw inputs; the score is derived here, never stored). Subcommands:

    rank   pull every open issue, parse its block, print the ranked list
           (default lens; Pull-override applied). This is what /start-work calls.
    lint   list open issues with no olo-score block (the "needs-score" nudge).
    apply  one-time/migration: write blocks into issue bodies from a local
           JSON source (--from issue-scores.json). Supports --dry-run / --only.

Requires the `gh` CLI authenticated against the repo.
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile

REPO = "drsnuggles8/OloEngineBase"
VALUE_AXES = ("capability", "craft", "stability", "decay")
ALL_AXES = VALUE_AXES + ("effort", "confidence", "learning", "fun")
KANO = ("table-stakes", "performance", "delighter")
DOC_URL = f"https://github.com/{REPO}/blob/master/docs/process/issue-scoring.md"

# --freeze (rank): during a feature freeze, only issues carrying one of these
# labels are eligible — perf/robustness/architecture/bug work, plus "tooling"
# (the standing MCP-server / dev-tooling / codegen exception). A pure "feature"
# label with none of these is excluded. See the user-facing freeze policy —
# this is not part of the base issue-scoring.md rubric, just a temporary lens.
FREEZE_LABELS = {"performance", "robustness", "architecture", "bug", "tooling", "cleanup"}

BEGIN, END = "<!-- olo-score:begin -->", "<!-- olo-score:end -->"
SECTION_RE = re.compile(r"\n*" + re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n*", re.DOTALL)
FENCE_RE = re.compile(r"```olo-score\s*\n(.*?)\n```", re.DOTALL)


# ---- gh helpers -------------------------------------------------------------
def gh_json(args):
    out = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8")
    if out.returncode != 0:
        sys.exit(f"gh failed: {' '.join(args)}\n{out.stderr}")
    return json.loads(out.stdout)


def open_issues():
    """Open issues eligible for scoring — excludes bot-authored tracking issues
    (e.g. Renovate's "Dependency Dashboard"), which aren't backlog work items."""
    issues = gh_json(["issue", "list", "--repo", REPO, "--state", "open",
                       "--limit", "300", "--json", "number,title,body,labels,author"])
    return [it for it in issues if not it.get("author", {}).get("is_bot")]


def label_names(it):
    return {l["name"] for l in it.get("labels", [])}


# ---- block parse / render ---------------------------------------------------
def _coerce(v):
    v = v.strip()
    if v.startswith("["):
        return json.loads(v)
    try:
        return int(v)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        return v.strip().strip('"').strip("'")


def parse_block(body):
    """Return the axes dict from an issue body, or None if no block."""
    if not body:
        return None
    m = FENCE_RE.search(body)
    if not m:
        return None
    d = {}
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        k, _, v = line.partition(":")
        d[k.strip()] = _coerce(v)
    return d


def render_section(d):
    lines = [f"{a}: {d.get(a, 3)}" for a in ALL_AXES]
    lines.append(f"kano: {d.get('kano', 'table-stakes')}")
    lines.append(f"blocked_by: {json.dumps(d.get('blocked_by', []))}")
    lines.append(f"blocks: {json.dumps(d.get('blocks', []))}")
    block = "\n".join(lines)
    caption = ("<sub>Rated per [issue-scoring](" + DOC_URL + ") · "
               "score = confidence × (capability + craft + stability + decay) / effort, "
               "derived by the picker.</sub>")
    return f"{BEGIN}\n## Score\n```olo-score\n{block}\n```\n{caption}\n{END}"


def splice(body, section):
    body = body or ""
    if SECTION_RE.search(body):
        return SECTION_RE.sub("\n\n" + section + "\n", body).strip() + "\n"
    return body.rstrip() + "\n\n" + section + "\n"


# ---- scoring ----------------------------------------------------------------
def score(d):
    eff = d.get("effort", 0) or 0
    if not eff:
        return 0.0
    cod = sum(d.get(a, 0) for a in VALUE_AXES)
    return round(d.get("confidence", 1.0) * cod / eff, 2)


def is_blocked(d):
    return bool(d.get("blocked_by"))


# ---- commands ---------------------------------------------------------------
def cmd_rank(args):
    rows = []
    excluded = 0
    for it in open_issues():
        d = parse_block(it.get("body"))
        if d is None:
            continue
        if args.freeze and not (label_names(it) & FREEZE_LABELS):
            excluded += 1
            continue
        rows.append((it["number"], it["title"], score(d), d))
    rows.sort(key=lambda r: (not (r[3].get("fun", 0) >= 8 and not is_blocked(r[3])),
                             is_blocked(r[3]), -r[2], r[0]))
    if args.freeze:
        print(f"[--freeze active: only {'/'.join(sorted(FREEZE_LABELS))} labels eligible; "
              f"{excluded} feature-only issue(s) excluded]\n")
    print(f"{'#':>5} {'score':>5} {'flags':<14} title")
    for num, title, s, d in rows:
        flags = []
        if d.get("fun", 0) >= 8 and not is_blocked(d):
            flags.append("PULL")
        if is_blocked(d):
            flags.append("blocked:" + ",".join(map(str, d["blocked_by"])))
        print(f"{num:>5} {s:>5} {' '.join(flags):<14} {title[:64]}")
    nxt = next((r for r in rows if not is_blocked(r[3])), None)
    if nxt:
        why = " (Pull-override: fun>=8)" if nxt[3].get("fun", 0) >= 8 else ""
        print(f"\nnext: #{nxt[0]} — {nxt[1]}{why}")


def cmd_lint(args):
    missing = [(it["number"], it["title"]) for it in open_issues()
               if parse_block(it.get("body")) is None]
    if not missing:
        print("all open issues have an olo-score block.")
        return
    print(f"{len(missing)} open issue(s) need scoring:")
    for num, title in missing:
        print(f"  #{num} {title[:70]}")
    sys.exit(1)


def cmd_apply(args):
    src = json.load(open(args.src, encoding="utf-8"))["scores"]
    items = sorted(src.items(), key=lambda kv: int(kv[0]))
    if args.only:
        items = [(k, v) for k, v in items if k in set(map(str, args.only))]
    for num, d in items:
        section = render_section(d)
        if args.dry_run:
            cur = gh_json(["issue", "view", num, "--repo", REPO, "--json", "body"])["body"]
            print(f"\n===== #{num} (spliced body preview) =====\n{splice(cur, section)}")
            continue
        cur = gh_json(["issue", "view", num, "--repo", REPO, "--json", "body"])["body"]
        new_body = splice(cur, section)
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as f:
            f.write(new_body)
            path = f.name
        r = subprocess.run(["gh", "issue", "edit", num, "--repo", REPO, "--body-file", path],
                           capture_output=True, text=True, encoding="utf-8")
        print(f"#{num}: {'ok' if r.returncode == 0 else 'FAIL ' + r.stderr.strip()}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("rank")
    rp.add_argument("--freeze", action="store_true",
                     help="feature-freeze lens: only performance/robustness/architecture/bug/tooling-labeled issues")
    rp.set_defaults(func=cmd_rank)
    sub.add_parser("lint").set_defaults(func=cmd_lint)
    ap = sub.add_parser("apply")
    ap.add_argument("--from", dest="src", default="issue-scores.json")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", type=int, nargs="*", help="limit to these issue numbers")
    ap.set_defaults(func=cmd_apply)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
