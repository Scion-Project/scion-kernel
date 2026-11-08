#!/usr/bin/env python3
"""Rewrite xnu/patches/0305 from xnu/src/StarbiteMali.cpp (new file) plus its
iokit/conf/files.arm64 entry."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / 'xnu/src/StarbiteMali.cpp').read_text().splitlines()
name = 'iokit/Kernel/arm/StarbiteMali.cpp'
files = (ROOT / 'tools/ios27/gpu/0305-files.arm64.diff').read_text()
new = (f'diff --git a/{name} b/{name}\nnew file mode 100644\n--- /dev/null\n+++ b/{name}\n'
       f'@@ -0,0 +1,{len(src)} @@\n' + ''.join('+' + l + '\n' for l in src))
(ROOT / 'xnu/patches/0305-starlte-mali-user-client.patch').write_text(new + files)
print('0305 regenerated')
