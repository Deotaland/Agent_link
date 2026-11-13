#!/usr/bin/env python3
"""Convert one Sugarat startup movie and package appdata; never open a serial port."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<8s10I')


def jpeg_frames(data):
    """The encoder emits baseline JPEG; split only complete SOI/EOI packets."""
    frames = []
    at = 0
    while at < len(data):
        if data[at:at+2] != b'\xff\xd8':
            raise ValueError('Invalid MJPEG packet boundary')
        end = data.find(b'\xff\xd9', at + 2)
        if end < 0:
            raise ValueError('Truncated MJPEG packet')
        frames.append(data[at:end+2])
        at = end + 2
    return frames


def convert(source, output):
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams',
                                               '-show_format', '-of', 'json', str(source)]))
    video = next(stream for stream in probe['streams'] if stream['codec_type'] == 'video')
    duration = float(video.get('duration', probe['format']['duration']))
    if not 0 < duration <= 10:
        raise ValueError('Startup video must be at most ten seconds')
    fps = 24
    encoded = subprocess.check_output(['ffmpeg', '-v', 'error', '-threads', '2', '-i', str(source),
        '-an', '-vf', 'scale=360:360:flags=lanczos,fps=24', '-c:v', 'mjpeg', '-threads', '2',
        '-pix_fmt', 'yuvj420p', '-q:v', '5', '-f', 'image2pipe', '-'])
    frames = jpeg_frames(encoded)
    if not frames or len(frames) > fps * 10 or any(len(frame) > 256 * 1024 for frame in frames):
        raise ValueError('Unsupported frame count or frame size')
    pcm = b''
    if any(stream['codec_type'] == 'audio' for stream in probe['streams']):
        pcm = subprocess.check_output(['ffmpeg', '-v', 'error', '-threads', '2', '-i', str(source),
            '-vn', '-t', str(len(frames)/fps), '-ac', '1', '-ar', '16000', '-f', 's16le', '-'])
        pcm = pcm[:(len(frames)*16000//fps)*2]
    index_offset = HEADER.size + len(pcm)
    data_offset = index_offset + len(frames) * 8
    cursor = data_offset
    entries = bytearray()
    for frame in frames:
        entries += struct.pack('<II', cursor, len(frame))
        cursor += len(frame)
    payload = pcm + entries + b''.join(frames)
    if cursor > 6 * 1024 * 1024:
        raise ValueError('Converted animation exceeds the runtime memory bound')
    header = HEADER.pack(b'SGBV0001', 360, 360, fps, len(frames), len(pcm), 16000,
                         index_offset, data_offset, cursor, zlib.crc32(payload))
    asset = output / 'appdata'
    asset.mkdir(parents=True, exist_ok=True)
    (asset / 'START.BVA').write_bytes(header + payload)
    shutil.copy2(source, asset / 'START.MP4')
    # Independently decode every generated frame before producing a flash image.
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'mjpeg', '-i', 'pipe:0',
                    '-f', 'null', '-'], input=encoded, check=True)
    return dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), width=360,
                height=360, fps=fps, frames=len(frames), seconds=len(frames)/fps,
                pcm_rate=16000, pcm_bytes=len(pcm), animation_bytes=cursor)


def package(output, info, include_factory=False):
    idf = Path(os.environ['IDF_PATH'])
    script = idf / 'components/fatfs/wl_fatfsgen.py'
    subprocess.run([sys.executable, str(script), str(output/'appdata'),
                    '--output_file', str(output/'appdata.bin'), '--partition_size', '0x1000000',
                    '--sector_size', '4096'], check=True)
    image = output/'appdata.bin'
    if image.stat().st_size != 0x1000000:
        raise ValueError('appdata image must exactly match the 16 MiB partition')
    # Round-trip the generated wear-levelled filesystem with IDF's own parser.
    # The temporary extraction is checked to remain inside this output directory.
    extracted = (output/'verify-fat').resolve()
    if not extracted.is_relative_to(output.resolve()):
        raise ValueError('Filesystem verification directory escaped output')
    if extracted.exists():
        if not (extracted/'.verification-owner').is_file():
            raise ValueError('Refusing to replace an unrelated verification directory')
        shutil.rmtree(extracted)
    extracted.mkdir()
    (extracted/'.verification-owner').write_text('prepare_boot_animation', encoding='utf-8')
    try:
        subprocess.run([sys.executable, str(idf/'components/fatfs/fatfsparse.py'),
                        str(image), '--wl-layer', 'enabled'], cwd=extracted,
                       stdout=subprocess.DEVNULL, check=True)
        for name in ('START.BVA', 'START.MP4'):
            matches = list(extracted.rglob(name))
            if len(matches) != 1 or matches[0].read_bytes() != (output/'appdata'/name).read_bytes():
                raise ValueError(f'FAT verification failed for {name}')
    finally:
        if extracted.resolve().is_relative_to(output.resolve()):
            shutil.rmtree(extracted)
    info.update(partition='appdata', offset='0x1000000', size=0x1000000,
                appdata_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                settings_impact='Writing appdata.bin replaces the entire appdata filesystem.')
    plan_path = ROOT/'build/flash-plan.json'
    if include_factory:
        plan = json.loads(plan_path.read_text(encoding='utf-8'))
        if plan['profile'] != 'sdgoods-wifi' or plan['role'] != 'factory':
            raise ValueError('Build sdgoods-wifi --image-role factory before packaging')
        partition = plan['partitions']['appdata']
        if (partition['offset'], partition['size']) != (0x1000000, 0x1000000):
            raise ValueError('Factory appdata geometry does not match animation package')
        metadata = json.loads((ROOT/'build/flasher_args.json').read_text(encoding='utf-8'))
        files = {offset: str((ROOT/'build'/file).resolve())
                 for offset, file in metadata['flash_files'].items()}
        files['0x1000000'] = str(image.resolve())
        merged = output/'factory-with-animation-merged.bin'
        command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', 'merge-bin', '-o', str(merged)]
        for key, value in metadata['flash_settings'].items():
            command += ['--'+key.replace('_', '-'), value]
        for offset, file in sorted(files.items(), key=lambda pair: int(pair[0], 0)):
            command += [offset, file]
        subprocess.run(command, check=True)
        with merged.open('rb') as stream:
            for offset, file in files.items():
                stream.seek(int(offset, 0))
                expected = Path(file).read_bytes()
                if stream.read(len(expected)) != expected:
                    raise ValueError('Merged package verification failed')
        (output/'flash-files.json').write_text(json.dumps(dict(chip='esp32s3',
            flash_files=files, flash_settings=metadata['flash_settings']), indent=2)+'\n', encoding='utf-8')
        info['merged_sha256'] = hashlib.sha256(merged.read_bytes()).hexdigest()
    (output/'animation.json').write_text(json.dumps(info, indent=2)+'\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT/'build/boot-animation')
    parser.add_argument('--package-factory', action='store_true',
                        help='merge appdata with the current, already built Factory image')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    info = convert(args.source.resolve(), output)
    package(output, info, args.package_factory)
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
