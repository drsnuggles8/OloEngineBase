#!/usr/bin/env bash
# Run from OloEditor on the headless Radeon conformance runner.
set -euo pipefail
out="${1:?supply the absolute results directory}"
mkdir -p "$out"
node=/dev/dri/renderD128
device=/sys/class/drm/renderD128/device
command -v fuser >/dev/null
test -r "$device/gpu_busy_percent"
test "$(cat "$device/vendor")" = 0x1002
cat "$device/uevent" > "$out/device.txt"
python3 ../OloEngine/tests/scripts/generate_perf_scenes.py --scene crowd_reference --counts 0,5000
python3 ../OloEngine/tests/scripts/generate_perf_scenes.py --scene crowd_lod --counts 200,1000,5000

captures=()
capture() {
  local scene="$1" directory="$out/$1-run$2"
  mkdir "$directory"
  local manifest="$PWD/SandboxProject/Assets/Scenes/PerfStress/$scene.capture.yaml"
  local trace="$directory/host.tsv"
  printf 'UTC\tGPU busy %%\tloadavg\trender node PIDs\n' > "$trace"
  ../build/OloEngine/tests/OloEngine-Tests --olo-gl-backend=egl --olo-require-gpu \
      --olo-perf-machine=selfhosted-amd-navi10-linux \
      --olo-capture-manifest="$manifest" --olo-capture-out="$directory" > "$directory/process.log" 2>&1 &
  local capture_pid=$! observed=false shared=false clients pid
  echo "$capture_pid" > "$directory/capture.pid"
  # Both olo-ci slots run as the same user and can open the same render node.
  # Record clients throughout each process, including initialization and teardown.
  while kill -0 "$capture_pid" 2>/dev/null; do
    clients=$(fuser "$node" 2>/dev/null || true)
    printf '%s\t%s\t%s\t%s\n' "$(date -u +%FT%T.%3NZ)" "$(cat "$device/gpu_busy_percent")" \
      "$(cat /proc/loadavg)" "$clients" >> "$trace"
    for pid in $clients; do
      if [ "$pid" = "$capture_pid" ]; then observed=true; else shared=true; fi
    done
    sleep 0.5
  done
  if ! wait "$capture_pid"; then
    tail -100 "$directory/process.log"
    return 1
  fi
  if [ "$observed" != true ] || [ "$shared" = true ]; then
    echo "No isolated capture on $node for $scene. See $trace." >&2
    return 1
  fi
  captures+=("$directory")
}
# Fresh process per arm. Alternate the control and workload to expose clock drift.
capture crowd_reference_0 1
capture crowd_lod_5000 1
capture crowd_lod_5000 2
capture crowd_reference_0 2
capture crowd_reference_0 3
capture crowd_lod_5000 3
capture crowd_lod_200 1
capture crowd_lod_1000 1
capture crowd_reference_5000 1
python3 ../scripts/perf/report-crowd.py "${captures[@]}" | tee "$out/report.md"
