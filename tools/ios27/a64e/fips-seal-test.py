#!/usr/bin/env python3
"""fips-seal-test.py - fips-seal.py on made-up Mach-O files (no Apple bytes): a true seal is reported sealed; changing one
instruction of __text makes it BROKEN; reseal against the reference makes it true again and logs the old value; restore puts
the old value back; a seal that is false in the reference is never written; a reseal without a reference is refused.
Pure Python; run from anywhere: python3 tools/ios27/a64e/fips-seal-test.py"""
import hashlib, hmac, os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'fips-seal.py')
BASE, TEXT_OFF, SEAL_OFF, SIZE = 0x100000, 0x200, 0x800, 0x1000
RETAB, RET, BLRAA_X16_X17, BLR_X16 = 0xD65F0FFF, 0xD65F03C0, 0xD73F0A11, 0xD63F0200


def macho(words, seal=None, extra_seg=True):
    text = struct.pack(f'<{len(words)}I', *words)
    mac = hmac.new(bytes(32), text, hashlib.sha256).digest() if seal is None else seal
    nsect = 2
    cmdsize = 72 + 80 * nsect
    lcs = struct.pack('<II16sQQQQiiII', 0x19, cmdsize, b'__TEXT', BASE, SIZE, 0, SIZE, 5, 5, nsect, 0)
    lcs += struct.pack('<16s16sQQIIIIIIII', b'__text', b'__TEXT', BASE + TEXT_OFF, len(text), TEXT_OFF, 2, 0, 0,
                       0x80000400, 0, 0, 0)
    lcs += struct.pack('<16s16sQQIIIIIIII', b'__fips_hmacs', b'__TEXT', BASE + SEAL_OFF, 32, SEAL_OFF, 0, 0, 0, 0, 0, 0, 0)
    ncmds = 1
    if extra_seg:      # a __DATA segment after it, so the reader walks more than one command
        lcs += struct.pack('<II16sQQQQiiII', 0x19, 72, b'__DATA', BASE + SIZE, 0x1000, SIZE, 0x1000, 3, 3, 0, 0)
        ncmds += 1
    hdr = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 2, 6, ncmds, len(lcs), 0, 0)
    b = bytearray(SIZE + 0x1000)
    b[:32] = hdr
    b[32:32 + len(lcs)] = lcs
    b[TEXT_OFF:TEXT_OFF + len(text)] = text
    b[SEAL_OFF:SEAL_OFF + 32] = mac
    return bytes(b)


def run(*args):
    r = subprocess.run([sys.executable, '-I', TOOL] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def expect(cond, what, out=''):
    if not cond:
        print('FAIL:', what)
        print(out)
        sys.exit(1)
    print('ok:', what)


def main():
    d = tempfile.mkdtemp(prefix='fips-seal-test.')
    try:
        words = [0xD503237F, 0xA9BF7BFD, BLRAA_X16_X17, 0xA8C17BFD, RETAB] * 50
        ref, cur, log = (os.path.join(d, n) for n in ('ref', 'cur', 'seal.log'))
        open(ref, 'wb').write(macho(words))
        shutil.copy(ref, cur)
        rc, out = run('check', cur, '--macho')
        expect(rc == 0 and 'sealed' in out and 'BROKEN' not in out, 'a true seal is sealed', out)

        rw = [RET if w == RETAB else BLR_X16 if w == BLRAA_X16_X17 else w for w in words]   # what a64e-rewrite does
        b = bytearray(open(cur, 'rb').read())
        b[TEXT_OFF:TEXT_OFF + 4 * len(rw)] = struct.pack(f'<{len(rw)}I', *rw)
        open(cur, 'wb').write(b)
        rc, out = run('check', cur, '--macho', '--ref', ref)
        expect(rc == 1 and 'BROKEN' in out, 'the rewritten code breaks the seal (check exits 1)', out)
        rc, out = run('reseal', cur, '--macho')
        expect(rc != 0, 'reseal without --ref/--log is refused', out)
        rc, out = run('reseal', cur, '--macho', '--ref', ref, '--log', log, '--dry-run')
        expect(rc == 0 and open(cur, 'rb').read() == bytes(b) and not os.path.exists(log), 'a dry run writes nothing', out)
        rc, out = run('reseal', cur, '--macho', '--ref', ref, '--log', log)
        expect(rc == 0 and 'resealed' in out, 'reseal against the reference', out)
        new = open(cur, 'rb').read()
        want = hmac.new(bytes(32), struct.pack(f'<{len(rw)}I', *rw), hashlib.sha256).digest()
        expect(new[SEAL_OFF:SEAL_OFF + 32] == want, 'the new seal is HMAC-SHA256(zero key, rewritten __text)')
        expect(new[:SEAL_OFF] == bytes(b[:SEAL_OFF]) and new[SEAL_OFF + 32:] == bytes(b[SEAL_OFF + 32:]),
               'nothing but the 32 seal bytes changed')
        rc, out = run('check', cur, '--macho', '--ref', ref)
        expect(rc == 0 and 'BROKEN' not in out, 'the resealed image checks sealed', out)

        b2 = bytearray(new)
        b2[TEXT_OFF] ^= 1
        open(cur + '2', 'wb').write(b2)
        rc, out = run('check', cur + '2', '--macho', '--ref', ref)
        expect(rc == 1 and 'BROKEN' in out, 'a change after the reseal breaks it again', out)
        rc, out = run('reseal', cur + '2', '--macho', '--ref', ref, '--log', log + '2')
        expect(rc == 1 and open(cur + '2', 'rb').read() == bytes(b2),
               'reseal refuses an image whose stored seal is no longer the reference one', out)

        rc, out = run('restore', cur, '--macho', '--log', log)
        expect(rc == 0 and open(cur, 'rb').read() == bytes(b), 'restore puts the original seal back', out)
        rc, out = run('restore', cur, '--macho', '--log', log)
        expect(rc == 0 and 'already' in out, 'restore twice is harmless', out)

        uref, ucur = os.path.join(d, 'uref'), os.path.join(d, 'ucur')
        open(uref, 'wb').write(macho(words, seal=bytes(range(32))))       # false in the reference
        open(ucur, 'wb').write(macho(rw, seal=bytes(range(32))))
        before = open(ucur, 'rb').read()
        rc, out = run('reseal', ucur, '--macho', '--ref', uref, '--log', log + 'u')
        expect(rc == 0 and 'unsealed-in-ref' in out and open(ucur, 'rb').read() == before,
               'a seal that is false in the reference is left alone', out)
        print('fips-seal-test: all ok')
    finally:
        shutil.rmtree(d)


if __name__ == '__main__':
    main()
