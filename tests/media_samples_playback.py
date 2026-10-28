"""Validate downloaded videos with real FFmpeg decoding and the Qt player."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import re
import threading


def run(command, log, environment=None):
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                            errors='replace', env=environment, timeout=90)
    log.write_text(result.stdout + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}); see {log}')
    return result


def verify(executable, samples, output):
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    reports = []
    for name in ('video.mp4', 'video-h265.mkv', 'video-vp9-opus.webm'):
        folder = output / name
        folder.mkdir(exist_ok=True)
        sample = samples / name
        reference = folder / 'first.rgb'
        result = run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', str(sample), '-map', '0:v:0',
                      '-frames:v', '1', '-vf', 'scale=flags=bilinear', '-pix_fmt', 'rgb24',
                      '-f', 'rawvideo', str(reference)], folder / 'reference.log')
        if result.stdout or result.stderr:
            raise RuntimeError(f'Unexpected reference diagnostics: {folder}')
        result = run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                      '-show_entries', 'stream=nb_read_frames', '-of', 'json', str(sample)], folder / 'count.log')
        if result.stderr:
            raise RuntimeError(f'Unexpected count diagnostics: {folder}')
        frames = json.loads(result.stdout)['streams'][0]['nb_read_frames']
        requests = []
        class Origin(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path != '/media' or self.headers.get('X-Lab') != 'media-samples':
                    self.send_error(403)
                    return
                size = sample.stat().st_size
                value = self.headers.get('Range', '')
                match = re.fullmatch(r'bytes=(\d+)-(\d*)', value)
                start, end = (int(match[1]), min(int(match[2] or size - 1), size - 1)) if match else (0, size - 1)
                if start > end:
                    self.send_response(416)
                    self.send_header('Content-Range', f'bytes */{size}')
                    self.end_headers()
                    return
                status = 206 if match else 200
                self.send_response(status)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(end - start + 1))
                self.send_header('Accept-Ranges', 'bytes')
                if match: self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
                self.end_headers()
                requests.append({'range': value, 'status': status})
                try:
                    with sample.open('rb') as source:
                        source.seek(start)
                        remaining = end - start + 1
                        while remaining:
                            chunk = source.read(min(65536, remaining))
                            if not chunk: raise RuntimeError('sample changed during HTTP read')
                            self.wfile.write(chunk)
                            remaining -= len(chunk)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            result = run([str(executable), str(sample), str(reference), frames, str(folder),
                          f'http://127.0.0.1:{server.server_port}/media'], folder / 'player.log', environment)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
        (folder / 'http-requests.json').write_text(json.dumps(requests, indent=2), encoding='utf-8')
        if not any(r['status'] == 206 for r in requests):
            raise RuntimeError(f'HTTP Range not exercised: {folder}')
        if result.stderr or not result.stdout.startswith('PASS '):
            raise RuntimeError(f'Unexpected player diagnostics: {folder}')
        reports.append({'sample': name, 'result': json.loads((folder / 'result.json').read_text())})
        print(f'PASS {name}: {frames} frames, RGB reference, local/HTTP pause/seek/reopen/EOF, RTC pipeline', flush=True)
    (output / 'summary.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('samples', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    verify(args.executable.resolve(), args.samples.resolve(), args.output.resolve())
