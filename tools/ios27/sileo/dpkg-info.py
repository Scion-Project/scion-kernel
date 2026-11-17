#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT-0
dpkg-info.py DEB ADMINDIR [--prefix-root DIR] : dpkg's own record of a package that was unpacked by hand (tools/mk-jb.sh puts Sileo
into the bootstrap that way): info/PKG.list (every path of the data archive, as dpkg writes it), info/PKG.md5sums (the
regular files) and the maintainer scripts (preinst, postinst, prerm, postrm) beside the status entry the caller wrote.
Without the list dpkg warns "files list file for package ... missing" at every run, "dpkg --audit" names the package as
broken, and a later upgrade or removal cannot know its files. Writes nothing but those files; a second run writes the same.
--prefix-root DIR: check that every regular file of the list is present under DIR (the unpacked tree), and stop if not."""
import hashlib, io, lzma, os, subprocess, sys, tarfile

def members(deb):
    data = open(deb, 'rb').read()
    assert data[:8] == b'!<arch>\n', 'not an ar archive'
    pos, out = 8, {}
    while pos < len(data):
        name = data[pos:pos + 16].decode().strip().rstrip('/')
        size = int(data[pos + 48:pos + 58])
        out[name] = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    return out

def tar_of(name, blob):
    if name.endswith('.zst'):
        blob = subprocess.run(['zstd', '-dc'], input=blob, stdout=subprocess.PIPE, check=True).stdout
        return tarfile.open(fileobj=io.BytesIO(blob), mode='r:')
    return tarfile.open(fileobj=io.BytesIO(blob), mode='r:*')

def main():
    args = sys.argv[1:]
    root = None
    if '--prefix-root' in args:
        i = args.index('--prefix-root'); root = args[i + 1]; del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    deb, admin = args
    m = members(deb)
    ctl = tar_of(*next((k, v) for k, v in m.items() if k.startswith('control.tar')))
    control = ctl.extractfile(ctl.getmember('./control')).read().decode()
    pkg = next(l.split(':', 1)[1].strip() for l in control.splitlines() if l.startswith('Package:'))
    data = tar_of(*next((k, v) for k, v in m.items() if k.startswith('data.tar')))
    paths, sums = ['/.'], []
    for t in data.getmembers():
        p = '/' + t.name.lstrip('./').rstrip('/') if t.name not in ('.', './') else None
        if not p or p == '/':
            continue
        paths.append(p)
        if t.isreg():
            sums.append('%s  %s' % (hashlib.md5(data.extractfile(t).read()).hexdigest(), p[1:]))
            if root and not os.path.isfile(root + p):
                raise SystemExit('dpkg-info.py: %s is not in %s' % (p, root))
    info = os.path.join(admin, 'info')
    os.makedirs(info, exist_ok=True)
    open(os.path.join(info, pkg + '.list'), 'w').write('\n'.join(paths) + '\n')
    open(os.path.join(info, pkg + '.md5sums'), 'w').write('\n'.join(sums) + '\n')
    n = 0
    for s in ('preinst', 'postinst', 'prerm', 'postrm'):
        try:
            body = ctl.extractfile(ctl.getmember('./' + s)).read()
        except KeyError:
            continue
        dst = os.path.join(info, pkg + '.' + s)
        open(dst, 'wb').write(body); os.chmod(dst, 0o755); n += 1
    print('dpkg-info.py: %s: %d paths, %d files summed, %d scripts' % (pkg, len(paths), len(sums), n))

main()
