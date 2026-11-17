#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""crashlog-symbolicate.py LOG [--dsc CACHE] [--ipsw PATH] [--only NAME[,NAME]] [--first N]

Symbolicates the kernel's CRASH lines in an S9 console log (iOS 27) offline, against YOUR OWN copy of the restore
image's shared cache. iOS's own crash reports are not written on the S9 (ReportCrash is not in the root build's job set:
docs/ios27/CONTROL-CENTRE.md), so these lines are the record of a user-space crash:

  CRASH name[pid] sig S code C sub X slide SLIDE mh MH      (bsd/kern/kern_exit.c -> osfmk/kern/exception.c)
  CRASH name pc PC lr LR sp .. fp .. far ..
  CRASH name bt A B C ...                                    (frame-pointer return addresses)
  CRASH name img what ADDR: dsc 0xUNSLID | PATH +0xOFF       (only with the boot argument sweetcrashimg=N)
  CRASH name reason ns N code C: TEXT                        (the exit reason)

An address minus SLIDE that lands in the shared cache's range is looked up with blacktop's `ipsw dyld a2s`; an address
in the first 256 MiB above MH is printed as an offset in the process's own binary (look it up in that file); the
others are printed as they are, with the image the img line names when there is one.

--dsc   the dyld_shared_cache_arm64e of the restore image the root was built from. The S9 root's own copy has no
        .symbols file, and ipsw needs it: use the one in the restore image's system cryptex (a mounted image of your own).
--ipsw  the ipsw binary (default: `ipsw` on PATH). Without --dsc only the unslid addresses are printed.
Nothing of Apple's is in this script; it reads only the log and the cache you give it.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

CACHE_LO, CACHE_HI = 0x180000000, 0x400000000   # where iOS 27's arm64e shared cache is mapped (unslid)
HEAD = re.compile(r'CRASH (\S+?)\[(\d+)\] sig (\d+) code (0x[0-9a-f]+) sub (0x[0-9a-f]+) slide (0x[0-9a-f]+) mh (0x[0-9a-f]+)')
PCLR = re.compile(r'CRASH (\S+) pc (0x[0-9a-f]+) lr (0x[0-9a-f]+)')
BT = re.compile(r'CRASH (\S+) bt((?: 0x[0-9a-f]+)+)')
IMG = re.compile(r'CRASH (\S+) img (\S+) (0x[0-9a-f]+): (.*)$')
REASON = re.compile(r'CRASH (\S+) reason (.*)$')


def parse(path):
    crashes, cur = [], {}
    for raw in open(path, 'rb'):
        line = raw.decode('utf-8', 'replace').rstrip('\n')
        m = HEAD.search(line)
        if m:
            cur = {'name': m[1], 'pid': int(m[2]), 'sig': int(m[3]), 'code': m[4], 'sub': m[5],
                   'slide': int(m[6], 16), 'mh': int(m[7], 16), 'frames': [], 'img': {}, 'reason': ''}
            crashes.append(cur)
            continue
        if not cur:
            continue
        m = PCLR.search(line)
        if m and m[1] == cur['name'] and not cur['frames']:
            cur['frames'] += [('pc', int(m[2], 16)), ('lr', int(m[3], 16))]
            continue
        m = BT.search(line)
        if m and m[1] == cur['name']:
            cur['frames'] += [('bt%d' % i, int(a, 16)) for i, a in enumerate(m[2].split())]
            continue
        m = IMG.search(line)
        if m and m[1] == cur['name']:
            cur['img'][int(m[3], 16)] = m[4]
            continue
        m = REASON.search(line)
        if m and m[1] == cur['name']:
            cur['reason'] = m[2]
    return crashes


def a2s(ipsw, dsc, cachefile, unslid, memo):
    if unslid in memo:
        return memo[unslid]
    r = subprocess.run([ipsw, 'dyld', 'a2s', dsc, hex(unslid), '-i', '-d', '--cache', cachefile, '--no-color'],
                       capture_output=True, text=True)
    image = sym = ''
    for l in (r.stdout + r.stderr).splitlines():
        l = l.strip()
        if l.startswith('>'):
            image = os.path.basename(l[1:].split('(')[0].strip())
        elif l.startswith(hex(unslid) + ':'):
            sym = l.split(':', 1)[1].strip()
    memo[unslid] = (image + ': ' + sym) if image or sym else '?'
    return memo[unslid]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log')
    ap.add_argument('--dsc')
    ap.add_argument('--ipsw', default=shutil.which('ipsw') or 'ipsw')
    ap.add_argument('--only', default='')
    ap.add_argument('--first', type=int, default=0, help='only the first N crashes of each process name')
    a = ap.parse_args()
    only = set(x for x in a.only.split(',') if x)
    crashes = parse(a.log)
    tmp = tempfile.mkdtemp(prefix='crashsym.')
    cachefile, memo, seen = os.path.join(tmp, 'a2s.cache'), {}, {}
    try:
        for c in crashes:
            if only and c['name'] not in only:
                continue
            seen[c['name']] = seen.get(c['name'], 0) + 1
            if a.first and seen[c['name']] > a.first:
                continue
            print('%s[%d] signal %d code %s sub %s slide 0x%x%s' % (c['name'], c['pid'], c['sig'], c['code'], c['sub'],
                  c['slide'], ('  reason ' + c['reason']) if c['reason'] else ''))
            for what, addr in c['frames']:
                if addr == 0:
                    continue
                un = addr - c['slide']
                if c['slide'] and CACHE_LO <= un < CACHE_HI:
                    where = 'dsc 0x%x' % un
                    if a.dsc:
                        where += '  ' + a2s(a.ipsw, a.dsc, cachefile, un, memo)
                elif c['mh'] and c['mh'] <= addr < c['mh'] + 0x10000000:
                    where = '%s +0x%x (the process\'s own binary)' % (c['name'], addr - c['mh'])
                else:
                    where = c['img'].get(addr, 'outside the cache and the main binary (boot with sweetcrashimg=N to name it)')
                print('  %-4s 0x%x  %s' % (what, addr, where))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
