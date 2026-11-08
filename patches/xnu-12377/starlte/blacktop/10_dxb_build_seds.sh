#!/bin/bash
# The edits darwin-xnu-build's build.sh (blacktop, MIT) makes to the XNU tree before it builds it (its patches() function,
# the sed lines), so that a tree made by xnu/apply-series.sh builds there with SKIP_PATCHES=1 exactly as the tree it makes itself.
# Run in the tree's root.
set -euo pipefail
[ -f makedefs/MakeInc.def ] || exit 1
python3 - <<'PY'
import re
from pathlib import Path
def edit(path, pairs):
    p = Path(path)
    t = p.read_text()
    for pat, rep in pairs:
        t = re.sub(pat, lambda m: rep(m) if callable(rep) else rep, t, flags=re.M)
    p.write_text(t)
edit('bsd/sys/make_symbol_aliasing.sh', [(r'^AVAILABILITY_PL="\$\{SDKROOT\}/\$\{DRIVERKITROOT\}', 'AVAILABILITY_PL="${FAKEROOT_DIR}')])
edit('libsyscall/Libsyscall.xcconfig', [(r'^#include.*BSD.xcconfig.*', '')])
edit('makedefs/MakeInc.def', [
    (r'^LDFLAGS_KERNEL_SDK\t= -L\$\(SDKROOT\).*', 'LDFLAGS_KERNEL_SDK\t= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel'),
    (r'^(INCFLAGS_SDK[ \t]*=) -I\$\(SDKROOT\)/usr/local/include/(kernel)?', r'\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel')])
edit('makedefs/MakeInc.cmd', [
    (r'export MIG := \$\(shell \$\(XCRUN\) -sdk \$\(SDKROOT\) -find mig\)', 'export MIG := $(shell find $(FAKEROOT_DIR) -name "mig")'),
    (r'export MIGCOM := \$\(shell \$\(XCRUN\) -sdk \$\(SDKROOT\) -find migcom\)', 'export MIGCOM := $(shell find $(FAKEROOT_DIR) -name "migcom")')])
PY
