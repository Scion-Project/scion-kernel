#!/usr/bin/env python3
"""Rewrite xnu/patches/0301 so its new-file hunks match xnu/src/starlte_gpu*.{c,h}."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'xnu/patches/0301-starlte-mali-g72-probe.patch'


def hunk(name):
    src = (ROOT / 'xnu/src' / name).read_text().splitlines()
    return (f'diff --git a/bsd/dev/{name} b/bsd/dev/{name}\nnew file mode 100644\n'
            f'--- /dev/null\n+++ b/bsd/dev/{name}\n@@ -0,0 +1,{len(src)} @@\n'
            + ''.join('+' + l + '\n' for l in src))


text = PATCH.read_text()
parts = text.split('diff --git ')
keep = ['diff --git ' + p for p in parts[1:] if not p.startswith('a/bsd/dev/starlte_gpu')]
PATCH.write_text(hunk('starlte_gpu.c') + hunk('starlte_gpu_tri.h') + ''.join(keep))
print(PATCH.name, 'regenerated')
