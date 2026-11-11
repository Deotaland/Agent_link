"""Generate a shared bootloader/application policy from the versioned layout."""
import argparse
import binascii
from pathlib import Path
import struct
from firmware_manifest import resolve_profiles

ENTRY = struct.Struct('<HBBII16sI')
SUBTYPES = {'nvs':2, 'ota':0, 'phy':1, 'nvs_keys':4, 'undefined':6, 'fat':0x81, 'factory':0}


def definition(profile):
    layout = profile['layout_info']
    dynamic = layout.get('dynamic_slots')
    if not dynamic:
        raise ValueError('This profile has no dynamic layout')
    pool = layout['partitions'][dynamic['pool_partition']]
    guard = layout['partitions'][dynamic['guard_partition']]
    if guard['size'] != 0x4000 or dynamic['max_slots'] != 16:
        raise ValueError('Unsupported dynamic journal geometry')
    fixed = []
    for label, value in layout['partitions'].items():
        if label in layout['app_slots']:
            continue
        subtype = SUBTYPES.get(value['subtype'])
        if subtype is None: subtype = int(value['subtype'], 0)
        fixed.append(ENTRY.pack(0x50AA, 0 if value['type']=='app' else 1, subtype,
                                value['offset'], value['size'], label.encode(), 0))
    fields = (0x8000, guard['offset'], pool['offset'], pool['offset']+pool['size'], dynamic['max_slots'])
    identity = binascii.crc32(struct.pack('<5I',*fields)+b''.join(fixed)) & 0xFFFFFFFF
    return fields, fixed, identity


def header(profile):
    fields, fixed, identity = definition(profile)
    rows = []
    for raw in fixed:
        magic, kind, subtype, offset, size, label, flags = ENTRY.unpack(raw)
        rows.append('    {0x50AA, %d, %d, 0x%X, 0x%X, "%s", 0},' %
                    (kind,subtype,offset,size,label.split(b'\0')[0].decode()))
    return ('#pragma once\n#include "agent_partition_store.h"\n'
            'static const alpt_entry alpt_fixed[] = {\n'+'\n'.join(rows)+'\n};\n'
            'static const alpt_policy alpt_config = {0x%X, 0x%X, 0x%X, 0x%X, %d, '
            'sizeof(alpt_fixed)/sizeof(alpt_fixed[0]), alpt_fixed};\n' % fields+
            '#define ALPT_POLICY_ID 0x%Xu\n' % identity)


def verify_bootloader(path, profile):
    marker = struct.pack('<12sII', b'ALPTGUARD01', 1, definition(profile)[2])
    if marker not in Path(path).read_bytes():
        raise ValueError('Dynamic layout requires the matching recovery bootloader')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',required=True)
    parser.add_argument('--header',type=Path)
    parser.add_argument('--reset',type=Path)
    args=parser.parse_args()
    profile=resolve_profiles()[args.profile]
    definition(profile)
    for path,data in ((args.header,header(profile).encode()),(args.reset,b'\xFF'*0x4000)):
        if path is not None:
            path.parent.mkdir(parents=True,exist_ok=True)
            if not path.exists() or path.read_bytes()!=data:path.write_bytes(data)
