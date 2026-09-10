#!/usr/bin/env python3
"""Issue scoring picker — see docs/process/issue-scoring.md.

Scores live in each GitHub issue body inside an `olo-score` fenced block
(only the raw inputs; the score is derived here, never stored). Subcommands:

    rank   pull every open issue, parse its block, print the ranked list
           (default lens; Pull-override applied). This is what /start-work calls.
    lint   list open issues with no olo-score block (the "needs-score" nudge),
           plus stale tech-tree edges (a `blocked_by` naming an issue that is
           already closed) and edges whose state could not be resolved at all.
           Exits non-zero on any of the three.
    apply  one-time/migration: write blocks into issue bodies from a local
           JSON source (--from issue-scores.json). Supports --dry-run / --only.

Requires the `gh` CLI authenticated against the repo. `rank` and `lint` each make
exactly one extra `gh api graphql` call to resolve every distinct `blocked_by`
number in one shot; see resolve_blocker_states.
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile

REPO = "drsnuggles8/OloEngineBase"
REPO_OWNER, REPO_NAME = REPO.split("/")
VALUE_AXES = ("capability", "craft", "stability", "decay")
ALL_AXES = VALUE_AXES + ("effort", "confidence", "learning", "fun")
KANO = ("table-stakes", "performance", "delighter")
DOC_URL = f"https://github.com/{REPO}/blob/master/docs/process/issue-scoring.md"

# Issue titles/bodies routinely carry non-cp1252 characters (em dashes, arrows).
# On a Windows console the default encoding is cp1252, so printing one raises
# UnicodeEncodeError *mid-report* — `lint` used to die partway through its list,
# silently under-reporting the unscored backlog. Force UTF-8 on our own streams.
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

# A ratio model rewards cheap work, which is correct — but it cannot distinguish
# "cheap and worth doing" from "cheap and not worth a slot": a near-valueless
# issue with a small effort denominator floats into the top of the list (the
# real case that motivated this: #411, CoD 5 / effort 2 = 1.25, out-ranking
# substantial work while having no remaining actionable content). Issues whose
# CoD falls below this floor are still listed and still scored, but sort BELOW
# every normal-value unblocked issue and are flagged `low-value`. See
# issue-scoring.md §3. Pass --no-low-value-floor to rank without low-value
# demotion (blocked-handling, the Pull-override and the tie-break still apply).
LOW_VALUE_COD = 6

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


# ---- blocker state ----------------------------------------------------------
# A blocker resolves to one of these. Anything that is not OPEN and not a clean
# CLOSED/MERGED is UNRESOLVED — never guessed in either direction.
OPEN, CLOSED, UNRESOLVED = "OPEN", "CLOSED", "UNRESOLVED"

# Alias-per-number GraphQL, chunked. GitHub caps aliases per query well above
# what a backlog's distinct blocker set reaches, but chunk anyway so a future
# tech tree cannot silently trip the limit.
_BLOCKER_CHUNK = 100

# Per-chunk wall clock. The call normally answers in about a second; this is here
# so a stalled connection degrades to UNRESOLVED instead of hanging `rank`.
_BLOCKER_TIMEOUT_S = 30


def blocker_key(raw):
    """Normalize one `blocked_by` entry to an int issue number.

    Entries are written as bare numbers, but "#1123" turns up when a block is
    hand-edited. Anything that is not a positive issue number comes back
    unchanged as a string, so it resolves to UNRESOLVED and gets reported —
    dropping it would be exactly the silent pass this change exists to remove,
    and letting it through would put an invalid alias (`i-5`) into the query and
    take the whole chunk down with it.
    """
    try:
        n = int(str(raw).strip().lstrip("#").strip())
    except ValueError:
        return str(raw)
    return n if n > 0 else str(raw)


def blocker_keys(d):
    return [blocker_key(raw) for raw in (d.get("blocked_by") or [])]


def blocker_sort_key(k):
    """Order blocker keys without ever comparing an int to a str.

    A block can yield both — 1123 for a real edge, "n/a" for a malformed one —
    and sorting the mixed list directly raises TypeError. Numbers sort first, in
    numeric order; malformed entries follow, lexicographically. The second slot
    is only reached when the first ties, which forces both sides to one type.
    """
    return (isinstance(k, str), k)


def resolve_blocker_states(numbers):
    """Return {blocker key: OPEN | CLOSED | UNRESOLVED} covering every key given.

    One `gh api graphql` call per chunk, resolving all distinct blocker numbers
    at once — not one `gh issue view` per blocker per run. `issueOrPullRequest`
    is used rather than `issue` because a `blocked_by` occasionally names a PR;
    a merged PR is as delivered as a closed issue, so MERGED folds into CLOSED.

    Nothing is cached across runs on purpose. A cache of "is #1123 still open?"
    is a second copy of a fact that changes without us — the same shape as the
    stale `blocked_by` body this function exists to stop trusting.

    Unresolvable is a first-class answer, never a fallback to OPEN or CLOSED:
    a number that does not exist, a `gh` that is offline, unauthenticated or
    rate-limited all land on UNRESOLVED, and every caller reports it.
    """
    keys = sorted(set(numbers), key=blocker_sort_key)
    # Only a positive int can become an alias. Callers normally pre-filter via
    # blocker_key, but one bad entry reaching the query text makes it invalid and
    # takes every real blocker in the chunk down with it, so re-check here.
    nums = [k for k in keys if isinstance(k, int) and k > 0]
    states = {}
    for i in range(0, len(nums), _BLOCKER_CHUNK):
        chunk = nums[i:i + _BLOCKER_CHUNK]
        fields = "\n".join(
            f'    i{n}: issueOrPullRequest(number: {n}) '
            f'{{ ... on Issue {{ state }} ... on PullRequest {{ state }} }}'
            for n in chunk)
        query = (f'query {{\n  repository(owner: "{REPO_OWNER}", name: "{REPO_NAME}") {{\n'
                 f'{fields}\n  }}\n}}')
        # NOT gh_json(): a partial answer is useful and a total failure must not
        # kill the report. `gh` exits non-zero when *any* alias 404s while still
        # writing the resolved ones to stdout, so parse stdout regardless
        # (check=False, spelled out because that is load-bearing here).
        #
        # The timeout matters as much as the parse: a stalled connection would
        # otherwise hang `rank` forever with no output at all. A timeout leaves
        # this chunk's keys unset, so the setdefault below makes them UNRESOLVED
        # — the same loud, counted answer as any other unreadable state.
        try:
            out = subprocess.run(["gh", "api", "graphql", "-f", f"query={query}"],
                                 capture_output=True, text=True, encoding="utf-8",
                                 check=False, timeout=_BLOCKER_TIMEOUT_S)
            repo = (json.loads(out.stdout) or {}).get("data", {}).get("repository") or {}
        except subprocess.TimeoutExpired:
            repo = {}
        except (ValueError, AttributeError):
            repo = {}
        for n in chunk:
            node = repo.get(f"i{n}") or {}
            state = node.get("state")
            if state == OPEN:
                states[n] = OPEN
            elif state in ("CLOSED", "MERGED"):
                states[n] = CLOSED
    for k in keys:
        states.setdefault(k, UNRESOLVED)
    return states


def split_blockers(d, states):
    """(still-open blockers, unresolvable blockers) for one score block.

    Closed blockers appear in neither list — that is the whole point. The split
    is what the `blocked:` flag prints, so a partially-delivered tech tree names
    only the edges that are actually still holding the issue up.
    """
    still_open, unresolved = [], []
    for k in blocker_keys(d):
        state = states.get(k, UNRESOLVED)
        if state == OPEN:
            still_open.append(k)
        elif state == UNRESOLVED:
            unresolved.append(k)
    return still_open, unresolved


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
    # Emitted only when set: an empty list on every issue would be noise, and the
    # absent-means-empty default already reads correctly in parse_block.
    if d.get("blocked_by_external"):
        lines.append(f"blocked_by_external: {json.dumps(d['blocked_by_external'])}")
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
def cod(d):
    """Cost of Delay — the value-axis numerator."""
    return sum(d.get(a, 0) for a in VALUE_AXES)


def score(d):
    eff = d.get("effort", 0) or 0
    if not eff:
        return 0.0
    return round(d.get("confidence", 1.0) * cod(d) / eff, 2)


def is_blocked(d, states):
    """Blocked = cannot be started today, whatever holds it up.

    `blocked_by` models the in-repo tech tree (issue numbers). `blocked_by_external`
    models everything the tech tree cannot express: an upstream release we are waiting
    on, a compiler feature that has not shipped, a vendor fix. Both drop the issue from
    the picker, because the picker's question is "can I start this now?" and the answer
    is no either way.

    Before this existed, an externally-blocked issue carried an empty `blocked_by` and
    therefore ranked as startable — #815 sat at rank 7 while waiting on two upstream
    repowise releases, and had to be skipped by hand on every sweep. A human silently
    re-deriving the same "oh, not that one" every time is exactly the cost this file
    exists to remove.

    A `blocked_by` entry only blocks while the issue it names is still OPEN — the
    written-down number is a pointer, not a fact. This used to be assumed rather than
    asked, so a blocker that had merged kept its dependents out of `rank` until a human
    hand-edited the body: on 2026-09-09 the closed #1123 and #1139 were hiding seven
    issues, including #1126 and #1130, the two highest-scoring rows in the whole
    backlog. A stale row does not sort low, it is absent, so nothing in the output
    could reveal it (#1161).

    An UNRESOLVED blocker keeps blocking. That direction is chosen, not defaulted: the
    other one invents startable work out of a network error, and a bad pick costs a
    whole task slot. It is never silent — the row is flagged `blocked?:` and `rank`
    prints a counted banner, so "we could not tell" never reads as "it is fine".

    `blocked_by_external` is left blocking unconditionally: it is free text naming
    things outside this repo (#1148), with no state to query.
    """
    still_open, unresolved = split_blockers(d, states)
    return bool(still_open) or bool(unresolved) or bool(d.get("blocked_by_external"))


def row_flags(d, states, floor):
    """The `flags` column for one ranked row."""
    still_open, unresolved = split_blockers(d, states)
    flags = []
    if d.get("fun", 0) >= 8 and not is_blocked(d, states):
        flags.append("PULL")
    if still_open:
        flags.append("blocked:" + ",".join(map(str, still_open)))
    if unresolved:
        flags.append("blocked?:" + ",".join(map(str, unresolved)))
    if d.get("blocked_by_external"):
        flags.append("external")
    if floor and is_low_value(d):
        flags.append(f"low-value:{cod(d)}")
    return flags


def is_low_value(d):
    return cod(d) < LOW_VALUE_COD


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
    floor = not args.no_low_value_floor
    states = resolve_blocker_states(k for _, _, _, d in rows for k in blocker_keys(d))

    # Sort order, outermost first (§3): Pull-override, then unblocked-before-blocked,
    # then normal-value-before-low-value, then score desc, then the documented
    # Learning/Fun tie-break, then issue number as a stable final key.
    #
    # Pull is deliberately OUTSIDE the low-value floor: scoring Fun >= 8 is an explicit
    # human "I want this", whereas the floor exists to catch work that floated up on
    # arithmetic alone. A cheap *and* fun task is exactly the case §3 says stays
    # pickable, so Pull is the documented exception to the floor's guarantee (§3.5) --
    # not an oversight. `next:` therefore just takes the first unblocked row, so the
    # recommendation can never disagree with the printed order.
    rows.sort(key=lambda r: (not (r[3].get("fun", 0) >= 8 and not is_blocked(r[3], states)),
                             is_blocked(r[3], states),
                             floor and is_low_value(r[3]),
                             -r[2],
                             -r[3].get("learning", 0), -r[3].get("fun", 0),
                             r[0]))
    if args.freeze:
        print(f"[--freeze active: only {'/'.join(sorted(FREEZE_LABELS))} labels eligible; "
              f"{excluded} feature-only issue(s) excluded]\n")

    # Loud and counted, above the table: an unresolvable blocker is the one case
    # where the printed order may be wrong, and it must not be inferred from a
    # `blocked?:` flag somebody has to spot. See is_blocked.
    unresolved_edges = [(num, k) for num, _, _, d in rows for k in split_blockers(d, states)[1]]
    if unresolved_edges:
        listed = ", ".join(f"#{k} (blocks #{num})" for num, k in
                           sorted(unresolved_edges, key=lambda e: (e[0], blocker_sort_key(e[1]))))
        print(f"[!] blocker state UNRESOLVED for {len(unresolved_edges)} edge(s) across "
              f"{len({num for num, _ in unresolved_edges})} issue(s): {listed}\n"
              f"    These stay blocked and are flagged `blocked?:`. An unresolvable "
              f"blocker is never assumed delivered.\n"
              f"    Check `gh auth status` / the network, or run "
              f"`issue_scores.py lint` for the per-edge detail.\n")

    print(f"{'#':>5} {'score':>5} {'flags':<20} title")
    for num, title, s, d in rows:
        print(f"{num:>5} {s:>5} {' '.join(row_flags(d, states, floor)):<20} {title[:64]}")
    nxt = next((r for r in rows if not is_blocked(r[3], states)), None)
    if nxt:
        why = " (Pull-override: fun>=8)" if nxt[3].get("fun", 0) >= 8 else ""
        print(f"\nnext: #{nxt[0]} — {nxt[1]}{why}")


def cmd_lint(args):
    missing, scored = [], []
    for it in open_issues():
        d = parse_block(it.get("body"))
        if d is None:
            missing.append((it["number"], it["title"]))
        else:
            scored.append((it["number"], d))

    # A stale edge no longer hides its issue from `rank` — that is fixed at read
    # time. It is still worth reporting: the body says something untrue, and the
    # cheap moment to correct it is now, not the next time somebody reads it.
    states = resolve_blocker_states(k for _, d in scored for k in blocker_keys(d))
    stale, unresolved = [], []
    for num, d in scored:
        for k in blocker_keys(d):
            state = states.get(k, UNRESOLVED)
            if state == CLOSED:
                stale.append((num, k))
            elif state == UNRESOLVED:
                unresolved.append((num, k))

    if not (missing or stale or unresolved):
        print("all open issues have an olo-score block; no stale blocker edges.")
        return
    if missing:
        print(f"{len(missing)} open issue(s) need scoring:")
        for num, title in missing:
            print(f"  #{num} {title[:70]}")
    if stale:
        print(f"\n{len(stale)} stale blocker edge(s) — blocker already closed, "
              f"drop it from `blocked_by`:")
        for num, k in stale:
            print(f"  #{num} blocked_by #{k} (closed)")
    if unresolved:
        print(f"\n{len(unresolved)} unresolvable blocker edge(s) — state could not be "
              f"read, so these still block:")
        for num, k in unresolved:
            print(f"  #{num} blocked_by {k} (state unknown: no such issue/PR, or "
                  f"`gh` offline / unauthenticated / rate-limited)")
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


# ---- self-test --------------------------------------------------------------
# Blocker handling is the one part of this file with no visible failure mode: a
# wrongly-blocked issue is *absent* from the output, not wrong in it. #1161 sat
# undetected until a sweep happened to compare the list against the tracker by
# hand. So the cases live here and run on every invocation — they are pure
# dict-in/flags-out and cost microseconds.
#
# The live data cannot stand in for them: the seven bodies #1161 found were
# healed by hand when it was filed, so nothing in the real backlog still
# exercises the stale case.
_SELF_TEST_STATES = {1123: CLOSED, 1139: CLOSED, 1126: OPEN, 1131: OPEN, "n/a": UNRESOLVED}

# (description, score-block fragment, expected is_blocked, expected flag list)
SELF_TEST_CASES = (
    ("no blockers at all", {}, False, []),
    ("the #1161 case: the only blocker is closed",
     {"blocked_by": [1123]}, False, []),
    ("every blocker closed",
     {"blocked_by": [1123, 1139]}, False, []),
    ("a still-open blocker blocks",
     {"blocked_by": [1126]}, True, ["blocked:1126"]),
    # The easy one to get wrong: the flag must name the open blocker only, or the
    # row tells you to go wait on work that already shipped.
    ("mixed open and closed: blocked, and the flag names only the open one",
     {"blocked_by": [1123, 1126]}, True, ["blocked:1126"]),
    ("closed blocker written as a #-prefixed string",
     {"blocked_by": ["#1123"]}, False, []),
    ("open blocker written as a #-prefixed string",
     {"blocked_by": ["#1126"]}, True, ["blocked:1126"]),
    # Unresolvable never silently passes and never silently blocks: it blocks,
    # under its own flag, so the row is distinguishable from a real edge.
    ("unresolvable blocker still blocks, under its own flag",
     {"blocked_by": ["n/a"]}, True, ["blocked?:n/a"]),
    ("a number nobody resolved is unresolvable, not open and not closed",
     {"blocked_by": [4242]}, True, ["blocked?:4242"]),
    # Kept out of the query rather than emitted as the invalid alias `i-5`,
    # which would fail the whole chunk and take real blockers down with it.
    ("a non-positive number is unresolvable, never an alias",
     {"blocked_by": [-5, 0]}, True, ["blocked?:-5,0"]),
    ("whitespace around a #-prefixed blocker still resolves",
     {"blocked_by": [" #1123 "]}, False, []),
    # One issue can yield an int key and a str key at once. `rank` sorts the
    # unresolvable edges for its banner, and sorting that mix directly raises
    # TypeError before a single row is printed — see blocker_sort_key.
    ("a numeric and a malformed blocker on one issue coexist",
     {"blocked_by": [4242, "n/a"]}, True, ["blocked?:4242,n/a"]),
    ("open + unresolvable are reported separately",
     {"blocked_by": [1126, "n/a"]}, True, ["blocked:1126", "blocked?:n/a"]),
    ("closed + unresolvable: the closed one is gone, the unknown one remains",
     {"blocked_by": [1123, "n/a"]}, True, ["blocked?:n/a"]),
    # #1148: external blockers are free text with no state to query.
    ("blocked_by_external keeps blocking even with every issue blocker closed",
     {"blocked_by": [1123], "blocked_by_external": ["upstream release"]}, True, ["external"]),
    ("blocked_by_external alone blocks",
     {"blocked_by_external": ["vendor fix"]}, True, ["external"]),
    # A closed blocker must restore the Pull-override too, not just the row.
    ("Pull-override returns once the only blocker is closed",
     {"blocked_by": [1123], "fun": 9}, False, ["PULL"]),
    ("Pull-override stays suppressed while a blocker is open",
     {"blocked_by": [1126], "fun": 9}, True, ["blocked:1126"]),
)


# `rank` sorts the unresolvable edges for its banner. A block yielding both an
# int key and a str key made that sort raise TypeError before a single row was
# printed, so the guard has to cover the ordering itself, not just the flags.
SORT_CASES = (
    ("numbers numerically, malformed entries after them",
     [999, "n/a", 42, "x"], [42, 999, "n/a", "x"]),
    ("ints never sort lexicographically", [999, 4242], [999, 4242]),
)


def self_test():
    """Guard the picker. Returns the failure count."""
    failures = 0
    for description, keys, want in SORT_CASES:
        try:
            got = sorted(keys, key=blocker_sort_key)
        except TypeError as exc:
            got = f"TypeError: {exc}"
        if got != want:
            failures += 1
            print(f"SELF-TEST FAILED (sort: {description}):\n"
                  f"  want={want}\n  got ={got}", file=sys.stderr)
    for description, d, want_blocked, want_flags in SELF_TEST_CASES:
        got_blocked = is_blocked(d, _SELF_TEST_STATES)
        # floor=False: the low-value flag is orthogonal and every fragment here
        # has CoD 0, which would add `low-value:0` to all of them.
        got_flags = row_flags(d, _SELF_TEST_STATES, floor=False)
        if got_blocked != want_blocked or got_flags != list(want_flags):
            failures += 1
            print(f"SELF-TEST FAILED ({description}):\n"
                  f"  block   {d}\n"
                  f"  blocked want={want_blocked} got={got_blocked}\n"
                  f"  flags   want={list(want_flags)} got={got_flags}", file=sys.stderr)
    return failures


def cmd_selftest(args):
    failures = self_test()
    print(f"{len(SELF_TEST_CASES) + len(SORT_CASES)} case(s), {failures} failure(s)")
    sys.exit(1 if failures else 0)


def main():
    if self_test():
        sys.exit("\nissue_scores.py blocker handling is broken; fix it before trusting "
                 "`rank` — a mis-blocked issue is missing from the output, not wrong in it.")
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("rank")
    rp.add_argument("--freeze", action="store_true",
                     help="feature-freeze lens: only performance/robustness/architecture/bug/tooling-labeled issues")
    rp.add_argument("--no-low-value-floor", action="store_true",
                     help=f"rank without low-value demotion (issues with CoD < {LOW_VALUE_COD} "
                          f"keep their score position; Pull and the tie-break are unaffected)")
    rp.set_defaults(func=cmd_rank)
    sub.add_parser("lint").set_defaults(func=cmd_lint)
    ap = sub.add_parser("apply")
    ap.add_argument("--from", dest="src", default="issue-scores.json")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", type=int, nargs="*", help="limit to these issue numbers")
    ap.set_defaults(func=cmd_apply)
    sub.add_parser("selftest", help="run the blocker-handling cases and report the count")\
       .set_defaults(func=cmd_selftest)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
