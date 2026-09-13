"""Local JSON-RPC client for optional ReSTIR PT live reproduction scripts.

The editor owns its discovery file in the platform temporary directory. Credentials
are used only for the Authorization header; discovery data/headers are never logged.
One Client is used per thread/session. This module performs no calls at import time.
"""
import base64
import json
import tempfile
import urllib.request
from pathlib import Path


class Client:
    def __init__(self, port=18311):
        discovery = Path(tempfile.gettempdir()) / f'oloengine-mcp-{port}.json'
        data = json.loads(discovery.read_text(encoding='utf-8-sig'))
        self.url = data['url']
        self.headers = {'Authorization': 'Bearer ' + data['token'],
                        'Content-Type': 'application/json',
                        'Accept': 'application/json, text/event-stream'}
        self.sequence = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                 'clientInfo': {'name': 'restir-pt-reproduction', 'version': '1'}})
        self.rpc('notifications/initialized', None, notify=True)

    def rpc(self, method, params, notify=False):
        self.sequence += 1
        body = {'jsonrpc': '2.0', 'method': method}
        if not notify:
            body['id'] = self.sequence
        if params is not None:
            body['params'] = params
        request = urllib.request.Request(self.url, data=json.dumps(body).encode('utf-8'),
                                         headers=self.headers, method='POST')
        # Match the calibrated helper: benchmark calls can take minutes. Harness
        # deadlines are checked BETWEEN calls, not advertised as hard cancellation.
        with urllib.request.urlopen(request, timeout=600) as response:
            session = response.headers.get('Mcp-Session-Id')
            if session:
                self.headers['Mcp-Session-Id'] = session
            raw = response.read()
        return json.loads(raw) if raw else {}

    def tool(self, name, args=None, out=None):
        result = self.rpc('tools/call', {'name': name, 'arguments': args or {}})
        if out:
            path = Path(out)
            path.parent.mkdir(parents=True, exist_ok=True)
            for index, block in enumerate(result.get('result', {}).get('content', [])):
                if block.get('type') == 'image':
                    image = path.with_suffix(f'.{index}.png')
                    image.write_bytes(base64.b64decode(block.pop('data')))
                    block['savedPath'] = str(image)
            path.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n',
                                                 encoding='utf-8', newline='\n')
        return result
