"""Exercise the real Windows build gate with OS handles in isolated Git repos."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

WRAPPER = Path(__file__).resolve().parents[1] / '.claude/skills/run-oloengine/build-lock.ps1'


def wait_for(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError('Timed out waiting for build-gate process')
        time.sleep(.05)


@unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows PowerShell gate')
class BuildLockExclusion(unittest.TestCase):
    def test_single_slot_request_observes_a_live_higher_slot(self):
        with tempfile.TemporaryDirectory(prefix='olo-build-gate-') as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            holder_script = root / 'holder.ps1'
            holder_script.write_text('''$lock=[IO.File]::Open('.git/olo-build.slot1.lock','OpenOrCreate','Write','Read')
try {
 $data=[Text.Encoding]::UTF8.GetBytes((@{command='cmake --build build-cached';pid=$PID;worktree=$PWD.Path}|ConvertTo-Json -Compress))
 $lock.Write($data,0,$data.Length); $lock.Flush()
 Set-Content ready yes
 while (-not (Test-Path release)) { Start-Sleep -Milliseconds 50 }
} finally { $lock.Dispose() }
''')
            with (root / 'holder.log').open('w') as holder_log, (root / 'waiter.log').open('w') as waiter_log:
                holder = subprocess.Popen(['pwsh', '-NoProfile', '-File', str(holder_script)], cwd=root,
                                          stdout=holder_log, stderr=subprocess.STDOUT)
                waiter = None
                try:
                    wait_for(lambda: (root / 'ready').exists())
                    waiter = subprocess.Popen(['pwsh', '-NoProfile', '-File', str(WRAPPER),
                        '-MaxConcurrent', '1', '-Jobs', '-1', '-PollSeconds', '1', '-NoParentWatch',
                        '-Command', 'Set-Content started yes'], cwd=root,
                        stdout=waiter_log, stderr=subprocess.STDOUT)
                    wait_for(lambda: (root / 'started').exists() or
                             'waiting' in (root / 'waiter.log').read_text())
                    self.assertFalse((root / 'started').exists(), 'Started while slot 1 was still held')
                    self.assertIn(f'held by pid={holder.pid}', (root / 'waiter.log').read_text())
                    (root / 'release').touch()
                    self.assertEqual(holder.wait(timeout=10), 0)
                    self.assertEqual(waiter.wait(timeout=15), 0)
                    self.assertTrue((root / 'started').exists(), 'Did not start after slot 1 released')
                finally:
                    (root / 'release').touch()
                    for process in (holder, waiter):
                        if process is not None:
                            try:
                                process.wait(timeout=15)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()

    def _assert_cached_admission(self, holder_limit):
        with tempfile.TemporaryDirectory(prefix='olo-build-gate-') as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'payload.ps1').write_text("Set-Content ready yes\nwhile (-not (Test-Path release)) { Start-Sleep -Milliseconds 50 }\n")
            with (root / 'holder.log').open('w') as holder_log, (root / 'waiter.log').open('w') as waiter_log:
                common = ['pwsh', '-NoProfile', '-File', str(WRAPPER), '-Jobs', '-1',
                          '-PollSeconds', '1', '-NoParentWatch', '-ConcurrentMinFreeGB', '0']
                holder = subprocess.Popen([*common, '-MaxConcurrent', str(holder_limit), '-Command',
                    '& ./payload.ps1 # build-cached'], cwd=root, stdout=holder_log, stderr=subprocess.STDOUT)
                waiter = None
                try:
                    wait_for(lambda: (root / 'ready').exists())
                    waiter = subprocess.Popen([*common, '-MaxConcurrent', '2', '-Command',
                        'Set-Content started yes # build-cached'], cwd=root,
                        stdout=waiter_log, stderr=subprocess.STDOUT)
                    wait_for(lambda: (root / 'started').exists() or 'waiting' in (root / 'waiter.log').read_text())
                    if holder_limit == 1:
                        self.assertFalse((root / 'started').exists(), 'A second job joined an exclusive cached job')
                    else:
                        wait_for(lambda: (root / 'started').exists())
                        self.assertIsNone(holder.poll(), 'The first cached job should still be running')
                    (root / 'release').touch()
                    self.assertEqual(holder.wait(timeout=15), 0)
                    self.assertEqual(waiter.wait(timeout=15), 0)
                    self.assertTrue((root / 'started').exists())
                finally:
                    (root / 'release').touch()
                    for process in (holder, waiter):
                        if process is not None:
                            try:
                                process.wait(timeout=15)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()

    def test_later_cached_request_cannot_join_an_exclusive_cached_job(self):
        self._assert_cached_admission(1)

    def test_two_ordinary_cached_requests_can_still_overlap(self):
        self._assert_cached_admission(2)

    def test_cached_waiter_does_not_pass_an_exclusive_waiter(self):
        # #1386: an exclusive request at the head of the queue starved while cached
        # requests queued behind it took each slot as it freed.
        with tempfile.TemporaryDirectory(prefix='olo-build-gate-') as temporary:
            root = Path(temporary)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'hold.ps1').write_text(
                "param($Ready, $Release)\nSet-Content $Ready yes\n"
                "while (-not (Test-Path $Release)) { Start-Sleep -Milliseconds 50 }\n")
            common = ['pwsh', '-NoProfile', '-File', str(WRAPPER), '-Jobs', '-1',
                      '-PollSeconds', '1', '-NoParentWatch', '-ConcurrentMinFreeGB', '0']
            logs = {name: (root / f'{name}.log').open('w') for name in ('holder', 'exclusive', 'cached')}
            processes = []
            try:
                def start(name, limit, command):
                    processes.append(subprocess.Popen([*common, '-MaxConcurrent', str(limit), '-Command', command],
                                                      cwd=root, stdout=logs[name], stderr=subprocess.STDOUT))
                start('holder', 2, '& ./hold.ps1 holder_ready release_holder # build-cached')
                wait_for(lambda: (root / 'holder_ready').exists())
                start('exclusive', 1, '& ./hold.ps1 exclusive_started release_exclusive # build-cached')
                wait_for(lambda: 'waiting' in (root / 'exclusive.log').read_text())
                start('cached', 2, 'Set-Content cached_started yes # build-cached')
                wait_for(lambda: (root / 'cached_started').exists() or 'waiting' in (root / 'cached.log').read_text())
                time.sleep(3)  # three polls with slot 1 free
                self.assertFalse((root / 'cached_started').exists(), 'A cached waiter passed the exclusive waiter')
                (root / 'release_holder').touch()
                wait_for(lambda: (root / 'exclusive_started').exists())
                time.sleep(3)
                self.assertFalse((root / 'cached_started').exists(), 'A cached waiter joined the exclusive job')
                (root / 'release_exclusive').touch()
                wait_for(lambda: (root / 'cached_started').exists())
                for process in processes:
                    self.assertEqual(process.wait(timeout=15), 0)
            finally:
                for marker in ('release_holder', 'release_exclusive'):
                    (root / marker).touch()
                for process in processes:
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                for log in logs.values():
                    log.close()


if __name__ == '__main__':
    unittest.main()
