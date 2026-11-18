# The re-pinned runtime's patches (`build-m2v-bridge.sh` with `M2V_REPIN=1`; opt-in, not the default)

Applied in this order to github.com/steelbrain/metal2vulkan at `46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71` (branch
`development`, 17 November 2026), the revision UniXNU's `scripts/data/gpu-metal-vulkan-inputs.json` names:

| file | from | what |
|---|---|---|
| `0063-main-optimizations-on-development-46e7cc96.patch` | UniXNU/AdrenoMetal `bridge/patches/` at `cc0a4e8e57d2` (18 Nov), unchanged | AdrenoMetal's consumer patch, rebased by UniXNU onto 46e7cc96 (uniform descriptor capacity, native arithmetic, cache storage and tracing) |
| `0064-copies-leave-unreachable-pages-alone.patch` | the same, unchanged | copied host buffers with unmapped pages are skipped |
| `0105-sweet-ios-read-pages.patch` | ours | 0064's `read_pages` (the first read of a copied buffer's pages through `mach_vm_read_overwrite`) on iOS too: it exists for macOS, Linux and Windows only, and the runtime does not build for `aarch64-apple-ios` without it |

UniXNU's `scripts/build-adreno-metal-bridge.py` applies exactly 0063 and 0064 on this revision. AdrenoMetal is MIT
(NativeMetal contributors). These files are patches to metal2vulkan and are under metal2vulkan's licence,
GPL-2.0-or-later OR LGPL-3.0-or-later, as the code they change, not MIT (NOTICE, THIRD_PARTY_LICENSES.md).

**Our patches of the default pin (`../patches/`, on 720ef1ce) are not carried here yet.** Cherry-picked onto
46e7cc96 + 0063 + 0064 on 18 November:
- 0002 is already in.
- The other 19 conflict. Upstream moved or reworked the code they touch: `render/bound.rs`, `render/executions.rs`, rebinds,
  the linear plan.
- Where upstream has its own form, the bridge uses it under `--cfg m2v_repin`:
  - 0008/0016/0101 (padded rows in place) → `DevicePolicy::host_page_textures = InPlaceWrites`, as AdrenoMetal sets it;
  - 0013's `refuse_rebinds` → no counterpart (rebinds were reworked upstream);
  - 0103's counters → left out.
- Still to be checked one by one against 46e7cc96, and re-based where upstream does not already do the same:
  - correctness: 0001, 0006 (draws without a pipeline), 0007/0015 (two-plane surfaces), 0100 (8 MiB memory blocks),
    0102 (linked specialized names);
  - start-up: 0003, 0005 (priming, module decodes);
  - per-draw CPU: 0009, 0010, 0011, 0012, 0014 (keep final pass, fewer allocations, kept draw commands/lists, tracker keys).

  Upstream's own per-draw work since 720ef1ce (6,158 commits: kept and carried draws, one state lock per encoder,
  continued render-pass instances, encoders recorded into one VkCommandBuffer) may already cover several of them.
