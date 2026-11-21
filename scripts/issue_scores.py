#!/usr/bin/env python3
"""Issue scoring picker — see docs/process/issue-scoring.md.

Scores live in each GitHub issue body inside an `olo-score` fenced block
(only the raw inputs; the score is derived here, never stored). Subcommands:

    rank   pull every open issue, parse its block, print the ranked list
           (default lens; the discovery term, Pull-override, the Tier-1 slot and
           the large-item slot applied; --no-discovery for the plain §3 order).
           This is what /start-work calls.
    yield  the discovery term's history: per area, the model behind the term;
           per merged task PR, the predicted vs actual severity-weighted yield.
    lint   everything that makes the backlog lie: open issues with no olo-score
           block or no `areas:`, stale tech-tree edges (a `blocked_by` naming a closed issue),
           unresolvable edges, scores older than STALE_DAYS, work merged since
           the last score, and a Fun >= 8 the user never confirmed.
           Exits non-zero on any of them.
    pulse  throughput: task PRs merged and issues opened/closed over the last 7
           and 28 days, work in flight, and the north-star milestone's progress.
           `/start-work` prints it in its report.
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
makes one more graphql call for the Tier-1 and large-item slots, and for the
discovery term one batched graphql call per 100 history issues plus one local
`git log` over the merge commits (`yield` makes the same calls).
"""
import argparse
import datetime as dt
import json
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

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

# Large-item slot (§3.8). Effort divides the score, so an issue of effort LARGE_EFFORT
# or more never reaches the top of `rank` on arithmetic alone, yet the broad
# structural items are where most latent bugs get found and fixed (#1694 fixed 30 in
# its own PR, a typical small fix 0-5). The slot is due when none of the last
# TIER1_EVERY started tasks closed a large issue and no open feature/* PR closes one;
# then `large:` names the top startable large row. Derived from PR history, no state.
LARGE_EFFORT = 5

# Close-recommended (§3, group 2). An issue a sweep proved done carries this label until the
# user closes it. It used to keep ranking: #1687 and #1688 sat at rank 1-2 for six
# days after their evidence comments. Now it sorts with needs-triage, never `next:`.
CLOSE_LABEL = "close-recommended"

# North star (§3.9). The project goal the backlog is steered toward, declared by the
# user in docs/process/north-star.md. Its `milestone:` names the GitHub milestone
# that holds the goal's critical path. Empty means no goal is set and nothing here
# changes the rank. Bug discovery feeds whichever area was last worked (a renderer
# sweep files renderer issues); the north star is the counterweight.
NORTH_STAR_PATH = Path(__file__).resolve().parent.parent / "docs" / "process" / "north-star.md"
NORTH_STAR_RE = re.compile(r"```olo-north-star\s*\n(.*?)\n```", re.DOTALL)
CHECKLIST_RE = re.compile(r"^\s*[-*]\s+\[( |x|X)\]\s+(.*)$", re.MULTILINE)
GOAL_DECAY = 5  # §1 Decay rung "near milestone": a critical-path issue scores at least this

# Fields `render_section` writes in this order; anything else in a block is kept,
# after these, rather than silently dropped on a rewrite.
META_FIELDS = ("kano", "blocked_by", "blocks", "areas", "blocked_by_external", "fun_by", "scored")
# Fields `set` writes without re-stamping `scored:`. Declaring an issue's areas is not a
# check of its axes against the code; stamping it would clear a real `stale:` flag (and
# the backfill of every open issue would have cleared all of them at once).
UNSTAMPED_FIELDS = ("areas",)

BEGIN, END = "<!-- olo-score:begin -->", "<!-- olo-score:end -->"
SECTION_RE = re.compile(r"\n*" + re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n*", re.DOTALL)
FENCE_RE = re.compile(r"```olo-score\s*\n(.*?)\n```", re.DOTALL)


# ---- gh helpers -------------------------------------------------------------
def gh_json(args):
    out = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8")
    if out.returncode != 0:
        sys.exit(f"gh failed: {' '.join(args)}\n{out.stderr}")
    return json.loads(out.stdout)


def is_backlog_item(it):
    """A human's issue is backlog work. A bot's is only when it says so, by carrying
    an olo-score block or the `ci-nightly-red` label: Renovate's "Dependency
    Dashboard" is a tracking page, but nightly_red_streak.py files its red-streak
    issues as `github-actions`, and dropping every bot author hid them from `rank`
    and `lint` and left `ci-nightly-red` in TIER1_LABELS matching nothing (#1516)."""
    if not it.get("author", {}).get("is_bot"):
        return True
    return parse_block(it.get("body")) is not None or "ci-nightly-red" in label_names(it)


def open_issues():
    """Open issues eligible for scoring (see is_backlog_item)."""
    issues = gh_json(["issue", "list", "--repo", REPO, "--state", "open",
                       "--limit", "300", "--json",
                       "number,title,body,labels,author,createdAt,milestone"])
    return [it for it in issues if is_backlog_item(it)]


def label_names(it):
    return {l["name"] for l in it.get("labels", [])}


def milestone_title(it):
    return (it.get("milestone") or {}).get("title") or ""


# ---- north star -------------------------------------------------------------
def scalar_value(val):
    """A YAML-like scalar. A quoted value keeps every character inside its quotes,
    `#` included (`"Demo #1"`). An unquoted one ends at an inline `# comment`."""
    val = val.strip()
    if val[:1] in ('"', "'"):
        end = val.find(val[0], 1)
        return val[1:end] if end > 0 else val[1:]
    return val.split("#", 1)[0].strip()


def parse_north_star(text):
    """{"milestone": str, "items": [(done, text, issue numbers)]} from north-star.md.

    `items` is the "Critical path" checklist: every `- [ ]` line in the file. A line
    with no `#N` is a capability nobody has filed yet, which `lint` reports.
    """
    out = {"milestone": "", "items": []}
    m = NORTH_STAR_RE.search(text or "")
    if m:
        for line in m.group(1).splitlines():
            key, _, val = line.partition(":")
            if key.strip() == "milestone":
                out["milestone"] = scalar_value(val)
    for done, item in CHECKLIST_RE.findall(text or ""):
        out["items"].append((done.lower() == "x", item.strip(),
                             [int(n) for n in TITLE_NUM_RE.findall(item)]))
    return out


def north_star():
    try:
        return parse_north_star(NORTH_STAR_PATH.read_text(encoding="utf-8"))
    except OSError:
        return parse_north_star("")


def is_goal(d, star):
    return bool(star["milestone"]) and d.get("_milestone") == star["milestone"]


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


def merged_prs(bodies=False):
    """The last 500 merged PRs. With `bodies`, also what the discovery history needs, so
    one fetch serves both it and merged-work staleness (§6.4, §1 Discovery); without, the
    small metadata `lint` needs, not 500 PR bodies."""
    fields = "number,title,headRefName,mergedAt,author" + (",body,mergeCommit" if bodies else "")
    return gh_json(["pr", "list", "--repo", REPO, "--state", "merged", "--limit", "500",
                    "--json", fields])


def merged_work_index(prs=None):
    """{issue number: [(PR number, merge date), ...]} over recent human-authored merges."""
    prs = merged_prs() if prs is None else prs
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


# ---- discovery (§1 Discovery, §3.10) ------------------------------------------
# The bugs a task will find are where much of its value lands: #1694 (renderer
# ownership) fixed 30 in its own PR and filed 6, a typical small fix finds 0-5. No
# axis scored that. The term is computed, never stored, from three inputs: each merged
# task PR's "Found and fixed" bullets and "Filed, not fixed" issues (its yield), the
# files its merge commit changed (its areas), and the `areas:` an open issue declares.
#
# AREAS is the fixed vocabulary of `areas:`. AREA_RULES maps a changed file to one of
# them: lowercased path, first match wins, None means the file says nothing about an
# area (docs, Markdown). Order matters: a groom shader is Groom, not Renderer.
AREAS = ("Renderer", "Groom", "Vulkan/RHI", "Scene/ECS", "Serialization", "Physics",
         "Scripting", "Animation", "Audio", "Networking", "Assets", "Editor", "Gameplay",
         "Core", "Tests/CI", "Tooling")
AREA_RULES = tuple((re.compile(rx), area) for rx, area in (
    (r"^docs/|\.md$|^experiments/|^\.git(?:ignore|attributes|modules)$|^\.coderabbit|"
     r"^\.markdownlint|/tests/visual/", None),
    (r"groom|/breeds/|/fur(?:/|coat)", "Groom"),
    (r"vulkan|/rhi/|spirv|lavapipe", "Vulkan/RHI"),
    (r"^\.github/|^cmake/|cmakelists\.txt$|cmakepresets|vcpkg|sonar-project|^\.clang", "Tests/CI"),
    (r"^scripts/|^\.claude/|^\.agents/|^oloctl/|^tools/|^\.pre-commit|^\.vscode/", "Tooling"),
    (r"physics|jolt|box2d|collider|/cloth", "Physics"),
    (r"scripting|scriptcore|\.cs$|(?<![a-z])lua", "Scripting"),  # not "evaluator"
    (r"serializ|savegame|/save|assetpack", "Serialization"),
    (r"\.oar$|^oloeditor/sandboxproject/|^oloeditor/assets/(?!shaders/)|^assets/", "Assets"),
    (r"/scene/|component|/ecs", "Scene/ECS"),
    (r"anim(?!al)|/skin|morph", "Animation"),
    (r"audio|sound", "Audio"),
    (r"network|/server/|^oloserver/|replicat|rollback", "Networking"),
    (r"/renderer/|shaders?/|opengl|/atmosphere|/terrain|/particle|/fluid|/precipitation|/snow|"
     r"/wind/|/video|render|visual|foliage|light|shadow|/rt/|raytrac|upscal|texture|material", "Renderer"),
    (r"/asset|^oloeditor/assets/|^assets/|volumecook|sandboxproject", "Assets"),
    (r"^oloeditor/|imgui|mcp|^oloruntime/", "Editor"),
    (r"/ai/|gameplay|dialogue|navigation|/ui/|cinematic|tilemap|locali[sz]|accessib|(?<![a-z])quest|inventory",
     "Gameplay"),
    (r"^oloengine/tests/", "Tests/CI"),
    (r"^oloengine/src/", "Core"),
))

# A tag uses the §1 Fibonacci rungs, so `[S8]` reads like a `stability: 8` score.
SEVERITY_TAG_RE = re.compile(r"\[S(13|[12358])\]")
# The keyword heuristic for an untagged "Found and fixed" bullet (every bullet before the
# task loop started writing tags). Its answer is marked *estimated* wherever it is shown.
SEVERITY_HIGH = 8
SEVERITY_LOW = 1
SEVERITY_DEFAULT = 3
SEVERITY_HIGH_RE = re.compile(
    r"crash|corrupt|data loss|lost (?:data|work|edits)|wrong frame|\bleaks?\b|\bleaked\b|"
    r"\brace\b|\braces\b|race condition|deadlock|use[- ]after[- ]free|dangling|"
    r"undefined behaviou?r|out of bounds|past the end", re.IGNORECASE)
SEVERITY_LOW_RE = re.compile(
    r"^(?:`?[0-9a-f]{7,12}`?:?\s+)?(?:docs|test|style|chore)(?:\(|:)|test-only|\btypos?\b|"
    r"log noise|log spam|\bwording\b|fix\(docs\)", re.IGNORECASE)

# The two sections the task-loop PR template ends with (task-loop.md, Phase 4). Earlier
# PRs used free headings ("Two bugs found on the way"); those PRs are left out of the
# history as *no data*, never counted as zero yield.
FOUND_HEAD_RE = re.compile(r"^#{2,4}\s*Found and fixed\b[^\n]*$", re.MULTILINE | re.IGNORECASE)
FILED_HEAD_RE = re.compile(r"^#{2,4}\s*Filed, not fixed\b[^\n]*$", re.MULTILINE | re.IGNORECASE)
NEXT_HEAD_RE = re.compile(r"^#{1,4}\s", re.MULTILINE)
BULLET_RE = re.compile(r"^[-*]\s+", re.MULTILINE)
FILED_NUM_RE = re.compile(r"^[-*]\s+(?:\*\*)?\[?#(\d+)")  # `- #N`, `- **#N`, `- [#N](url)`

# The model (§1 Discovery). Constants were set against the 101 template PRs merged
# 2026-09-05..2026-11-20 (issue-scoring-details.md, "What the discovery term was set
# on"); `yield` prints the predicted-vs-actual check they must keep passing.
#
# Yield grows with effort, but far slower than linearly: a log-log fit over the 79
# PRs with a known effort gave yield = 10.2 x effort^0.39 (mean 14 at effort 1, 49 at
# effort 8). Effort enters as effort^EFFORT_EXPONENT, so a large item is not credited
# eight times a small one's bugs. `yield` prints the current fit beside the constant.
EFFORT_EXPONENT = 0.5
DEFAULT_PR_EFFORT = 2     # a PR whose delivered issues are unknown or still open (a slice)
RATE_HALF_LIFE_DAYS = 60  # how fast an old PR stops counting toward an area's yield rate
PRIOR_EFFORT = 3          # effort units of all-area rate every area starts with (shrinkage)
SWEEP_YIELD = 20          # severity points of fresh harvest that halve an area's raw staleness
RECOVER_DAYS = 30         # a sweep's depletion halves every this many days...
RECOVER_CHURN = 10        # ...and every this many later PRs' worth of changes to the area
STALE_MAX = 2.0           # the most a fresh area's expectation exceeds its measured rate
# Expected severity points per point of Stability. An effort-3 issue in an average area
# expects about 20 points (the history's mean for effort 3 was 17: six bugs of the
# default severity), which this turns into +2, one rung step on the other axes. Expected
# bugs are a forecast with a measured rank correlation near 0.29, so they are not
# credited one-for-one like a known bug.
DISC_SCALE = 10
DISC_CAP = 5              # the most Stability discovery can add (§1): never beats a crash bug


def area_of(path):
    p = path.replace("\\", "/").lower()
    for rx, area in AREA_RULES:
        if rx.search(p):
            return area
    return None


def area_shares(paths):
    """{area: share of the PR's area-mapped directories}; {} when nothing maps.

    The unit is a distinct directory, not a file: 24 terrain layers added in one
    folder are one place the PR worked, and counting them as 24 drowned the code
    change beside them."""
    dirs = {}
    for path in paths:
        area = area_of(path)
        if area:
            dirs.setdefault(area, set()).add(path.replace("\\", "/").rpartition("/")[0])
    counts = {a: len(ds) for a, ds in dirs.items()}
    total = sum(counts.values())
    return {a: c / total for a, c in counts.items()} if total else {}


def _git(args, stdin=None):
    return subprocess.run(["git", "-c", "core.quotepath=off", *args], input=stdin,
                          capture_output=True, text=True, encoding="utf-8", check=False,
                          cwd=Path(__file__).resolve().parent.parent)


def merge_paths(oids):
    """{oid: files the merge commit changed against its first parent}. An oid that is
    not in the local clone maps to None (then `git fetch origin`; build_history leaves
    the PR out and `rank` / `yield` count it). One `git log` for all of them, not a
    `git diff` per PR: that cost about 5 s per `rank`."""
    oids = [o for o in dict.fromkeys(oids) if o and re.fullmatch(r"[0-9a-f]{40}", o)]
    check = _git(["cat-file", "--batch-check"], stdin="\n".join(oids) + "\n")
    present = [line.split()[0] for line in check.stdout.splitlines()
               if line.endswith(("commit", "commit\r")) or " commit " in line]
    out = {o: None for o in oids}
    if not present:
        return out
    log = _git(["log", "--no-walk=unsorted", "-m", "--first-parent", "--name-only",
                "--format=%x00%H", *present])
    if log.returncode != 0:
        return out
    for chunk in log.stdout.split("\0")[1:]:
        lines = [line for line in chunk.splitlines() if line.strip()]
        if lines:
            out[lines[0].strip()] = lines[1:]
    return out


# A fence closes only on a run of the same character at least as long as its opener
# (CommonMark), so ```` around an example that itself holds ``` stays one block.
FENCE_BLOCK_RE = re.compile(r"^[ \t]*(?:(`{3,})|(~{3,}))[^\n]*\n.*?^[ \t]*(?(1)\1`*|\2~*)[ \t]*$",
                            re.MULTILINE | re.DOTALL)


def _section(body, head_re):
    """The text under the first heading `head_re` matches, up to the next heading.
    Fenced code is blanked first: a PR that quotes the template in a code block must
    not have the example read as its findings."""
    body = FENCE_BLOCK_RE.sub("", body)
    m = head_re.search(body)
    if not m:
        return None
    nxt = NEXT_HEAD_RE.search(body, m.end())
    return body[m.end():nxt.start() if nxt else len(body)]


def _bullets(section):
    """Top-level bullets, each with its continuation lines. Nested bullets belong to
    their parent (one finding, several details). A blank line followed by an
    unindented paragraph ends the last bullet."""
    starts = [m.start() for m in BULLET_RE.finditer(section)]
    out = []
    for i, s in enumerate(starts):
        chunk = section[s:starts[i + 1] if i + 1 < len(starts) else len(section)]
        chunk = re.split(r"\n\s*\n(?=\S)", chunk, maxsplit=1)[0]
        out.append(chunk.strip())
    return out


NESTED_BULLET_RE = re.compile(r"^([ \t]+)[-*]\s+", re.MULTILINE)


def _findings(bullet):
    """The bugs one top-level bullet reports. A bullet whose first line ends with `:`
    and carries nested bullets introduces a list (#1694's `fix(instancing), seven
    items:`), and each direct child bullet is one bug; deeper bullets are that child's
    evidence. Otherwise nested bullets are details of the one bug (#1694's #1692 entry
    lists its evidence that way)."""
    first = bullet.splitlines()[0].rstrip().rstrip("*").rstrip()
    nested = list(NESTED_BULLET_RE.finditer(bullet))
    if not first.endswith(":") or not nested:
        return [bullet]
    depth = min(len(m.group(1).expandtabs(4)) for m in nested)
    starts = [m.start() for m in nested if len(m.group(1).expandtabs(4)) == depth]
    return [bullet[s:starts[i + 1] if i + 1 < len(starts) else len(bullet)].strip()
            for i, s in enumerate(starts)]


def heuristic_severity(text):
    text = re.sub(r"^[-*]\s+(?:\*\*)?", "", text.strip())  # the bullet marker, so `^docs(` can match
    if SEVERITY_HIGH_RE.search(text):
        return SEVERITY_HIGH
    if SEVERITY_LOW_RE.search(text):
        return SEVERITY_LOW
    return SEVERITY_DEFAULT


def bullet_severity(text):
    """(severity, estimated). A `[S8]` tag is the task loop's answer; without one the
    keyword heuristic guesses, and the guess is marked."""
    m = SEVERITY_TAG_RE.search(text)
    if m:
        return int(m.group(1)), False
    return heuristic_severity(text), True


def parse_yield(body):
    """{"fixed": [(severity, estimated, text)], "filed": [issue numbers]} from a PR
    body, or None when it has neither template section (no data, not zero)."""
    body = body or ""
    found, filed = _section(body, FOUND_HEAD_RE), _section(body, FILED_HEAD_RE)
    if found is None and filed is None:
        return None
    fixed = [(*bullet_severity(f), f) for b in _bullets(found or "") for f in _findings(b)]
    nums = []
    for b in _bullets(filed or ""):
        m = FILED_NUM_RE.match(b)
        if m:
            nums.append(int(m.group(1)))
    return {"fixed": fixed, "filed": nums}


def issue_details(numbers):
    """({number: (title, body, state, closedAt)}, unread) for the issues given, by
    batched GraphQL like resolve_blocker_states. A PR number is absent from the dict.
    `unread` counts the numbers with no answer (a failed chunk: offline, rate limit,
    timeout; or a null alias in a partial answer): their PRs fall back to the default effort and a guessed severity,
    so callers must say so rather than print a quietly different rank."""
    nums = sorted({n for n in numbers if isinstance(n, int) and n > 0})
    found, unread = {}, 0
    for i in range(0, len(nums), _BLOCKER_CHUNK):
        chunk = nums[i:i + _BLOCKER_CHUNK]
        fields = "\n".join(f"  i{n}: issueOrPullRequest(number: {n}) "
                           f"{{ ... on Issue {{ title body state closedAt }} }}" for n in chunk)
        query = f'query {{ repository(owner: "{REPO_OWNER}", name: "{REPO_NAME}") {{\n{fields}\n}} }}'
        try:
            out = subprocess.run(["gh", "api", "graphql", "-f", f"query={query}"],
                                 capture_output=True, text=True, encoding="utf-8",
                                 check=False, timeout=_BLOCKER_TIMEOUT_S)
            repo = (json.loads(out.stdout) or {}).get("data", {}).get("repository") or {}
        except (subprocess.TimeoutExpired, ValueError, AttributeError):
            repo = {}
        for n in chunk:
            # A PR number answers `{}` (no Issue fragment matched); a missing or null
            # alias was not read: a failed chunk, or a partial answer with an error.
            if repo.get(f"i{n}") is None:
                unread += 1
                continue
            node = repo[f"i{n}"]
            if "title" in node:
                found[n] = (node["title"], node.get("body") or "", node.get("state") or "",
                            node.get("closedAt") or "")
    return found, unread


def history_numbers(prs):
    """Every issue number the history needs details for: delivered and filed."""
    nums = set()
    for pr in prs:
        nums |= delivered_numbers(pr.get("headRefName"), pr.get("title"))
        parsed = parse_yield(pr.get("body"))
        if parsed:
            nums |= set(parsed["filed"])
    return nums


def build_history(prs, details, paths_of):
    """The yield history: one record per merged task PR with template sections.

    `details` is issue_details()[0] over every delivered and filed number; `paths_of`
    maps a merge-commit oid to its changed files (merge_paths().get). A filed issue is
    credited once, to the first merged PR that lists it (a later PR often re-lists an
    issue it only saw). Returns (records oldest first, [(skipped PR, "local" | "no area")]).
    """
    records, skipped, credited = [], [], set()
    task = [pr for pr in prs if (pr.get("headRefName") or "").startswith("feature/")
            and not pr.get("author", {}).get("is_bot") and pr.get("mergedAt")]
    task.sort(key=lambda p: p["mergedAt"])
    # An issue's effort sizes only the PR that closed it: the last PR naming it that
    # merged by the day after it closed. An umbrella delivered in slices (#607, effort
    # 13) would otherwise re-size every earlier slice to the epic's effort the day the
    # last one closes it, and a follow-up naming a closed issue would be charged again.
    closer = {}
    for pr in task:
        for n in delivered_numbers(pr.get("headRefName"), pr.get("title")):
            _, _, state, closed = details.get(n, ("", "", "", ""))
            if (state == CLOSED and closed
                    and gh_day(pr["mergedAt"]) <= gh_day(closed) + dt.timedelta(days=1)):
                closer[n] = pr["number"]
    for pr in task:
        parsed = parse_yield(pr.get("body"))
        if parsed is None:
            continue
        paths = paths_of((pr.get("mergeCommit") or {}).get("oid"))
        if paths is None:
            skipped.append((pr["number"], "local"))
            continue
        shares = area_shares(paths)
        if not shares:  # docs-only: no area to credit
            skipped.append((pr["number"], "no area"))
            continue
        items = [(sev, est, "fixed: " + text.splitlines()[0][:70]) for sev, est, text in parsed["fixed"]]
        for n in parsed["filed"]:
            if n in credited:
                continue
            credited.add(n)
            title, body, _, _ = details.get(n, ("", "", "", ""))
            d = parse_block(body)
            if d and isinstance(d.get("stability"), (int, float)):
                items.append((d["stability"], False, f"filed #{n}: {title[:60]}"))
            else:
                items.append((heuristic_severity(title), True, f"filed #{n}: {title[:60]}"))
        # Only the delivered issues this PR closed say how big it was. A slice of an
        # issue still open, or closed later by another PR, carries the epic's effort:
        # slices of #607 (effort 13) averaged 9 points of yield, not the 30+ it implies.
        efforts = []
        for n in delivered_numbers(pr.get("headRefName"), pr.get("title")):
            d = parse_block(details.get(n, ("", "", "", ""))[1])
            if (closer.get(n) == pr["number"] and d
                    and isinstance(d.get("effort"), (int, float)) and d["effort"] > 0):
                efforts.append(d["effort"])
        records.append({"pr": pr["number"], "day": gh_day(pr["mergedAt"]), "areas": shares,
                        "effort": min(sum(efforts), 13) if efforts else DEFAULT_PR_EFFORT,
                        "effort_known": bool(efforts), "items": items,
                        "yield": sum(sev for sev, _, _ in items)})
    return records, skipped


def _raw_stale(past, area, as_of):
    """(raw staleness, depletion) of `area` on `as_of` from the records in `past`.

    Depletion is the yield already harvested from the area: each PR's share of its
    yield, halving every RECOVER_DAYS and every RECOVER_CHURN later PRs' worth of
    changes to the area. A sweep finds what is there; time and new code refill it."""
    depletion, churn_after = 0.0, 0.0
    for r in reversed(past):  # newest first, so churn_after counts the later PRs
        share = r["areas"].get(area, 0)
        if share:
            depletion += (r["yield"] * share * 0.5 ** ((as_of - r["day"]).days / RECOVER_DAYS)
                          * 0.5 ** (churn_after / RECOVER_CHURN))
            churn_after += share
    return 1.0 / (1.0 + depletion / SWEEP_YIELD), depletion


def _raw_stale_at_merge(history, r):
    """{area: raw staleness when `r` merged} for r's areas, cached on the record (keyed
    by the constants, so a calibration that changes them recomputes)."""
    key = (SWEEP_YIELD, RECOVER_DAYS, RECOVER_CHURN)
    if r.get("_raw_at", (None,))[0] != key:
        past = [q for q in history if q["day"] < r["day"]]
        r["_raw_at"] = (key, {a: _raw_stale(past, a, r["day"])[0] for a in r["areas"]})
    return r["_raw_at"][1]


def area_model(history, as_of):
    """{area: {"rate", "depletion", "stale", "factor", ...}} from the PRs merged before `as_of`.

    rate:  recent severity-weighted yield per unit of sized effort (effort^EFFORT_EXPONENT)
           in the area, each PR weighted by 0.5^(age / RATE_HALF_LIFE_DAYS) and split
           across its areas by share, shrunk toward the all-area rate by PRIOR_EFFORT
           units so a thinly-worked area does not swing on one PR.
    stale: the area's raw staleness now, 1 / (1 + depletion / SWEEP_YIELD) (see
           _raw_stale), divided by its raw staleness when the PRs behind its rate merged,
           weighted like the rate and capped at STALE_MAX. The rate was measured on a
           partly harvested area; this asks how much more or less harvested it is today.
           A sweep drops it, quiet months raise it.
    factor = rate x stale: expected severity points per unit of sized effort.
    """
    past = [r for r in history if r["day"] < as_of]
    weights = [0.5 ** ((as_of - r["day"]).days / RATE_HALF_LIFE_DAYS) for r in past]
    sized = [r["effort"] ** EFFORT_EXPONENT for r in past]
    at_merge = [_raw_stale_at_merge(history, r) for r in past]
    total_y = sum(w * r["yield"] for w, r in zip(weights, past))
    total_e = sum(w * e for w, e in zip(weights, sized))
    global_rate = total_y / total_e if total_e else 0.0
    seen = sum(w * se * sum(r["areas"][a] * raw for a, raw in at.items())
               for w, se, r, at in zip(weights, sized, past, at_merge))
    global_seen = seen / total_e if total_e else 1.0
    model = {}
    for area in AREAS:
        y = e = seen = 0.0
        for w, se, r, at in zip(weights, sized, past, at_merge):
            share = r["areas"].get(area, 0)
            if share:
                y += w * r["yield"] * share
                e += w * se * share
                seen += w * se * share * at[area]
        rate = (y + PRIOR_EFFORT * global_rate) / (e + PRIOR_EFFORT)
        typical = (seen + PRIOR_EFFORT * global_seen) / (e + PRIOR_EFFORT)
        raw, depletion = _raw_stale(past, area, as_of)
        stale = min(STALE_MAX, raw / typical) if typical else 1.0
        model[area] = {"rate": rate, "depletion": depletion, "work": e, "raw_stale": raw,
                       "stale": stale, "factor": rate * stale}
    model["_global"] = {"rate": global_rate, "prs": len(past)}
    return model


def expected_yield(effort, areas, model):
    """Severity points an issue is expected to find: effort^EFFORT_EXPONENT x the mean
    factor of its areas. A mean, not a sum: listing more areas spreads the same effort
    over them (in the history, the number of areas a PR mainly worked in, each 20 % or
    more of its directories, did not predict its yield; effort did)."""
    known = [model[a]["factor"] for a in dict.fromkeys(areas) if a in AREAS]  # a repeat counts once
    return (max(effort, 0) ** EFFORT_EXPONENT) * sum(known) / len(known) if known else 0.0


def calibrated_model(history, as_of):
    """area_model() with every factor scaled by the history's level: sum of actual
    yield / sum of predicted, each PR predicted from the PRs merged before it.

    The staleness ratio keeps the order right (its rank correlation with the actual
    yield is what the constants were chosen on) but not the level: uncalibrated, the
    2026-11-21 history predicted about half of what was found. The level is computed
    from the same inputs on every run, never stored."""
    model = area_model(history, as_of)
    rows = [(r, p) for r, p in predictions([r for r in history if r["day"] < as_of])
            if p is not None]
    predicted, actual = sum(p for _, p in rows), sum(r["yield"] for r, _ in rows)
    level = actual / predicted if predicted and actual else 1.0
    for a in AREAS:
        model[a]["factor"] *= level
    model["_global"]["level"] = level
    return model


def predict_pr(record, model):
    """What the model expected of a merged PR, from its effort and its actual areas
    (a closed issue has no `areas:`; the diff stands in for the declaration)."""
    return (record["effort"] ** EFFORT_EXPONENT) * sum(
        share * model[a]["factor"] for a, share in record["areas"].items())


def discovery_points(x):
    """Expected severity points -> Stability points, capped, to one decimal (§1)."""
    return round(min(float(DISC_CAP), max(x, 0.0) / DISC_SCALE), 1)


def issue_areas(d):
    """The `areas:` list of a block, as written (a bare string becomes a list of one)."""
    raw = d.get("areas")
    if raw in (None, ""):
        return []
    return [str(a).strip() for a in (raw if isinstance(raw, list) else [raw]) if str(a).strip()]


# ---- Tier-1 and large-item slots ---------------------------------------------
class StartedTask(tuple):
    """One started task: (PR, is_tier1, is_large, is_open). The last two default to
    False so a slot test can state only what it is about."""

    def __new__(cls, pr, tier1, large=False, is_open=False):
        return super().__new__(cls, (pr, tier1, large, is_open))

    pr = property(lambda self: self[0])
    tier1 = property(lambda self: self[1])
    large = property(lambda self: self[2])
    is_open = property(lambda self: self[3])


def is_large_issue(body):
    """An issue whose olo-score effort is LARGE_EFFORT or more."""
    d = parse_block(body)
    return bool(d) and d.get("effort", 0) >= LARGE_EFFORT


def _pull_requests(query):
    out = subprocess.run(["gh", "api", "graphql", "-f", f"query={query}"],
                         capture_output=True, text=True, encoding="utf-8",
                         check=False, timeout=_BLOCKER_TIMEOUT_S)
    return json.loads(out.stdout)["data"]["repository"]["pullRequests"]


def open_large_prs():
    """Every open feature/* PR that closes a large issue, over all pages of open PRs.

    Not taken from the history page: a large PR can stay open while 40 newer PRs
    are created, and the slot must still see it."""
    found, cursor = [], None
    while True:
        after = f', after: "{cursor}"' if cursor else ""
        page = _pull_requests(
            f'query {{ repository(owner: "{REPO_OWNER}", name: "{REPO_NAME}") {{ '
            f'pullRequests(first: 100, states: [OPEN]{after}) {{ '
            'pageInfo { hasNextPage endCursor } nodes { number headRefName '
            'closingIssuesReferences(first: 10) { nodes { body } } } } } }')
        for pr in page["nodes"]:
            if ((pr.get("headRefName") or "").startswith("feature/")
                    and any(is_large_issue(i.get("body"))
                            for i in pr["closingIssuesReferences"]["nodes"])):
                found.append(pr["number"])
        if not page["pageInfo"]["hasNextPage"]:
            return found
        cursor = page["pageInfo"]["endCursor"]


def recent_started_tasks():
    """(last, open_large): the last TIER1_EVERY feature/* PRs (open or merged) as
    [StartedTask], and the numbers of every open feature/* PR that closes a large
    issue (open_large_prs). None when either could not be read; `rank` then says so
    and follows the plain order instead of guessing that a slot is due.
    """
    query = (f'query {{ repository(owner: "{REPO_OWNER}", name: "{REPO_NAME}") {{ '
             'pullRequests(first: 40, states: [OPEN, MERGED], '
             'orderBy: {field: CREATED_AT, direction: DESC}) { nodes { number headRefName '
             'labels(first: 20) { nodes { name } } '
             'state closingIssuesReferences(first: 10) { nodes { body labels(first: 20) { nodes { name } } } } '
             '} } } }')
    try:
        nodes = _pull_requests(query)["nodes"]
        open_large = open_large_prs()
    except (subprocess.TimeoutExpired, ValueError, KeyError, TypeError):
        return None
    started = []
    for pr in nodes:
        if not (pr.get("headRefName") or "").startswith("feature/"):
            continue
        labels = {l["name"] for l in pr["labels"]["nodes"]}
        issues = pr["closingIssuesReferences"]["nodes"]
        for issue in issues:
            labels |= {l["name"] for l in issue["labels"]["nodes"]}
        started.append(StartedTask(pr["number"], bool(labels & TIER1_LABELS),
                                   any(is_large_issue(i.get("body")) for i in issues),
                                   pr.get("state") == "OPEN"))
        if len(started) == TIER1_EVERY:
            break
    return started, open_large


def tier1_due(started):
    """True when none of the last TIER1_EVERY started tasks was Tier 1."""
    return len(started) == TIER1_EVERY and not any(t[1] for t in started)


def large_due(started, open_large=()):
    """True when none of the last TIER1_EVERY started tasks closed a large issue and
    no open feature/* PR closes one: a large item takes a week or more, so one still
    in review keeps the slot filled after three small tasks have started beside it."""
    return (len(started) == TIER1_EVERY and not any(StartedTask(*t).large for t in started)
            and not open_large)


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
        try:
            return json.loads(re.sub(r'"(?:\\.|[^"\\])*"|#(\d+)', lambda m: m.group(1) or m.group(0), v))
        except ValueError:
            # A list of bare names (`[Renderer, Vulkan/RHI]`), as people write one. json.loads
            # raised on it, and one such block took `rank` and `lint` down for every issue.
            return [x.strip().strip('"').strip("'") for x in v.strip("[]").split(",") if x.strip()]
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
    if "areas" in d:
        # JSON, not bare names: every checkout running an older copy of this file parses
        # the block too, and its `_coerce` raises on `[Renderer]` (the first backfill wrote
        # bare names and took `rank` down on master until it was rewritten).
        lines.append(f"areas: {json.dumps(issue_areas(d))}")
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
               "score = confidence × (capability + craft + stability + decay + discovery) / effort, "
               "derived by the picker; discovery is computed from `areas` and merged-PR yield.</sub>")
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
    """Confidence x (CoD + discovery) / Effort, plus a goal issue's lift (§3.10).

    `_disc` and `_goal_lift` are transient: apply_discovery sets them on a ranked
    block and nothing writes them back. Without them this is the plain §3 score."""
    eff = d.get("effort", 0) or 0
    if not eff:
        return 0.0
    return round(d.get("confidence", 1.0) * (cod(d) + d.get("_disc", 0)) / eff
                 + d.get("_goal_lift", 0.0), 2)


GOAL_LIFT_MARGIN = 0.02  # survives rounding both scores to 2 decimals (worst case 0.01)


def discovery_bonus(d):
    """What a block's discovery adds to its score."""
    eff = d.get("effort", 0) or 0
    return d.get("confidence", 1.0) * d.get("_disc", 0) / eff if eff else 0.0


def apply_discovery(blocks, model, star):
    """Set `_disc` on every block from its effort and `areas:`, and lift goal issues.

    A block with no known area gets 0 and is flagged `areas?`. Once a north star is set,
    a goal issue is lifted (`_goal_lift`) to the largest bonus among the non-goal issues
    that scored at or below it without discovery: discovery may reorder the rest of the
    queue, but it never moves a non-goal issue past a goal issue that outranked it
    (§3.10). The lift is no larger than that, so the goal still works through Decay
    (§3.9). Discovery favours small items (yield per unit of effort falls with effort),
    so without the lift a large goal issue can sink below small issues in a fresh area.
    """
    plain = [score(d) for d in blocks]  # before `_disc` is set: the §3 score
    for d in blocks:
        areas = [a for a in issue_areas(d) if a in AREAS]
        d["_disc"] = discovery_points(expected_yield(d.get("effort", 0) or 0, areas, model))
    if star and star.get("milestone"):
        for d, s in zip(blocks, plain):
            if is_goal(d, star):
                trailing = max((discovery_bonus(o) for o, so in zip(blocks, plain)
                                if not is_goal(o, star) and so <= s), default=0.0)
                # GOAL_LIFT_MARGIN: `score` rounds to 2 decimals and ties then fall to
                # Learning/Fun, so an exact lift could still tie and lose.
                need = trailing - discovery_bonus(d)
                # No trailing bonus, nothing to undo: zero discovery adds no margin either.
                d["_goal_lift"] = (round(need + GOAL_LIFT_MARGIN, 4)
                                   if trailing > 0 and need > -GOAL_LIFT_MARGIN else 0.0)


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


def row_flags(d, states, floor, fresh=None, labels=(), star=None):
    """The `flags` column for one ranked row. `fresh` is freshness(), when known."""
    still_open, unresolved = split_blockers(d, states)
    flags = []
    if CLOSE_LABEL in labels:
        flags.append("close?")
    if star and is_goal(d, star):
        flags.append("goal")
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
    if "_disc" in d:  # discovery is on for this rank
        if d["_disc"] > 0:
            flags.append(f"disc:+{d['_disc']:g}")
        if d.get("_goal_lift", 0) > 0:
            flags.append("disc:goal")
        if not any(a in AREAS for a in issue_areas(d)):
            flags.append("areas?")
    return flags


def is_low_value(d):
    """Below the §3.5 floor. On the plain CoD: discovery is arithmetic too, and the
    floor exists to catch work that floated up on arithmetic alone."""
    return cod(d) < LOW_VALUE_COD


def bad_areas(d):
    """Names in `areas:` that are not in AREAS (a typo would silently score 0)."""
    return [a for a in issue_areas(d) if a not in AREAS]


def discovery_state(prs, today):
    """(model, history, skipped, unread) for `today` from the merged PRs (fetched with
    bodies); see build_history and issue_details."""
    # Only PRs with the template sections can enter the history: fetch details and
    # diffs for those, not for all 500 (that tripled the cost of every `rank`).
    prs = [pr for pr in prs if (pr.get("headRefName") or "").startswith("feature/")
           and not pr.get("author", {}).get("is_bot") and parse_yield(pr.get("body")) is not None]
    details, unread = issue_details(history_numbers(prs))
    paths = merge_paths((pr.get("mergeCommit") or {}).get("oid") for pr in prs)
    history, skipped = build_history(prs, details, paths.get)
    return calibrated_model(history, today), history, skipped, unread


# ---- commands ---------------------------------------------------------------
def ranked_rows(args, today, discovery=False):
    """Score every open scored issue. Returns (rows, states, excluded, unscored, disc).

    A row is (number, title, score, block, labels, freshness). `disc` is
    (model, history, skipped) when `discovery` is on, else None.
    """
    rows, excluded, unscored = [], 0, []
    issues = open_issues()
    prs = merged_prs(bodies=discovery)
    index = merged_work_index(prs)
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
        d["_milestone"] = milestone_title(it)  # transient: never written back (set re-reads)
        rows.append([it["number"], it["title"], 0.0, d, labels, fresh])
    disc = None
    if discovery:
        disc = discovery_state(prs, today)
        apply_discovery([r[3] for r in rows], disc[0], north_star())
    rows = [(n, t, score(d), d, labels, fresh) for n, t, _, d, labels, fresh in rows]
    states = resolve_blocker_states(k for r in rows for k in blocker_keys(r[3]))
    return rows, states, excluded, unscored, disc


def row_group(row, states):
    """Outermost sort key (§3): 0 Pull, 1 startable, 2 needs triage, 3 blocked.

    Needs-triage is work merged since the last score: the issue may be finished, so
    it must be looked at before it can be picked, but it is not hidden. An issue a
    sweep recommended for closure (CLOSE_LABEL) waits there for the user.
    """
    d, fresh = row[3], row[5]
    if is_blocked(d, states):
        return 3
    if fresh["merged"] or CLOSE_LABEL in row[4]:
        return 2
    return 0 if is_pull(d, states) else 1


def discovery_banner(disc):
    """Lines saying what the discovery term was computed from, and what it left out."""
    _, history, skipped, unread = disc
    lines = []
    if unread:
        lines.append(f"[!] discovery: {unread} history issue(s) could not be read from GitHub "
                     f"(offline, rate limit or timeout); their PRs use effort "
                     f"{DEFAULT_PR_EFFORT} and guessed severities, so the term is off. Re-run, "
                     f"or use --no-discovery")
    local = [n for n, why in skipped if why == "local"]
    if local:  # before the empty-history case: missing commits can be why it is empty
        lines.append(f"[!] discovery: {len(local)} PR(s) left out, merge commit not in this clone "
                     f"({', '.join(f'#{n}' for n in local[:8])}): run `git fetch origin`")
    if not history:
        why = ("none of their merge commits is in this clone" if local
               else "no merged task PR has the template sections")
        return lines + [f"[!] discovery: {why}; every issue gets +0 "
                        f"(`issue_scores.py yield` shows why)"]
    items = [it for r in history for it in r["items"]]
    lines.insert(1 if unread else 0,
                 f"discovery: {len(history)} task PR(s) merged {history[0]['day']}.."
                 f"{history[-1]['day']}, {len(items)} found bug(s), "
                 f"{sum(1 for it in items if it[1])} with an estimated severity "
                 f"(`issue_scores.py yield`; --no-discovery for the plain order)")
    return lines


def rank_key(row, states, floor):
    """The full `rank` sort key, outermost first (§3): the row group (Pull, startable,
    needs triage, blocked), then normal-value-before-low-value, then score desc, then
    the documented Learning/Fun tie-break, then issue number as a stable final key.

    Pull is deliberately OUTSIDE the low-value floor: a user-confirmed Fun >= 8 is an
    explicit human "I want this", whereas the floor exists to catch work that floated
    up on arithmetic alone (§3.5)."""
    d = row[3]
    return (row_group(row, states), floor and is_low_value(d), -row[2],
            -d.get("learning", 0), -d.get("fun", 0), row[0])


def cmd_rank(args):
    rows, states, excluded, _, disc = ranked_rows(args, dt.date.today(),
                                                  discovery=not args.no_discovery)
    floor = not args.no_low_value_floor
    if disc:
        print("\n".join(discovery_banner(disc)) + "\n")

    rows.sort(key=lambda r: rank_key(r, states, floor))
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
    star = north_star()
    for num, title, s, d, labels, fresh in rows:
        flags = " ".join(row_flags(d, states, floor, fresh, labels, star))
        print(f"{num:>5} {s:>5} {flags:<28} {title[:60]}")

    startable = [r for r in rows if row_group(r, states) <= 1]
    plain = startable[0] if startable else None
    tier1 = next((r for r in startable if r[4] & TIER1_LABELS), None)
    large = next((r for r in startable if r[3].get("effort", 0) >= LARGE_EFFORT), None)
    goal = next((r for r in startable if is_goal(r[3], star)), None)
    history = recent_started_tasks()
    started, open_large = history if history else (None, [])
    nxt, why = plain, ""
    if plain and is_pull(plain[3], states):
        why = " (Pull-override: Fun >= 8, confirmed by the user)"
    print()
    if not star["milestone"]:
        print(f"north star: NOT SET — nothing steers the backlog toward a goal "
              f"({NORTH_STAR_PATH.name}; issue-scoring.md §3.9)")
    else:
        goal_rows = [r for r in rows if is_goal(r[3], star)]
        unfiled = sum(1 for done, _, nums in star["items"] if not done and not nums)
        print(f"north star: {star['milestone']} — {len(goal_rows)} open scored issue(s), "
              f"{len([r for r in goal_rows if r in startable])} startable, "
              f"{unfiled} critical-path item(s) not filed yet")
        if goal:
            print(f"goal: #{goal[0]} — {goal[1]}")
    if tier1:
        print(f"tier-1: #{tier1[0]} — {tier1[1]}")
    if large:
        print(f"large: #{large[0]} — {large[1]} (effort {large[3].get('effort')})")
    if started is None:
        print("tier-1 slot: UNKNOWN — the PR history could not be read; `next:` follows the plain rank")
        print("large slot: UNKNOWN — the PR history could not be read")
    else:
        def tags(t):
            return "".join(f" ({x})" for x, on in (("tier 1", t.tier1), ("large", t.large)) if on)
        listed = ", ".join(f"#{t.pr}{tags(t)}" for t in started)
        due = tier1_due(started)
        print(f"tier-1 slot: {'DUE' if due else 'not due'} — last {len(started)} started task(s): {listed}")
        held = f"; open large PR(s): {', '.join(f'#{n}' for n in open_large)}" if open_large else ""
        print(f"large slot: {'DUE' if large_due(started, open_large) else 'not due'}{held}")
        if due and tier1 and plain and tier1[0] != plain[0]:
            nxt = tier1
            why = (f" (Tier-1 slot: none of the last {TIER1_EVERY} started tasks was Tier 1; "
                   f"the plain rank would pick #{plain[0]})")
        elif (not (due and tier1) and large_due(started, open_large) and large and plain
              and large[0] != plain[0]):
            # A due Tier-1 slot owns the single pick even when its candidate already
            # leads the rank; the large slot stays due and takes the next one.
            nxt = large
            why = (f" (large-item slot: none of the last {TIER1_EVERY} started tasks was large and "
                   f"no large PR is open; the plain rank would pick #{plain[0]})")
    if nxt:
        print(f"next: #{nxt[0]} — {nxt[1]}{why}")


def cmd_lint(args):
    rows, states, _, missing, _ = ranked_rows(args, dt.date.today())

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
    star = north_star()
    goal_low = [r for r in rows if is_goal(r[3], star) and r[3].get("decay", 0) < GOAL_DECAY]
    unfiled = [item for done, item, nums in star["items"] if star["milestone"] and not done and not nums]
    no_areas = [r for r in rows if not issue_areas(r[3])]
    wrong_areas = [(r, bad_areas(r[3])) for r in rows if bad_areas(r[3])]

    # Informational, never a failure: these wait on the user, not on the sweep.
    awaiting = [r for r in rows if CLOSE_LABEL in r[4]]
    if awaiting:
        print(f"{len(awaiting)} issue(s) recommended for closure, waiting for the user "
              f"(`{CLOSE_LABEL}`; they never reach `next:`):")
        for num, title, *_ in awaiting:
            print(f"  #{num} {title[:70]}")
        print()

    if not (missing or stale_edges or unresolved or merged or old or unconfirmed or goal_low
            or unfiled or no_areas or wrong_areas):
        print("all open issues have a current olo-score block with `areas:`; no stale edges, "
              "merged work or unconfirmed Pulls.")
        return
    if missing:
        print(f"{len(missing)} open issue(s) need scoring:")
        for num, title in missing:
            print(f"  #{num} {title[:70]}")
    if no_areas:
        print(f"\n{len(no_areas)} open issue(s) with no `areas:`, so the discovery term gives them "
              f"+0 (§1 Discovery). Add the areas the work will touch, from {', '.join(AREAS)}: "
              f"`issue_scores.py set N 'areas=[Renderer,Vulkan/RHI]'` (it keeps `scored:`):")
        for num, title, *_ in no_areas:
            print(f"  #{num} {title[:70]}")
    if wrong_areas:
        print(f"\n{len(wrong_areas)} open issue(s) name an area that does not exist (it scores 0); "
              f"the areas are {', '.join(AREAS)}:")
        for r, bad in wrong_areas:
            print(f"  #{r[0]} {', '.join(bad)} — {r[1][:50]}")
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
    if goal_low:
        print(f"\n{len(goal_low)} north-star issue(s) (milestone `{star['milestone']}`) score Decay "
              f"below {GOAL_DECAY}, the near-milestone rung (§3.9) — raise it with `set N decay=5` "
              f"(8 if it blocks the milestone's next demo), or move the issue out of the milestone:")
        for num, title, _, d, _, _ in goal_low:
            print(f"  #{num} decay {d.get('decay')} — {title[:60]}")
    if unfiled:
        print(f"\n{len(unfiled)} north-star critical-path item(s) with no issue — file and score "
              f"each in milestone `{star['milestone']}`, then add its `#N` to {NORTH_STAR_PATH.name}:")
        for item in unfiled:
            print(f"  - {item[:80]}")
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


def pulse_counts(prs, issues, today, days):
    """(task PRs merged, issues opened, issues closed) in the `days` before `today`.

    A task PR is a human-authored `feature/*` merge; Renovate and Dependabot are not
    work anybody chose. Issues count every human-filed issue and the bots' scored ones
    (is_backlog_item), so a red-streak issue counts and the dependency dashboard does not.
    """
    since = today - dt.timedelta(days=days)

    def within(stamp):
        return bool(stamp) and gh_day(stamp) > since

    merged = sum(1 for pr in prs if (pr.get("headRefName") or "").startswith("feature/")
                 and not pr.get("author", {}).get("is_bot") and within(pr.get("mergedAt")))
    opened = sum(1 for it in issues if within(it.get("createdAt")))
    closed = sum(1 for it in issues if within(it.get("closedAt")))
    return merged, opened, closed


PULSE_LIMIT = 1000
_PULSE_ISSUE_FIELDS = "number,author,body,labels,createdAt,closedAt,state,milestone"


def gh_list_complete(args):
    """A gh list that must be complete. Each pulse query is bounded (a date window, open
    items, one milestone), so PULSE_LIMIT rows means the bound failed: exit loudly
    rather than print a count that silently drops the rest."""
    rows = gh_json([*args, "--repo", REPO, "--limit", str(PULSE_LIMIT)])
    if len(rows) >= PULSE_LIMIT:
        sys.exit(f"pulse: `gh {' '.join(args[:3])}` returned {len(rows)} rows, its full limit; "
                 f"the count would be truncated, so nothing is printed")
    return rows


def cmd_pulse(args):
    today = dt.date.today()
    since = (today - dt.timedelta(days=28)).isoformat()
    prs = gh_list_complete(["pr", "list", "--state", "merged", "--search", f"merged:>={since}",
                            "--json", "number,headRefName,mergedAt,author"])
    in_flight = gh_list_complete(["pr", "list", "--state", "open", "--json", "number,headRefName"])
    by_number = {}
    for query in (["--state", "all", "--search", f"created:>={since}"],
                  ["--state", "closed", "--search", f"closed:>={since}"],
                  ["--state", "open"]):
        for it in gh_list_complete(["issue", "list", *query, "--json", _PULSE_ISSUE_FIELDS]):
            by_number[it["number"]] = it
    issues = [it for it in by_number.values() if is_backlog_item(it)]
    print("window   task PRs merged   issues opened   issues closed   net backlog")
    for days in (7, 28):
        merged, opened, closed = pulse_counts(prs, issues, today, days)
        print(f"{days:>3} d    {merged:>15}   {opened:>13}   {closed:>13}   {opened - closed:>+11}")
    open_now = [it for it in issues if it.get("state") == "OPEN"]
    feature = [pr for pr in in_flight if (pr.get("headRefName") or "").startswith("feature/")]
    listed = ", ".join("#" + str(pr["number"]) for pr in feature)
    print(f"\nopen backlog: {len(open_now)} issue(s); open feature PRs: {len(feature)}"
          + (f" ({listed})" if feature else ""))
    star = north_star()
    if not star["milestone"]:
        print("north star: NOT SET (docs/process/north-star.md)")
        return
    goal = [it for it in gh_list_complete(["issue", "list", "--state", "all", "--milestone",
                                           star["milestone"], "--json", _PULSE_ISSUE_FIELDS])
            if is_backlog_item(it)]
    done = sum(1 for it in goal if it.get("state") != "OPEN")
    items_done = sum(1 for d, _, _ in star["items"] if d)
    print(f"north star: {star['milestone']} — milestone issues {done}/{len(goal)} closed; "
          f"critical-path items {items_done}/{len(star['items'])} ticked")


def _ranks(values):
    """Average ranks (ties share their mean rank), for spearman()."""
    order = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2
        i = j + 1
    return ranks


def spearman(a, b):
    """Rank correlation, or None with fewer than three pairs or a constant side."""
    if len(a) < 3:
        return None
    ra, rb = _ranks(a), _ranks(b)
    ma, mb = sum(ra) / len(ra), sum(rb) / len(rb)
    va = sum((x - ma) ** 2 for x in ra)
    vb = sum((y - mb) ** 2 for y in rb)
    if not va or not vb:
        return None
    return sum((x - ma) * (y - mb) for x, y in zip(ra, rb)) / (va * vb) ** 0.5


def effort_fit(history):
    """(k, exponent, n): least squares of log yield on log effort over the PRs whose
    effort is known and whose yield is positive, i.e. yield = k x effort^exponent."""
    pts = [(math.log(r["effort"]), math.log(r["yield"])) for r in history
           if r["effort_known"] and r["yield"] > 0]
    if len(pts) < 3:
        return None
    mx = sum(x for x, _ in pts) / len(pts)
    my = sum(y for _, y in pts) / len(pts)
    sxx = sum((x - mx) ** 2 for x, _ in pts)
    if not sxx:
        return None
    b = sum((x - mx) * (y - my) for x, y in pts) / sxx
    return math.exp(my - b * mx), b, len(pts)


YIELD_MIN_PRIOR_PRS = 10  # a prediction from fewer earlier PRs is shown but not scored


def predictions(history):
    """[(record, predicted or None)]: each PR predicted from the PRs merged before it."""
    out = []
    for r in history:
        model = area_model(history, r["day"])
        out.append((r, predict_pr(r, model) if model["_global"]["prs"] >= YIELD_MIN_PRIOR_PRS
                    else None))
    return out


def cmd_yield(args):
    as_of = dt.date.fromisoformat(args.as_of) if args.as_of else dt.date.today()
    model, history, skipped, unread = discovery_state(merged_prs(bodies=True), as_of)
    history = [r for r in history if r["day"] < as_of]
    if unread:
        print(f"[!] {unread} history issue(s) could not be read from GitHub (offline, rate limit "
              f"or timeout); their PRs show effort {DEFAULT_PR_EFFORT} and guessed severities. "
              f"Re-run before trusting these numbers.\n")
    if args.as_of:
        # Only the PRs are cut at the date. Issue metadata (a filed issue's stability, a
        # delivered issue's effort and closing date) is read as it is today, so a later
        # re-score can change a dated report.
        print(f"[as of {as_of}: PRs merged before it; issue metadata read as of today]\n")
    if args.pr:
        rec = next((r for r in history if r["pr"] == args.pr), None)
        if rec is None:
            sys.exit(f"#{args.pr} is not in the yield history (not a merged feature/* PR with "
                     f"the template sections, or merged on/after {as_of})")
        print(f"#{rec['pr']} merged {rec['day']}, effort {rec['effort']}"
              f"{'' if rec['effort_known'] else ' (unknown: default)'}, yield {rec['yield']:g}")
        print("areas: " + ", ".join(f"{a} {s:.0%}" for a, s in
                                    sorted(rec["areas"].items(), key=lambda kv: -kv[1])))
        for sev, est, label in rec["items"]:
            print(f"  S{sev:<3g}{' (estimated)' if est else '            '} {label}")
        return
    if not history:
        sys.exit("no merged feature/* PR before that date has a `Found and fixed` or "
                 "`Filed, not fixed` section")
    items = [it for r in history for it in r["items"]]
    print(f"yield history as of {as_of}: {len(history)} task PR(s) merged {history[0]['day']}.."
          f"{history[-1]['day']}, {len(items)} found bug(s), "
          f"{sum(1 for it in items if it[1])} with an estimated severity (~)")
    for n, why in skipped:
        print(f"  left out #{n}: " + ("merge commit not in this clone (git fetch origin)"
                                      if why == "local" else "no file maps to an area"))
    fit = effort_fit(history)
    if fit:
        print(f"effort fit: yield = {fit[0]:.1f} x effort^{fit[1]:.2f} over {fit[2]} PR(s) "
              f"(the model uses effort^{EFFORT_EXPONENT:g})")
    print(f"\n{'area':<14}{'work':>6}{'rate':>7}{'harvest':>9}{'stale':>7}{'factor':>8}"
          f"{'disc@1':>8}{'disc@3':>8}{'disc@8':>8}")
    for a in sorted(AREAS, key=lambda a: -model[a]["factor"]):
        v = model[a]
        discs = "".join(f"{discovery_points(expected_yield(e, [a], model)):>+8.1f}" for e in (1, 3, 8))
        print(f"{a:<14}{v['work']:>6.1f}{v['rate']:>7.1f}{v['depletion']:>9.1f}{v['stale']:>7.2f}"
              f"{v['factor']:>8.1f}{discs}")
    print("work: recent sized effort spent in the area; harvest: yield still counted as "
          "depleting it;\nfactor: expected severity points per effort^0.5; disc@E: the Stability "
          "an issue of effort E in only that area gets.")
    level = model["_global"].get("level", 1.0)
    rows = [(r, p * level if p is not None else None) for r, p in predictions(history)]
    print(f"\n{'PR':>6} {'merged':<10} {'eff':>4} {'fixed':>5} {'filed':>5} {'actual':>7} "
          f"{'predicted':>9}  areas")
    for r, pred in reversed(rows[-args.limit:] if args.limit else rows):
        est = any(it[1] for it in r["items"])
        fixed = sum(1 for it in r["items"] if it[2].startswith("fixed"))
        top = ", ".join(a for a, _ in sorted(r["areas"].items(), key=lambda kv: -kv[1])[:2])
        print(f"#{r['pr']:<5} {r['day']!s:<10} {r['effort']:>3g}{'' if r['effort_known'] else '?'}"
              f"{' ' if r['effort_known'] else ''} {fixed:>5} {len(r['items']) - fixed:>5} "
              f"{r['yield']:>6g}{'~' if est else ' '} "
              f"{(f'{pred:9.1f}' if pred is not None else '        -')}  {top}")
    scored = [(r, p) for r, p in rows if p is not None]
    if scored:
        actual = [r["yield"] for r, _ in scored]
        rho = spearman([p for _, p in scored], actual)
        base = spearman([r["effort"] ** EFFORT_EXPONENT for r, _ in scored], actual)
        fmt = lambda x: "n/a" if x is None else f"{x:.2f}"
        print(f"\ncheck over {len(scored)} PR(s), each predicted from the PRs merged before it "
              f"(effort ? = unknown, {DEFAULT_PR_EFFORT} assumed; ~ = includes estimated severities):\n"
              f"  rank correlation, predicted vs actual: {fmt(rho)} (effort alone: {fmt(base)})\n"
              f"  level: predictions scaled x{level:.2f} (sum actual / sum predicted), so "
              f"`predicted` is in found severity points")


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
    value = _coerce(value)
    if key == "areas":
        value = issue_areas({"areas": value})
        bad = bad_areas({"areas": value})
        if bad or not value:
            sys.exit(f"areas must name one or more of {', '.join(AREAS)}; got {bad or value!r}")
    return key, value


def cmd_set(args):
    cur = gh_json(["issue", "view", str(args.number), "--repo", REPO, "--json", "body"])["body"]
    block = parse_block(cur)
    d = dict(block or {})
    keys = set()
    for text in args.fields:
        key, value = parse_assignment(text)
        d[key] = value
        keys.add(key)
    if d.get("blocked_by_external") in ([], ""):
        d.pop("blocked_by_external")
    if keys and keys <= set(UNSTAMPED_FIELDS):
        if block is None:
            sys.exit(f"#{args.number} has no olo-score block; score it first (`set "
                     f"{args.number} capability=... effort=...`), or every axis would be a default 3.")
    else:
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

_STAR = """# North star
```olo-north-star
milestone: "Game: vertical slice"   # comment
```
## Critical path
- [x] Save mid-voyage (#12)
- [ ] Quest log
- [ ] Controller input (#13, #14)
"""
# (description, text, want milestone, want [(done, issue numbers)])
NORTH_STAR_CASES = (
    ("a set milestone, with comment and quotes stripped, and its checklist", _STAR,
     "Game: vertical slice", [(True, [12]), (False, []), (False, [13, 14])]),
    ("a quoted milestone keeps its '#', the comment after the quote goes",
     '```olo-north-star\nmilestone: "Demo #1"   # first demo\n```', "Demo #1", []),
    ("an unquoted milestone ends at the comment", "```olo-north-star\nmilestone: Slice # x\n```",
     "Slice", []),
    ("an empty milestone is no goal", "```olo-north-star\nmilestone:\n```", "", []),
    ("no file at all is no goal", "", "", []),
)

_D_ = {"capability": 3, "craft": 3, "stability": 3, "decay": 1, "effort": 3, "confidence": 1.0}
_FRESH = {"merged": [], "stale": False, "age": 0, "since": None, "source": "scored"}
# (description, labels, block extras, want row_group)
GROUP_CASES = (
    ("a plain issue is startable", set(), {}, 1),
    ("a close-recommended issue waits with needs-triage", {CLOSE_LABEL}, {}, 2),
    ("a close-recommended blocked issue stays blocked", {CLOSE_LABEL}, {"blocked_by_external": "x"}, 3),
)

_T = dt.datetime(2026, 11, 20, 12)
_PULSE_PRS = [
    {"headRefName": "feature/a-1", "mergedAt": "2026-11-19T10:00:00Z", "author": {"is_bot": False}},
    {"headRefName": "feature/b-2", "mergedAt": "2026-11-01T10:00:00Z", "author": {"is_bot": False}},
    {"headRefName": "renovate/x", "mergedAt": "2026-11-19T10:00:00Z", "author": {"is_bot": True}},
]
_PULSE_ISSUES = [
    {"createdAt": "2026-11-18T00:00:00Z", "closedAt": None},
    {"createdAt": "2026-10-01T00:00:00Z", "closedAt": "2026-11-17T00:00:00Z"},
]
# (description, days, want (merged, opened, closed))
PULSE_CASES = (
    ("7 days: one task PR (Renovate excluded), one opened, one closed", 7, (1, 1, 1)),
    ("28 days adds the older task PR", 28, (2, 1, 1)),
)

# (description, started, open_large, want large_due)
LARGE_CASES = (
    ("none of the last three was large, none open", [(1, False), (2, True), (3, False)], [], True),
    ("one large task among them", [(1, False), (2, False, True), (3, False)], [], False),
    ("a large PR still open beyond the window holds the slot",
     [(1, False), (2, False), (3, False)], [4], False),
    ("too little history is never due", [(1, False)], [], False),
)


# Which open issues reach `rank` and `lint` at all.
_BOT = {"login": "app/github-actions", "is_bot": True}
_SCORED_BODY = "```olo-score\nfun: 3\n```"
BACKLOG_CASES = (
    ("a human's issue, unscored", {"author": {"login": "someone", "is_bot": False}}, True),
    ("Renovate's dashboard", {"author": {"login": "app/renovate", "is_bot": True},
                              "body": "## Detected dependencies", "labels": [{"name": "tooling"}]},
     False),
    ("a bot's scored issue", {"author": _BOT, "body": _SCORED_BODY}, True),
    ("a red-streak issue whose block was edited away",
     {"author": _BOT, "body": "red", "labels": [{"name": "ci-nightly-red"}]}, True),
)


# `rank` sorts the unresolvable edges for its banner. A block yielding both an
# int key and a str key made that sort raise TypeError before a single row was
# printed, so the guard has to cover the ordering itself, not just the flags.
SORT_CASES = (
    ("numbers numerically, malformed entries after them",
     [999, "n/a", 42, "x"], [42, 999, "n/a", "x"]),
    ("ints never sort lexicographically", [999, 4242], [999, 4242]),
)


# ---- discovery cases ----
# (path, want area). Order in AREA_RULES matters, so the cases pin the collisions.
AREA_CASES = (
    ("OloEditor/assets/shaders/GroomStrand.glsl", "Groom"),
    ("OloEngine/src/OloEngine/Renderer/Passes/ScenePass.cpp", "Renderer"),
    ("OloEngine/src/Platform/Vulkan/VulkanContext.cpp", "Vulkan/RHI"),
    ("OloEngine/src/Platform/OpenGL/OpenGLShader.cpp", "Renderer"),
    ("OloEngine/src/Platform/Windows/WindowsWindow.cpp", "Core"),
    ("OloEditor/SandboxProject/Assets/Terrain/DriftIslands/height.png", "Assets"),
    ("OloEditor/SandboxProject/Assets/AssetRegistry.oar", "Assets"),
    ("OloEditor/assets/tests/visual/Bloom_GL_Forward.png", None),
    ("docs/process/issue-scoring.md", None),
    ("OloEngine/src/OloEngine/Scene/SceneSerializer.cpp", "Serialization"),
    ("OloEngine/src/OloEngine/Scene/Components.h", "Scene/ECS"),
    ("OloEngine/tests/Physics3D/JoltBodyTest.cpp", "Physics"),
    ("OloEngine/tests/ContainerTest.cpp", "Tests/CI"),
    ("scripts/issue_scores.py", "Tooling"),
    (".github/workflows/windows.yml", "Tests/CI"),
    # Substrings that used to land in the wrong area: "evaluator" holds "lua", "furnace"
    # holds "/fur", "animal" holds "anim", "request" holds "quest".
    ("OloEngine/src/OloEngine/Animation/MorphTargets/MorphTargetEvaluator.cpp", "Animation"),
    ("OloEditor/assets/shaders/include/TerrainDepthTess_EvaluationStage.glsl", "Renderer"),
    ("OloEditor/assets/textures/hdr/furnace_white_1x1.hdr", "Assets"),
    ("OloEngine/src/OloEngine/Scene/AnimalPath.cpp", "Scene/ECS"),
    ("OloEngine/src/OloEngine/Async/RequestQueue.cpp", "Core"),
    ("OloEngine-LuaScriptCore/src/Bindings.cpp", "Scripting"),
)

# (bullet, want (severity, estimated))
SEVERITY_CASES = (
    ("- abc1234 fix(io): a save drops a component [S13]", (13, False)),
    ("- [S2] a small thing that says crash", (2, False)),     # the tag beats the heuristic
    ("- abc1234 fix(gl): a crash on resize", (SEVERITY_HIGH, True)),
    ("- the pass leaks a framebuffer per frame", (SEVERITY_HIGH, True)),
    ("- `abc1234` docs: a stale link", (SEVERITY_LOW, True)),
    ("- abc1234 test(vulkan): the tenant calls BeginFrame twice", (SEVERITY_LOW, True)),
    ("- the trace output was misaligned", (SEVERITY_DEFAULT, True)),  # "trace" is no race
    # #1698: a real rendering fix whose evidence mentions the docs is not a docs fix.
    ("- abc1234 fix(shaders): the BRDF table is read off by a texel (the doc said so)",
     (SEVERITY_DEFAULT, True)),
    ("- abc1234 fix(docs): a dead anchor", (SEVERITY_LOW, True)),
    ("- [S4] not a rung, so the heuristic answers", (SEVERITY_DEFAULT, True)),
)

_YIELD_BODY = """Intro.

## Found and fixed

Bugs in code this branch did not set out to change:

- aaa1111 fix(a): a crash on load [S8]
- **bbb2222 fix(b): a wrong label.** It read the old one.
  - detail one
  - detail two

A closing paragraph that is not a bullet.

## Filed, not fixed
- #12 something big. Too big.
- #13 (seen here, filed elsewhere)
- [#14](https://github.com/o/r/issues/14) tracks the rest.

## Review guide
- not a finding
"""
# (description, body, want parse_yield summary or None)
PARSE_YIELD_CASES = (
    ("both sections, nested details, trailing paragraph", _YIELD_BODY,
     {"fixed": [(8, False), (SEVERITY_DEFAULT, True)], "filed": [12, 13, 14]}),
    ("None. in both sections is zero yield, not no data",
     "## Found and fixed\nNone.\n\n## Filed, not fixed\nNone.\n", {"fixed": [], "filed": []}),
    ("a free-form heading is no data", "## Two bugs found on the way\n- a crash\n", None),
    ("a bullet ending in ':' introduces one bug per nested bullet",
     "## Found and fixed\n- abc1234 fix(x), two items:\n  - a crash on load\n  - a typo [S1]\n",
     {"fixed": [(SEVERITY_HIGH, True), (1, False)], "filed": []}),
    ("a child finding's own nested evidence stays with it",
     "## Found and fixed\n- fix(x), two items:\n  - a crash on load\n    - regression test passed\n"
     "  - a typo [S1]\n", {"fixed": [(SEVERITY_HIGH, True), (1, False)], "filed": []}),
    ("a template quoted in a code block is not the PR's section",
     "```markdown\n## Found and fixed\n- an example [S13]\n```\n\n## Found and fixed\n- a real one [S3]\n",
     {"fixed": [(3, False)], "filed": []}),
    ("a ```` fence is not closed by the ``` inside it",
     "````markdown\n```\n## Found and fixed\n- example [S13]\n````\n## Found and fixed\n- real [S3]\n",
     {"fixed": [(3, False)], "filed": []}),
    ("an unclosed fence hides nothing after it from a later real section",
     "## Found and fixed\n- real [S5]\n\n```\nunclosed\n",
     {"fixed": [(5, False)], "filed": []}),
)

_DD = dt.date(2026, 10, 1)


def _rec(pr, day, areas, yld, effort=2):
    return {"pr": pr, "day": day, "areas": areas, "effort": effort, "effort_known": True,
            "items": [], "yield": yld}


# A steady history in two areas, then one big sweep of area A (the #1694 shape).
_STEADY = [_rec(i, _DD + dt.timedelta(days=4 * i), {"Renderer" if i % 2 else "Physics": 1.0}, 10)
           for i in range(12)]
_SWEEP_DAY = _DD + dt.timedelta(days=50)
_SWEPT = _STEADY + [_rec(99, _SWEEP_DAY, {"Renderer": 1.0}, 100, effort=8)]


def _goal_case():
    """(order without discovery, order with it and a milestone, order with it and none).

    Synthetic, per the #1725 comment: a large goal issue (#1) and a small non-goal
    issue (#2) in a fresh area. Without discovery #1 leads by score."""
    star = {"milestone": "Slice", "items": []}
    model = {a: {"factor": 5.0} for a in AREAS}
    model["Audio"]["factor"] = 25.0
    out = []
    for discovery, ms in ((False, "Slice"), (True, "Slice"), (True, "")):
        g = {"capability": 13, "craft": 3, "stability": 3, "decay": 5, "effort": 5,
             "confidence": 1.0, "areas": ["Renderer"], "_milestone": "Slice"}
        n = {"capability": 5, "craft": 1, "stability": 1, "decay": 1, "effort": 2,
             "confidence": 1.0, "areas": ["Audio"], "_milestone": ""}
        if discovery:
            apply_discovery([g, n], model, {**star, "milestone": ms})
        out.append(_rank_order([(1, g), (2, n)]))
    return out


def _rank_order(numbered_blocks):
    """Issue numbers in `rank` order, through the real sort key (rank_key)."""
    rows = [(num, "t", score(d), d, set(), _FRESH) for num, d in numbered_blocks]
    return [r[0] for r in sorted(rows, key=lambda r: rank_key(r, {}, True))]


def _tie_case():
    """A goal issue a hair ahead of a non-goal issue (plain 4.01 vs 4.00) whose lift
    would round to the same score, with the non-goal winning the Learning tie-break.
    The lift must keep the goal first through the full sort key."""
    model = {a: {"factor": 1.0} for a in AREAS}
    model["Audio"]["factor"] = 30 / 3 ** 0.5  # effort 3 -> 30 points -> +3.0
    g = {"capability": 5, "craft": 5, "stability": 3, "decay": 2, "effort": 3,
         "confidence": 0.80102, "learning": 1, "_milestone": "Slice"}
    n = {"capability": 5, "craft": 5, "stability": 3, "decay": 2, "effort": 3,
         "confidence": 0.80098, "learning": 8, "areas": ["Audio"], "_milestone": ""}
    apply_discovery([g, n], model, {"milestone": "Slice", "items": []})
    return _rank_order([(1, g), (2, n)])


def discovery_failures():
    """The discovery term's cases (§1 Discovery). Returns a list of failure messages."""
    bad = []
    for path, want in AREA_CASES:
        if area_of(path) != want:
            bad.append(f"area: {path} want={want} got={area_of(path)}")
    # Directories, not files: 24 layers in one folder are one place worked.
    shares = area_shares([f"OloEditor/SandboxProject/Assets/L/{i}.png" for i in range(24)]
                         + ["OloEngine/src/OloEngine/Renderer/A.cpp"])
    if shares != {"Assets": 0.5, "Renderer": 0.5}:
        bad.append(f"area shares count directories: got {shares}")
    for text, want in SEVERITY_CASES:
        if bullet_severity(text) != want:
            bad.append(f"severity: {text!r} want={want} got={bullet_severity(text)}")
    for description, body, want in PARSE_YIELD_CASES:
        got = parse_yield(body)
        got = got and {"fixed": [(s, e) for s, e, _ in got["fixed"]], "filed": got["filed"]}
        if got != want:
            bad.append(f"parse_yield ({description}): want={want} got={got}")
    # History: a filed issue is credited once, to the first PR; effort only from a
    # CLOSED delivered issue; a filed issue's severity is its own `stability`.
    blk = "```olo-score\nstability: 8\neffort: 5\n```"
    prs = [{"number": 10, "headRefName": "feature/x-1", "title": "", "author": {},
            "mergedAt": "2026-10-01T00:00:00Z", "mergeCommit": {"oid": "a"},
            "body": "## Filed, not fixed\n- #7 a\n- #8 b"},
           {"number": 11, "headRefName": "feature/y-2", "title": "", "author": {},
            "mergedAt": "2026-10-02T00:00:00Z", "mergeCommit": {"oid": "b"},
            "body": "## Filed, not fixed\n- #7 a again"}]
    # #9 is an earlier slice of #1: #10 closed it, so only #10 is sized by #1's effort.
    prs.append({"number": 9, "headRefName": "feature/w-1", "title": "", "author": {},
                "mergedAt": "2026-09-20T00:00:00Z", "mergeCommit": {"oid": "c"},
                "body": "## Found and fixed\nNone."})
    details = {7: ("A crash", blk, OPEN, ""), 8: ("a typo", "", OPEN, ""),
               1: ("one", blk, CLOSED, "2026-10-01T05:00:00Z"), 2: ("two", blk, OPEN, "")}
    hist, _ = build_history(prs, details, lambda oid: ["scripts/x.py"])
    got = [(r["pr"], r["yield"], r["effort"], r["effort_known"]) for r in hist]
    want = [(9, 0, DEFAULT_PR_EFFORT, False), (10, 8 + SEVERITY_LOW, 5, True),
            (11, 0, DEFAULT_PR_EFFORT, False)]
    if got != want:
        bad.append(f"history crediting: want={want} got={got}")
    # Staleness: the sweep drops the swept area's expectation, and time restores it.
    before = area_model(_SWEPT, _SWEEP_DAY)["Renderer"]["factor"]
    after = area_model(_SWEPT, _SWEEP_DAY + dt.timedelta(days=1))["Renderer"]["factor"]
    later = area_model(_SWEPT, _SWEEP_DAY + dt.timedelta(days=150))["Renderer"]["factor"]
    other = area_model(_SWEPT, _SWEEP_DAY + dt.timedelta(days=1))["Physics"]["factor"]
    if not (after < before and later > after and other > after):
        bad.append(f"staleness: before={before:.2f} after={after:.2f} later={later:.2f} "
                   f"unswept={other:.2f}; want after < before, later > after, unswept > after")
    if discovery_points(10 ** 6) != DISC_CAP or discovery_points(-3) != 0:
        bad.append("cap: discovery must stay within [0, DISC_CAP]")
    if expected_yield(4, ["Nowhere"], {a: {"factor": 9.0} for a in AREAS}) != 0:
        bad.append("an unknown area must give no discovery, not a guess")
    plain, with_star, without_star = _goal_case()
    if plain != [1, 2] or with_star != [1, 2] or without_star != [2, 1]:
        bad.append(f"goal check: want [1, 2] / [1, 2] / [2, 1] (no discovery / milestone set / "
                   f"none), got {plain} / {with_star} / {without_star}")
    # Zero discovery everywhere (no history): the goal lift must be zero too, so a goal
    # issue at 4.00 stays behind a non-goal issue at 4.01, as without discovery.
    zg = {"capability": 5, "craft": 5, "stability": 2, "decay": 0, "effort": 3,
          "confidence": 1.0, "_milestone": "Slice"}
    zn = {"capability": 5, "craft": 5, "stability": 2, "decay": 0, "effort": 3,
          "confidence": 1.002, "_milestone": ""}
    apply_discovery([zg, zn], {a: {"factor": 0.0} for a in AREAS}, {"milestone": "Slice", "items": []})
    if zg.get("_goal_lift", 0) != 0 or _rank_order([(1, zg), (2, zn)]) != [2, 1]:
        bad.append(f"zero discovery must give no goal lift: lift={zg.get('_goal_lift')}")
    if _tie_case() != [1, 2]:
        bad.append(f"goal lift survives rounding and the Learning tie-break: got {_tie_case()}")
    if expected_yield(1, ["Audio", "Audio", "Core"], {a: {"factor": 30.0 if a == "Audio" else 0.0}
                                                      for a in AREAS}) != 15.0:
        bad.append("a repeated area must count once in the mean")
    # `areas:` as people write it parses, survives a rewrite, and keeps `scored:` intact.
    d = parse_block("```olo-score\neffort: 2\nareas: [Renderer, Vulkan/RHI]\nscored: 2026-01-01\n```")
    written = splice("", d)
    line = next((l for l in written.splitlines() if l.startswith("areas:")), "")
    try:
        strict = json.loads(line.partition(":")[2])  # how a pre-#1725 checkout reads it
    except ValueError:
        strict = None
    if issue_areas(d) != ["Renderer", "Vulkan/RHI"] or strict != ["Renderer", "Vulkan/RHI"]:
        bad.append(f"areas round trip: must be written as a JSON list, got {line!r}")
    return bad


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
    for description, it, want in BACKLOG_CASES:
        if is_backlog_item(it) != want:
            failures += 1
            print(f"SELF-TEST FAILED (backlog: {description}): want={want}", file=sys.stderr)
    for description, started, want in TIER1_CASES:
        if tier1_due(started) != want:
            failures += 1
            print(f"SELF-TEST FAILED (tier-1: {description}): want={want}", file=sys.stderr)
    for description, text, want_ms, want_items in NORTH_STAR_CASES:
        got = parse_north_star(text)
        if got["milestone"] != want_ms or [(d, n) for d, _, n in got["items"]] != want_items:
            failures += 1
            print(f"SELF-TEST FAILED (north star: {description}): got={got}", file=sys.stderr)
    for description, labels, extra, want in GROUP_CASES:
        got = row_group((1, "t", 1.0, {**_D_, **extra}, labels, _FRESH), {})
        if got != want:
            failures += 1
            print(f"SELF-TEST FAILED (group: {description}): want={want} got={got}", file=sys.stderr)
    for description, days, want in PULSE_CASES:
        got = pulse_counts(_PULSE_PRS, _PULSE_ISSUES, _T.date(), days)
        if got != want:
            failures += 1
            print(f"SELF-TEST FAILED (pulse: {description}): want={want} got={got}", file=sys.stderr)
    for description, started, open_large, want in LARGE_CASES:
        if large_due([StartedTask(*t) for t in started], open_large) != want:
            failures += 1
            print(f"SELF-TEST FAILED (large: {description}): want={want}", file=sys.stderr)
    # A rewrite must keep every field, and replace a marker-less fence in place.
    block = {"fun": 9, "fun_by": "user", "scored": "2026-10-15", "custom": "kept"}
    bare = "text\n```olo-score\nfun: 3\n```\nmore"
    rewritten = splice(bare, block)
    if parse_block(rewritten) != {**{a: 3 for a in ALL_AXES}, **block, "kano": "table-stakes",
                                  "blocked_by": [], "blocks": []} or rewritten.count("olo-score") != 1:
        failures += 1
        print(f"SELF-TEST FAILED (splice keeps fields, replaces a bare fence):\n{rewritten}",
              file=sys.stderr)
    for message in discovery_failures():
        failures += 1
        print(f"SELF-TEST FAILED (discovery: {message})", file=sys.stderr)
    return failures


# discovery_failures(): one case per row of each table, plus its seven single checks
# (directory shares, history crediting, staleness, cap, unknown area, goal, zero-lift, goal tie,
# repeated area, areas).
DISCOVERY_SINGLE_CHECKS = 10


def self_test_count():
    return (len(SELF_TEST_CASES) + len(SORT_CASES) + len(FRESHNESS_CASES)
            + len(DELIVERED_CASES) + len(BACKLOG_CASES) + len(TIER1_CASES) + len(LARGE_CASES)
            + len(NORTH_STAR_CASES) + len(GROUP_CASES) + len(PULSE_CASES) + 1
            + len(AREA_CASES) + len(SEVERITY_CASES) + len(PARSE_YIELD_CASES)
            + DISCOVERY_SINGLE_CHECKS)


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
    rp.add_argument("--no-discovery", action="store_true",
                     help="rank without the discovery term (§1 Discovery): the plain §3 order")
    rp.set_defaults(func=cmd_rank)
    yp = sub.add_parser("yield", help="the discovery term's history: per-area model, and "
                                      "predicted vs actual yield per merged task PR")
    yp.add_argument("--as-of", help="compute the model as of this day (YYYY-MM-DD)")
    yp.add_argument("--pr", type=int, help="list one PR's found bugs and their severities")
    yp.add_argument("--limit", type=int, default=0, help="show only the newest N PR rows")
    yp.set_defaults(func=cmd_yield)
    sub.add_parser("lint").set_defaults(func=cmd_lint)
    sub.add_parser("pulse", help="throughput over 7 and 28 days, work in flight, north-star progress")\
       .set_defaults(func=cmd_pulse)
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
