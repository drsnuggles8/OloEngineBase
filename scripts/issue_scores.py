#!/usr/bin/env python3
"""Issue scoring picker — see docs/process/issue-scoring.md.

Scores live in each GitHub issue body inside an `olo-score` fenced block
(only the raw inputs; the score is derived here, never stored). Subcommands:

    rank   pull every open issue, parse its block, print the ranked list
           (default lens; Pull-override and the Tier-1 slot applied). This is
           what /start-work calls.
    lint   everything that makes the backlog lie: open issues with no olo-score
           block, stale tech-tree edges (a `blocked_by` naming a closed issue),
           unresolvable edges, scores older than STALE_DAYS, work merged since
           the last score, and a Fun >= 8 the user never confirmed.
           Exits non-zero on any of them.
    set    re-score one issue: `set 1584 effort=2 confidence=1.0`. Writes the
           given axes and stamps `scored:` with today's date, which is also how
           a stale or merged-work flag is acknowledged (`set N` with no axes).
    pull   record the USER's Pull decision: `pull 1558 1559` writes
           `fun_by: user` (and raises Fun to 8); `pull --off N` writes
           `fun_by: agent` (declined: Fun stays a tie-break, lint stops asking).
           Gated by olo-command-guards: run only with the user's approval.
    apply  one-time/migration: write blocks into issue bodies from a local
           JSON source (--from issue-scores.json). Supports --dry-run / --only.

Requires the `gh` CLI authenticated against the repo. `rank` and `lint` each make
one `gh api graphql` call to resolve every distinct `blocked_by` number in one
shot (see resolve_blocker_states) and one `gh pr list` for merged work; `rank`
makes one more graphql call for the Tier-1 slot.
"""
import argparse
import datetime as dt
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

# Pull-override (§3.4). Fun is a personal axis about the USER, but the session that
# files an issue scores it too, and on 2026-10-15 five of the seven Fun >= 8 issues
# were 2-10 days old and agent-scored: the agent was voting its own fresh issues to
# the front of the queue. So Fun >= 8 jumps the queue only with `fun_by: user`,
# which only `pull` writes and olo-command-guards gates. A Fun >= 8 the user has not
# ruled on is still a tie-break, flagged `PULL?`, and reported by `lint`; `pull --off`
# records the user's "no" as `fun_by: agent`, which keeps the Fun and ends the asking.
PULL_FUN = 8

# Staleness (§6.4). A score is a claim about the issue on the day it was made, and
# nothing used to revisit it: the scoring session's enthusiasm stayed in the ranking
# for months. A block older than this (its `scored:` date, or the day the issue was
# filed when there is none) is flagged `stale:<days>d` and reported by `lint`.
STALE_DAYS = 45

# Tier-1 slot (§3.7). Tier 1 is start-work's force-multiplier work (CI, test infra,
# MCP / dev tooling), marked with these labels. If none of the last TIER1_EVERY
# started tasks (feature/* PRs, open or merged) closed a Tier-1 issue or carried
# the label, `next:` becomes the top Tier-1 row. Derived from PR history, no state.
TIER1_LABELS = {"tooling", "ci-nightly-red"}
TIER1_EVERY = 3

# Fields `render_section` writes in this order; anything else in a block is kept,
# after these, rather than silently dropped on a rewrite.
META_FIELDS = ("kano", "blocked_by", "blocks", "blocked_by_external", "fun_by", "scored")

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
                       "--limit", "300", "--json", "number,title,body,labels,author,createdAt"])
    return [it for it in issues if not it.get("author", {}).get("is_bot")]


def label_names(it):
    return {l["name"] for l in it.get("labels", [])}


def gh_day(stamp):
    """`2026-10-15T08:17:47Z` -> date(2026, 10, 15)."""
    return dt.date.fromisoformat(stamp[:10])


# ---- merged work ------------------------------------------------------------
# A merged PR "names" an issue through its branch's trailing numbers (the task-loop
# convention: feature/voxel-state-runtime-postfx-1561-1566-1563) or a `#N` in its
# title. Either is a claim that work for the issue landed. It is not proof the issue
# is finished (umbrellas and partial deliveries are real), which is why it asks for
# triage instead of closing anything.
BRANCH_NUMS_RE = re.compile(r"((?:-\d+)+)$")
TITLE_NUM_RE = re.compile(r"#(\d+)\b")


def delivered_numbers(head_ref, title):
    nums = set()
    m = BRANCH_NUMS_RE.search(head_ref or "")
    if m:
        nums |= {int(x) for x in m.group(1).split("-") if x}
    nums |= {int(x) for x in TITLE_NUM_RE.findall(title or "")}
    return nums


def merged_work_index():
    """{issue number: [(PR number, merge date), ...]} over recent human-authored merges."""
    prs = gh_json(["pr", "list", "--repo", REPO, "--state", "merged", "--limit", "500",
                    "--json", "number,title,headRefName,mergedAt,author"])
    index = {}
    for pr in prs:
        if pr.get("author", {}).get("is_bot") or not pr.get("mergedAt"):
            continue
        for n in delivered_numbers(pr.get("headRefName"), pr.get("title")):
            index.setdefault(n, []).append((pr["number"], gh_day(pr["mergedAt"])))
    return index


def score_date(d, filed):
    """(day this score was last looked at, source): `scored:` if set, else the filing day.

    An unparseable `scored:` returns (None, "invalid"); callers treat that as stale
    and `lint` names it, rather than guessing a date.
    """
    raw = d.get("scored")
    if raw in (None, ""):
        return filed, "filed"
    try:
        return dt.date.fromisoformat(str(raw)), "scored"
    except ValueError:
        return None, "invalid"


def merged_since(num, since, index):
    """PR numbers that named `num` and merged after `since` (all of them if `since` is None)."""
    return sorted(pr for pr, day in index.get(num, []) if since is None or day > since)


def freshness(d, filed, num, index, today):
    """Everything staleness needs about one scored issue, as a dict."""
    since, source = score_date(d, filed)
    age = (today - since).days if since else None
    return {"since": since, "source": source, "age": age,
            "stale": age is None or age > STALE_DAYS,
            "merged": merged_since(num, since, index)}


def is_pull(d, states):
    return d.get("fun", 0) >= PULL_FUN and d.get("fun_by") == "user" and not is_blocked(d, states)


def is_unconfirmed_pull(d):
    """Fun >= 8 the user has neither confirmed (`user`) nor declined (`agent`)."""
    return d.get("fun", 0) >= PULL_FUN and d.get("fun_by") not in ("user", "agent")


# ---- Tier-1 slot -------------------------------------------------------------
def recent_started_tasks():
    """The last TIER1_EVERY feature/* PRs (open or merged) as [(PR, is_tier1)], or None.

    None means the history could not be read; `rank` then says so and follows the
    plain order instead of guessing that a slot is or is not due.
    """
    query = (f'query {{ repository(owner: "{REPO_OWNER}", name: "{REPO_NAME}") {{ '
             'pullRequests(first: 40, states: [OPEN, MERGED], '
             'orderBy: {field: CREATED_AT, direction: DESC}) { nodes { number headRefName '
             'labels(first: 20) { nodes { name } } '
             'closingIssuesReferences(first: 10) { nodes { labels(first: 20) { nodes { name } } } } '
             '} } } }')
    try:
        out = subprocess.run(["gh", "api", "graphql", "-f", f"query={query}"],
                             capture_output=True, text=True, encoding="utf-8",
                             check=False, timeout=_BLOCKER_TIMEOUT_S)
        nodes = json.loads(out.stdout)["data"]["repository"]["pullRequests"]["nodes"]
    except (subprocess.TimeoutExpired, ValueError, KeyError, TypeError):
        return None
    started = []
    for pr in nodes:
        if not (pr.get("headRefName") or "").startswith("feature/"):
            continue
        labels = {l["name"] for l in pr["labels"]["nodes"]}
        for issue in pr["closingIssuesReferences"]["nodes"]:
            labels |= {l["name"] for l in issue["labels"]["nodes"]}
        started.append((pr["number"], bool(labels & TIER1_LABELS)))
        if len(started) == TIER1_EVERY:
            break
    return started


def tier1_due(started):
    """True when none of the last TIER1_EVERY started tasks was Tier 1."""
    return len(started) == TIER1_EVERY and not any(t for _, t in started)


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
        # `[#1131]` is the natural way to write an issue list but is not JSON; one such block
        # used to raise JSONDecodeError and take `rank` and `lint` down for the whole backlog.
        # Only bare tokens are rewritten; `#N` inside a JSON string (a blocked_by_external
        # reason) is left as written.
        return json.loads(re.sub(r'"(?:\\.|[^"\\])*"|#(\d+)', lambda m: m.group(1) or m.group(0), v))
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


def render_fence(d):
    lines = [f"{a}: {d.get(a, 3)}" for a in ALL_AXES]
    lines.append(f"kano: {d.get('kano', 'table-stakes')}")
    lines.append(f"blocked_by: {json.dumps(d.get('blocked_by', []))}")
    lines.append(f"blocks: {json.dumps(d.get('blocks', []))}")
    # Emitted only when set: an empty list on every issue would be noise, and the
    # absent-means-empty default already reads correctly in parse_block.
    if d.get("blocked_by_external"):
        lines.append(f"blocked_by_external: {json.dumps(d['blocked_by_external'])}")
    for k in ("fun_by", "scored"):
        if d.get(k):
            lines.append(f"{k}: {d[k]}")
    # A field this file does not know is kept, not dropped on rewrite.
    for k, v in d.items():
        if k not in ALL_AXES and k not in META_FIELDS:
            lines.append(f"{k}: {json.dumps(v) if isinstance(v, list) else v}")
    return "```olo-score\n" + "\n".join(lines) + "\n```"


def render_section(d):
    caption = ("<sub>Rated per [issue-scoring](" + DOC_URL + ") · "
               "score = confidence × (capability + craft + stability + decay) / effort, "
               "derived by the picker.</sub>")
    return f"{BEGIN}\n## Score\n{render_fence(d)}\n{caption}\n{END}"


def splice(body, d):
    """`body` with its score block replaced by `d`, or `d` appended as a new section.

    Some bodies carry the fence without the begin/end markers (hand-written or
    filed from a template copy). Replacing only the fence there matters: appending
    a second section would leave the old fence first, and parse_block reads the
    first one, so the edit would silently not take.
    """
    body = body or ""
    if SECTION_RE.search(body):
        return SECTION_RE.sub(lambda _: "\n\n" + render_section(d) + "\n", body).strip() + "\n"
    if FENCE_RE.search(body):
        return FENCE_RE.sub(lambda _: render_fence(d), body, count=1)
    return body.rstrip() + "\n\n" + render_section(d) + "\n"


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


def row_flags(d, states, floor, fresh=None):
    """The `flags` column for one ranked row. `fresh` is freshness(), when known."""
    still_open, unresolved = split_blockers(d, states)
    flags = []
    if is_pull(d, states):
        flags.append("PULL")
    elif is_unconfirmed_pull(d) and not is_blocked(d, states):
        flags.append("PULL?")
    if fresh and fresh["merged"]:
        flags.append("merged:" + ",".join(f"#{n}" for n in fresh["merged"]))
    if fresh and fresh["stale"]:
        flags.append(f"stale:{fresh['age']}d" if fresh["age"] is not None else "scored?")
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
def ranked_rows(args, today):
    """Score every open scored issue. Returns (rows, states, excluded, unscored).

    A row is (number, title, score, block, labels, freshness).
    """
    rows, excluded, unscored = [], 0, []
    issues = open_issues()
    index = merged_work_index()
    for it in issues:
        d = parse_block(it.get("body"))
        if d is None:
            unscored.append((it["number"], it["title"]))
            continue
        labels = label_names(it)
        if getattr(args, "freeze", False) and not (labels & FREEZE_LABELS):
            excluded += 1
            continue
        fresh = freshness(d, gh_day(it["createdAt"]), it["number"], index, today)
        rows.append((it["number"], it["title"], score(d), d, labels, fresh))
    states = resolve_blocker_states(k for r in rows for k in blocker_keys(r[3]))
    return rows, states, excluded, unscored


def row_group(row, states):
    """Outermost sort key (§3): 0 Pull, 1 startable, 2 needs triage, 3 blocked.

    Needs-triage is work merged since the last score: the issue may be finished, so
    it must be looked at before it can be picked, but it is not hidden.
    """
    d, fresh = row[3], row[5]
    if is_blocked(d, states):
        return 3
    if fresh["merged"]:
        return 2
    return 0 if is_pull(d, states) else 1


def cmd_rank(args):
    rows, states, excluded, _ = ranked_rows(args, dt.date.today())
    floor = not args.no_low_value_floor

    # Sort order, outermost first (§3): the row group (Pull, startable, needs
    # triage, blocked), then normal-value-before-low-value, then score desc, then
    # the documented Learning/Fun tie-break, then issue number as a stable final key.
    #
    # Pull is deliberately OUTSIDE the low-value floor: a user-confirmed Fun >= 8 is
    # an explicit human "I want this", whereas the floor exists to catch work that
    # floated up on arithmetic alone (§3.5).
    rows.sort(key=lambda r: (row_group(r, states),
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
    unresolved_edges = [(r[0], k) for r in rows for k in split_blockers(r[3], states)[1]]
    if unresolved_edges:
        listed = ", ".join(f"#{k} (blocks #{num})" for num, k in
                           sorted(unresolved_edges, key=lambda e: (e[0], blocker_sort_key(e[1]))))
        print(f"[!] blocker state UNRESOLVED for {len(unresolved_edges)} edge(s) across "
              f"{len({num for num, _ in unresolved_edges})} issue(s): {listed}\n"
              f"    These stay blocked and are flagged `blocked?:`. An unresolvable "
              f"blocker is never assumed delivered.\n"
              f"    Check `gh auth status` / the network, or run "
              f"`issue_scores.py lint` for the per-edge detail.\n")
    triage = [r for r in rows if row_group(r, states) == 2]
    stale = [r for r in rows if r[5]["stale"]]
    if triage or stale:
        print(f"[!] {len(triage)} issue(s) have work merged since their score (`merged:`, sorted "
              f"below startable work) and {len(stale)} score(s) are older than {STALE_DAYS} days "
              f"(`stale:`).\n    Triage them before picking: `issue_scores.py lint`.\n")

    print(f"{'#':>5} {'score':>5} {'flags':<28} title")
    for num, title, s, d, _, fresh in rows:
        print(f"{num:>5} {s:>5} {' '.join(row_flags(d, states, floor, fresh)):<28} {title[:60]}")

    startable = [r for r in rows if row_group(r, states) <= 1]
    plain = startable[0] if startable else None
    tier1 = next((r for r in startable if r[4] & TIER1_LABELS), None)
    started = recent_started_tasks()
    nxt, why = plain, ""
    if plain and is_pull(plain[3], states):
        why = " (Pull-override: Fun >= 8, confirmed by the user)"
    print()
    if tier1:
        print(f"tier-1: #{tier1[0]} — {tier1[1]}")
    if started is None:
        print("tier-1 slot: UNKNOWN — the PR history could not be read; `next:` follows the plain rank")
    else:
        listed = ", ".join(f"#{n}{' (tier 1)' if t else ''}" for n, t in started)
        due = tier1_due(started)
        print(f"tier-1 slot: {'DUE' if due else 'not due'} — last {len(started)} started task(s): {listed}")
        if due and tier1 and plain and tier1[0] != plain[0]:
            nxt = tier1
            why = (f" (Tier-1 slot: none of the last {TIER1_EVERY} started tasks was Tier 1; "
                   f"the plain rank would pick #{plain[0]})")
    if nxt:
        print(f"next: #{nxt[0]} — {nxt[1]}{why}")


def cmd_lint(args):
    rows, states, _, missing = ranked_rows(args, dt.date.today())

    # A stale edge no longer hides its issue from `rank` — that is fixed at read
    # time. It is still worth reporting: the body says something untrue, and the
    # cheap moment to correct it is now, not the next time somebody reads it.
    stale_edges, unresolved = [], []
    for num, _, _, d, _, _ in rows:
        for k in blocker_keys(d):
            state = states.get(k, UNRESOLVED)
            if state == CLOSED:
                stale_edges.append((num, k))
            elif state == UNRESOLVED:
                unresolved.append((num, k))
    merged = [r for r in rows if r[5]["merged"]]
    old = sorted((r for r in rows if r[5]["stale"]), key=lambda r: -(r[5]["age"] or 10 ** 6))
    unconfirmed = [r for r in rows if is_unconfirmed_pull(r[3])]

    if not (missing or stale_edges or unresolved or merged or old or unconfirmed):
        print("all open issues have a current olo-score block; no stale edges, merged work "
              "or unconfirmed Pulls.")
        return
    if missing:
        print(f"{len(missing)} open issue(s) need scoring:")
        for num, title in missing:
            print(f"  #{num} {title[:70]}")
    if merged:
        print(f"\n{len(merged)} issue(s) with work merged since their score — the work may be "
              f"done. Check the code: if it is, post the evidence and recommend closure; if "
              f"not, re-score with what remains (`issue_scores.py set N ...`):")
        for num, title, _, _, _, fresh in merged:
            print(f"  #{num} merged since {fresh['since']}: "
                  f"{', '.join(f'#{n}' for n in fresh['merged'])} — {title[:50]}")
    if old:
        print(f"\n{len(old)} score(s) older than {STALE_DAYS} days — re-check the issue against "
              f"the code, then re-score it (`set N ...`, which stamps `scored:`) or recommend closure:")
        for num, title, _, _, _, fresh in old:
            when = (f"{fresh['source']} {fresh['since']} ({fresh['age']}d)" if fresh["since"]
                    else "`scored:` is not a YYYY-MM-DD date")
            print(f"  #{num} {when} — {title[:50]}")
    if unconfirmed:
        print(f"\n{len(unconfirmed)} Fun >= {PULL_FUN} the user has not ruled on — the Pull-override "
              f"does not apply. Ask the user, then record their answer: `issue_scores.py pull N` "
              f"(yes) or `pull --off N` (no):")
        for num, title, _, d, _, _ in unconfirmed:
            print(f"  #{num} fun {d.get('fun')} — {title[:60]}")
    if stale_edges:
        print(f"\n{len(stale_edges)} stale blocker edge(s) — blocker already closed, "
              f"drop it from `blocked_by`:")
        for num, k in stale_edges:
            print(f"  #{num} blocked_by #{k} (closed)")
    if unresolved:
        print(f"\n{len(unresolved)} unresolvable blocker edge(s) — state could not be "
              f"read, so these still block:")
        for num, k in unresolved:
            print(f"  #{num} blocked_by {k} (state unknown: no such issue/PR, or "
                  f"`gh` offline / unauthenticated / rate-limited)")
    sys.exit(1)


def write_body(num, body, dry_run):
    if dry_run:
        print(f"\n===== #{num} (spliced body preview) =====\n{body}")
        return True
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8",
                                     newline="\n") as f:
        f.write(body)
        path = f.name
    r = subprocess.run(["gh", "issue", "edit", str(num), "--repo", REPO, "--body-file", path],
                       capture_output=True, text=True, encoding="utf-8")
    print(f"#{num}: {'ok' if r.returncode == 0 else 'FAIL ' + r.stderr.strip()}")
    return r.returncode == 0


def parse_assignment(text):
    """`effort=2` / `blocked_by=[1611]` / `kano=performance` -> (key, value)."""
    key, sep, value = text.partition("=")
    key = key.strip()
    if not sep or not key:
        sys.exit(f"expected axis=value, got {text!r}")
    if key == "fun_by":
        sys.exit("fun_by is the user's confirmation and only `issue_scores.py pull` writes it.")
    if key == "scored":
        sys.exit("`scored` is stamped by `set` itself; it is not an input.")
    if key not in ALL_AXES and key not in META_FIELDS:
        sys.exit(f"unknown field {key!r}; fields are {', '.join(ALL_AXES + META_FIELDS)}")
    return key, _coerce(value)


def cmd_set(args):
    cur = gh_json(["issue", "view", str(args.number), "--repo", REPO, "--json", "body"])["body"]
    d = parse_block(cur) or {}
    for text in args.fields:
        key, value = parse_assignment(text)
        d[key] = value
    if d.get("blocked_by_external") in ([], ""):
        d.pop("blocked_by_external")
    d["scored"] = dt.date.today().isoformat()
    sys.exit(0 if write_body(args.number, splice(cur, d), args.dry_run) else 1)


def cmd_pull(args):
    ok = True
    for num in args.numbers:
        cur = gh_json(["issue", "view", str(num), "--repo", REPO, "--json", "body"])["body"]
        d = parse_block(cur)
        if d is None:
            print(f"#{num}: no olo-score block — score it first (`set {num} ...`)")
            ok = False
            continue
        if args.off:
            d["fun_by"] = "agent"
        else:
            d["fun_by"] = "user"
            if d.get("fun", 0) < PULL_FUN:
                print(f"#{num}: fun {d.get('fun')} -> {PULL_FUN}")
                d["fun"] = PULL_FUN
        ok = write_body(num, splice(cur, d), args.dry_run) and ok
    sys.exit(0 if ok else 1)


def cmd_apply(args):
    src = json.load(open(args.src, encoding="utf-8"))["scores"]
    items = sorted(src.items(), key=lambda kv: int(kv[0]))
    if args.only:
        items = [(k, v) for k, v in items if k in set(map(str, args.only))]
    for num, d in items:
        if "fun_by" in d:
            sys.exit(f"#{num}: fun_by is the user's confirmation; `apply` does not write it (use `pull`).")
        cur = gh_json(["issue", "view", num, "--repo", REPO, "--json", "body"])["body"]
        write_body(num, splice(cur, d), args.dry_run)


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
     {"blocked_by": [1123], "fun": 9, "fun_by": "user"}, False, ["PULL"]),
    ("Pull-override stays suppressed while a blocker is open",
     {"blocked_by": [1126], "fun": 9, "fun_by": "user"}, True, ["blocked:1126"]),
    # 2026-10-15: agent-scored Fun was jumping fresh issues to the front.
    ("an agent-scored Fun >= 8 is not a Pull; it is flagged PULL?",
     {"fun": 8}, False, ["PULL?"]),
    ("fun_by: user with Fun below 8 is no Pull",
     {"fun": 5, "fun_by": "user"}, False, []),
    ("a Pull the user declined (fun_by: agent) is no Pull and no longer asks",
     {"fun": 13, "fun_by": "agent"}, False, []),
)


_D = dt.date
_SELF_TEST_TODAY = _D(2026, 10, 15)
_SELF_TEST_INDEX = {1593: [(1599, _D(2026, 10, 12))], 607: [(1551, _D(2026, 10, 1)), (1570, _D(2026, 10, 7))]}

# (description, block, filed, issue number, expected freshness subset)
FRESHNESS_CASES = (
    ("no `scored:` falls back to the filing day", {}, _D(2026, 10, 1), 1, {"age": 14, "stale": False}),
    ("older than STALE_DAYS is stale", {}, _D(2026, 7, 1), 1, {"age": 106, "stale": True}),
    ("`scored:` overrides the filing day", {"scored": "2026-10-01"}, _D(2026, 1, 1), 1,
     {"age": 14, "stale": False, "source": "scored"}),
    ("an unparseable `scored:` is stale with no age, never guessed", {"scored": "last week"},
     _D(2026, 10, 14), 1, {"age": None, "stale": True, "source": "invalid"}),
    ("the #1593 case: work merged after the score", {}, _D(2026, 10, 11), 1593, {"merged": [1599]}),
    ("re-scoring after the merge acknowledges it", {"scored": "2026-10-12"}, _D(2026, 10, 11), 1593,
     {"merged": []}),
    ("an umbrella keeps only the merges after its last score", {"scored": "2026-10-03"},
     _D(2026, 6, 1), 607, {"merged": [1570]}),
)

DELIVERED_CASES = (
    ("feature/voxel-state-runtime-postfx-1561-1566-1563", "fix(scene,terrain): x", {1561, 1566, 1563}),
    ("feature/mcp-material-physics2d-asset-pack-607", "Expose material fields", {607}),
    ("feature/claude-code-mods", "Claude Code mods", set()),
    ("feature/bug-sweep-x", "fix(groom,build): a (#1595); b (#1575)", {1595, 1575}),
    ("renovate/vendored-c++-deps", "chore(deps): update", set()),
)

TIER1_CASES = (
    ("none of the last three was Tier 1", [(1, False), (2, False), (3, False)], True),
    ("one Tier-1 task among them", [(1, False), (2, True), (3, False)], False),
    ("too little history is never due", [(1, False)], False),
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
    for description, d, filed, num, want in FRESHNESS_CASES:
        got = freshness(d, filed, num, _SELF_TEST_INDEX, _SELF_TEST_TODAY)
        if any(got[k] != v for k, v in want.items()):
            failures += 1
            print(f"SELF-TEST FAILED (freshness: {description}):\n"
                  f"  want={want}\n  got ={got}", file=sys.stderr)
    for head, title, want in DELIVERED_CASES:
        got = delivered_numbers(head, title)
        if got != want:
            failures += 1
            print(f"SELF-TEST FAILED (delivered: {head} / {title}): want={want} got={got}",
                  file=sys.stderr)
    for description, started, want in TIER1_CASES:
        if tier1_due(started) != want:
            failures += 1
            print(f"SELF-TEST FAILED (tier-1: {description}): want={want}", file=sys.stderr)
    # A rewrite must keep every field, and replace a marker-less fence in place.
    block = {"fun": 9, "fun_by": "user", "scored": "2026-10-15", "custom": "kept"}
    bare = "text\n```olo-score\nfun: 3\n```\nmore"
    rewritten = splice(bare, block)
    if parse_block(rewritten) != {**{a: 3 for a in ALL_AXES}, **block, "kano": "table-stakes",
                                  "blocked_by": [], "blocks": []} or rewritten.count("olo-score") != 1:
        failures += 1
        print(f"SELF-TEST FAILED (splice keeps fields, replaces a bare fence):\n{rewritten}",
              file=sys.stderr)
    return failures


def self_test_count():
    return (len(SELF_TEST_CASES) + len(SORT_CASES) + len(FRESHNESS_CASES)
            + len(DELIVERED_CASES) + len(TIER1_CASES) + 1)


def cmd_selftest(args):
    failures = self_test()
    print(f"{self_test_count()} case(s), {failures} failure(s)")
    sys.exit(1 if failures else 0)


def main():
    if self_test():
        sys.exit("\nissue_scores.py self-test failed; fix it before trusting `rank` — a "
                 "mis-blocked or mis-dated issue is missing from the output or wrong in it.")
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
    sp = sub.add_parser("set", help="re-score one issue and stamp `scored:` with today")
    sp.add_argument("number", type=int)
    sp.add_argument("fields", nargs="*", metavar="axis=value")
    sp.add_argument("--dry-run", action="store_true")
    sp.set_defaults(func=cmd_set)
    pp = sub.add_parser("pull", help="record the USER's Pull-override (gated: user approval only)")
    pp.add_argument("numbers", type=int, nargs="+")
    pp.add_argument("--off", action="store_true",
                    help="record the user's decline as `fun_by: agent` (no Pull, no more asking)")
    pp.add_argument("--dry-run", action="store_true")
    pp.set_defaults(func=cmd_pull)
    sub.add_parser("selftest", help="run the picker's built-in cases and report the count")\
       .set_defaults(func=cmd_selftest)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
