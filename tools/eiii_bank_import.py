#!/usr/bin/env python3
"""Convert Emulator IIIX and ESI-32 banks into Emulator III banks, on a disc image the EIII can load from.

  eiii_bank_import.py list SOURCE.iso
  eiii_bank_import.py convert SOURCE.iso OUT.iso [--bank NAME ...] [--presets BANK=0,3,7 ...]

SOURCE is any EMU3-format CD or hard disk image (EIII CDs with "X" banks, ESI-32 CDs). OUT is a new
CD image holding only the converted banks; mount it as a CD (or extra drive) and use Load Bank.
Without --bank every EIIIX / ESI-32 bank on SOURCE is converted. A bank whose samples do not fit
the EIII's 8 MB is split into parts ("<name> A", "<name> B", ...), each holding whole presets
(a preset and the presets it links to stay together). --presets keeps only the listed preset
numbers of a bank (plus their links).

Formats (see analysis notes; all little-endian, 512-byte blocks):
  bank file = 0x4C-byte header ("EMULATOR THREE " / "EMULATOR 3X    " / "EMU SI-32 v3   ") followed by a
  RAM image based at 0x1A000 and a sample-RAM image based at 0x400000 (EIII: plus a tail ending at
  0xBFD800). Preset records and sample headers are the same in all three; the bank tables differ:
    EIII      presets: 101 absolute pointers at 0x1A020, heap from 0x1A6FE; samples: 100 pointers at 0x1A1B4
    3X/ESI    presets: 101 offsets at 0x1B77E from heap 0x1CB26;          samples: 1000 pointers at 0x1BB82
  Preset record: +0x35 zone count, 0x8E-byte head, 4 bytes per zone, 0x30-byte voice records
  (voice record +1 = sample number, preset +0x31 = linked preset). Sample pitch is relative to 44.1 kHz in
  all three formats, so headers are copied unchanged.
"""
import argparse, os, struct, sys

BLOCK = 512
RAM_BASE, HEAP_EIII, HEAP_X = 0x1A000, 0x1A6FE, 0x1CB26
SND_BASE, SND_TOP = 0x400000, 0xBFD800          # sample RAM of an 8 MB EIII; the OS keeps its tables above SND_TOP
RAM_LIMIT = 0x28000                             # end of the EIII's bank RAM
TAGS = {b'EMULATOR THREE ': 'EIII', b'EMULATOR 3X    ': 'EIIIX', b'EMU SI-32 v3   ': 'ESI-32'}


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]


class Volume:
    """Read-only EMU3 volume (CD or hard disk image)."""
    def __init__(self, path):
        self.f = open(path, 'rb')
        vh = self.blocks(0)
        if vh[:4] != b'EMU3':
            raise SystemExit('%s: not an EMU3 (Emulator III / ESI) disc image' % path)
        self.dir_lba, self.dir_n, fat_lba, fat_n, self.data_lba = (u32(vh, o) for o in (0x10, 0x14, 0x18, 0x1C, 0x20))
        self.csize = 0x80 << (vh[0x28] - 1)
        self.fat = self.blocks(fat_lba, fat_n)
        d = self.blocks(self.dir_lba, self.dir_n)
        self.entries = [d[i:i + 32] for i in range(0, len(d), 32) if d[i + 0x1A] == 0x81 and d[i] >= 0x20]

    def blocks(self, lba, n=1):
        self.f.seek(lba * BLOCK)
        return self.f.read(n * BLOCK)

    def read(self, e):
        c, out = u16(e, 0x12), []
        while c and c != 0x7FFF and len(out) < 0x10000:
            out.append(self.blocks(self.data_lba + (c - 1) * self.csize, self.csize))
            c = u16(self.fat, 2 * c)
        return b''.join(out)

    def banks(self):
        for e in self.entries:
            head = self.read_head(e)
            kind = TAGS.get(head[:15])
            if kind:
                yield e, kind

    def read_head(self, e):
        c = u16(e, 0x12)
        return self.blocks(self.data_lba + (c - 1) * self.csize)[:0x4C]


class Bank:
    def __init__(self, data):
        self.kind = TAGS.get(data[:15])
        if not self.kind:
            raise ValueError('not a bank file')
        self.name = data[0x10:0x20]
        ram_n, snd_n = u32(data, 0x30), u32(data, 0x34)
        self.ram = data[0x4C:0x4C + ram_n]
        self.snd = data[0x4C + ram_n:0x4C + ram_n + snd_n]
        if self.kind == 'EIII':
            ptr = [self.r32(0x1A020 + 4 * i) for i in range(101)]
            self.heap = HEAP_EIII
            nsmp, stab = 100, 0x1A1B4
        else:
            ptr = [HEAP_X + self.r32(0x1B77E + 4 * i) for i in range(101)]
            self.heap = HEAP_X
            nsmp, stab = 1000, 0x1BB82
        # preset i occupies [ptr[i], ptr[i+1]); an empty slot has zero length
        self.presets = {}
        for i in range(100):
            a, n = ptr[i], ptr[i + 1] - ptr[i]
            if n >= 0x8E:
                self.presets[i] = self.ram[a - RAM_BASE:a - RAM_BASE + n]
        sp = [self.r32(stab + 4 * i) for i in range(nsmp)]
        live = sorted(set(p for p in sp if SND_BASE <= p < SND_BASE + len(self.snd)))
        ends = {p: q for p, q in zip(live, live[1:] + [SND_BASE + len(self.snd)])}
        self.samples = {i: self.snd[p - SND_BASE:ends[p] - SND_BASE] for i, p in enumerate(sp) if p in ends}

    def r32(self, a): return u32(self.ram, a - RAM_BASE)

    @staticmethod
    def voice_records(rec):
        nz = rec[0x35]
        base = 0x8E + 4 * nz
        return [base + 0x30 * k for k in range((len(rec) - base) // 0x30)]

    def preset_samples(self, i):
        rec = self.presets[i]
        return sorted(set(rec[v + 1] for v in self.voice_records(rec)) & set(self.samples))

    def link(self, i):
        """The preset this one links to (played along with it), or None."""
        l = self.presets[i][0x31]
        return l - 1 if 0 < l <= 100 and (l - 1) in self.presets and l - 1 != i else None

    def group(self, i):
        g, j = [], i
        while j is not None and j not in g:
            g.append(j)
            j = self.link(j)
        return g

    def sample_bytes(self, presets):
        return sum(len(self.samples[s]) for s in set(s for p in presets for s in self.preset_samples(p)))

    def preset_name(self, i): return self.presets[i][:16].decode('latin1').rstrip()


def eiii_bank_file(src, name, number, keep):
    """EIII bank file (header + RAM image + sample-RAM image) holding presets `keep` of `src`."""
    used = sorted(set(s for p in keep for s in src.preset_samples(p)))
    # sample numbers: keep the originals when they fit the EIII's 1..99, else renumber in order
    remap = {s: s for s in used} if all(s < 100 for s in used) else {s: k + 1 for k, s in enumerate(used)}
    if len(used) > 99:
        raise ValueError('more than 99 samples')
    snd, sptr = bytearray(), [0] * 100
    for s in used:
        sptr[remap[s]] = SND_BASE + len(snd)
        snd += src.samples[s]
        snd += b'\0' * (len(snd) & 1)                 # sample headers sit on word addresses
    if SND_BASE + len(snd) > SND_TOP:
        raise ValueError('samples need %.2f MB' % (len(snd) / 1048576))

    heap, pptr = bytearray(), []
    for i in range(100):
        pptr.append(HEAP_EIII + len(heap))
        if i in keep:
            rec = bytearray(src.presets[i])
            for v in Bank.voice_records(rec):
                if rec[v + 1] in remap:
                    rec[v + 1] = remap[rec[v + 1]]
            heap += rec
    pptr.append(HEAP_EIII + len(heap))
    ram = bytearray(HEAP_EIII - RAM_BASE)
    # channel tables at 0x1A686 as every factory bank has them (all MIDI channels on the selected preset)
    ram[0x690:0x6C0] = bytes.fromhex('000000000000ffff000000feffff2800'
                                     '02000500556e00000080ffff000000fe'
                                     'ffff280002000500556e000000800000')
    ram[0:16] = name
    # start on the first preset that plays (ESI banks keep an empty "Untitled Preset" at 0): selected preset, and
    # the MIDI channel -> preset map at 0x1A686
    first = min((p for p in keep if src.preset_samples(p)), default=min(keep))
    struct.pack_into('<II', ram, 0x10, first, 1)                  # +0x14 as in factory banks
    ram[0x686:0x696] = bytes([first]) * 16
    struct.pack_into('<101I', ram, 0x20, *pptr)
    struct.pack_into('<100I', ram, 0x1B4, *sptr)
    struct.pack_into('<II', ram, 0x344, SND_BASE + len(snd), SND_TOP)  # sample memory used / top
    ram += heap
    ram += b'\0'                                    # factory banks end one byte past the last preset
    if RAM_BASE + len(ram) > RAM_LIMIT:
        raise ValueError('presets need too much RAM')

    def nblk(n): return -(-n // BLOCK)
    b0, b1 = nblk(0x4C + len(ram)), len(snd) // BLOCK + 1
    head = bytearray(0x4C)
    head[0:16] = b'EMULATOR THREE \0'
    head[16:32] = name
    struct.pack_into('<4I', head, 0x20, number, 1, 1, -(-(b0 + b1) // 1600))  # last: 800 KB floppies needed
    struct.pack_into('<3I', head, 0x30, len(ram), len(snd), 0)
    struct.pack_into('<4I', head, 0x3C, b0, b1, 0, b0 + b1)
    return bytes(head) + bytes(ram) + bytes(snd), len(snd)


def plan(src, keep=None):
    """Split the presets to keep into parts that fit the EIII; linked presets stay together."""
    keep = sorted(src.presets) if keep is None else sorted(set(p for k in keep if k in src.presets for p in src.group(k)))
    parts, cur, done = [], [], set()
    for p in keep:
        if p in done:
            continue
        g = [q for q in src.group(p) if q in keep]
        done.update(g)
        if src.sample_bytes(g) > SND_TOP - SND_BASE or len(set(s for q in g for s in src.preset_samples(q))) > 99:
            print('  skipped preset %d %s: too large for the EIII' % (p, src.preset_name(p)))
            continue
        trial = cur + [q for q in g if q not in cur]
        if cur and (src.sample_bytes(trial) > SND_TOP - SND_BASE - 0x100 or
                    len(set(s for q in trial for s in src.preset_samples(q))) > 99):
            parts.append(cur)
            trial = g
        cur = trial
    if cur:
        parts.append(cur)
    # a part whose presets only link to presets that did not fit has nothing to play
    for p in [p for p in parts if not src.sample_bytes(p)]:
        print('  skipped %d preset(s) without samples: %s' % (len(p), ', '.join(src.preset_name(q) for q in p)))
    return [p for p in parts if src.sample_bytes(p)]


def write_volume(path, files):
    """CD image (EMU3 volume, 256 KB clusters) holding the given (name, id, data) bank files."""
    csize = 0x200                                   # blocks per cluster (cluster code 3, as on E-mu's CDs)
    fat_lba, fat_n, vol_lba, vol_n, dir_lba, dir_n = 2, 5, 7, 2, 9, 0x66
    data_lba = dir_lba + dir_n
    fat = [0] * (fat_n * BLOCK // 2)
    fat[0] = 0x8000
    entries, next_c, placed = bytearray(dir_n * BLOCK), 1, []
    for k, (name, num, data) in enumerate(files):
        nb = -(-len(data) // BLOCK)
        nc = -(-nb // csize)
        if next_c + nc >= len(fat) or k >= 7 * BLOCK // 32:
            raise SystemExit('too many banks for one image')
        for c in range(next_c, next_c + nc):
            fat[c] = c + 1
        fat[next_c + nc - 1] = 0x7FFF
        e = bytearray(32)
        e[0:16] = name
        e[0x11] = num
        struct.pack_into('<4H', e, 0x12, next_c, nc, nb - (nc - 1) * csize, len(data) - (nb - 1) * BLOCK)
        e[0x1A] = 0x81
        entries[32 * k:32 * k + 32] = e
        placed.append((data, next_c))
        next_c += nc
    total = data_lba + (next_c - 1) * csize
    total += -total % 4                             # whole 2048-byte sectors
    vh = bytearray(BLOCK)
    vh[0:4] = b'EMU3'
    struct.pack_into('<9I', vh, 4, total, vol_lba, vol_n, dir_lba, dir_n, fat_lba, fat_n, data_lba, next_c - 1)
    vh[0x28:0x2A] = b'\x03\x01'
    struct.pack_into('<I', vh, 0x2A, total + 0x0B)
    vh[0x32:0x34] = b'\x01\x0d'
    struct.pack_into('<H', vh, 510, sum(struct.unpack_from('<255H', vh)) & 0xFFFF)
    # block 1: next free directory block, then one byte per directory block (as on E-mu's CDs)
    used_dir = max(1, -(-len(files) * 32 // BLOCK))
    alloc = bytearray(b'\x80' * BLOCK)
    struct.pack_into('<I', alloc, 0, dir_lba + used_dir)
    # volume records (32 bytes): name shown for the drive, a word 0x4000, then up to 7 directory block numbers
    vol = bytearray(vol_n * BLOCK)
    vol[0:16] = b'EIII Rack Import'
    struct.pack_into('<8H', vol, 16, 0x4000, *[dir_lba + k if k < used_dir else 0xFFFF for k in range(7)])
    with open(path, 'wb') as f:
        f.write(vh)
        f.write(alloc)
        f.write(struct.pack('<%dH' % len(fat), *fat))
        f.write(vol)
        f.write(entries)
        for data, c in placed:
            f.seek((data_lba + (c - 1) * csize) * BLOCK)
            f.write(data)
        f.truncate(total * BLOCK)


def cmd_list(a):
    v = Volume(a.source)
    for e, kind in v.banks():
        b = Bank(v.read(e))
        print('%-16s  %-6s  %3d presets  %6.2f MB of samples' % (e[:16].decode('latin1'), kind, len(b.presets),
                                                             len(b.snd) / 1048576))
        if a.presets:
            for i in sorted(b.presets):
                print('    %2d  %-16s  %5.2f MB  link %s' % (i, b.preset_name(i), b.sample_bytes([i]) / 1048576, b.link(i)))


def cmd_convert(a):
    v = Volume(a.source)
    want = [w.strip().lower() for w in a.bank]
    sel = {}
    for s in a.presets:
        n, _, l = s.partition('=')
        sel[n.strip().lower()] = [int(x) for x in l.split(',') if x.strip()]
    out, num = [], 0
    for e, kind in v.banks():
        bname = e[:16].decode('latin1').strip()
        if want or sel:
            if bname.lower() not in want and bname.lower() not in sel:
                continue
        elif kind == 'EIII':
            continue
        src = Bank(v.read(e))
        parts = plan(src, sel.get(bname.lower()))
        print('%s (%s): %d preset(s) -> %d bank(s)' % (bname, kind, sum(len(p) for p in parts), len(parts)))
        for k, keep in enumerate(parts):
            base = e[:16].decode('latin1').rstrip()
            name = base if len(parts) == 1 else '%-14.14s %c' % (base, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'[k])
            name = name.ljust(16)[:16].encode('latin1')
            data, sb = eiii_bank_file(src, name, num, keep)
            print('  B%02d %s  %2d presets  %5.2f MB' % (num, name.decode('latin1'), len(keep), sb / 1048576))
            out.append((name, num, data))
            num += 1
            if num > 99:
                raise SystemExit('more than 100 banks: convert fewer at a time')
    if not out:
        raise SystemExit('nothing to convert')
    write_volume(a.out, out)
    print('wrote %s (%d banks)' % (a.out, len(out)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    l = sub.add_parser('list', help='list the banks on a disc image')
    l.add_argument('source')
    l.add_argument('--presets', action='store_true', help='also list each bank\'s presets')
    c = sub.add_parser('convert', help='write converted banks to a new CD image')
    c.add_argument('source')
    c.add_argument('out')
    c.add_argument('--bank', action='append', default=[], help='bank name to convert (repeatable)')
    c.add_argument('--presets', action='append', default=[], help='BANK=0,3,7: only these presets of BANK')
    a = ap.parse_args()
    {'list': cmd_list, 'convert': cmd_convert}[a.cmd](a)


if __name__ == '__main__':
    main()
