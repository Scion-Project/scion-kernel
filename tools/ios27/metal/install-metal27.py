#!/usr/bin/env python3
"""install-metal27.py ROOT [--clients "mtl27test PosterBoard ..."] [--oslog "PosterBoard ..."] [--wallpaper|--no-wallpaper] [--no-job] [--uninstall]
Copies the staged iOS 27 Metal stack (stage/) into a MOUNTED iOS 27 root
(hdiutil attach -owners on, run with sudo), and adds the one-shot launchd job com.starbite.mtl27test.
  ROOT/System/Library/Extensions/StarbiteMetal.bundle   (frontend + metal2vulkan bridge + PanVK, arm64)
  ROOT/usr/lib/sbmtlc.dylib                              (empty keys-off library for sweetinject)
  ROOT/usr/local/bin/mtl27test                           (smoke test, log /private/var/tmp/mtl27test.log)
  ROOT/private/var/local/{mtl2-clients, mtl2.log (666), m2v-cache/ (777)}
  ROOT/System/Library/xpc/launchd.plist  += LaunchDaemons['/System/Library/LaunchDaemons/com.starbite.mtl27test.plist']
Every file it replaces is kept once as NAME.before-metal27."""
import os, plistlib, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
# mk-bundle27.sh's output, plus sbmtlc.dylib and mtl27test (and sbwallpaper.png for --wallpaper)
STAGE = os.environ.get('STAGE') or os.path.join(os.getcwd(), 'build/metal27/stage')
JOBKEY = '/System/Library/LaunchDaemons/com.starbite.mtl27test.plist'
JOBKEY2 = '/System/Library/LaunchDaemons/com.starbite.m2vtest.plist'
args = sys.argv[1:]
if not args:
    sys.exit(__doc__)
root = args[0]
clients = 'mtl27test PosterBoard CollectionsPoster'
if '--clients' in args:
    clients = args[args.index('--clients') + 1]
assert os.path.isdir(os.path.join(root, 'System/Library')), 'not an iOS root: ' + root

def keep(p):
    if os.path.exists(p) and not os.path.exists(p + '.before-metal27'):
        if os.path.isdir(p):
            shutil.copytree(p, p + '.before-metal27', symlinks=True)
        else:
            shutil.copy2(p, p + '.before-metal27')

def put(src, dst, mode):
    dst = os.path.join(root, dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    keep(dst)
    if os.path.isdir(src):
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src, dst, symlinks=True)
        for d, ds, fs in os.walk(dst):
            os.chown(d, 0, 0); os.chmod(d, 0o755)
            for f in fs:
                q = os.path.join(d, f); os.chown(q, 0, 0); os.chmod(q, 0o755 if os.access(q, os.X_OK) else 0o644)
    else:
        shutil.copy2(src, dst); os.chown(dst, 0, 0); os.chmod(dst, mode)
    print('installed', dst)

lp = os.path.join(root, 'System/Library/xpc/launchd.plist')
if '--uninstall' in args:
    d = plistlib.load(open(lp, 'rb'))
    d['LaunchDaemons'].pop(JOBKEY, None); d['LaunchDaemons'].pop(JOBKEY2, None)
    plistlib.dump(d, open(lp, 'wb'), fmt=plistlib.FMT_BINARY)
    for p in ['System/Library/Extensions/StarbiteMetal.bundle', 'usr/lib/sbmtlc.dylib', 'usr/local/bin/mtl27test', 'usr/local/bin/m2vtest', JOBKEY[1:], JOBKEY2[1:]]:
        q = os.path.join(root, p)
        shutil.rmtree(q, ignore_errors=True) if os.path.isdir(q) else (os.path.exists(q) and os.remove(q))
    print('uninstalled (mtl2-clients left; empty it to turn Metal off for everything)')
    sys.exit(0)

put(os.path.join(STAGE, 'StarbiteMetal.bundle'), 'System/Library/Extensions/StarbiteMetal.bundle', 0o755)
put(os.path.join(STAGE, 'sbmtlc.dylib'), 'usr/lib/sbmtlc.dylib', 0o755)
put(os.path.join(STAGE, 'mtl27test'), 'usr/local/bin/mtl27test', 0o755)
put(os.path.join(STAGE, 'm2vtest'), 'usr/local/bin/m2vtest', 0o755)
local = os.path.join(root, 'private/var/local')
os.makedirs(local, exist_ok=True)
keep(os.path.join(local, 'mtl2-clients'))
open(os.path.join(local, 'mtl2-clients'), 'w').write(clients + '\n')
os.chmod(os.path.join(local, 'mtl2-clients'), 0o644)
log = os.path.join(local, 'mtl2.log'); open(log, 'a').close(); os.chmod(log, 0o666)
cache = os.path.join(local, 'm2v-cache'); os.makedirs(cache, exist_ok=True); os.chmod(cache, 0o777)
# os_log capture for injected (sbmtlc) programs: names in oslog-clients -> /private/var/local/oslog/<name>.log
ol = os.path.join(local, 'oslog'); os.makedirs(ol, exist_ok=True); os.chmod(ol, 0o777)
# diagnostics for every Metal client: plug-in lines on the console, metal2vulkan's runtime messages to a file
env = os.path.join(local, 'mtl2-env'); keep(env)
lines = [l for l in (open(env).read().splitlines() if os.path.exists(env) else []) if l and not l.startswith(('HOT_MTL2_CONSOLE=', 'SWEET_M2V_MESSAGES='))]
open(env, 'w').write('\n'.join(lines + ['HOT_MTL2_CONSOLE=1', 'SWEET_M2V_MESSAGES=/private/var/local/m2v-messages.log']) + '\n'); os.chmod(env, 0o644)
mm = os.path.join(local, 'm2v-messages.log'); open(mm, 'a').close(); os.chmod(mm, 0o666)
# stop-gap static wallpaper drawn by sbmtlc inside SpringBoard (needs SpringBoard in sbmtlc's sweetinject group)
if '--wallpaper' in args:
    shutil.copy2(os.path.join(STAGE, 'sbwallpaper.png'), os.path.join(local, 'sbwallpaper.png'))
    os.chmod(os.path.join(local, 'sbwallpaper.png'), 0o644)
    print('static wallpaper: /private/var/local/sbwallpaper.png (SpringBoard draws it while sbmtlc is injected into it)')
if '--no-wallpaper' in args and os.path.exists(os.path.join(local, 'sbwallpaper.png')):
    os.remove(os.path.join(local, 'sbwallpaper.png'))
oc = os.path.join(local, 'oslog-clients')
if '--oslog' in args:
    open(oc, 'w').write(args[args.index('--oslog') + 1] + '\n'); os.chmod(oc, 0o644)
print('mtl2-clients:', clients)
if '--no-job' not in args:
    job = {'Label': 'com.starbite.mtl27test', 'Program': '/usr/local/bin/mtl27test',
           'ProgramArguments': ['/usr/local/bin/mtl27test', '--delay', '90', '--shaders', '8'],
           'RunAtLoad': True, 'LaunchOnlyOnce': True, 'UserName': 'root',
           'StandardOutPath': '/private/var/tmp/mtl27test.log', 'StandardErrorPath': '/private/var/tmp/mtl27test.log',
           'JetsamProperties': {'JetsamMemoryLimit': 1024}, 'POSIXSpawnType': 'Interactive'}
    keep(lp)
    d = plistlib.load(open(lp, 'rb'))
    d['LaunchDaemons'][JOBKEY] = job
    # the bridge's own self-test, straight on PanVK (no Metal.framework): device, compute, render, host import
    job2 = {'Label': 'com.starbite.m2vtest', 'Program': '/usr/local/bin/m2vtest',
            'ProgramArguments': ['/usr/local/bin/m2vtest', '/System/Library/Extensions/StarbiteMetal.bundle/libvulkan_panfrost.dylib',
                                 '/private/var/local/m2v-cache', '/nonexistent-skip-quartzcore'],
            'EnvironmentVariables': {'PAN_I_WANT_A_BROKEN_VULKAN_DRIVER': '1', 'MESA_VK_VERSION_OVERRIDE': '1.2'},
            'RunAtLoad': True, 'LaunchOnlyOnce': True, 'UserName': 'root',
            'StandardOutPath': '/private/var/tmp/m2vtest.log', 'StandardErrorPath': '/private/var/tmp/m2vtest.log',
            'JetsamProperties': {'JetsamMemoryLimit': 1024}, 'POSIXSpawnType': 'Interactive'}
    d['LaunchDaemons'][JOBKEY2] = job2
    plistlib.dump(d, open(lp, 'wb'), fmt=plistlib.FMT_BINARY)
    for k, j in ((JOBKEY, job), (JOBKEY2, job2)):
        jp = os.path.join(root, k[1:]); os.makedirs(os.path.dirname(jp), exist_ok=True); plistlib.dump(j, open(jp, 'wb')); os.chown(jp, 0, 0); os.chmod(jp, 0o644)
    print('launchd jobs com.starbite.m2vtest (at load) and com.starbite.mtl27test (90 s delay) added, run once each')
