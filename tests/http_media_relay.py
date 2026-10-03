"""Forward real media responses; a gate file temporarily stalls network delivery.

No media or authorization responses are synthesized. Used only for bounded-buffer,
rebuffering and cancellation checks against the real ChatServer.
"""
import argparse
import http.client
import http.server
import json
from pathlib import Path
import threading
import time
from urllib.parse import urlparse

parser = argparse.ArgumentParser()
parser.add_argument('origin')
parser.add_argument('gate', type=Path)
parser.add_argument('address_file', type=Path)
args = parser.parse_args()
origin = urlparse(args.origin)
lock = threading.Lock()
requests_file = args.address_file.with_suffix('.requests.jsonl')
requests_file.write_text('', encoding='utf-8')


class Relay(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        upstream = http.client.HTTPConnection(origin.hostname, origin.port or 80, timeout=15)
        started = int(time.time() * 1000)
        sent = 0
        status = 0
        try:
            headers = {key: self.headers[key] for key in ('Authorization', 'Range', 'If-Range')
                       if key in self.headers}
            upstream.request('GET', origin.path, headers=headers)
            response = upstream.getresponse()
            status = response.status
            self.send_response(status)
            for key in ('Content-Length', 'Content-Range', 'Accept-Ranges', 'Content-Type'):
                if response.getheader(key) is not None:
                    self.send_header(key, response.getheader(key))
            self.send_header('Connection', 'close')
            self.end_headers()
            while chunk := response.read(16384):
                while args.gate.exists():
                    time.sleep(.02)
                self.wfile.write(chunk)
                sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # seek/关闭主动取消的真实 TCP 连接。
        finally:
            upstream.close()
            self.close_connection = True
            with lock:
                record = json.dumps(dict(started_ms=started, ended_ms=int(time.time() * 1000),
                    range=self.headers.get('Range'), status=status, sent=sent))
                with requests_file.open('a', encoding='utf-8') as output:
                    output.write(record + '\n')

    def log_message(self, *unused):
        pass


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Relay)
args.address_file.write_text(f'http://127.0.0.1:{server.server_port}/media')
try:
    server.serve_forever()
except KeyboardInterrupt:
    pass
finally:
    args.gate.unlink(missing_ok=True)
    server.server_close()
