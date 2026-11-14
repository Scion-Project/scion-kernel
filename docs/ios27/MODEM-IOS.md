# The Galaxy S9's modem on iOS 27: what iOS needs for SIM, carrier and bars

Written 13 November 2026. This is a research note and plan. Nothing here is built yet, except the kernel side under
"What the kernel gives today". Facts about iOS come from public documentation and published research. No Apple
binary, offset or table is in this file.

## What the kernel gives today (scion-kernel `drivers/modem/hakios.shannon.c`, all off by default)

| Boot-arg | What it does |
|---|---|
| `cp=1`…`cp=7` | The S9's Shannon CP: loaded from RADIO, checked by the secure world, started, linked. Then low power mode, the SIM's state, normal mode (registration), one data context (`cp_apn=`), and three test packets through it |
| `cp_nv=2` | The CP's non-volatile data read at boot from the EFS partition, `/nv_data.bin`, through a read-only ext4 reader (`hakios.ext4ro.h`). It is kept in RAM only. EFS is never mounted and never written; only its size and a CRC prefix are logged |
| (with `cp_nv=2`) | The CP's file requests for its NV data (RFS NV read/write) are answered from that RAM copy. Its writes stay in RAM for the run and are never written back |
| `cp_if=1` | The active data context becomes `pdp_ip0`: IPv4, point to point, with the context's address on it. The context stays up until `sysctl kern.hakios.cp.go=2` |
| `cp_route=1` | An IPv4 default route through `pdp_ip0`, if there is none yet |
| `cp_if_kb=N` | A per-boot byte budget on `pdp_ip0`, 2048 KB by default |
| `cp_rfs=1` | With `cp_nv=2`: the CP's file opens (RFS command 0x11, e.g. `CP_OMC_AUDIO_SLSI.bin`) are answered "not there" (handle -1, errno 2) from RAM. Nothing is read or opened |
| (always) | `kern.hakios.cp.info`: one line for user space (stage, registration, technology, bars, dBm, operator code, SIM, data, bytes used, budget, a change counter). `kern.hakios.cp.addr` (root only): the context's address and DNS servers |
| (with `cp_if=1`) | Idle polling in the steady state (1 ms while anything moves, 10 ms after 0.3 s quiet, 50 ms after 5 s); a lost registration gets 10 minutes to come back; the modem is put in low power and reset on an iOS shutdown or restart (xnu `PEHaltRestartInternal`) |

User space (`tools/ios27/modem/`, built by `build.sh`, installed by `66-celld.sh`):

| Piece | What it does |
|---|---|
| `scion-celld` | Root daemon (job `com.scion.celld`). While the context is up it publishes `State:/Network/Service/<id>` (`PrimaryRank` Last), `.../IPv4` (address, `InterfaceName` pdp_ip0, `Router` = the address) and `.../DNS` (the context's servers) to configd; withdraws them when it goes. Exits at once on a boot without the modem |
| `scion-bars` (`c.dylib`) | Injected into SpringBoard (`sweetinject=...;/usr/lib/c.dylib,SpringBoard`), on only while `/private/var/local/scion-bars` exists: the status bar's cellular entry (bars, the network's name) from `kern.hakios.cp.info` |
| `scion-fetch`, job `com.scion.celltest` | Test roots only: one https GET by name through iOS's own stack, logged with configd's view and the routes |

Seen on phone 1 (SM-G960F, GoMo SIM on eir, 272-03), 13 Nov 2026:
- SIM ready, registered HOME.
- The NV data came from EFS, and the CP's NV writes were answered.
- `pdp_ip0` came up with the context's address and a default route, and packets went both ways from user space.

## What iOS has on top of a baseband, and why none of it sees ours

- **CommCenter** (CoreTelephony's daemon) owns the baseband. It finds the modem through IOKit services that Apple's
  baseband drivers publish. On an iPhone 11 (the identity this port uses, iPhone12,1) that is an Intel XMM modem over
  PCIe, spoken to in Apple's ARI protocol ("Apple Remote Invocation"; reverse-engineered in public by the ARIstoteles
  project, Kröll et al., WiSec 2021).
- CommCenter does everything above the modem: SIM handling, registration, the operator's name (from the SIM, the
  network and the carrier bundles), signal bars, data contexts, the cellular network service and its DNS, and SMS
  and calls.
- **Clients** (SpringBoard's status bar, Settings > Mobile Service, Preferences) ask CommCenter over private XPC
  (CoreTelephony's `CTServerConnection`, the `com.apple.commcenter.*` services). They never see the modem.
- **Data on iOS:** CommCenter brings a context up, configures `pdp_ipN`, and registers a cellular network service with
  configd (the IPv4 and DNS state that IPMonitor turns into the routing table and the resolver). Without that service,
  a kernel route through `pdp_ip0` carries traffic, but iOS's resolver has no DNS server for it, and iOS's own
  reachability does not treat cellular as up.

Our CP speaks Samsung IPC over SIPC5 rings, not ARI. CommCenter finds no baseband it knows, so iOS shows no SIM, no
carrier and no bars.

## The ways to the iOS side, in order of size

1. **DNS and "cellular is up" for user space (small; next).**
   - A root daemon in the package-manager prefix reads the kernel's modem state (new read-only sysctls:
     registration, the context's address and DNS servers, signal).
   - It publishes a network service for `pdp_ip0` into configd's dynamic store: `State:/Network/Service/<id>/IPv4`
     (address, interface `pdp_ip0`, router) and `.../DNS` (the context's DNS servers).
   - IPMonitor then makes the default route and the resolver as it does for any service. The kernel's own default
     route (`cp_route=1`) is no longer needed.
   - Wi-Fi keeps priority through service order (IPMonitor's rank).
   - Gives: names resolve, and every app's traffic goes over cellular when Wi-Fi is off.
   - Does not give: bars or a carrier name.
2. **Bars and carrier name in the status bar (medium; cosmetic).**
   - The port already injects dylibs into SpringBoard and backboardd (`sweetinject`).
   - A small injected library in SpringBoard would feed the status bar's cellular entry (signal bars, operator name,
     "4G"/"LTE") from the same sysctls: the registration state, the technology (33 = LTE seen), the signal reports,
     and the network's name from the CP's operator notification (not decoded yet).
   - Settings would still say "No SIM": that comes from CommCenter.
   - This touches only our own injected code. No Apple binary changes.
3. **A real CoreTelephony baseband (large).** Either:
   - a shim that answers CommCenter as an ARI baseband would (the IOKit services, the ARI messages for SIM,
     registration, signal and data) and translates them to Samsung IPC. This needs ARI's message set for the
     iPhone12,1 firmware generation.
   - or a replacement for CommCenter's XPC services. The private interface is large.

   Either one gives SIM in Settings, the operator from the carrier bundle, bars, and data through iOS's own
   path. Weeks of work, with real risk. Do not start without the owner's go.

What stays out of every path:
- No IMEI or NV change.
- No unlock of any kind.
- No calls or SMS without the owner's explicit say-so.
- EFS read-only.
- No Apple binary patched.

## Next concrete steps

1. Kernel: expose read-only sysctls `kern.hakios.cp.{reg,tech,signal,ip4,dns4}`. Decode the operator name
   notification, group 0x0a number 0x52 seen, unconfirmed.
2. A `scion-celld` root daemon (prefix, launchd job like `com.scion.jbseed`) publishing the pdp_ip0 service to configd
   (step 1 above).
3. Then the status-bar library (step 2).
4. Then decide on step 3 with the owner.

## The CommCenter shim (plan only; not started: needs the owner's go)

What a real CoreTelephony baseband would take, written down so that the decision can be made on facts:

1. **Find what CommCenter opens.** On the iPhone12,1 identity CommCenter expects an Intel baseband: IOKit services of
   Apple's baseband drivers (the PCIe "ARI" transport and its user client), a boot/firmware path (BBUpdater), and the
   ARI protocol once the modem is up. On this port none of these services exist, so CommCenter decides there is no
   baseband. The first step is read-only: boot with CommCenter's own logging and list every IOKit service, XPC service
   and file it looks for (no code).
2. **Two shapes, both large:**
   - *An ARI baseband in the kernel:* a fake transport service (our own IOService with the matching names and a user
     client) that answers ARI messages for the few groups CommCenter needs to say "SIM ready, registered, signal N,
     data context up", translating to and from Samsung IPC (`hakios.shannon.c` already speaks the SIM status,
     registration, signal and the data context). ARI's message set for this firmware generation is public only in
     part (ARIstoteles, WiSec 2021); every message would have to be learned from CommCenter's behaviour, never from
     Apple binaries copied into the tree.
   - *A replacement for CommCenter's XPC services:* our own daemon answering `CTServerConnection` clients. The
     interface is large and private; Settings, SpringBoard and many daemons use it.
3. **What it would give:** SIM in Settings, the operator from the carrier bundle, bars everywhere, data through iOS's own
   path (CommCenter makes `pdp_ip` and the configd service itself, replacing `scion-celld`).
4. **Limits that stay:** no IMEI or NV change, no unlock, no calls or SMS without the owner's say-so, EFS read-only,
   no Apple binary patched.
5. **Smallest useful first step if the owner says go:** step 1 (the list), then a kernel-only stub service that makes
   CommCenter log "baseband found" and nothing more, behind a boot-arg.

