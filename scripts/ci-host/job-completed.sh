#!/usr/bin/env bash
# GitHub Actions runner hook: ACTIONS_RUNNER_HOOK_JOB_COMPLETED.
#
# Releases the shutdown inhibitor job-started.sh took for this job. See that
# file for why the inhibitor exists and where the pid lives.
#
# Like the started hook, this must never fail the job: a job whose work is
# already done must not be reported red because a pid file was missing.

runner="${RUNNER_NAME:-unknown-runner}"
pidfile="${XDG_RUNTIME_DIR:-/tmp}/olo-runner-inhibit-${runner}.pid"

if [ ! -f "$pidfile" ]; then
  echo "olo-ci job-completed: no inhibitor was held for this job"
  exit 0
fi

pid=$(cat "$pidfile" 2>/dev/null || true)
rm -f "$pidfile"
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
  # systemd-inhibit releases the lock when it exits; SIGTERM ends it and its
  # `sleep infinity` child together.
  kill "$pid" 2>/dev/null || true
  echo "olo-ci job-completed: shutdown inhibitor released (pid ${pid})"
else
  echo "olo-ci job-completed: inhibitor pid ${pid:-<none>} was already gone"
fi
exit 0
