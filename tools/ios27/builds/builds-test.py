#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""builds-test.py: the iOS build manifests (tools/ios27/builds/) and the tools that read them, without Apple's files.

  - every <BUILD>.conf has the keys the tools read, its BUILD is its file name, a supported build has a patch list whose
    lines parse, and DEFAULT names a supported build;
  - build-conf.sh (sourced by bash) gives for 24A446 exactly what the tools had written in them before the manifests:
    build 24A446, class n104ap, the patch list tools/ios27/s9/patches-24A446.txt, the three image names of
    build-full-root.sh's defaults, the refusal text of prepare-root-inputs.sh;
  - the refusals: a build with no .conf (fetch-apple-inputs.sh, prepare-root-inputs.sh), a candidate with no patch list
    (prepare-root-inputs.sh), and a mixture of builds (build-root.sh: inputs of one build, a private directory of another).
Runs on Linux (CI) and macOS; it never reaches a step that needs a restore image, root, or the network.
usage: tools/ios27/builds/builds-test.py"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
T27 = os.path.dirname(HERE)
LIB = os.path.join(HERE, 'build-conf.sh')
KEYS = {'BUILD', 'VERSION', 'PRODUCT', 'DEVICE_CLASS', 'IPSW_NAME', 'IPSW_SIZE', 'KERNEL', 'OS_DMG', 'SYSTEMOS_DMG',
        'APPOS_DMG', 'PATCHES', 'STATUS'}
fails = 0


def check(cond, what):
    global fails
    print(('ok    ' if cond else 'FAIL  ') + what)
    fails += not cond


def conf(path):
    out = {}
    for line in open(path):
        line = line.split('#', 1)[0].rstrip()
        if not line.strip():
            continue
        k, _, v = line.partition('=')
        out[k] = v.strip()
    return out


def sh(script, env=None, cwd=None):
    e = dict(os.environ)
    for k in ('IOS27_BUILD', 'WANT_BUILD', 'DEVICE_CLASS', 'BUILD_VERSION', 'IOS27_PRIVATE'):
        e.pop(k, None)
    e.update(env or {})
    p = subprocess.run(['bash', '-c', script], capture_output=True, text=True, env=e, cwd=cwd)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


# 1. the manifests
builds = sorted(f[:-5] for f in os.listdir(HERE) if f.endswith('.conf'))
check('24A446' in builds, 'manifests: %s' % ' '.join(builds))
for b in builds:
    c = conf(os.path.join(HERE, b + '.conf'))
    check(set(c) == KEYS, '%s.conf has exactly the keys the tools read' % b)
    check(c.get('BUILD') == b, '%s.conf: BUILD is its file name' % b)
    check(c.get('STATUS') in ('supported', 'candidate'), '%s.conf: STATUS supported or candidate' % b)
    check(re.fullmatch(r'\d+', c.get('IPSW_SIZE', '')) is not None, '%s.conf: IPSW_SIZE is a number of bytes' % b)
    check(c.get('IPSW_NAME') == '%s_%s_%s_Restore.ipsw' % (c.get('PRODUCT'), c.get('VERSION'), b), '%s.conf: IPSW_NAME is Apple\'s name' % b)
    if c.get('STATUS') == 'supported':
        p = os.path.join(T27, c.get('PATCHES', ''))
        check(bool(c.get('PATCHES')) and os.path.isfile(p), '%s.conf: a supported build has its patch list' % b)
        if os.path.isfile(p):
            rows = [l.split(None, 3) for l in open(p) if l.strip() and not l.startswith('#')]
            good = all(len(r) == 4 and re.fullmatch(r'0x[0-9a-f]{9}', r[0]) and re.fullmatch(r'[0-9a-f]{8}', r[1])
                       and re.fullmatch(r'[0-9a-f]{8}', r[2]) for r in rows)
            check(good and rows, '%s: %d patch lines of "VA EXPECT NEW note"' % (c['PATCHES'], len(rows)))
default = open(os.path.join(HERE, 'DEFAULT')).read().strip()
check(default in builds and conf(os.path.join(HERE, default + '.conf')).get('STATUS') == 'supported',
      'DEFAULT names a supported build (%s)' % default)

# 2. build-conf.sh gives the old fixed values for 24A446
rc, out, err = sh('. "$0"; ios27_build_default; ios27_build_get 24A446 DEVICE_CLASS; ios27_build_patches 24A446; '
                  'ios27_build_get 24A446 OS_DMG; ios27_build_get 24A446 SYSTEMOS_DMG; ios27_build_get 24A446 APPOS_DMG; '
                  'ios27_build_get 24A446 VERSION; ios27_build_get 24A446 PRODUCT; ios27_build_check 24A446'.replace('"$0"', '"%s"' % LIB))
want = ['24A446', 'n104ap', os.path.join(T27, 's9', 'patches-24A446.txt'), '043-70213-665.dmg', '043-69708-712.dmg',
        '043-69297-793.dmg', '27.0.1', 'iPhone12,1']
check(rc == 0 and out.split('\n') == want and err == '', 'build-conf.sh for 24A446: the values the scripts had fixed in them')
v, pr = (out.split('\n') + ['', ''])[6:8]
check('(iOS %s, 24A446, %s)' % (v, pr) == '(iOS 27.0.1, 24A446, iPhone12,1)', 'prepare-root-inputs.sh: the same refusal text for 24A446')
rc, out, err = sh('. "%s"; ios27_build_check 99Z999' % LIB)
check(rc == 1 and '99Z999.conf' in err, 'a build with no .conf is refused')
rc, out, err = sh('. "%s"; ios27_build_check 24B156 && ios27_build_patches 24B156' % LIB)
check(rc == 1 and 'NOTE: build 24B156 is candidate' in err and 'no cache patch list yet' in err, 'a candidate is named and has no patch list')
rc, out, err = sh('. "%s"; ios27_build_same A 24A446 B 24A446 && ios27_build_same A "" B 24B156 && ! ios27_build_same A 24A446 B 24B156' % LIB)
check(rc == 0, 'ios27_build_same: equal or unknown passes, two different builds fail')

with tempfile.TemporaryDirectory() as t:
    with open(os.path.join(t, 'SOURCE.txt'), 'w') as f:      # the first line as fetch-apple-inputs.sh writes it
        f.write('made by tools/ios27/s9/fetch-apple-inputs.sh from iPhone12,1_27.0.1_24A446_Restore.ipsw, build 24A446\n')
    rc, out, err = sh('. "%s"; ios27_build_of_inputs "%s"; ios27_build_of_inputs /nonexistent' % (LIB, t))
    check(rc == 0 and out == '24A446', 'ios27_build_of_inputs reads SOURCE.txt (and gives nothing without one)')

    # 3. the scripts refuse before they need anything of Apple's
    ipsw = os.path.join(t, 'x.ipsw')
    open(ipsw, 'w').close()
    rc, out, err = sh('"%s" "%s" "%s/out"' % (os.path.join(T27, 's9', 'fetch-apple-inputs.sh'), ipsw, t), {'IOS27_BUILD': '99Z999'})
    check(rc == 1 and '99Z999.conf' in err, 'fetch-apple-inputs.sh refuses a build with no .conf')
    rc, out, err = sh('"%s" /nonexistent' % os.path.join(T27, 's9', 'prepare-root-inputs.sh'), {'IOS27_BUILD': '24B156'})
    check(rc == 1 and 'no cache patch list yet' in err, 'prepare-root-inputs.sh refuses 24B156 until it has a patch list')
    rc, out, err = sh('"%s" /nonexistent' % os.path.join(T27, 's9', 'prepare-root-inputs.sh'))
    check(rc == 1 and 'no dyld_shared_cache_arm64e' in err and 'NOTE' not in err, 'prepare-root-inputs.sh goes on for 24A446 as before')

    # build-root.sh: inputs of 24B156, a private directory made for 24A446
    inp, priv = os.path.join(t, 'inputs'), os.path.join(t, 'private')
    os.makedirs(inp)
    os.makedirs(os.path.join(priv, 'overlay'))
    for f in ('os.dmg', 'systemos.dmg', 'appos.dmg'):
        open(os.path.join(inp, f), 'w').close()
    for f in ('herolte-tune-ios-root.sh', 'patches.txt', 'undef.txt', 'overlay/MANIFEST.sha256'):
        open(os.path.join(priv, f), 'w').close()
    with open(os.path.join(priv, 'BUILD'), 'w') as f:
        f.write('24A446\n')
    with open(os.path.join(inp, 'SOURCE.txt'), 'w') as f:
        f.write('made by tools/ios27/s9/fetch-apple-inputs.sh from iPhone12,1_27.2.1_24B156_Restore.ipsw, build 24B156\n')
    br = os.path.join(T27, 's9', 'build-root.sh')
    rc, out, err = sh('"%s" "%s"' % (br, inp), {'IOS27_PRIVATE': priv})
    check(rc == 1 and 'a mixture of iOS builds' in err, 'build-root.sh refuses inputs of 24B156 with a private directory of 24A446')
    rc, out, err = sh('"%s" "%s"' % (br, inp), {'IOS27_PRIVATE': priv, 'IOS27_BUILD': '24A446'})
    check(rc == 1 and 'a mixture of iOS builds' in err, 'build-root.sh refuses IOS27_BUILD=24A446 for inputs of 24B156')
    if os.geteuid() != 0:   # consistent inputs: build-root.sh hands over to build-full-root.sh, which stops at "run as root"
        with open(os.path.join(inp, 'SOURCE.txt'), 'w') as f:
            f.write('made by tools/ios27/s9/fetch-apple-inputs.sh from iPhone12,1_27.0.1_24A446_Restore.ipsw, build 24A446\n')
        rc, out, err = sh('"%s" "%s"' % (br, inp), {'IOS27_PRIVATE': priv})
        check(rc == 2 and 'run as root' in err and 'mixture' not in err, 'build-root.sh lets one build through (to build-full-root.sh)')

print('ALL BUILD-MANIFEST CHECKS PASSED' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
