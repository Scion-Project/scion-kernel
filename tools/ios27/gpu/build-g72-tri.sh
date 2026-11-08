#!/bin/bash
# Build and run the Mali-G72 triangle generator in the lima VM against Mesa
# 25.0.7 (~/mesa/mesa-25.0.7, built in ~/mesa/build with
# -Dgallium-drivers=panfrost), writing ~/mesa/starbite/starlte_gpu_tri.h (the
# VM sees the project read-only; the host copies it to xnu/src/).
#   limactl shell starbite -- bash $HOME/project-starbite/tools/gpu/build-g72-tri.sh
set -euo pipefail
PROJ=$HOME/project-starbite
M=$HOME/mesa/mesa-25.0.7
B=$HOME/mesa/build
OUT=$HOME/mesa/starbite
mkdir -p "$OUT"
LIBS=(src/panfrost/lib/libpan-arch-v6.a src/panfrost/lib/libpan-format-v6.a
	src/panfrost/lib/libpanfrost_lib.a src/panfrost/lib/genxml/libpandecode-arch-v4.a src/panfrost/lib/genxml/libpandecode-arch-v5.a
	src/panfrost/lib/genxml/libpandecode-arch-v6.a src/panfrost/lib/genxml/libpandecode-arch-v7.a
	src/panfrost/lib/genxml/libpandecode-arch-v9.a src/panfrost/lib/genxml/libpandecode-arch-v10.a
	src/panfrost/lib/genxml/libpanfrost_decode.a
	src/panfrost/compiler/libpanfrost_bifrost.a src/panfrost/compiler/libpanfrost_bifrost_disasm.a
	src/panfrost/compiler/valhall/libpanfrost_valhall_disasm.a
	src/panfrost/midgard/libpanfrost_midgard_disasm.a src/panfrost/midgard/libpanfrost_midgard.a
	src/panfrost/util/libpanfrost_util.a src/panfrost/shared/libpanfrost_shared.a
	src/compiler/nir/libnir.a src/compiler/libcompiler.a src/util/libmesa_util.a
	src/util/libmesa_util_sse41.a src/c11/impl/libmesa_util_c11.a src/util/blake3/libblake3.a)
(cd "$B" && ninja "${LIBS[@]}" >/dev/null)
# The flags Meson used for Mesa's own v6 pan_shader.c, minus its -o/-c/-M parts.
FLAGS=$(python3 - "$B" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1] + "/compile_commands.json")):
    if "libpan-arch-v6.a.p/pan_shader.c.o" in e["output"]:
        a = shlex.split(e["command"])[1:]
        out, skip = [], False
        for x in a:
            if skip: skip = False; continue
            if x in ("-o", "-c", "-MF", "-MQ"): skip = True; continue
            if x in ("-MD",) or x.endswith(".c"): continue
            out.append(x if not x.startswith("-I") or x[2:].startswith("/") else "-I" + sys.argv[1] + "/" + x[2:])
        print(" ".join(shlex.quote(x) for x in out)); break
PY
)
# CANVAS=828x1792 (iOS 26, iPhone 11) or 1242x2208 (the default, iPadOS 18's canvas)
CANVAS=${CANVAS:-1242x2208}
eval "gcc $FLAGS -I$M/src/panfrost/lib -Wno-error -DCANVAS_W=${CANVAS%x*}u -DCANVAS_H=${CANVAS#*x}u" \
	"$PROJ/tools/gpu/starlte-g72-tri.c" -o "$OUT/g72-tri" \
	-Wl,--start-group $(printf "$B/%s " "${LIBS[@]}") -Wl,--end-group -lm -lpthread -lstdc++ -lz -lexpat -ldrm
BIFROST_MESA_DEBUG=shaders "$OUT/g72-tri" "$OUT/starlte_gpu_tri.h" 2> "$OUT/g72-tri.log"
tail -3 "$OUT/g72-tri.log"
echo G72_TRI_OK
