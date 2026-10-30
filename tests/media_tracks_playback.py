"""Check real track playback over local files and authenticated HTTP Range."""
import argparse
from collections import Counter
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading


def check_diagnostics(stderr):
    active = None
    captured = {}
    for line in stderr.splitlines():
        if line.startswith('BEGIN '):
            if active is not None: raise AssertionError('Nested diagnostic block')
            active = line[6:]
            captured[active] = []
        elif line.startswith('END '):
            if active != line[4:]: raise AssertionError('Unmatched diagnostic block')
            active = None
        elif line:
            if active is None: raise AssertionError('Unexpected diagnostic: ' + line)
            captured[active].append(re.sub(r' @ [0-9a-fA-F]+', ' @ ADDRESS', line))
    if active: raise AssertionError('Incomplete diagnostic block')
    unknown = '[matroska,webm @ ADDRESS] Unknown/unsupported AVCodecID A_BAD.'
    advice = "Consider increasing the value for the 'analyzeduration' (0) and 'probesize' (5000000) options"
    def audio(index):
        return f'[matroska,webm @ ADDRESS] Could not find codec parameters for stream {index} (Audio: none, 48000 Hz, 1 channels): unknown codec'
    expected = {
        'audio-fallback.mkv': [unknown, audio(2), advice],
        'audio-init-failure.mkv': ['[aac @ ADDRESS] Invalid chan_config 15'] * 3 + [
            '[matroska,webm @ ADDRESS] Failed to open codec in avformat_find_stream_info',
            '[matroska,webm @ ADDRESS] Could not find codec parameters for stream 2 (Audio: aac, 48000 Hz, 0 channels, fltp): unspecified number of channels', advice],
        'audio-unavailable.mkv': [unknown, unknown, audio(1), advice, audio(2), advice] * 2,
        'video-unavailable.mkv': ['[matroska,webm @ ADDRESS] Unknown/unsupported AVCodecID V_MPEG4/ISO/XYZ.',
            '[matroska,webm @ ADDRESS] Could not find codec parameters for stream 0 (Video: none, none(progressive), 160x96): unknown codec', advice],
        'audio-damaged.mp4': ['[aac @ ADDRESS] Error decoding AAC frame header.'] * 2,
        'rate-change.ts': ['[mpegts @ ADDRESS] Packet corrupt (stream = 0, dts = 219750).'] * 2,
    }
    if captured.keys() != expected.keys(): raise AssertionError('Missing failure cases')
    for name, lines in expected.items():
        if Counter(captured[name]) != Counter(lines):
            raise AssertionError(f'Unexpected {name} diagnostics: {captured[name]}')


def verify(executable, samples, output):
    output.mkdir(parents=True, exist_ok=True)
    content = (samples/'dual.mp4').read_bytes()
    requests = []
    class Origin(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/media' or self.headers.get('X-Lab') != 'media-tracks':
                self.send_error(403)
                return
            value = self.headers.get('Range', '')
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', value)
            start, end = (int(match[1]), min(int(match[2] or len(content)-1), len(content)-1)) if match else (0, len(content)-1)
            if start > end:
                self.send_response(416)
                self.send_header('Content-Range', f'bytes */{len(content)}')
                self.end_headers()
                return
            self.send_response(206 if match else 200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(end-start+1))
            self.send_header('Accept-Ranges', 'bytes')
            if match: self.send_header('Content-Range', f'bytes {start}-{end}/{len(content)}')
            self.end_headers()
            requests.append(value)
            try: self.wfile.write(content[start:end+1])
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError): pass
        def log_message(self, *args): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        for mode in ('local', 'http'):
            command = [str(executable), str(samples)]
            if mode == 'http': command += [f'http://127.0.0.1:{server.server_port}/media']
            result = subprocess.run(command, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'),
                                    capture_output=True, text=True, encoding='utf-8', timeout=120)
            (output/(mode+'.stdout.log')).write_text(result.stdout, encoding='utf-8')
            (output/(mode+'.stderr.log')).write_text(result.stderr, encoding='utf-8')
            if result.returncode or not result.stdout.startswith('PASS '):
                raise RuntimeError(f'{mode} failed; see {output}')
            check_diagnostics(result.stderr)
            print('PASS', mode, 'track selection, real decoding, UI state and exact expected diagnostics', flush=True)
    finally:
        server.shutdown()
        thread.join()
        server.server_close()
    if len(requests) < 5 or not all(requests): raise AssertionError('HTTP Range switches not exercised')
    (output/'http-requests.json').write_text(json.dumps(requests, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('samples', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    verify(args.executable, args.samples, args.output)
