#!/usr/bin/env python3
"""
List the imports of interest in homebrew eboots: which SceCtrl, SceTouch,
SceMotion, SceDisplay and GXM entry points a title calls, to compare with
the helper's hook table (docs/HACKING.md). Found the vitaGL
`sceGxmVshInitialize` miss on 2026-10-27.

Homebrew eboots are zlib-compressed SELFs: each program segment is
decompressed before its bytes are searched for the NIDs. Retail eboots are
encrypted and print nothing. NIDs and names come from the vitasdk database.

Usage: tools/scan_eboot.py EBOOT... [-m REGEX] [--db DIR]
       (pull eboots with FTP from ux0:/app/<TITLEID>/eboot.bin)
"""
import argparse, glob, os, re, struct, sys, zlib

DEFAULT_DB = os.path.join(os.environ.get('VITASDK', '/usr/local/vitasdk'),
                          'share/vita-headers/db/360')
DEFAULT_MATCH = ('sceCtrl(Peek|Read)|sceTouch(Peek|Read)|sceMotion|sceDisplayWait'
                 '|sceDisplaySetFrameBuf|sceGxmInitialize|sceGxmVshInitialize'
                 '|sceGxmDisplayQueueAddEntry|sceKernelLoadStartModule|sceKernelStartModule')
DB_FILES = ['SceCtrl.yml', 'SceTouch.yml', 'SceMotion.yml', 'SceDisplay.yml', 'SceGxm.yml',
            'SceLibKernel.yml', 'SceKernelModulemgr.yml']


def load_nids(db_dir):
    """function NID -> name, for the libraries in DB_FILES."""
    nids = {}
    for name in DB_FILES:
        path = os.path.join(db_dir, name)
        if not os.path.exists(path):
            continue
        for line in open(path):
            m = re.match(r'^          (\w+):\s*(0x[0-9A-Fa-f]+)', line)
            if m:
                nids[int(m.group(2), 16)] = m.group(1)
    return nids


def segments(data):
    """Program segments of a SELF, decompressed; a plain file is one segment."""
    if data[:4] != b'SCE\0':
        return [data]
    hdr = struct.unpack_from('<4sIHHIQQQQQQQQQQQQQ', data, 0)
    elf_off, phdr_off, secinfo_off = hdr[11], hdr[12], hdr[14]
    phnum = struct.unpack_from('<H', data, elf_off + 44)[0]
    out = []
    for i in range(phnum):
        off, length, comp, _enc = struct.unpack_from('<QQQQ', data, secinfo_off + 32 * i)
        raw = data[off:off + length]
        if comp == 2:
            try:
                raw = zlib.decompress(raw)
            except zlib.error:
                raw = b''
        out.append(raw)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('eboot', nargs='+', help='eboot.bin files or globs')
    ap.add_argument('-m', '--match', default=DEFAULT_MATCH, help='regex on function names')
    ap.add_argument('--db', default=DEFAULT_DB, help='vitasdk NID database directory')
    args = ap.parse_args()
    want = re.compile(args.match)
    nids = {nid: name for nid, name in load_nids(args.db).items() if want.search(name)}
    if not nids:
        sys.exit('no NIDs loaded from %s' % args.db)
    paths = [p for pat in args.eboot for p in sorted(glob.glob(pat))] or args.eboot
    for path in paths:
        data = open(path, 'rb').read()
        found = set()
        for raw in segments(data):
            for nid, name in nids.items():
                if raw.find(struct.pack('<I', nid)) >= 0:
                    found.add(name)
        tag = '' if found else '(encrypted, or no import of interest)'
        print(os.path.basename(path), tag, ' '.join(sorted(found)))


if __name__ == '__main__':
    main()
