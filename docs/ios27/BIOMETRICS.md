# iOS 27 and Face ID on the Galaxy S9: what it is made of, what the S9 can offer, and what Scion should build (17 November 2026)

The owner asked for the iris camera and "Face ID". This page is the study that comes before any code: what Face ID is on iOS 27, what the
S9's hardware can supply, and the honest options. **Nothing here is implemented, and nothing should be until the owner has decided
the questions in [The owner's decisions](#the-owners-decisions).**

**In short.** Face ID's security lives in the Secure Enclave: the match, the templates and the keys it releases. Scion has no Secure
Enclave, its key store answers as a phone without one ([KEYSTORE.md](KEYSTORE.md)), and its data volume is not encrypted. The S9's
infrared iris camera cannot help: its frames reach only the S9's secure world, by Samsung's design ([../iris-camera.md](../iris-camera.md)).
What is left is the ordinary front camera, which gives a 2D colour picture that a printed photo or a video on another screen can fool.
**Recommendation: a passcode only, for now (option a). Possibly later a face match of Scion's own, labelled as a convenience that opens
the screen and nothing else (option b). Never presenting anything to iOS as Face ID (option c).**

## 1. What Face ID is made of on iOS 27

From Apple's public *Platform Security* guide, and from the names and layout of the iOS 27.0.1 (24A446) root that Scion boots (read on the
build machine; nothing of it is copied here):

**The hardware ("Pearl").** The TrueDepth camera: a dot projector that lays a pattern of over 30,000 infrared dots on the face, an
infrared flood illuminator, and an infrared camera that photographs both, with a proximity sensor beside them. The pattern gives a depth
map, the flood a 2D infrared picture. The camera's frames go over a protected path to the Secure Enclave; the application processor
drives the camera but does not get to use what it sees for the match.

**The Secure Enclave's part.** In the Secure Enclave (its own processor, memory, boot chain and keys), neural networks turn the depth map
and the infrared picture into a mathematical representation, compare it with the enrolled ones, and check attention (eyes open and on
the phone) and spoofing (masks, photos). Apple gives the chance of a random other person matching as less than 1 in 1,000,000 for one
enrolled appearance (Touch ID: 1 in 50,000). The enrolled data is encrypted by the Secure Enclave, never leaves the phone and is not in
backups. Face ID is enrolled only on a phone with a passcode, and the passcode is required after a restart, after 48 hours without an
unlock, after five failed matches, and in some other cases.

**The keys.** This is the part that matters. When Face ID is on, the key store does not throw away the keys of the "complete protection"
class when the phone locks; it keeps them wrapped by a key that only the Face ID part of the Secure Enclave holds. A match lets the Secure
Enclave unwrap them. The application processor never sees a "yes" that it could forge into a key: the key store and the matcher are in
the same protected place. The same holds for apps: a keychain item that asks for biometry (`SecAccessControl` with a biometry flag) and
`LAContext` policies are decided by the Secure Enclave and the credential manager, and the app gets a result or a key, not a template.

**The kernel's part.** The kernelcache has an IOKit driver for the Secure Enclave's biometric service (the shared cache names
`ApplePearlSEPDriver`; `AppleMesaSEPDriver` is Touch ID's), the SEP manager, the camera driver that runs the TrueDepth streams, the key
store (AppleKeyStore) and the credential manager (AppleCredentialManager, "ACM"). Scion's iOS 27 kernel is its own build and has none of
these; its key store is the stand-in `iokit/Kernel/arm/SweetKeyStore.cpp` of the fork.

**User space, as laid out in the 24A446 root:**

- `biometrickitd` (`/usr/libexec/biometrickitd`), launchd job `com.apple.biometrickitd.pearl`. launchd loads it only on a device whose
  device tree has a `pearl-sep` node, starts it when IOKit publishes `ApplePearlSEPDriver`, and gives it the Mach service
  `com.apple.pearld`. It also wakes on keybagd's lock-status notification and on ManagedConfiguration's "passcode changed". The daemon is
  small and loads a device plug-in, `libBKDM<n>.dylib` (`libBKDM2.dylib` in the shared cache), which speaks to the driver. Its job
  declares a conclave, iOS's process isolation, which was not looked at further.
- `BiometricKit.framework` (the client side every other part uses), `BiometricKitUI.framework` (enrolment), `BiometricSupport.framework`
  (its strings, among them a "Periocular" table: Face ID with a mask), `PearlSupport.framework`.
- `LocalAuthentication.framework` and the private `LocalAuthenticationCore` and friends, with the daemon `coreauthd`
  (`com.apple.CoreAuthentication.daemon`): the policies apps ask about, decided with `AppleCredentialManager.framework` and the kernel's ACM.
- `PasscodeAndBiometricsSettings.framework`: Settings' "Face ID & Passcode" pane.
- keybagd and MobileKeyBag: the lock state and the unlock that the lock screen asks for ([KEYSTORE.md](KEYSTORE.md) has the selectors).

How it is consumed: the lock screen listens to BiometricKit for a match and then finds the device unlocked (the Secure Enclave did it);
Settings enrols through BiometricKit after asking for the passcode as an ACM credential ([../passcode.md](../passcode.md), "Towards Face
ID"); apps use LocalAuthentication or the keychain's access control and never see biometric data.

## 2. What the S9 can supply

| Part | What it is | Can Scion's kernel use it? |
|---|---|---|
| Iris camera | Samsung S5K5F1, infrared, 2400 x 2400, with an IR LED; CSIS3 | **No frames.** The sensor answers and powers up, but its frames go only to the secure world ([../iris-camera.md](../iris-camera.md)) |
| Front camera | Samsung S5K3H1, colour, 8 MP; CSIS1 | **Yes**: frames reach memory and `/dev/hakios-camera` at 1632 x 1224, 30 a second (`hakios.exynos_camera.c`) |
| Depth | none: no dot projector, no time-of-flight sensor | no |
| Fingerprint sensor | on the back, on SPI1, which the device tree marks `secure-mode` | **No**: reading its bus from the normal world is an external abort (the S9 repository's `CAMERA.md`, `fp_s9`); Samsung's design keeps it in the secure world, like the iris camera |
| Secure Enclave | none. The S9 has a TrustZone secure OS (Kinibi), which Scion only probes (`hakios.exynos_tee.c`) | no |

What that means for the strength of a match, in plain terms:

- **A 2D colour face match** (front camera only) is fooled by a good printed photo, by a photo or video on another phone held up to it,
  and often by a sibling. With no depth and no infrared there is little to tell a face from a picture of one; "blink" or "turn your head"
  checks are beaten by a video. Samsung itself did not let the S9's own face recognition be used for Samsung Pass or payments.
- **An infrared iris match**, if its frames were available, is harder to fool by accident but not by intent: the Galaxy S8's iris
  scanner was publicly fooled in 2017 with an infrared photograph of an eye and a contact lens on it. And it is not available.
- **Face ID** combines depth, infrared and attention checks with a matcher and key store that the application processor cannot touch.
  Nothing on the S9 comes close, and the gap is not one that software on the application processor can close.

## 3. The options

### (a) Nothing biometric: a passcode only

- **For the user:** the lock screen asks for the passcode every time. Settings shows "Passcode", no Face ID; apps that offer Face ID see
  none and fall back to their own passcode or password.
- **Security, really:** a screen gate. The passcode stage (`wip/ios27-keystore-passcode`, `sweetspeed&0x4000000`) keeps a PBKDF2
  verifier and wrong-attempt delays in `/private/var/keybags/scion-passcode`; no data is encrypted. Someone with the phone can still
  boot something else (the boot loader is unlocked) and read the volume, or remove the record.
- **Cost:** already written; it needs the lock screen's keypad tried by hand, a restart with a passcode, and Settings' panes (KEYSTORE.md).
- **What must be said:** "the passcode locks the screen; it does not encrypt your data".

### (b) A face match of Scion's own, as a convenience unlock that never releases keys

- **For the user:** looking at the phone opens the lock screen, as Face ID would, after the passcode has been entered once since boot.
- **Where the match runs:** a daemon of Scion's own in user space, reading the front camera through `/dev/hakios-camera` and running an
  openly licensed face model on the CPU. Not in the kernel: a matcher has no business there.
- **Where the templates live:** a root-only file in `/private/var`. Without a Secure Enclave there is nothing that can keep them from a
  root process on the running phone. Sealing them with a key held by the S9's secure OS (its keymaster, through its normal interface, as
  `tee_probe=4` does for HMAC) would protect them against someone reading the flash after booting something else, not against root on the
  phone, and it brings the secure OS into the boot: an owner's decision. Face templates are personal data: the file holds the minimum, is
  deleted with the enrolment, and never leaves the phone.
- **How it would unlock:** the passcode stage would need one more call, "unlock without the passcode", refused until the passcode has
  been entered since boot, refused after 48 hours without a passcode, after five failed matches and after a restart, and answered only to
  Scion's daemon. How SpringBoard's lock screen is then told to go away without a passcode entry is an open engineering question (on an
  iPhone the Secure Enclave unlocks the bag and BiometricKit tells the lock screen); it needs logs from a phone.
- **What it is not:** it is not Face ID. Settings' Face ID pane stays as on a phone without Face ID, LocalAuthentication answers that
  there is no biometry, keychain items with biometric access control cannot be made, and apps never get a "biometric" result from it.
  It opens the screen and nothing else.
- **What someone with the phone in hand could do:** hold up a photo or a video of the owner; boot another kernel or TWRP and read or
  replace the template file and the passcode record; as root on the running phone, call the unlock directly. So it adds a spoofing risk to
  a gate that is already only a gate. It must be off by default and the user told so in plain words when enrolling.
- **On iOS 16.5 with the real key store** (`keystore_real=1`, [../keystore.md](../keystore.md)) the passcode does wrap the keychain's
  keys. A convenience unlock there must not unwrap them: the screen would open and keychain items would stay locked until the passcode
  is entered, which makes (b) of little use with that key store.
- **What must never be claimed:** that it is Face ID, that it is secure, that it protects data, that it is as safe as the passcode, that
  it detects photos or masks (unless a test on a phone shows it does, and then only what the test showed).
- **Cost:** large. A front-camera stream to user space exists; the rest does not: the daemon, a model with a licence that allows it,
  enrolment and its own small UI (Scion's own app, not Apple's pane), the key store call, the lock screen's side, and tests on a phone
  with photos and videos of the owner.

### (c) Presenting it to iOS as Face ID through an emulated biometric service

- **For the user:** "Face ID" in Settings, Face ID prompts in apps, the lock screen behaving as on an iPhone.
- **What it would take:** a `pearl-sep` node in the device tree, a kernel service that answers as `ApplePearlSEPDriver` and speaks the
  Secure Enclave biometric protocol that `libBKDM2` expects (closed and undocumented), the ACM credential manager, the key store's
  biometric operations (`aks_ref_key_*`, the bag operations a match triggers), MobileGestalt's answers that the phone has Face ID, and the
  matcher of option (b) behind it all. Months of reading Apple's closed binaries.
- **Security, really:** the same as (b), which is to say none against a determined person, but now **presented to iOS and to every app as
  Secure Enclave biometry**. A banking app, a password manager or a keychain item that requires Face ID would accept a match that a photo
  can produce and that any root process can forge, believing the guarantees in section 1. That is a false statement of security made to the
  user and to third parties on their behalf.
- **Recommendation:** refuse.

## Recommendation, and what I would refuse to build

1. **Now: option (a).** Finish and test the passcode stage; keep LocalAuthentication and the key store answering "no biometry".
2. **Later, only if the owner wants it: option (b)**, with the front camera, off by default, enrolled only with a passcode, never
   unlocking before the passcode has been entered since boot, with the limits above, and labelled as a convenience in Scion's own app.
3. **The iris camera** stays a probe of the sensor (`iris_s9=N`); it does not feed a matcher. Samsung's own iris trusted application,
   used through its normal interface, is the only legitimate way to use those frames, and what it would need is listed in
   [../iris-camera.md](../iris-camera.md#the-one-legitimate-alternative-and-what-it-would-need). Even then its answer could only open the screen.

What I would refuse to build, and why:

- **Option (c)**, for the reason above: it would tell iOS and every app that a spoofable, forgeable match is Secure Enclave biometry.
- **Any way to the iris frames around the secure world**: patching, exploiting or tricking EL3, Kinibi, the TZPC, the secure SysMMU or
  Knox, including calling PREPARE and UNPREPARE out of their order to get frames out of the protected path. It is the owner's and
  Claudius's hard limit, and it would also mean defeating a protection Samsung put there for the owner's own biometric data.
- **A match that releases keys** of the key store or the keychain: without a Secure Enclave there is nothing to keep such a key from root,
  and it would only pretend to protect data.
- **Templates kept anywhere but a root-only file on the phone**, or sent off it.
- **Any change to the IR LED's current, pulse length or timing**, or a continuous mode, for any of this.

## The owner's decisions

- Whether to do option (b) at all (a spoofable unlock, and face data stored on a phone whose volume is not encrypted).
- Whether the S9's secure OS may be brought into the boot to seal templates (option b) or to run Samsung's iris application (the
  alternative): loading Samsung's binaries from the phone's partitions at run time, reading Samsung's closed normal-world libraries to
  learn the iris application's commands, and the secure OS's storage for enrolment, which may mean RPMB or EFS, which this project does
  not write today.
- Whether `iris_s9` stays in the tree as a probe or is retired, and that the "PREPARE, power up, UNPREPARE, stream" experiment of the S9
  repository's `CAMERA.md` is struck (recommended: struck).

## Sources

- Apple, *Apple Platform Security* (Face ID, Touch ID, passcodes and the Secure Enclave; data protection and the keychain).
- The iOS 27.0.1 (24A446) root: the launchd job of `biometrickitd`, the frameworks named above, the image list of its shared cache. Only
  names and layout were read.
- [../iris-camera.md](../iris-camera.md) for the iris camera; [KEYSTORE.md](KEYSTORE.md) and [../passcode.md](../passcode.md) for the key
  store and the passcode; the fork's `iokit/Kernel/arm/SweetKeyStore.cpp` and, on `wip/ios27-keystore-passcode`,
  `SweetKeyStorePasscode.cpp`.
- Public reports: the Chaos Computer Club's Galaxy S8 iris demonstration (May 2017); Samsung's statements that the S8 and S9 face
  recognition was not for Samsung Pass or payments.
