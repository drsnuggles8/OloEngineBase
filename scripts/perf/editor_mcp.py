"""Local editor MCP client; discovery credentials are never persisted in reports."""
import json
from pathlib import Path
import time
import urllib.request


class Client:
    def __init__(self, discovery: Path, timeout=300):
        self.timeout = timeout
        endpoint = json.loads(discovery.read_text(encoding='utf-8-sig'))
        self.url = endpoint['url']
        self.headers = {'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
                        'Authorization': 'Bearer ' + endpoint['token']}
        self.next_id = 0
        self.rpc('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                               'clientInfo': {'name': 'editor-attribution', 'version': '1'}})
        self.rpc('notifications/initialized', {}, notification=True)

    def rpc(self, method, parameters, notification=False):
        body = {'jsonrpc': '2.0', 'method': method, 'params': parameters}
        if not notification:
            self.next_id += 1
            body['id'] = self.next_id
        request = urllib.request.Request(self.url, data=json.dumps(body).encode(), headers=self.headers)
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            session = response.headers.get('Mcp-Session-Id')
            if session:
                self.headers['Mcp-Session-Id'] = session
            raw = response.read()
        if not raw:
            return None
        result = json.loads(raw)
        if 'error' in result:
            raise RuntimeError(result['error'])
        return result['result']

    def wait_ready(self, timeout=300):
        deadline = time.monotonic() + timeout
        while True:
            try:
                return self.tool('olo_perf_snapshot')
            except RuntimeError as error:
                if 'Timed out waiting for the editor main thread' not in str(error) or time.monotonic() >= deadline:
                    raise
                time.sleep(1)

    def tool(self, name, **arguments):
        result = self.rpc('tools/call', {'name': name, 'arguments': arguments})
        if result.get('isError'):
            raise RuntimeError(result)
        if 'structuredContent' in result:
            return result['structuredContent']
        return json.loads(next(c['text'] for c in result['content'] if c['type'] == 'text'))
