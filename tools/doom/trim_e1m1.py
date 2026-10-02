"""Build an E1M1-only IWAD. Preserve Freedoom COPYING/CREDITS with output."""

import struct
import sys


def name8(raw):
    return raw.rstrip(b'\0').decode('ascii', 'replace')


source = open(sys.argv[1], 'rb').read()
magic, count, directory = struct.unpack_from('<4sII', source)
assert magic == b'IWAD'
entries = []
for i in range(count):
    offset, size, name = struct.unpack_from('<II8s', source, directory + i * 16)
    entries.append((name8(name), offset, size, name))

by_name = {name: (offset, size) for name, offset, size, _ in entries}
map_start = next(i for i, entry in enumerate(entries) if entry[0] == 'E1M1')
map_end = map_start + 11
assert [entry[0] for entry in entries[map_start:map_start + 4]] == [
    'E1M1', 'THINGS', 'LINEDEFS', 'SIDEDEFS']

sidedef_offset, sidedef_size = entries[map_start + 3][1:3]
assert sidedef_size % 30 == 0
textures_used = {'SKY1'}
for offset in range(sidedef_offset, sidedef_offset + sidedef_size, 30):
    for at in (4, 12, 20):
        name = name8(source[offset + at:offset + at + 8])
        if name and name[0] != '-':
            textures_used.add(name)

pnames_offset, pnames_size = by_name['PNAMES']
patch_count = struct.unpack_from('<I', source, pnames_offset)[0]
patch_names = [name8(source[pnames_offset + 4 + i * 8:pnames_offset + 12 + i * 8])
               for i in range(patch_count)]
assert 4 + 8 * patch_count <= pnames_size

texture_records = {}
all_texture_names = set()
for lump_name in ('TEXTURE1', 'TEXTURE2'):
    if lump_name not in by_name:
        continue
    offset, size = by_name[lump_name]
    num_textures = struct.unpack_from('<I', source, offset)[0]
    records = []
    for i in range(num_textures):
        relative = struct.unpack_from('<I', source, offset + 4 + 4 * i)[0]
        record = offset + relative
        assert record + 22 <= offset + size
        name = name8(source[record:record + 8])
        count_patches = struct.unpack_from('<H', source, record + 20)[0]
        length = 22 + 10 * count_patches
        assert record + length <= offset + size
        records.append((name, source[record:record + length]))
        all_texture_names.add(name)
    texture_records[lump_name] = records

# Vanilla-compatible engines initialize the full built-in switch table before
# loading the first map, even when E1M1 does not place those switches.
textures_used.update(name for name in all_texture_names
                     if name.startswith(('SW1', 'SW2')))

# Keep both states of switches present in E1M1, plus nearby waterfall frames.
for texture_name in tuple(textures_used):
    if texture_name.startswith(('SW1', 'SW2')):
        partner = 'SW2' + texture_name[3:] if texture_name.startswith('SW1') else 'SW1' + texture_name[3:]
        if partner in all_texture_names:
            textures_used.add(partner)
    if texture_name.startswith(('SFALL', 'WFALL')):
        textures_used.update(name for name in all_texture_names
                             if name.startswith(texture_name[:5]))

patches_used = set()
missing_textures = set(textures_used)
texture_payloads = {}
for lump_name, records in texture_records.items():
    selected = []
    for texture_name, record_data in records:
        if texture_name not in textures_used:
            continue
        selected.append(record_data)
        missing_textures.discard(texture_name)
        count_patches = struct.unpack_from('<H', record_data, 20)[0]
        for p in range(count_patches):
            patch_index = struct.unpack_from('<H', record_data, 22 + 10 * p + 4)[0]
            patches_used.add(patch_names[patch_index])
    payload = bytearray(struct.pack('<I', len(selected)))
    next_offset = 4 + 4 * len(selected)
    for record_data in selected:
        payload.extend(struct.pack('<I', next_offset))
        next_offset += len(record_data)
    for record_data in selected:
        payload.extend(record_data)
    texture_payloads[lump_name] = bytes(payload)

assert not missing_textures, sorted(missing_textures)

all_map_starts = {i for i, (name, _, _, _) in enumerate(entries)
                  if len(name) == 4 and name[0] == 'E' and name[2] == 'M'
                  and name[1].isdigit() and name[3].isdigit()}
keep = []
in_patches = False
drop_reason = {'other_maps': 0, 'audio': 0, 'patches': 0}
for i, entry in enumerate(entries):
    name, _, size, _ = entry
    if name == 'P_START':
        in_patches = True
    if name == 'P_END':
        in_patches = False
    in_other_map = any(start <= i < start + 11 and start != map_start
                       for start in all_map_starts)
    if in_other_map:
        drop_reason['other_maps'] += size
        continue
    if (name.startswith('D_') and name != 'D_E1M1') or name.startswith('DS'):
        drop_reason['audio'] += size
        continue
    if in_patches and size and name not in patches_used:
        drop_reason['patches'] += size
        continue
    keep.append(entry)

output = bytearray(b'IWAD' + b'\0' * 8)
new_entries = []
for name, offset, size, raw_name in keep:
    payload = texture_payloads.get(name, source[offset:offset + size])
    new_entries.append((len(output), len(payload), raw_name))
    output.extend(payload)
directory_offset = len(output)
for entry in new_entries:
    output.extend(struct.pack('<II8s', *entry))
struct.pack_into('<II', output, 4, len(new_entries), directory_offset)
open(sys.argv[2], 'wb').write(output)
print('selected textures', len(textures_used), 'patches', len(patches_used))
print('source', len(source), 'output', len(output), 'lumps', len(new_entries))
print('dropped bytes', drop_reason)
