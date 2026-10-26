"""Run the actual Qt player against a local Range origin and gated real-byte relay."""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time


def verify_player_log(output, hls):
    lines = (output / "player.log").read_text(encoding="utf-8").splitlines()
    opening = re.compile(r"\[hls @ [^]]+\] Opening 'http://127\.0\.0\.1:\d+/[^']+' for reading")
    duplicate = re.compile(r"\[mov,mp4,m4a,3gp,3g2,mj2 @ [^]]+\] Found duplicated MOOV Atom\. Skipped it")
    allowed_pass = "PASS HTTP HLS playback, paused seek and drain" if hls else \
        "PASS network starvation, frozen clock, recovery and cancellation"
    duplicates = sum(bool(duplicate.fullmatch(line)) for line in lines)
    init_reads = sum("/init.mp4' for reading" in line for line in lines)
    unexpected = [line for line in lines if line != allowed_pass and
                  not (hls and (opening.fullmatch(line) or duplicate.fullmatch(line)))]
    if allowed_pass not in lines or unexpected or (duplicates and init_reads < 2):
        raise RuntimeError(f"Unexpected player diagnostics: {unexpected or lines}")
    (output / "diagnostics.json").write_text(json.dumps(dict(
        init_reads=init_reads, duplicate_moov=duplicates,
        explanation="HLS seek rereads init.mp4; the MOV demuxer skips its repeated moov" if duplicates else "",
        unexpected=unexpected), indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("player", type=Path)
    parser.add_argument("sample", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--hls", action="store_true", help="sample is a directory containing index.m3u8")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sample = args.sample.resolve(strict=True)
    gate = args.output.resolve() / "pause.gate"
    address = args.output.resolve() / "relay.txt"
    if gate.exists() or address.exists():
        parser.error("Choose a fresh output directory; it already contains lab state")
    requests = []

    class Origin(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/sample.mp4":
                self.send_error(404)
                return
            size = sample.stat().st_size
            start, end, status = 0, size - 1, 200
            value = self.headers.get("Range")
            if value:
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", value)
                if not match or int(match[1]) >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    return
                start, end, status = int(match[1]), min(int(match[2] or end), end), 206
                if end < start:
                    self.send_error(416)
                    return
            self.send_response(status)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Content-Length", str(end - start + 1))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            sent = 0
            try:
                with sample.open("rb") as file:
                    file.seek(start)
                    while sent <= end - start:
                        chunk = file.read(min(16384, end - start + 1 - sent))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        sent += len(chunk)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            finally:
                requests.append(dict(range=value, status=status, sent=sent))

        def log_message(self, *_):
            pass

    class HlsOrigin(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *values, **kwargs):
            super().__init__(*values, directory=str(sample), **kwargs)

        def log_message(self, format, *values):
            requests.append(dict(message=format % values))

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HlsOrigin if args.hls else Origin)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    relay = None
    try:
        if args.hls:
            command = [str(args.player.resolve()), "hls",
                f"http://127.0.0.1:{server.server_port}/index.m3u8", str(args.output.resolve())]
            with (args.output / "player.log").open("w", encoding="utf-8") as output:
                run = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                     timeout=30, creationflags=flags)
            if run.returncode:
                raise RuntimeError(f"HLS player failed with {run.returncode}; inspect player.log")
            verify_player_log(args.output, True)
            print("PASS local HTTP HLS playback, paused seek and drain")
            return
        with (args.output / "relay.log").open("w", encoding="utf-8") as log:
            relay = subprocess.Popen([sys.executable, str(Path(__file__).with_name("http_media_relay.py")),
                f"http://127.0.0.1:{server.server_port}/sample.mp4", str(gate), str(address)],
                stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
            deadline = time.monotonic() + 10
            while not address.exists() and relay.poll() is None and time.monotonic() < deadline:
                time.sleep(.05)
            if not address.exists():
                raise RuntimeError("relay did not start")
            command = [str(args.player.resolve()), "network", address.read_text().strip(),
                       str(gate), str(args.output.resolve())]
            with (args.output / "player.log").open("w", encoding="utf-8") as output:
                run = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT,
                                     timeout=60, creationflags=flags)
            if run.returncode:
                raise RuntimeError(f"player failed with {run.returncode}; inspect player.log")
            verify_player_log(args.output, False)
            print("PASS local HTTP Range, starvation, recovery and blocked-read cancellation")
    finally:
        gate.unlink(missing_ok=True)
        if relay is not None:
            relay.terminate()
            relay.wait(timeout=10)
        server.shutdown()
        server.server_close()
        (args.output / "origin.json").write_text(json.dumps(requests, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
