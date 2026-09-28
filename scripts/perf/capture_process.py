"""Fresh-process capture hosts with explicit startup and measurement boundaries."""
from pathlib import Path
import subprocess
import tempfile
import time
import json
import urllib.error
import hashlib


def editor_preferences(arm):
    try:
        import yaml
    except ImportError as error:
        raise ValueError('editor capture requires PyYAML') from error

    path = Path(arm['repo']) / 'OloEditor/SandboxProject/EditorPreferences.yaml'
    raw = path.read_bytes()
    try:
        document = yaml.safe_load(raw)
    except yaml.YAMLError as error:
        raise ValueError(f'invalid editor preferences: {path}') from error
    preferences = document.get('EditorPreferences') if isinstance(document, dict) else None
    if not isinstance(preferences, dict):
        raise ValueError(f'missing editor preferences: {path}')
    for key in ('ThrottleEditMode', 'ThrottlePlayMode', 'EnableAutoSave'):
        if preferences.get(key) is not False:
            raise ValueError(f'controlled editor requires {key}: false in {path}')
    if preferences.get('FrameRateCap') != 0:
        raise ValueError(f'controlled editor requires FrameRateCap: 0 in {path}')
    return {'path': str(path), 'sha256': hashlib.sha256(raw).hexdigest(),
            'values': preferences}

from editor_mcp import Client


def run_capture(arm, manifest, destination, environment, timeout, stdout, stderr):
    exe = str(Path(arm['exe']).resolve())
    host = arm.get('host', 'test-binary')
    if host == 'test-binary':
        command = [exe, f'--olo-capture-manifest={manifest.resolve()}',
                   f'--olo-capture-out={destination.resolve()}']
        result = subprocess.run(command, cwd=arm['repo'], env=environment,
                                stdout=stdout, stderr=stderr, timeout=timeout)
        return {'command': command, 'exitCode': result.returncode,
                'startupReadySeconds': None, 'startupBoundary': 'not instrumented by test host'}
    if host != 'editor-mcp' or arm.get('backend') not in ('opengl', 'vulkan'):
        raise ValueError('host must be test-binary or editor-mcp with an explicit backend')
    if arm.get('injectDelayMs', 0):
        raise ValueError('wall-delay negative control is implemented only by the test host')
    preferences = editor_preferences(arm)
    # Each process gets a private discovery file. It contains a credential and
    # must never be copied into the retained capture artefacts.
    with tempfile.TemporaryDirectory(prefix='olo-controlled-') as temporary:
        discovery = Path(temporary) / 'discovery.json'
        env = {**environment, 'OLO_MCP_AUTOSTART': '1', 'OLO_MCP_ALLOW_WRITES': '1',
               'OLO_MCP_PORT': str(arm['mcpPort']), 'OLO_MCP_DISCOVERY_FILE': str(discovery)}
        env.pop('OLO_CAPTURE_TEST_DELAY_MS', None)
        command = [exe, '--rhi=' + arm['backend']]
        start = time.perf_counter()
        process = subprocess.Popen(command, cwd=Path(arm['repo']) / 'OloEditor', env=env,
                                   stdout=stdout, stderr=stderr)
        try:
            client = None
            while client is None:
                if process.poll() is not None:
                    raise ValueError(f'editor exited before MCP became ready: {process.returncode}')
                if time.perf_counter() - start >= timeout:
                    raise ValueError('editor startup timed out')
                if discovery.exists():
                    try:
                        client = Client(discovery, timeout=timeout)
                    except (json.JSONDecodeError, urllib.error.URLError, ConnectionError):
                        pass  # the file can be visible before JSON/listening is ready
                if client is None:
                    time.sleep(.25)
            client.wait_ready(timeout=timeout)
            ready = time.perf_counter() - start
            result = client.tool('olo_benchmark_capture', manifest=str(manifest.resolve()),
                                 outDir=str(destination.resolve()))
            if result['backend'] != arm['backend'] or result['host'] != host:
                raise ValueError('editor capture host/backend differs from requested arm')
            if result['warmupTimedOut'] or result['attachmentFailures']:
                raise ValueError(f'editor capture incomplete: {result}')
            return {'command': command, 'exitCode': 0, 'startupReadySeconds': ready,
                    'editorPreferences': preferences,
                    'startupBoundary': 'process launch to responsive MCP after editor initialization',
                    'termination': 'runner terminates editor after capture and state restoration'}
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=30)
