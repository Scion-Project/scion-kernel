#!/usr/bin/env python3
"""Print a struct's flattened layout (offset, size, member path) from a Mach-O's DWARF, via
dwarfdump: dwarf-struct.py BINARY TYPEDEF [--arch arm64e]. Nested structs/unions are expanded;
bitfields show bit offset/size."""
import re, subprocess, sys
binary, name = sys.argv[1], sys.argv[2]
arch = sys.argv[sys.argv.index('--arch') + 1] if '--arch' in sys.argv else None
args = ['dwarfdump', '--name=' + name] + (['--arch', arch] if arch else []) + [binary]
out = subprocess.run(args, capture_output=True, text=True).stdout
m = re.search(r'DW_AT_type\s+\((0x[0-9a-f]+)', out)
if not m:
    sys.exit(f'{name}: not found')
def die(off):
    a = ['dwarfdump', f'--debug-info={off}', '--show-children'] + (['--arch', arch] if arch else []) + [binary]
    return subprocess.run(a, capture_output=True, text=True).stdout
text = die(m.group(1))
# parse DIEs with indentation
dies = []
for blk in re.split(r'\n(?=0x[0-9a-f]+: +DW_TAG)', text):
    h = re.match(r'(0x[0-9a-f]+):( +)(DW_TAG_\w+)', blk.strip('\n'))
    if not h:
        continue
    attrs = dict(re.findall(r'(DW_AT_\w+)\s+\((.*)\)', blk))
    dies.append((len(h.group(2)), h.group(3), attrs))
base = dies[0][0]
print(f'{name}: size {dies[0][2].get("DW_AT_byte_size")}')
stack = []   # (indent, path, offset)
for ind, tag, at in dies[1:]:
    while stack and stack[-1][0] >= ind:
        stack.pop()
    prefix = stack[-1][1] + '.' if stack else ''
    poff = stack[-1][2] if stack else 0
    if tag == 'DW_TAG_member':
        nm = at.get('DW_AT_name', '"?"').strip('()"')
        off = int(at.get('DW_AT_data_member_location', '0'), 16) if 'DW_AT_data_member_location' in at else 0
        bits = ''
        if 'DW_AT_data_bit_offset' in at:
            bits = f' bit {int(at["DW_AT_data_bit_offset"],0)}:{int(at.get("DW_AT_bit_size","0"),0)}'
            off = 0
        ty = re.sub(r'^0x[0-9a-f]+ ', '', at.get('DW_AT_type', '')).strip('"')
        print(f'  {poff + off:#06x} {prefix}{nm} : {ty}{bits}')
        stack.append((ind, prefix + nm, poff + off))
    elif tag in ('DW_TAG_structure_type', 'DW_TAG_union_type'):
        pass
