#!/usr/bin/env python3
"""Resolve git conflict blocks in a file.
  conflict.py FILE                 list blocks (index, ours, theirs)
  conflict.py FILE N MODE [TEXT]   MODE: ours | theirs | both (ours then theirs)
                                   | text (TEXT, '-' = stdin) ; N = 'all' allowed
"""
import re, sys
path = sys.argv[1]
src = open(path).read()
pat = re.compile(r'^<<<<<<< [^\n]*\n(.*?)^(?:\|\|\|\|\|\|\| [^\n]*\n.*?)?^=======\n(.*?)^>>>>>>> [^\n]*\n', re.S | re.M)
blocks = list(pat.finditer(src))
if len(sys.argv) == 2:
    for i, m in enumerate(blocks):
        print(f'--- [{i}] ours:\n{m.group(1)}--- [{i}] theirs:\n{m.group(2)}')
    sys.exit(0)
which, mode = sys.argv[2], sys.argv[3]
idx = range(len(blocks)) if which == 'all' else [int(which)]
out, last = [], 0
for i, m in enumerate(blocks):
    out.append(src[last:m.start()])
    if i in idx:
        o, t = m.group(1), m.group(2)
        if mode == 'ours': out.append(o)
        elif mode == 'theirs': out.append(t)
        elif mode == 'both': out.append(o + t)
        elif mode == 'text':
            txt = sys.stdin.read() if sys.argv[4] == '-' else sys.argv[4]
            out.append(txt if txt.endswith('\n') or not txt else txt + '\n')
    else:
        out.append(m.group(0))
    last = m.end()
out.append(src[last:])
open(path, 'w').write(''.join(out))
left = len(pat.findall(''.join(out)))
print(f'{path}: {left} conflict(s) left')
