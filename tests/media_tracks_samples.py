"""Generate small, reproducible media for real track-selection tests."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess


def run(args):
    result = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=120)
    if result.returncode or result.stderr:
        raise RuntimeError(f'{args[0]} failed: {result.stderr}')
    return result.stdout


def prepare(folder):
    folder.mkdir(parents=True, exist_ok=True)
    run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
         'testsrc2=size=160x96:rate=24:duration=8', '-f', 'lavfi', '-i',
         'sine=frequency=440:sample_rate=48000:duration=8', '-f', 'lavfi', '-i',
         'sine=frequency=880:sample_rate=48000:duration=8', '-map', '0:v', '-map', '1:a',
         '-map', '2:a', '-c:v', 'libx264', '-threads:v', '1', '-g', '24', '-c:a', 'aac',
         '-disposition:a:0', '0', '-disposition:a:1', 'default',
         '-metadata:s:a:0', 'language=eng', '-metadata:s:a:0', 'handler_name=English 440 Hz',
         '-metadata:s:a:1', 'language=zho', '-metadata:s:a:1', 'handler_name=Chinese 880 Hz',
         str(folder / 'dual.mp4')])
    def convert(name, options):
        run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', str(folder/'dual.mp4'),
             *options, str(folder/name)])
    convert('dual.mkv', ['-map', '0', '-c', 'copy', '-metadata:s:a:0', 'title=English 440 Hz',
                         '-metadata:s:a:1', 'title=Chinese 880 Hz'])
    convert('silent.mp4', ['-map', '0:v', '-c', 'copy'])
    convert('first-default.mp4', ['-map', '0', '-c', 'copy', '-disposition:a:0', 'default',
                                 '-disposition:a:1', '0'])
    run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-i', str(folder/'silent.mp4'), '-f', 'lavfi',
         '-i', 'color=blue:size=320x180:rate=24:duration=1', '-map', '0:v', '-map', '1:v', '-t', '1',
         '-c:v', 'libx264', '-threads:v', '1', '-disposition:v:0', '0', '-disposition:v:1', 'default',
         str(folder/'two-video.mp4')])
    for name, codec, pixel in [('av1-10bit.mkv', 'libaom-av1', 'yuv420p10le'),
                               ('vp9-10bit.mkv', 'libvpx-vp9', 'yuv420p10le'),
                               ('hevc-10bit.mkv', 'libx265', 'yuv420p10le')]:
        options = ['-map', '0:v', '-t', '1', '-c:v', codec, '-threads', '1', '-pix_fmt', pixel]
        if codec == 'libaom-av1': options += ['-cpu-used', '8']
        if codec == 'libvpx-vp9': options += ['-deadline', 'realtime', '-cpu-used', '8']
        if codec == 'libx265': options += ['-x265-params', 'log-level=error:pools=none:frame-threads=1']
        convert(name, options)
    convert('opus.webm', ['-map', '0:v', '-map', '0:a:1', '-t', '1', '-c:v', 'libvpx-vp9',
                          '-deadline', 'realtime', '-cpu-used', '8', '-threads', '1', '-c:a', 'libopus'])
    convert('surround.mkv', ['-map', '0:v', '-map', '0:a:1', '-t', '1', '-c:v', 'copy',
                            '-c:a', 'pcm_s16le', '-ac', '6', '-ar', '96000'])
    for offset, rate in ((0, 44100), (1, 48000)):
        convert(f'rate-{rate}.ts', ['-map', '0:v', '-map', '0:a:1', '-t', '1', '-c:v', 'copy',
                                   '-c:a', 'aac', '-ar', str(rate), '-output_ts_offset', str(offset)])
    (folder/'rate-change.ts').write_bytes((folder/'rate-44100.ts').read_bytes() + (folder/'rate-48000.ts').read_bytes())
    facts = {}
    for path in sorted(folder.iterdir()):
        if path.suffix not in ('.mp4', '.mkv', '.webm'): continue
        if any(word in path.name for word in ('unavailable', 'fallback', 'damaged', 'failure')): continue
        facts[path.name] = json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                                          '-of', 'json', str(path)]))
        facts[path.name]['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    content = (folder/'dual.mkv').read_bytes()
    assert content.count(b'A_AAC') == 2
    (folder/'audio-unavailable.mkv').write_bytes(content.replace(b'A_AAC', b'A_BAD'))
    position = content.rfind(b'A_AAC')
    (folder/'audio-fallback.mkv').write_bytes(content[:position] + b'A_BAD' + content[position+5:])
    assert content.count(b'V_MPEG4/ISO/AVC') == 1
    (folder/'video-unavailable.mkv').write_bytes(content.replace(b'V_MPEG4/ISO/AVC', b'V_MPEG4/ISO/XYZ'))
    invalid = bytearray(content)
    position = invalid.rfind(b'\x63\xa2\x85\x11\x88')
    assert position > 0
    invalid[position+4] = 0xf8
    (folder/'audio-init-failure.mkv').write_bytes(invalid)
    packets = json.loads(run(['ffprobe', '-v', 'error', '-show_packets', '-select_streams', 'a:1',
                              '-show_entries', 'packet=pos,size,pts_time', '-of', 'json', str(folder/'dual.mp4')]))
    packet = next(p for p in packets['packets'] if float(p['pts_time']) >= 2)
    damaged = bytearray((folder/'dual.mp4').read_bytes())
    start, size = int(packet['pos']), int(packet['size'])
    damaged[start:start+size] = b'\xff' * size
    (folder/'audio-damaged.mp4').write_bytes(damaged)
    facts['audio-damaged.mp4'] = {'corrupted_packet': packet, 'purpose': 'midstream AAC decoding failure'}
    (folder / 'manifest.json').write_text(json.dumps(facts, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    prepare(parser.parse_args().folder)
