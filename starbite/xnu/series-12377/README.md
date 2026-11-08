# STARLTE kernel series on xnu-12377.121.6 (iOS 26 / macOS 26.5)

`xnu/series-11417` ported forward to xnu-12377.121.6 (macOS 26.5, `macos-265`
release.json; the iOS 26 kernel line). Port tree: `build/xnu-12377`, a worktree
of `build/xnu-11417` on branch `port-12377`, one commit per patch, replayed with
`tools/port-12377-next.sh` (cherry-picks of the 11417 port tree's commits; conflict
blocks resolved with `tools/conflict.py`). Built by `tools/build-xnu-12377.sh`.

- Staging scripts pinned to macOS 26.5's sources: libpthread-539.100.4
  (1f4f5265), hfs-715.120.4 (124b970f), BearSSL unchanged. All their exact-match
  asserts held on the new sources.
- Not exported, empty on 12377 as on 11417: 0001, 0002, 0005, 0083.
- `0317-port-board-master-configs-12377.patch`: the board MASTER configs three-way
  merged onto 12377's MacOSX config (base = 11417's MacOSX).

Port notes (patches that needed changes; everything else applied unchanged):

- 0004: SWEET added to 12377's longer board list (T6050/T8142, H17.h, APPLEACC8 cache block kept)
- 0006: 12377 links lib<board>.os.<config>.a whenever a board is set (was: only with KDKROOT); the filter for our boards is kept inside the new condition
- 0009: 12377 grew the Apple-SoC-only export list (iommu scratch, aligned buffers, sptm_gen3dart_*); all dropped except _pmap_cache_attributes (open source in arm/pmap/pmap.c)
- 0010: arm_setup_pre_sign() is gone in 12377; mark 1 sits before arm_slide_rebase_and_sign_image
- 0012: 12377 dropped mt_early_init/arm_set_user_tbi here (TBI0 is in the boot TCR); only sweet_early_map_final kept before arm_vm_physmap_init
- 0013: 12377 moved kmem guard objects out of vm_kern.c into the vm map store (vm_map_store.c, ks_rng_ctx, vmgo_*); the SplitMix64 per-CPU RNG now replaces crypto_random_kmem_init/uniform there
- 0014: 12377's write_random hashes with SHA512_* (covered by 0033's BearSSL SHA-512 on our boards); its SHA-256 ccdigest swap dropped, rest of the PRNG kept
- 0065: start.s endif comment now names PV_CTRR too
- 0202: 12377's shared region has a second nested map (config_map); code-signing enforcement is turned off for it as well
- 0238: 12377 moved the realtime CPU choosers to sched_rt.c (pset_choose_processor_for_realtime_thread[_smt]); both fast-core hunks go there, sweet_fast_map is no longer static (extern in sched_rt.c)
- 0310: 12377 canonicalizes the address first (MTE) and passes the flags inline; our hardware-page mask path uses the 9-argument vm_sanitize_addr_size with VM_SANITIZE_FLAGS_SIZE_ZERO_FALLTHROUGH
- 0307 (new 0317): board MASTER configs re-merged onto 12377's MacOSX config
