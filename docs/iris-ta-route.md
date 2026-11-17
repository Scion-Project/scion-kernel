# The iris scanner through Samsung's own trusted application: a feasibility study (17 November 2026)

The owner asked whether the Galaxy S9's iris scanner could unlock Scion the way it unlocks One UI: the phone's own secure OS (Kinibi)
running under Scion's boot, Samsung's own iris trusted application (TA) loaded from the phone at run time, and Scion asking it to
enrol and to match through its ordinary client interface. This page answers that question. **Nothing here is built and nothing was
run for it: there was no phone.** It was written from Samsung's GPL kernel, the public LineageOS trees, a public file listing of the
S9's stock firmware, public documentation and public community reports. No Samsung binary was disassembled, decompiled or opened.

The limits this study works inside, set by the owner and Claudius: (1) no bypassing, exploiting, patching or tricking of the secure
world (EL3, Kinibi, TZPC, the secure SysMMU, Knox), and no extraction or reverse engineering of Samsung's TAs or closed libraries in
order to defeat their protection; (2) the IR LED's eye-safety limits in Scion's driver are not weakened. Where the route would cross
one of them, this page says so and stops there.

## The verdict

**Unknown until four things are proven, and dead without one decision of the owner's.** Nothing found rules the route out: the
secure OS already starts under Scion, a tripped Knox warranty bit does not by itself stop Samsung's biometric TAs, One UI keeps the iris
enrolment in ordinary user data, and every piece of the secure camera path that the normal world supplies on Android is
described in Samsung's GPL kernel. But **the commands that Samsung's iris daemon sends the iris TA are not published anywhere**, and
neither is the order in which the daemon, the camera service and the TA work together. They can be learned only from Samsung's
closed normal-world code or by watching it run on a One UI phone. If the owner decides that neither may be done, the route is dead
here and nothing else on this page needs doing.

The fact most likely to kill the route is that one. Two more could kill it and are not known yet: the iris TA may accept only a
client that presents Samsung's own daemon's identity (presenting it from Scion would be impersonation, which limit 1 forbids), and
the per-shot IR LED values Samsung's camera service uses may be stronger than Scion's eye-safety limits allow (limit 2). Both can be
found out cheaply and early, in steps 2 and 3 below.

What Scion would get at the end, if all of it works: Samsung's TA answering "this is the enrolled eye" or "it is not", usable only as
the convenience unlock of [BIOMETRICS.md](ios27/BIOMETRICS.md) (option b): it opens the screen, releases no key, and is never presented
to iOS as Face ID. The answer crosses into the normal world as a word that root on the running phone could forge, so it adds
Samsung's matcher and keeps the frames and templates out of the normal world, and nothing more. Its cost is months.

## What must be proven, in this order

Each step can end the route. None of them may be worked around: a refusal from the secure world is the answer.

| # | What | How it would be proven | What it costs | What ends the route |
|---|---|---|---|---|
| 0 | The owner's decisions | [The owner's decisions](#the-owners-decisions), below | a conversation | D1 or D2 refused |
| 1 | Samsung's TAs run commands under Scion | Run `tee_probe=3` and then `tee_probe=4` ([hakios.exynos_tee.c](../drivers/soc/exynos/hakios.exynos_tee.c): built, never run): open and close a TA, then a keymaster HMAC. This is the first Samsung TA command ever made from XNU | two boots of an existing kernel | the secure OS halts, or refuses a session for a reason that is not a bug of ours |
| 2 | Which TA is the iris TA, what it maps, whether it uses RPMB, what the LED does per shot | On a One UI phone of the owner's, after one iris enrolment and one iris unlock, read what Samsung's own GPL kernel already reports: `/sys/kernel/debug/trustonic_tee/last_mcp_commands` and `sessions` (each MCP command with the caller's PID and the TA's UUID; each session's TCI and mapped buffer lengths), the kernel log's `start rpmb workqueue with command(N)` lines (secure RPMB traffic), and its `led current value`, `led pulse_width value`, `IRON1/IRON2` lines (the camera service's per-shot LED values, printed by the LED driver); then the file times under `/data/bio/ir`, `/data/vendor_de/0/irisdata` and `/efs` | an evening by the owner, by hand; root and debugfs on the One UI phone | the LED values exceed Scion's limits (limit 2); or an ordinary unlock writes RPMB or EFS (every unlock would need the owner's go); or enrolment needs EFS and the owner says no |
| 3 | The iris TA lets Scion in | Add the UUID found in step 2 to `tee_allowed[]`, open a session with the public login `tee_probe=3` uses, close it. Nothing else | one boot | the TA refuses a public login and wants an identity that only Samsung's daemon has (presenting it would be impersonation: dead); or `SERVICE_BLOCKED`/`LOCKED` (29/30) |
| 4 | The protocol | Learn the TA's commands and the order of work between the daemon, the camera service and the TA, by whichever way the owner allows in D2. Then write it down in Scion's own words, as `tee_probe=4`'s header does for keymaster | weeks; legal advice first | D2 refused, or what is learned cannot be used under limit 1 |
| 5 | A frame reaches the TA | Make the secure buffer the way Samsung's ION does (below), PREPARE and stream as Samsung's kernel does, and hand the buffer to the TA as Samsung's daemon does (step 4). The TA's own answer about the frame (whatever step 4 says it gives: an eye found, a quality) is the proof; nothing of the frame is ever seen | one to two weeks on top of `iris_s9` and `tee_probe` | the secure world refuses the buffer, or the TA never sees a frame with the LED within Scion's limits |
| 6 | Enrol and match | Enrol the owner's eye, keep what the TA returns in a root-only file under `/private/var` (USERDATA), then match; then the lock path of option (b) | weeks to months, including the lock screen's side that [BIOMETRICS.md](ios27/BIOMETRICS.md) leaves open | the TA's enrolment needs a write the owner refuses |

Steps 1 and 2 are independent and cheap, and either can end the route: do them first, in either order.

## The owner's decisions

- **D1. Loading Samsung's binaries at run time.** The iris TA (and any secure driver it needs) would be read from the phone's own
  VENDOR partition at boot, never copied into a repository, as `tee_probe=3` already does for two TAs. Samsung's licence for its
  firmware was not reviewed here.
- **D2. Learning the protocol.** It is not published. The lawful ways that exist, to be decided by the owner with advice, not here:
  (a) observing the normal-world traffic on the owner's own One UI phone while it runs (the MCP log above shows which TA and which
  buffers; the TCI's contents would need a build of Samsung's GPL kernel for that phone that logs them on each notification, and its
  V4L2 calls for the camera side); (b) reading Samsung's closed normal-world iris libraries (`irisd`, `libIrisTlc.so`, `iris.default.so`)
  for interoperability, as was done once already for keymaster's client (`libkeymaster_helper_vendor.so`, per `tee_probe=4`'s header).
  Neither touches the TA itself. In the EU, Directive 2009/24/EC lets a lawful user observe, study and test a program while running
  it (Article 5(3)) and allows decompilation only where indispensable for interoperability, under conditions (Article 6), one of which
  limits passing the information to others; the project's repositories are public, which is exactly the question to ask. In the US the
  nearest rule is 17 U.S.C. 1201(f). Samsung's end-user licence may say more. This study did neither (a) nor (b).
- **D3. Any work on a One UI phone** (step 2, and (a) of D2): root on it, debugfs, and whichever phone it is. Agents never touch a
  phone; this is done by the owner by hand.
- **D4. Each EFS or RPMB touch**, if step 2 shows the iris TA needs one. What is known now: on One UI the iris enrolment is kept
  under `/data` (USERDATA), which rule 4 allows; nothing found says it uses EFS; whether it uses RPMB is not known (step 2 shows it).
- **D5. Keeping the iris camera's driver on the path.** Today `iris_s9` is a probe. This route would make it stream into a secure buffer
  for the TA. Its eye-safety limits stay as they are (limit 2): if the TA needs more light than they allow, the route ends.

## The risks

- **It may simply not be possible within the limits.** Steps 2 to 4 each have a plausible "no".
- **The protocol work is the bulk of the cost**, and what it yields is a convenience unlock that a printed eye has fooled before (the
  Galaxy S8's, by the Chaos Computer Club in May 2017) and that root can forge.
- **Eye safety.** Samsung's kernel does not clamp the LED's per-shot current or pulse width
  (`drivers/leds/leds-s2mpb02.c` 727-765 write what they are given); the camera service's values are the only bound on One UI, and
  they are not known. Scion's limits stay; nothing in this route raises them.
- **Writes the owner does not want.** If the iris TA keeps a counter in RPMB (anti-hammering, rollback), every failed match may write
  it. Scion has never registered the secure RPMB buffer, so today the secure world cannot ask Scion for RPMB at all (inference).
- **Legal**: D2.
- **A hang or a halted secure OS** while a TA runs. `tee_probe` already has a watcher that panics and lets the watchdog restart the
  phone into TWRP; the same would be needed here.

## The evidence

Samsung's kernel is `android_kernel_samsung_exynos9810`, branch `lineage-24.0`, commit `e5906667` (GPL; read for facts, nothing
copied). "The stock listing" is the public file listing, init scripts and SELinux policy of the S9's stock firmware G960FXXUHFVG4
(Android 10) at github.com/Exynos7870-labs/samsung_starlte_dump: only names, init scripts and policy text were read. "LineageOS's
trees" are Scion-Project's copies of `android_device_samsung_exynos9810-common` and `proprietary_vendor_samsung_exynos9810-common`
(branch `lineage-24.0`) and LineageOS's `android_hardware_samsung` (branch `lineage-23.2`). What a source states is marked by its
citation; what is inferred says "inference".

### 1. Knox, the unlocked boot loader, and what still works

What the sources state:

- Samsung's Knox documentation: rooting trips the warranty bit, and "Knox services do not run on devices that have tripped the
  Warranty bit" (docs.samsungknox.com, *Knox Platform for Enterprise FAQ*). Community reports agree that Samsung Pay, Secure Folder and
  Samsung Pass stop working once it is tripped, and that from the Galaxy S8 on Samsung Pass refuses even where earlier phones did not
  (XDA, "Samsung pass on Knox tripped devices", thread 3687977; Samsung Members community answers on "WARRANTY VOID: 0X1").
- **Kinibi starts under Scion.** On one S9, `tee_probe=2` got MC_FC_INIT x1=0 x2=1, then state 2 (initialised) with MCI version
  0x10006 after ten N_YIELDs ([hakios.exynos_tee.c](../drivers/soc/exynos/hakios.exynos_tee.c), header). No TA has been opened under
  Scion yet: `tee_probe=3` and `4` are built and not run.
- **Samsung's biometric TAs run with an unlocked boot loader and a custom OS.** LineageOS for the S9 has a working fingerprint
  reader through Samsung's own stack: its HAL (`android_hardware_samsung`, `aidl/fingerprint/LegacyHAL.cpp` 20-57) loads Samsung's
  closed `libbauthserver.so` and calls its `ss_fingerprint_*` functions; the vendor tree carries that library, `libMcClient.so`,
  `mcDriverDaemon` and the TAs under `/vendor/app/mcRegistry` (`proprietary-files.txt` 30-153), and LineageOS's `mobicore.rc` starts
  the daemon as stock does. LineageOS phones are unlocked and Knox-tripped by construction.
- **Iris unlock on stock One UI after rooting**: first-hand reports say it keeps working. "Knox tripped (0x1) But the Fingerprint,
  Irisscanner and samsung account works" (XDA, "How to | Galaxy S9 (SM-G960F & F/DS) Root Android PIE", thread 3893469); "S health and
  iris scanner work perfect now after root" (Galaxy S8, XDA thread 3657455); "I had my Note 7 rooted. The iris scanner still worked.
  Just not Samsung Pay" (XDA thread 3588194). One report in that last thread says the opposite: after rooting, iris enrolment
  scanned the eye and then did not save it. All of these are from 2017 to 2019, on Android 7 to 9.
- **No custom ROM for the S9, S9+ or Note9 has a working iris scanner.** LineageOS's trees carry nothing of it; Samsung-ported ROMs
  report it broken (Reddit r/GalaxyS9, "Galaxy S9 hardware support", 2024: Noble ROM, a One UI 6 port, "Iris scanner does not work");
  users asking for one were told none exists (Reddit r/androidroot, "New phone or S9? LineageOS 18.1 with Iris Scanning?"). One unsourced
  comment says it can be made to work "if I flash some more things"; nothing backs it.

What follows from it, by inference:

- A tripped warranty bit does not by itself stop Kinibi, Samsung's system TAs, or the fingerprint TA, and on stock firmware it did
  not stop iris unlock (moderately strong: several first-hand reports, one contrary). Samsung Pass refusing is an app-level policy of
  Samsung Pass, not a refusal of the iris TA, since unlock kept working on the same phones.
- Scion boots the way a Magisk-rooted stock phone does as far as the secure world can tell: Samsung's unlocked S-Boot loads a boot image
  Samsung did not sign (the S9 repository's README, "S-Boot (Samsung's, unlocked) --> boot image on BOOT --> HakiOS UEFI --> ...").
  So whatever boot state the secure world is told is of the same kind. What is different, and might matter, is not known: Scion runs its
  own hypervisor at EL2 (Carbon) in place of whatever runs there on stock, and no Android framework is running.
- **No evidence was found that the iris TA or the secure camera path checks the warranty bit or a verified-boot state**, and none that it
  does not beyond the reports above. That is weak in both directions; step 3 settles it for opening, step 6 for enrolment (the contrary
  report was about enrolment).

### 2. What starts Kinibi and what a complete client is

From Samsung's MobiCore (Trustonic "t-base") driver, `drivers/gud/gud-exynos9810/MobiCoreDriver/` (build tag
`t-base-EXYNOS64-Android-400C-V005`, `build_tag.h`); the start sequence, the queues and the MCP buffer are summarised in
`hakios.exynos_tee.c`'s header, which was written from the same files:

- **Fast calls and scheduling.** MC_FC_INIT (0xFF000001) gives the secure OS the MCI block; MC_FC_INFO (0xFF000002) reads its state;
  N_SIQ (4) tells it there is work; N_YIELD (3) gives it time. The secure OS runs only when the normal world yields to it; Samsung's
  worker does that on core 0 while MCP's flags say "not idle" (`scheduler.c`, `fastcall.c`). The secure OS's interrupt to the normal
  world is SPI 233 (the device tree's `tee` node, line 10339-10342); `tee_probe` polls instead.
- **MCP**, the command channel to the secure OS (`mci/mcimcp.h` 106-126): OPEN_SESSION 1, CLOSE_SESSION 3, MAP 4, UNMAP 5, SUSPEND 6,
  RESUME 7, GET_MOBICORE_VERSION 9, CLOSE_MCP 10, LOAD_TOKEN 11, CHECK_LOAD_TA 12; answers 0 to 51, among them
  SERVICE_BLOCKED 29, SERVICE_LOCKED 30, SERVICE_KILLED 31 and DOWNGRADE_NOT_AUTHORIZED 32 (60-92). A TA session is a TCI (a buffer
  shared with the TA) plus up to some mapped buffers (MAP, up to 1 MB each, `MCP_MAP_MAX`, 347), and notifications in both directions.
- **The daemon's part.** In this Kinibi the kernel asks the daemon (`mcDriverDaemon`, through `/dev/mobicore`) for a TA's bytes and, for
  an SP trustlet only, for its root, SP and trustlet containers (`admin.c` 392-483, 541-581; the containers are fetched only when the
  TA's header says SP trustlet, 666-694). Samsung's TAs have system UUIDs (`ffffffff...`), and `tee_probe` checks each file's MCLF header
  for the system service type before loading it, so no container is needed for them (inference from those two facts). The daemon also
  loads secure drivers at start: stock's `mobicore.rc` passes it four drivers and TAs with `-r` (`FFFFFFFF...01.drbin`, `ffffffffd0...0a`,
  `...16`, `...1c`, `...45.drbin`), and sets `MC_AUTH_TOKEN_PATH` to `/efs` and makes `/efs/TEE` (stock listing,
  `vendor/etc/init/mobicore.rc`). The fourth (`ffffffffd0000000000000000000001c.tlbin`) is not in the stock listing's
  `vendor/app/mcRegistry`, and LineageOS's copy of the script drops it. What each of these is was not determined (names only).
- **Client identity.** A session carries a login. Public and kernel logins carry nothing; user, group and application logins carry the
  caller's uid, a gid it holds, or a SHA-1 of its executable's path, all computed by the normal-world driver (`session.c` 291-376). The
  TA can only take the normal world's word for them. Whether the iris TA asks for one is not known (step 3).
- **Secure storage and RPMB.** Samsung runs its own `secure_storage_daemon` (stock `secure_storage_daemon_kinibi.rc`, storage under
  `/data/vendor/secure_storage`); the iris daemon's policy gives it no access to it (section 3). RPMB is reached by the secure world
  through the normal world: the kernel registers a buffer with SMC_SRPMB_WSM 0x82003811 and an interrupt (SPI 460, the device tree's
  `ufs-srpmb`, 10349-10352), and on each interrupt carries out GET_WRITE_COUNTER (1), WRITE_DATA (2) or READ_DATA (3) as SCSI security
  protocol commands to the UFS (`drivers/scsi/scsi_srpmb.c` 93-226, 348; `scsi_srpmb.h` 16-18), logging the command number each time.
- **A debug view that names what runs.** Samsung's driver publishes `/sys/kernel/debug/trustonic_tee/` with `sessions`, `structs` and
  `last_mcp_commands`, the last a table of recent MCP commands with the caller's PID, the session, the result and the TA's UUID
  (`main.c` 593-596, 647; `mcp.c` 847-860). That is what step 2 reads on One UI.

**What an XNU client must implement, and how big it is.** Samsung's driver is about 10,000 lines of C, and it leans on a closed
daemon. `hakios.exynos_tee.c` (1,272 lines) already has the start, the queues, MCP open, close and MAP, the TA's bytes
from VENDOR with the header check, one TCI session with notifications, a hang watcher and a core check. For the iris TA it would also
need, as far as can be told without the protocol: several mapped buffers per session and their unmapping; secure drivers loaded at start
if the iris TA uses one (unknown); the secure-OS time kept current; probably the SPI 233 interrupt rather than a 1 ms poll, for latency;
the secure RPMB service only if step 2 shows RPMB traffic, and then with writes refused unless the owner allows them; and the
session's work for the iris TA itself (step 4). An estimate, not a measurement: two to four thousand more lines.

### 3. The iris TA and its clients on One UI (names and structure only)

From the stock listing:

- **The daemon and libraries are in `/system`, not `/vendor`**: `system/bin/irisd`, `system/lib64/libIrisTlc.so` (the name Samsung gives
  its TEE client libraries, "TLC"), `libIrisService.so`, `libIrisSensorListener.so`, `libIrisLibrary_preview.so`,
  `system/lib64/hw/iris.default.so`, the privileged apps `SecIrisService` and `IrisUserTest`, and the feature
  `com.samsung.android.camera.iris` (`etc/permissions/irisservice.xml`). `irisd` is started by the vendor's `init.samsungexynos9810.rc`
  (1109-1113) as user `system`. LineageOS replaces `/system`, which is why none of it is there.
- **Its SELinux policy** (`vendor_sepolicy.cil`): `irisd` may open `/dev/mobicore-user` (10202), so it is the TA's client itself; it may
  use file descriptors from the camera HAL (10201) and from `cameraserver`, `mediaserver` and `surfaceflinger`, so the frame buffers are
  allocated elsewhere and passed to it; it may open `/dev/ion` (10189) and read `/dev/s5p-smem` (Samsung's `secmem`, 14708); it reads
  and writes `/data/bio/ir` (`iris_data_file`, 10175; `vendor_file_contexts` 397) and `/data/vendor_de/N/irisdata` exists
  (`plat_file_contexts` 164); it adds authentication tokens to keystore (10187). It has no rule for RPMB, EFS, the secure storage daemon,
  or `/dev/video*`. The camera HAL has no rule for `/dev/mobicore-user`.
- **`secmem`'s one call that matters** (`drivers/soc/samsung/secmem.c` 163-200): SECMEM_IOC_GET_FD_PHYS_ADDR takes an ION buffer's file
  descriptor and returns its physical address.
- **Which TA it is** is not determinable from names. The stock `/vendor/app/mcRegistry` has 30 files; LineageOS keeps 25 of them, and the
  five it drops are `00060308060501020000000000000000`, `ffffffff...0f`, `...2e`, `...3e` (keymaster, per `tee_probe`) and `...59`.
  `tee_probe`'s list calls `ffffffff...60` "the bio submodule". Step 2 settles it in one read.
- **What the TA stores at enrolment, and where.** Samsung: the iris information "is stored as an encrypted code" and "encrypted and
  stored safely in the hardware protected by the Knox platform" (Samsung Newsroom, "[In-Depth Look] Samsung's Biometric Technologies...
  Galaxy S8", 30 March 2017; also: only one person can register). From the policy, the daemon is the one with the files, so the inference
  is that the TA returns a blob sealed with a key that never leaves the secure world and `irisd` keeps it under `/data/bio/ir`. Whether the
  TA also keeps anything in RPMB is not known.

Inferred from all of the above, to be confirmed by step 2 and step 4: the camera HAL opens the iris sensor in the secure scenario, which
makes Samsung's kernel call PREPARE before it powers the sensor ([iris-camera.md](iris-camera.md), section 1), allocates protected
buffers from the `secure_camera` ION heap and queues them to the sensor's video node, so the kernel registers each with the secure world
(section 4 here); it passes the buffers' descriptors to `irisd`, which asks `secmem` for their physical addresses and gives those to the
TA through its TCI or a mapped buffer; the TA reads the frames in place. Exposure, gain and the LED are set by the camera HAL through
V4L2 controls. The protocol between `irisd` and the TA, and the timing among the three, are not public: no source describes them, no
custom ROM has implemented them, and no public research on them was found (searches for `libIrisTlc` and `irisd` with "trustlet"
found nothing). The public research on Samsung's Kinibi (Quarkslab's "A Deep Dive Into Samsung's TrustZone"; Lapid and
Wool, ESORICS 2018) covers the architecture and keymaster, not iris.

### 4. What Scion would have to supply that Android supplies

- **Protected buffers, made as ION makes them.** An allocation from the `secure_camera` region (the device tree's 30 MB at 0xE1900000,
  `ion,untouchable`, heap id 11; DTS 81-86) is described to the secure world by a `struct ion_buffer_prot_info` { chunk_count 1,
  dma_addr, flags = the heap id, chunk_size, bus_address = the physical address } (`ion_priv.h` 111-117; `ion_carveout_heap.c` 127-130),
  given a secure IOVA from 0x80000000-0xE0000000 (`exynos_ion.c` 35-36, 146-187) and handed over with SMC_DRM_PPMP_PROT 0x82002110 and
  the descriptor's physical address; the answer must be DRMDRV_OK (`exynos_ion.c` 212-247; `include/linux/smc.h` 62). Freeing is
  SMC_DRM_PPMP_UNPROT 0x82002111. The camera's DMA is then given the secure IOVA, not the physical address. That the secure world maps the
  IOVA to the buffer in the secure SysMMU context from this call is inference (it is told both numbers and nothing else does it). Scion
  would also have to keep the region out of XNU's memory; whether its loader does today was not checked.
- **The secure SysMMU** needs nothing more from the normal world as far as the sources show; its secure frame is the secure world's
  ([iris-camera.md](iris-camera.md), section 3).
- **The sensor's power and PREPARE** in Samsung's order: PREPARE first, then MCLK, the power table, the CSI and the sensor; UNPREPARE
  after power-off. `iris_s9` with `iris_s9_smc=1` already does this, and the phone answered (iris-camera.md, "What the phones showed").
- **The IR LED's per-shot values.** Set by the camera HAL through four V4L2 controls (`fimc-is-video-sensor.c` 696-723:
  `V4L2_CID_IRLED_SET_WIDTH`, `_DELAY`, `_CURRENT`, `_ONTIME`), not by the TA as far as the sources show. Not public; the LED driver
  prints them as they are set (`leds-s2mpb02.c` 733, 752, 757), which is how step 2 learns them without opening anything closed. Scion
  keeps its own limits whatever they are.
- **A file system for the enrolment**: a root-only file under `/private/var`, in USERDATA, which rule 4 allows. Nothing found needs EFS:
  `/efs/TEE` and `MC_AUTH_TOKEN_PATH` serve the daemon's tokens for SP trustlets (inference), and `irisd` has no EFS rule.
- **RPMB**, only if step 2 shows the iris TA uses it. A read-only variant is possible in principle: serve GET_WRITE_COUNTER and READ_DATA
  and refuse WRITE_DATA; whether the TA would then enrol or match at all is unknown. A RAM-only variant of enrolment is possible if the TA
  keeps nothing but the blob it returns: keep the blob in memory and enrol at each boot.

### 5. What Scion would get

- **A yes or no from Samsung's matcher**, delivered to XNU over the TA's normal interface. Under option (b) of
  [BIOMETRICS.md](ios27/BIOMETRICS.md): a convenience unlock, refused before the passcode has been entered since boot, never releasing
  a key, never presented to iOS as Face ID.
- **Liveness and spoofing.** Samsung's public material describes the iris pattern, the IR LED and encrypted storage; no specific
  liveness claim for the S8 or S9 iris scanner was found. The Chaos Computer Club fooled the Galaxy S8's with an infrared photograph of an
  eye and a contact lens on it (ccc.de, "Chaos Computer Clubs breaks iris recognition system of the Samsung Galaxy S8", May 2017).
  Samsung allowed iris, but not Intelligent Scan or face alone, for Samsung Pay on the S9 (CNET, "Unlocking the Galaxy S9 with Intelligent
  Scan? It's easy, but maybe not so secure", 2018).
- **What it adds over Scion's own front-camera matcher**: infrared, and frames and templates that never enter the normal world. What it
  does not add: any protection of data, since the answer is a word in normal memory.

## What could not be found out

- Which `mcRegistry` file is the iris TA, and whether it needs a secure driver (step 2 answers both).
- The iris TA's commands, its TCI layout and the order of work among `irisd`, the camera HAL and the TA (D2).
- Whether the iris TA accepts a public login (step 3).
- Whether the iris TA uses RPMB, at enrolment or at each match (step 2).
- The camera service's per-shot LED values (step 2), and the exposure and gain it uses.
- Whether Android 10 firmware (G960FXXUHFVG4) behaves after a Knox trip as the 2017-2019 reports describe.
- Whether anything in the secure world notices that no Android, and a different EL2, is running.

## Sources

- Samsung's GPL kernel (`lineage-24.0`, `e5906667`): `drivers/gud/gud-exynos9810/MobiCoreDriver/` (`admin.c`, `build_tag.h`, `main.c`,
  `mcp.c`, `scheduler.c`, `fastcall.c`, `session.c`, `mci/mcimcp.h`, `public/mc_admin.h`), `drivers/soc/samsung/secmem.c`,
  `drivers/staging/android/ion/exynos/exynos_ion.c`, `ion_carveout_heap.c`, `ion_priv.h`, `include/linux/smc.h`,
  `include/linux/exynos_ion.h`, `drivers/scsi/scsi_srpmb.c` and `.h`, `drivers/leds/leds-s2mpb02.c`,
  `drivers/media/platform/exynos/fimc-is2/fimc-is-video-sensor.c`, `arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts`.
- The stock firmware's public listing (github.com/Exynos7870-labs/samsung_starlte_dump, branch
  `starltexx-user-10-QP1A.190711.020-G960FXXUHFVG4-release-keys`): the file list, `vendor/etc/init/mobicore.rc`,
  `secure_storage_daemon_kinibi.rc`, `init.samsungexynos9810.rc`, `vendor/ueventd.rc`, `system/etc/permissions/irisservice.xml`, the
  file contexts and `vendor_sepolicy.cil`. Names and policy only.
- LineageOS: Scion-Project's `android_device_samsung_exynos9810-common` (`configs/init/mobicore.rc`, `proprietary-files.txt`,
  `common.mk` (106: the fingerprint HAL), `manifest.xml`, `sepolicy/vendor/tee.te`) and
  `proprietary_vendor_samsung_exynos9810-common` (file list);
  LineageOS's `android_hardware_samsung`, `aidl/fingerprint/LegacyHAL.cpp`.
- This repository: [hakios.exynos_tee.c](../drivers/soc/exynos/hakios.exynos_tee.c), [iris-camera.md](iris-camera.md),
  [BIOMETRICS.md](ios27/BIOMETRICS.md); the S9 repository's README and `CAMERA.md`.
- Samsung: Knox Platform for Enterprise FAQ (docs.samsungknox.com); Samsung Newsroom, 30 March 2017 (above).
- Community: XDA threads 3893469, 3657455, 3588194, 3766325, 3687977; Reddit r/GalaxyS9 "Galaxy S9 hardware support" and r/androidroot
  "New phone or S9? LineageOS 18.1 with Iris Scanning?"; CCC, May 2017; CNET, 2018; Quarkslab, "A Deep Dive Into Samsung's
  TrustZone"; Lapid and Wool, ESORICS 2018.
- Law, named only for the owner's question: Directive 2009/24/EC, Articles 5(3) and 6; 17 U.S.C. 1201(f).
