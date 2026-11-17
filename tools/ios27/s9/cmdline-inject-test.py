#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""cmdline-inject-test.py: what tools/ios27/s9/cmdline-inject.py makes of made-up records, lines and kernels, and what it
refuses. No input of anybody's: the records and the kernel bytes are written here. Run by CI (.github/workflows/check.yml)."""
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ci', os.path.join(HERE, 'cmdline-inject.py'))
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)

LINE = 'sbgl=0 sweetspeed=408782847 sweetepoch=1763390000'
B = ci.BUILTIN
# a stand-in for the kernel's built-in line (the real one is longer; what matters is its form)
DEFAULTS = ('debug=0xc serial=3 cpus=8 rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 '
            'sweetinject=' + B + ' sbprof=0 sweetspeed=100351 tmu=3 shub=6 shub_hid=3 cp=7 cp_nv=2 cp_if=1')


def kernel(defaults=DEFAULTS):
    return b'\xcf\xfa\xed\xfe' + b'\0' * 64 + b'other string\0' + defaults.encode() + b'\0' + b'\0' * 32


fails = 0


def ok(cond, what):
    global fails
    print(('ok    ' if cond else 'FAIL  ') + what)
    fails += 0 if cond else 1


def refused(fn, needle, what):
    try:
        fn()
    except ci.Refused as e:
        ok(needle in str(e), f'{what} (refused: {e})')
        return
    ok(False, f'{what}: not refused')


# 1. no record lines (an empty record, or only comments): the line exactly as given
for rec in ('', '# sweetinject record\n# made then\n'):
    out, _ = ci.compose(LINE, rec, kernel())
    ok(out == LINE, f'a record with no library leaves the line unchanged ({rec!r})')
# 2. the record's libraries after the built-in three, in the record's order, nothing else
out, _ = ci.compose(LINE, '/usr/lib/o.dylib backboardd\n/usr/lib/c.dylib SpringBoard\n', kernel())
ok(out == LINE + ' sweetinject=' + B + ';/usr/lib/o.dylib,backboardd;/usr/lib/c.dylib,SpringBoard',
   'o.dylib and c.dylib after the built-in three')
v = out.split('sweetinject=', 1)[1]
libs = [g.split(',')[0] for g in v.split(';')]
ok(libs == ['/usr/lib/s.dylib', '/usr/lib/m.dylib', '/usr/lib/b.dylib', '/usr/lib/o.dylib', '/usr/lib/c.dylib'],
   'every library named is a built-in one or a recorded one')
# 3. one library, several programs; the same library on two lines is one group; comments and blank lines skipped
out, _ = ci.compose(LINE, '# c\n\n/usr/lib/x.dylib A B\n/usr/lib/x.dylib B C\n', None)
ok(out.endswith(';/usr/lib/x.dylib,A,B,C'), 'a library on two lines: one group, each program once')
# 4. a recorded program a built-in group already gives the same library is not given it twice
out, _ = ci.compose(LINE, '/usr/lib/m.dylib SpringBoard InputUI\n', None)
ok(out.endswith(B + ';/usr/lib/m.dylib,InputUI'), 'm.dylib for SpringBoard (built in) not repeated, InputUI added')
out, _ = ci.compose(LINE, '/usr/lib/s.dylib backboardd\n', None)
ok(out == LINE, 'a record that only repeats a built-in group leaves the line unchanged')
# 5. the kernel's own merge: the composed line keeps every built-in argument of the kernel but sweetinject
out, _ = ci.compose(LINE, '/usr/lib/o.dylib backboardd\n', kernel())
merged, dropped = ci.merged_line(out, DEFAULTS)
ok(not dropped and merged.count('sweetinject=') == 1 and 'cp_if=1' in merged and 'sweetspeed=408782847' in merged
   and 'sweetspeed=100351' not in merged, 'the kernel would merge it with all its built-in arguments, ours winning')
# 6. refusals
refused(lambda: ci.compose(LINE + ' sweetinject=/usr/lib/z.dylib,launchd', '/usr/lib/o.dylib backboardd\n', None),
        'names sweetinject= itself', 'a line with its own sweetinject=')
refused(lambda: ci.compose(LINE + ' sweetdef=0', '/usr/lib/o.dylib backboardd\n', None), 'sweetdef', 'a line with sweetdef=')
refused(lambda: ci.compose(LINE, '/usr/lib/o.dylib\n', None), 'names no program', 'a record line without a program')
refused(lambda: ci.compose(LINE, 'usr/lib/o.dylib backboardd\n', None), 'not a library path', 'a relative library path')
refused(lambda: ci.compose(LINE, '/usr/lib/o,x.dylib backboardd\n', None), 'not a library path', 'a "," in a library path')
refused(lambda: ci.compose(LINE, '/usr/lib/o.dylib back;boardd\n', None), 'not a program name', 'a ";" in a program name')
refused(lambda: ci.compose('x' * 500, '/usr/lib/o.dylib backboardd\n', None), 'holds 607', 'a line over 607 bytes')
many = ''.join(f'/usr/lib/lib{i:03d}.dylib backboardd SpringBoard\n' for i in range(12))
refused(lambda: ci.compose('', many, None), 'the kernel keeps 511', 'a sweetinject= value over 511 bytes')
refused(lambda: ci.compose(LINE, '/usr/lib/o.dylib backboardd\n', b'no line here'), 'no built-in S9 command line',
        'a kernel without the S9 line')
refused(lambda: ci.compose(LINE, '/usr/lib/o.dylib backboardd\n', kernel(DEFAULTS.replace('/usr/lib/b.dylib,backboardd', '/usr/lib/b.dylib,backboardd,X'))),
        'not the one', 'a kernel whose built-in sweetinject= differs')
long_defaults = DEFAULTS + ' ' + ' '.join(f'arg{i}=1' for i in range(120))
refused(lambda: ci.compose(LINE, '/usr/lib/o.dylib backboardd\n', kernel(long_defaults)), 'would leave out',
        'a line that would push the kernel\'s own arguments off its 1024 bytes')
# 7. the command as make-boot-image.sh runs it
with tempfile.TemporaryDirectory() as t:
    rec = os.path.join(t, 'root-full.img.inject')
    with open(rec, 'w') as f:
        f.write('# made up\n/usr/lib/o.dylib backboardd\n')
    kern = os.path.join(t, 'kernel')
    with open(kern, 'wb') as f:
        f.write(kernel())
    r = subprocess.run([sys.executable, os.path.join(HERE, 'cmdline-inject.py'), 'compose', '--line', LINE,
                        '--record', rec, '--kernel', kern], capture_output=True, text=True)
    ok(r.returncode == 0 and r.stdout.strip() == LINE + ' sweetinject=' + B + ';/usr/lib/o.dylib,backboardd',
       'the command prints the composed line')
    with open(rec, 'w') as f:
        f.write('/usr/lib/o.dylib\n')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'cmdline-inject.py'), 'compose', '--line', LINE,
                        '--record', rec], capture_output=True, text=True)
    ok(r.returncode == 1 and r.stdout == '' and 'names no program' in r.stderr, 'the command refuses with status 1 and prints no line')
print('CMDLINE-INJECT-TEST: ' + ('ALL PASSED' if not fails else f'{fails} FAILED'))
sys.exit(1 if fails else 0)
