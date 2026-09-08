#!/usr/bin/env bash
# GitHub Actions runner hook: ACTIONS_RUNNER_HOOK_JOB_STARTED.
#
# Holds a systemd shutdown inhibitor for the duration of the job, so that
# `systemctl reboot` / `poweroff`, `shutdown`, and Cockpit refuse (or warn) while
# a job is executing on this runner. The self-hosted box was rebooted from a
# logged-in session at 2026-09-07 06:54 UTC under a running sanitizer job; the
# only reboot guard at the time was an ExecCondition on the unattended-update
# service, which a human, Cockpit, or any other path bypasses entirely.
#
# Installed to /usr/local/lib/olo-ci/ by scripts/setup-olo-ci-host.sh and pointed
# at from each runner's .env. The runner runs it with its job environment
# (RUNNER_NAME, GITHUB_REPOSITORY, GITHUB_RUN_ID, ...) and NO arguments.
#
# A JOB_STARTED hook that exits non-zero FAILS THE JOB. Nothing in here may do
# that: every failure path prints a line and exits 0, because a job that cannot
# take an inhibitor must still run -- the lock is protection, not a precondition.
#
# The lock is a `systemd-inhibit --mode=block` process whose child is
# `sleep infinity`; killing it releases the lock. Its pid is recorded per runner
# under XDG_RUNTIME_DIR so the JOB_COMPLETED hook -- and a later JOB_STARTED, if
# a runner crash skipped the completed hook -- can find it.

runner="${RUNNER_NAME:-unknown-runner}"
why="GitHub Actions job is executing on ${runner}: ${GITHUB_REPOSITORY:-?} run ${GITHUB_RUN_ID:-?}"
pidfile="${XDG_RUNTIME_DIR:-/tmp}/olo-runner-inhibit-${runner}.pid"

if ! command -v systemd-inhibit >/dev/null 2>&1; then
  echo "olo-ci job-started: systemd-inhibit not found; running without a shutdown inhibitor"
  exit 0
fi

# A stale lock from a job whose completed hook never ran (runner crash, host
# reboot mid-job) would hold the box un-rebootable forever. Release it first.
if [ -f "$pidfile" ]; then
  old=$(cat "$pidfile" 2>/dev/null || true)
  if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
    echo "olo-ci job-started: releasing a stale inhibitor (pid ${old}) left by a previous job"
    kill "$old" 2>/dev/null || true
  fi
  rm -f "$pidfile"
fi

# Probe synchronously before holding one in the background: a runner service
# user has no active seat, so taking a BLOCK inhibitor needs the polkit rule
# setup-olo-ci-host.sh installs. If that is missing, the background command
# would fail after this hook has already returned, silently.
if ! systemd-inhibit --what=shutdown --mode=block --who="olo-ci probe" --why="probe" true 2>/dev/null; then
  echo "olo-ci job-started: cannot take a shutdown inhibitor (polkit denies inhibit-block-shutdown for $(id -un)?); running without one"
  echo "olo-ci job-started: fix with  sudo bash scripts/setup-olo-ci-host.sh  on the host"
  exit 0
fi

systemd-inhibit --what=shutdown --mode=block --who="actions-runner ${runner}" --why="${why}" \
  sleep infinity >/dev/null 2>&1 &
pid=$!
echo "$pid" > "$pidfile"
echo "olo-ci job-started: shutdown inhibitor held (pid ${pid}) -- ${why}"
exit 0
