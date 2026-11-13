#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""The access point names of the world's mobile networks, from Android's public list, as a table to choose from.

  apn-table.py fetch  OUT.xml                 the list, from its public repository at the pinned commit, hash checked
  apn-table.py table  IN.xml OUT.tbl          the "default" (internet) entries as a compact table
  apn-table.py select IN.tbl --mcc 272 --mnc 03 [--spn NAME] [--gid1 HEX] [--imsi DIGITS] [--iccid DIGITS]
                                              the entry Android's rules choose for such a SIM, or why none

A phone's data connection needs the name of the operator's access point (the APN). Nothing in the modem knows it:
Android ships a list of every network's, device/sample/etc/apns-full-conf.xml (Apache-2.0, The Android Open Source
Project), and chooses from it by what the SIM says. This tool takes that list exactly as published (a pinned commit,
its SHA-256 checked; entries are never edited here) and makes the table a driver or a small helper searches. The
list is not in this repository and neither is the table: both are made when an image is built. An image that
carries the table carries data derived from the list: NOTICE and THIRD_PARTY_LICENSES.md have its row.

THE CHOICE (Android's, android.telephony.data.ApnSetting and the telephony provider's rules, as facts):
  1. the entries whose mcc and mnc are the SIM's own (the first digits of its subscriber number; how many digits
     the mnc has is in the SIM's EF_AD), of type "default" (an entry with no type, or "*", serves every type);
  2. an entry may be for one brand on a network (an MVNO): mvno_type says what tells the brand, mvno_match_data
     its value: "spn" the SIM's service provider name (compared without case), "gid" the first bytes of its
     group identifier (hex, without case), "imsi" a pattern of leading digits of the subscriber number ('x' any
     digit), "iccid" leading digits of the card's number;
  3. if any brand entry matches the SIM, only brand entries are candidates; otherwise only the entries with no
     brand. The more specific match wins;
  4. HERE, one step stricter than Android (which then lets the user pick): if the candidates do not all name the
     same access point, protocol and credentials, none is chosen. No guess: no data connection, and a line saying so.

What is compared with the SIM (its name, group identifier, the leading digits of its numbers) is read from the SIM,
used for the comparison and forgotten: a chooser logs the chosen entry's carrier name and access point, never the
SIM's values. ("select" takes them on its command line for tests with made-up values only.)

THE TABLE (little-endian; every length is checked by a reader before use)
  header   "SAPN", u8 version (1), 3 x 0, u32 rows, u32 bytes of all rows, the list's SHA-256 (32 bytes)
  a row    u16 its length, u16 mcc, u16 mnc, u8 digits of the mnc (2 or 3), u8 brand kind (0 none, 1 spn, 2 imsi,
           3 gid, 4 iccid), u8 protocol and u8 roaming protocol (0 IP, 1 IPV6, 2 IPV4V6), u8 authentication
           (0 none, 1 PAP, 2 CHAP, 3 either; 255 not given), u8 0, then five strings, each a length byte and
           that many bytes: carrier name, access point name, brand value, user name, password
  rows are in the order of (mcc, mnc), then the list's.
"""
import base64
import hashlib
import struct
import sys
import urllib.request
import xml.etree.ElementTree as ET

PIN_REPO = 'https://android.googlesource.com/device/sample'
PIN_COMMIT = '73f904fbeb872f5675cb5f18cd9dfb15856425a6'
PIN_PATH = 'etc/apns-full-conf.xml'
PIN_SHA256 = '9e44015793ec01d0773123103dca19b181fbc5fdd43cfc1b1d5cfacbda622726'

KINDS = {'': 0, 'spn': 1, 'imsi': 2, 'gid': 3, 'iccid': 4}
PROTOCOLS = {'IP': 0, 'IPV6': 1, 'IPV4V6': 2}
HEADER = struct.Struct('<4sB3xII32s')
ROW = struct.Struct('<HHHBBBBBB')


def fetch(out):
    url = '%s/+/%s/%s?format=TEXT' % (PIN_REPO, PIN_COMMIT, PIN_PATH)
    data = base64.b64decode(urllib.request.urlopen(url, timeout=120).read())
    check(data, url)
    open(out, 'wb').write(data)
    print('%s: %d bytes, sha256 as pinned (%s at %s)' % (out, len(data), PIN_PATH, PIN_COMMIT[:12]))


def check(data, where):
    got = hashlib.sha256(data).hexdigest()
    if got != PIN_SHA256:
        sys.exit('%s: sha256 %s is not the pinned list (%s)' % (where, got, PIN_SHA256))


def text(value, what):
    raw = (value or '').encode('utf-8')
    if len(raw) > 255:
        sys.exit('%s longer than 255 bytes' % what)
    return bytes([len(raw)]) + raw


def table(source, out):
    data = open(source, 'rb').read()
    check(data, source)
    rows = []
    for order, apn in enumerate(ET.fromstring(data).iter('apn')):
        get = lambda name: (apn.get(name) or '').strip()
        kinds = [k.strip() for k in get('type').split(',') if k.strip()]
        if kinds and 'default' not in kinds and '*' not in kinds:
            continue
        if get('carrier_enabled').lower() in ('false', '0') or not get('apn'):
            continue
        mcc, mnc = get('mcc'), get('mnc')
        if not (mcc.isdigit() and len(mcc) == 3 and mnc.isdigit() and len(mnc) in (2, 3)):
            continue
        kind = get('mvno_type').lower()
        if kind not in KINDS:
            sys.exit('entry %d: mvno_type %r is not one this table knows' % (order, kind))
        protocol = PROTOCOLS.get(get('protocol') or 'IP')
        roaming = PROTOCOLS.get(get('roaming_protocol') or get('protocol') or 'IP')
        if protocol is None or roaming is None:
            sys.exit('entry %d: a protocol this table does not know' % order)
        auth = int(get('authtype')) if get('authtype') not in ('', '-1') else 255
        strings = b''.join((text(get('carrier'), 'a carrier name'), text(get('apn'), 'an access point name'),
                            text(get('mvno_match_data'), 'a brand value'), text(get('user'), 'a user name'),
                            text(get('password'), 'a password')))
        row = ROW.pack(ROW.size + len(strings), int(mcc), int(mnc), len(mnc), KINDS[kind], protocol, roaming,
                       auth, 0) + strings
        rows.append(((int(mcc), int(mnc), len(mnc), order), row))
    rows.sort(key=lambda r: r[0])
    body = b''.join(row for _, row in rows)
    open(out, 'wb').write(HEADER.pack(b'SAPN', 1, len(rows), len(body), hashlib.sha256(data).digest()) + body)
    print('%s: %d default entries, %d bytes' % (out, len(rows), HEADER.size + len(body)))


def read(path):
    blob = open(path, 'rb').read()
    if len(blob) < HEADER.size:
        sys.exit('%s: too short for a table' % path)
    magic, version, count, size, _ = HEADER.unpack_from(blob)
    if magic != b'SAPN' or version != 1 or HEADER.size + size != len(blob):
        sys.exit('%s: not a version 1 table of its stated size' % path)
    at, rows = HEADER.size, []
    for _ in range(count):
        if at + ROW.size > len(blob):
            sys.exit('%s: a row beyond the table' % path)
        length, mcc, mnc, digits, kind, protocol, roaming, auth, _ = ROW.unpack_from(blob, at)
        if length < ROW.size + 5 or at + length > len(blob):
            sys.exit('%s: a row of a wrong length' % path)
        p, strings = at + ROW.size, []
        for _ in range(5):
            if p >= at + length or p + 1 + blob[p] > at + length:
                sys.exit('%s: a string beyond its row' % path)
            strings.append(blob[p + 1:p + 1 + blob[p]].decode('utf-8'))
            p += 1 + blob[p]
        rows.append(dict(mcc=mcc, mnc=mnc, digits=digits, kind=kind, protocol=protocol, roaming=roaming, auth=auth,
                         carrier=strings[0], apn=strings[1], brand=strings[2], user=strings[3], password=strings[4]))
        at += length
    return rows


def brand_matches(row, spn, gid1, imsi, iccid):
    value = row['brand']
    if row['kind'] == 1:
        return spn is not None and spn.lower() == value.lower()
    if row['kind'] == 2:
        return imsi is not None and len(value) <= len(imsi) and all(c in 'xX' or c == d for c, d in zip(value, imsi))
    if row['kind'] == 3:
        return gid1 is not None and gid1.lower().startswith(value.lower()) and value != ''
    if row['kind'] == 4:
        return iccid is not None and any(p and iccid.startswith(p) for p in value.split(','))
    return False


def choose(rows, mcc, mnc, digits, spn=None, gid1=None, imsi=None, iccid=None):
    """The chosen row, or None and the reason."""
    mine = [r for r in rows if r['mcc'] == mcc and r['mnc'] == mnc and r['digits'] == digits]
    if not mine:
        return None, 'the list has no default entry for this network'
    brand = [r for r in mine if r['kind'] != 0 and brand_matches(r, spn, gid1, imsi, iccid)]
    candidates = brand or [r for r in mine if r['kind'] == 0]
    if not candidates:
        return None, "the list's entries for this network are all for brands, and none is this SIM's"
    same = {(r['apn'].lower(), r['protocol'], r['user'], r['password'], r['auth']) for r in candidates}
    if len(same) != 1:
        return None, '%d entries with different access points fit this SIM equally: none is chosen' % len(candidates)
    return candidates[0], 'a brand entry' if brand else "the network's own entry"


def select(argv):
    rows = read(argv[0])
    options = dict(zip(argv[1::2], argv[2::2]))
    mnc = options.get('--mnc', '')
    if not (options.get('--mcc', '').isdigit() and mnc.isdigit() and len(mnc) in (2, 3)):
        sys.exit('select: --mcc NNN and --mnc NN or NNN')
    row, why = choose(rows, int(options['--mcc']), int(mnc), len(mnc), options.get('--spn'), options.get('--gid1'),
                      options.get('--imsi'), options.get('--iccid'))
    if row is None:
        sys.exit('no access point chosen: ' + why)
    print('chosen (%s): carrier "%s", access point "%s", protocol %s, user name %s' % (
        why, row['carrier'], row['apn'], ['IP', 'IPV6', 'IPV4V6'][row['protocol']], 'given' if row['user'] else 'none'))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'fetch':
        fetch(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'table':
        table(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 7 and sys.argv[1] == 'select':
        select(sys.argv[2:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
