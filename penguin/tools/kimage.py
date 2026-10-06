#!/usr/bin/env python3
"""Recover the load address and symbol table of a raw (headerless) Linux image.

Bring-your-own-kernel starts with a blob: a vendor kernel extracted from a
firmware image, stripped, not an ELF, with no headers saying where it was
linked. Everything `bootwatch` needs -- the boot ladder, `__log_buf` -- is a
kernel virtual address, so the load base has to be recovered before anything
else can happen.

Two independent tables in the image make that deterministic:

  __ksymtab          array of {value, name_ptr} pairs, both absolute virtual
                     addresses. Since the *name* is a string we can also find
                     by content, `base = name_ptr - file_offset_of_string`
                     falls straight out, with no guessing and no heuristics.
                     This is what fixes the base.

  kallsyms           addresses[], num_syms, a token-compressed name stream,
                     markers[] and a 256-entry token table. This is what gives
                     every symbol, including static ones.

Do them in that order. Anchoring on kallsyms first is tempting and wrong: its
sub-tables are separated by linker alignment padding whose size is not implied
by anything in the format, so deriving one table's offset from another's is
off by a few entries, which silently shifts every name/address pairing. Fixing
the base from __ksymtab first turns kallsyms into a solved lookup: search for
the address-array offset that reproduces the ksymtab pairs and take the
unanimous answer.

Usage:
    kimage.py <image> [--syms out.txt] [--ladder sym,sym,...]

`--ladder` prints a ready-made `bootwatch` argument string.
"""
import argparse
import collections
import struct
import sys

KERNEL_LO, KERNEL_HI = 0x80000000, 0x81000000   # MIPS kseg0; adjust per target


class Image:
    def __init__(self, path, endian=">"):
        self.d = open(path, "rb").read()
        self.e = endian
        self.base = None
        self.syms = []
        self._wpos = None
        self._seed = []

    def word(self, off):
        return struct.unpack_from(self.e + "I", self.d, off)[0]

    def cstr(self, off):
        end = self.d.index(b"\0", off)
        return self.d[off:end].decode("ascii", "replace")

    # -- step 1: the load base, from __ksymtab ------------------------------

    def _index(self):
        """Position index of every word that looks like a kernel address."""
        if getattr(self, "_wpos", None) is None:
            n = len(self.d) // 4
            words = struct.unpack(self.e + "%dI" % n, self.d[:n * 4])
            wpos = collections.defaultdict(list)
            for i, w in enumerate(words):
                if KERNEL_LO <= w < KERNEL_HI:
                    wpos[w].append(i * 4)
            self._wpos = wpos
        return self._wpos

    def find_base(self, probes=("printk", "kmalloc", "memcpy", "schedule",
                                "kfree", "panic", "jiffies", "vfree",
                                "strlen", "memset", "kstrdup", "vmalloc")):
        """A ksymtab entry's name pointer minus the file offset of that same
        string is the load base. Two adjacent entries pin it unambiguously."""
        wpos = self._index()

        # File offsets of strings that are certain to be exported-symbol names.
        strs = {}
        for name in probes:
            pat = name.encode() + b"\0"
            hits, i = [], 0
            while True:
                j = self.d.find(pat, i)
                if j < 0:
                    break
                if j and self.d[j - 1] == 0:   # preceded by a NUL: a table entry
                    hits.append(j)
                i = j + 1
            if len(hits) == 1:
                strs[name] = hits[0]
        if len(strs) < 2:
            raise RuntimeError("not enough unique exported-name strings")

        # Score a candidate base by how many *distinct* probe strings have a
        # pointer to them somewhere in the image. Every probe is an exported
        # symbol, so the true base scores all of them; a coincidence scores one
        # or two. Adjacency only breaks ties.
        cand = collections.defaultdict(dict)
        for name, off in strs.items():
            for b in range(KERNEL_LO, KERNEL_LO + 0x100000, 4):
                positions = wpos.get(b + off)
                if positions:
                    cand[b][name] = positions

        best = None
        for b, hits in cand.items():
            pos = sorted({p for ps in hits.values() for p in ps})
            adj = sum(1 for x, y in zip(pos, pos[1:]) if y - x == 8)
            score = (len(hits), adj)
            if best is None or score > best[0]:
                best = (score, b, pos)
        if not best or best[0][0] < 2:
            raise RuntimeError("no load base found")
        self.base = best[1]
        self._seed = best[2]
        return self.base

    def ksymtab(self):
        """Grow the array outwards from a confirmed name-pointer position."""
        assert self.base is not None
        best = None
        for seed in self._seed:
            off = seed - 4                      # entry = {value, name_ptr}
            if not self._entry_ok(off):
                continue
            lo = off
            while lo >= 8 and self._entry_ok(lo - 8):
                lo -= 8
            hi = off
            while self._entry_ok(hi):
                hi += 8
            if best is None or hi - lo > best[1] - best[0]:
                best = (lo, hi)
        if not best:
            return {}
        lo, hi = best
        return {self.cstr(self.word(o + 4) - self.base): self.word(o)
                for o in range(lo, hi, 8)}

    def _entry_ok(self, off):
        if off < 0 or off + 8 > len(self.d):
            return False
        v, p = self.word(off), self.word(off + 4)
        if not (KERNEL_LO <= v < KERNEL_HI and KERNEL_LO <= p < KERNEL_HI):
            return False
        if not (0 <= p - self.base < len(self.d)):
            return False
        s = self.cstr(p - self.base)
        return bool(s) and all(c.isalnum() or c == "_" for c in s)

    # -- step 2: kallsyms ---------------------------------------------------
    #
    # Order matters here too. The name stream can be *walked* without knowing
    # the token table at all -- each record is an explicit length byte -- so
    # the whole layout can be pinned down before a single token is decoded:
    #
    #   addresses[]  longest sorted run of kernel-range words
    #   num_syms     the count, just past it (alignment padding in between)
    #   names        walk num_syms records for their offsets
    #   markers[]    every 256th name offset -- an exact check on all of it
    #   token_table  immediately after the markers
    #
    # Trying to find the token table first (it is the one humanly recognisable
    # part) turns this into a search with hundreds of false starts.

    def _address_run(self):
        """Longest run of non-decreasing kernel-range words: kallsyms_addresses.

        Its ends are approximate -- adjacent data can look sorted too -- so the
        caller must refine both, which _solve_addresses() does exactly."""
        n = len(self.d) // 4
        words = struct.unpack(self.e + "%dI" % n, self.d[:n * 4])
        best = (0, 0, 0)
        i = 0
        while i < n:
            if not (KERNEL_LO <= words[i] < KERNEL_HI):
                i += 1
                continue
            j = i + 1
            while j < n and KERNEL_LO <= words[j] < KERNEL_HI \
                    and words[j] >= words[j - 1]:
                j += 1
            if j - i > best[2] - best[1]:
                best = (0, i, j)
            i = max(j, i + 1)
        return best[1] * 4, best[2] * 4

    def kallsyms(self, truth):
        lo, hi = self._address_run()
        # num_syms sits just past the array, after ALIGN padding.
        for q in range(hi - 16, hi + 32, 4):
            if q < 0 or q + 4 > len(self.d):
                continue
            num = self.word(q)
            if not (1000 < num < 500000):
                continue
            if not (lo - 64 <= q - 4 * num <= lo + 64):
                continue
            r = self._layout(q, num, truth)
            if r:
                return r
        raise RuntimeError("kallsyms tables not resolved")

    def _layout(self, num_off, num, truth):
        # The name stream starts after the padding; padding is zero and a real
        # record never starts with a zero length byte.
        p = num_off + 4
        while p < len(self.d) and self.d[p] == 0:
            p += 1
        names_off = p
        offs = []
        for _ in range(num):
            if p >= len(self.d):
                return None
            offs.append(p - names_off)
            p += 1 + self.d[p]
        names_end = p

        # markers[] proves the walk stayed in sync, at every 256th symbol.
        nmark = (num + 255) // 256
        mk = None
        for cand in range(names_end, names_end + 32):
            if cand % 4 or cand + 4 * nmark > len(self.d):
                continue
            if all(self.word(cand + 4 * k) == offs[k * 256]
                   for k in range(nmark)):
                mk = cand
                break
        if mk is None:
            return None

        tok_off = mk + 4 * nmark
        toks, p = [], tok_off
        for _ in range(256):
            end = self.d.find(b"\0", p)
            if end < 0:
                return None
            toks.append(self.d[p:end])
            p = end + 1

        names = []
        p = names_off
        for _ in range(num):
            ln = self.d[p]
            p += 1
            names.append(b"".join(toks[c] for c in self.d[p:p + ln])
                         .decode("ascii", "replace"))
            p += ln

        addr_off = self._solve_addresses(names, truth)
        if addr_off is None:
            return None
        addrs = struct.unpack_from(self.e + "%dI" % num, self.d, addr_off)
        self.syms = [(a, s[0], s[1:]) for a, s in zip(addrs, names) if s]
        return self.syms

    def _solve_addresses(self, names, truth):
        """Vote for the address-array offset that reproduces the ksymtab.

        This is why the base has to come first: with ground truth in hand the
        offset is not searched for, it is *elected*, and a unanimous vote over
        thousands of symbols is proof rather than a heuristic."""
        wpos = self._index()
        idx = collections.defaultdict(list)
        for i, s in enumerate(names):
            if s:
                idx[s[1:]].append(i)
        votes = collections.Counter()
        for name, va in truth.items():
            ii = idx.get(name)
            if not ii or len(ii) != 1:
                continue
            for o in wpos.get(va, ()):
                cand = o - 4 * ii[0]
                if 0 <= cand < len(self.d):
                    votes[cand] += 1
        if not votes:
            return None
        off, hits = votes.most_common(1)[0]
        return off if hits >= max(8, len(truth) // 4) else None

    def lookup(self, name):
        for a, _, n in self.syms:
            if n == name:
                return a
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--little", action="store_true", help="little-endian image")
    ap.add_argument("--syms", help="write the recovered symbol table here")
    ap.add_argument("--ladder", help="comma-separated symbols for a bootwatch "
                                     "rung= argument")
    args = ap.parse_args()

    img = Image(args.image, "<" if args.little else ">")
    base = img.find_base()
    truth = img.ksymtab()
    syms = img.kallsyms(truth)
    agree = sum(1 for n, va in truth.items() if img.lookup(n) == va)

    print("load base       0x%08x" % base, file=sys.stderr)
    print("exported syms   %d (%d/%d agree with kallsyms)"
          % (len(truth), agree, len(truth)), file=sys.stderr)
    print("kallsyms        %d symbols" % len(syms), file=sys.stderr)
    for want in ("_text", "_stext", "kernel_entry", "start_kernel", "__log_buf"):
        a = img.lookup(want)
        if a:
            print("  %-14s 0x%08x  (file offset 0x%x)"
                  % (want, a, a - base), file=sys.stderr)

    if args.syms:
        with open(args.syms, "w") as f:
            for a, t, n in syms:
                f.write("%08x %s %s\n" % (a, t, n))
    if args.ladder:
        parts = []
        for n in args.ladder.split(","):
            a = img.lookup(n)
            if a is None:
                print("  no symbol %s" % n, file=sys.stderr)
                continue
            parts.append("rung=0x%x:%s" % (a, n))
        print(",".join(parts))


if __name__ == "__main__":
    main()
