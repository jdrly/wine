#!/usr/bin/env python3
# gw2 (jdrly/gw2-on-mac#40): compare my ntdll.so with the one in Mach9's engine. It reads files
# and runs nothing.
#
#   ntdll-compare.py baseline <mine> <engine-unix-dir>
#   ntdll-compare.py patched <mine> <engine-unix-dir> <base ntdll.so> <base objects> <objects>
#                            <base make log> <make log> <checkout time> [<touched source>...]
#   ntdll-compare.py selftest <engine-unix-dir>
#
# <engine-unix-dir> is the engine's lib/wine/aarch64-unix. Prints a Markdown report and exits 1
# when a check fails. Both modes check the linked file's install name, rpaths, libraries, minos,
# pipe2, the seven names compatdb.so looks up and every symbol the engine's win32u.so, winemac.so
# and winemetal.so import from ntdll.so.
#
# baseline: my build of the engine's commit. Its exports and imports are the engine's, and every
#   section of every loaded segment, every load command and the link edit data are the engine's
#   byte for byte (byte_gate: what it leaves out, and why, is in its report line). Where they
#   aren't, the functions are compared with the addresses masked, only to list what differs.
# patched: my build of a later commit, made in the same tree after the baseline with every object
#   compiled again. Its exports are a superset of the engine's. Its objects are compared with the
#   base build's (compare_objects): first that both sets are what their files linked (the link
#   line in the make log, the debug map's paths and times) and that this build compiled each after
#   the checkout; then every object of a source file git doesn't list as touched is the base's,
#   compile command, code, data, relocations and symbols, debug sections aside; the functions of
#   the touched ones are listed as changed, new or gone, through their bytes and relocations.
#   With the baseline that makes the chain: the engine's file = my base build, byte for byte, and
#   my base build's objects = this build's, except the touched files'.
import bisect
import contextlib
import functools
import hashlib
import os
import struct
import sys

# The names compatdb.so looks up with dlsym (Mach9's docs/maintenance.md, "Wine-side contract")
COMPATDB = ["prepend_dll_path", "add_load_order_override", "set_compat_dpi_awareness",
            "set_compat_large_address_aware", "set_compat_gcmouse", "set_compat_force_nx",
            "set_compat_opengl"]
DEPENDENTS = ["win32u.so", "winemac.so", "winemetal.so"]

LC_REQ_DYLD = 0x80000000
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB = 0x19, 0x2, 0xb
LC_LOAD_DYLIB, LC_ID_DYLIB, LC_LOAD_WEAK_DYLIB = 0xc, 0xd, 0x18 | LC_REQ_DYLD
LC_REEXPORT_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB = 0x1f | LC_REQ_DYLD, 0x20, 0x23 | LC_REQ_DYLD
LC_RPATH, LC_BUILD_VERSION = 0x1c | LC_REQ_DYLD, 0x32
DYLIB_CMDS = {LC_LOAD_DYLIB: "", LC_LOAD_WEAK_DYLIB: " (weak)", LC_REEXPORT_DYLIB: " (reexport)",
              LC_LAZY_LOAD_DYLIB: " (lazy)", LC_LOAD_UPWARD_DYLIB: " (upward)"}
N_STAB, N_TYPE, N_EXT, N_UNDF, N_SECT = 0xe0, 0x0e, 0x01, 0x0, 0xe
N_FUN, N_STSYM, N_SO, N_OSO = 0x24, 0x26, 0x64, 0x66
S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS, S_SYMBOL_STUBS = 0x6, 0x7, 0x8
INDIRECT_SYMBOL_LOCAL, INDIRECT_SYMBOL_ABS = 0x80000000, 0x40000000


def version(v):
    return f"{v >> 16}.{(v >> 8) & 0xff}" + (f".{v & 0xff}" if v & 0xff else "")


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("utf-8", "replace")


class MachO:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = d = f.read()
        magic, cputype, _, filetype, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", d, 0)
        if magic != 0xfeedfacf or cputype != 0x0100000c:
            raise SystemExit(f"{path}: not an arm64 Mach-O file")
        self.filetype = filetype
        self.sections, self.segments, self.commands, self.dylibs, self.rpaths = [], [], [], [], []
        self.install_name = self.minos = self.sdk = None
        symtab = dysymtab = None
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            self.commands.append((cmd, off, size))
            if cmd == LC_SEGMENT_64:
                segname = d[off + 8:off + 24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects = struct.unpack_from("<QQQQiiI", d, off + 24)
                self.segments.append(dict(name=segname, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff,
                                          filesize=filesize, maxprot=maxprot, initprot=initprot))
                for i in range(nsects):
                    s = off + 72 + i * 80
                    sect, seg = (d[s:s + 16].rstrip(b"\0").decode(), d[s + 16:s + 32].rstrip(b"\0").decode())
                    addr, sz, foff, align, reloff, nreloc, flags, r1, r2 = struct.unpack_from("<QQIIIIIII", d, s + 32)
                    self.sections.append(dict(name=f"{seg},{sect}", addr=addr, size=sz, offset=foff, align=align,
                                              reloff=reloff, nreloc=nreloc, flags=flags, type=flags & 0xff,
                                              reserved1=r1, reserved2=r2))
            elif cmd in DYLIB_CMDS or cmd == LC_ID_DYLIB:
                name = cstr(d, off + struct.unpack_from("<I", d, off + 8)[0])
                if cmd == LC_ID_DYLIB:
                    self.install_name = name
                else:
                    self.dylibs.append(name + DYLIB_CMDS[cmd])
            elif cmd == LC_RPATH:
                self.rpaths.append(cstr(d, off + struct.unpack_from("<I", d, off + 8)[0]))
            elif cmd == LC_BUILD_VERSION:
                _, minos, sdk = struct.unpack_from("<III", d, off + 8)
                self.minos, self.sdk = version(minos), version(sdk)
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<IIII", d, off + 8)
            elif cmd == LC_DYSYMTAB:
                dysymtab = struct.unpack_from("<18I", d, off + 8)
            off += size
        if not symtab:
            raise SystemExit(f"{path}: no symbol table")

        self.dysymtab = dysymtab
        symoff, nsyms, stroff, _ = symtab
        self.syms = []
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from("<IBBHQ", d, symoff + i * 16)
            self.syms.append((cstr(d, stroff + strx), ntype, nsect, ndesc, value))
        self.indirect = []
        if dysymtab:
            indirectoff, nindirect = dysymtab[12], dysymtab[13]
            self.indirect = list(struct.unpack_from(f"<{nindirect}I", d, indirectoff))

        # what this file defines and exports, and what it imports from which library
        self.exports = sorted(n[1:] for n, t, _, _, _ in self.syms
                              if not t & N_STAB and t & N_EXT and t & N_TYPE == N_SECT)
        self.imports = {}
        for n, t, _, desc, _ in self.syms:
            if not t & N_STAB and t & N_EXT and t & N_TYPE == N_UNDF:
                ordinal = (desc >> 8) & 0xff
                lib = (self.dylibs[ordinal - 1].split(" (")[0] if 0 < ordinal <= len(self.dylibs)
                       else {0: "self", 0xfe: "executable", 0xff: "flat"}.get(ordinal, f"ordinal {ordinal}"))
                self.imports[n[1:]] = lib

    def section(self, name):
        return next((s for s in self.sections if s["name"] == name), None)

    def bytes_of(self, addr, size):
        for s in self.sections:
            if s["addr"] <= addr and addr + size <= s["addr"] + s["size"] and s["offset"]:
                o = s["offset"] + addr - s["addr"]
                return self.data[o:o + size]
        raise SystemExit(f"{self.path}: no bytes at {addr:#x}")

    def functions(self):
        """{(object, name): (address, size)} from the debug map"""
        funcs, obj, pending = {}, None, None
        for n, t, _, _, value in self.syms:
            if t == N_OSO:
                obj = os.path.splitext(os.path.basename(n))[0]
            elif t == N_SO and not n:
                obj = None
            elif t == N_FUN:
                if n:
                    pending = (n, value)
                elif pending:
                    funcs[(obj, pending[0])] = (pending[1], value)
                    pending = None
        return funcs

    @functools.cached_property
    def const_bounds(self):
        """where __TEXT,__const's pieces start: its symbols, and every address the code computes
        into it (a straight pass over each function, ADRP and the ADD or load after it)"""
        s = self.section("__TEXT,__const")
        if not s:
            return []
        lo, hi = s["addr"], s["addr"] + s["size"]
        bounds = {hi} | {v for n, t, _, _, v in self.syms
                         if not t & N_STAB and t & N_TYPE == N_SECT and lo <= v < hi}
        for addr, size in self.functions().values():
            code, page = self.bytes_of(addr, size), {}
            for i in range(0, size - size % 4, 4):
                w = struct.unpack_from("<I", code, i)[0]
                rd, rn, t = w & 31, (w >> 5) & 31, None
                if w & 0x9f000000 == 0x90000000:
                    page[rd] = ((addr + i) & ~0xfff) + (sext(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21) << 12)
                    continue
                if w & 0xffc00000 == 0x91000000 and rn in page:
                    t = page[rn] + ((w >> 10) & 0xfff)
                elif w & 0x3b000000 == 0x39000000 and rn in page:
                    t = page[rn] + (((w >> 10) & 0xfff) << (4 if w & 0x04800000 == 0x04800000 else w >> 30))
                if t is not None and lo <= t < hi:
                    bounds.add(t)
                page.pop(rd, None)
        return sorted(bounds)

    def const_end(self, addr):
        """where the piece of __TEXT,__const that starts at addr ends, by what the code computes"""
        bounds = self.const_bounds
        return bounds[bisect.bisect_right(bounds, addr)]

    @functools.cached_property
    def const_symbols(self):
        s = self.section("__TEXT,__const")
        if not s:
            return []
        lo, hi = s["addr"], s["addr"] + s["size"]
        return sorted({hi} | {v for n, t, _, _, v in self.syms
                              if not t & N_STAB and t & N_TYPE == N_SECT and lo <= v < hi})

    def const_extent_end(self, addr):
        """the farthest the constant at addr can reach: the next symbol, or the section's end. An
        address the code computes may be inside an array, so it doesn't end one."""
        bounds = self.const_symbols
        return bounds[bisect.bisect_right(bounds, addr)]

    def jump_table(self, table, width, signed, base, lo, hi, count=None):
        """the targets in [lo, hi) of a jump table at <table>: <count> entries of <width> bytes,
        each a count of instructions after <base>. Without a count, as far as the table can reach:
        entries past its real end only add cases that aren't, so more ways in, never fewer."""
        s = self.section("__TEXT,__const")
        if not s or not s["addr"] <= table < s["addr"] + s["size"]:
            return None
        n = min((self.const_extent_end(table) - table) // width, 4096)
        if count is not None:
            if count > n:
                return None
            n = count
        fmt = {1: "b", 2: "h", 4: "i"}[width]
        fmt = fmt if signed else fmt.upper()
        raw = self.bytes_of(table, n * width)
        targets = (base + 4 * e for e in struct.unpack_from(f"<{n}{fmt}", raw))
        return sorted({t for t in targets if lo <= t < hi})

    def statics(self):
        """address -> object file, for the static functions and data the debug map lists"""
        where, obj = {}, None
        for n, t, _, _, value in self.syms:
            if t == N_OSO:
                obj = os.path.splitext(os.path.basename(n))[0]
            elif t in (N_FUN, N_STSYM) and n and obj:
                where[value] = obj
        return where

    def resolver(self):
        """address -> what it points to, independent of where the linker put it. A file-local
        symbol is named with its object file, since two files may each have one of that name."""
        where = self.statics()
        defined = sorted((v, n if t & N_EXT or v not in where else f"{where[v]}:{n}")
                         for n, t, sect, _, v in self.syms
                         if not t & N_STAB and t & N_TYPE == N_SECT and n and not n.startswith(("ltmp", "l_", "L")))
        addrs = [v for v, _ in defined]
        pointers = []
        for s in self.sections:
            if s["type"] in (S_SYMBOL_STUBS, S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS):
                width = s["reserved2"] if s["type"] == S_SYMBOL_STUBS else 8
                pointers.append((s, width))
        # cached: the selftest changes only __TEXT,__text, which no name is read from
        @functools.lru_cache(maxsize=None)
        def name(addr):
            for s, width in pointers:
                if s["addr"] <= addr < s["addr"] + s["size"]:
                    i = s["reserved1"] + (addr - s["addr"]) // width
                    sym = self.indirect[i] if i < len(self.indirect) else INDIRECT_SYMBOL_ABS
                    what = "local" if sym & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) else self.syms[sym][0]
                    return f"{s['name']}:{what}+{(addr - s['addr']) % width}"
            for s in self.sections:
                if s["addr"] <= addr < s["addr"] + s["size"]:
                    o = s["offset"] + addr - s["addr"]
                    if s["name"] == "__TEXT,__cstring":
                        return f"str:{cstr(self.data, o)!r}"
                    if s["name"] == "__TEXT,__ustring":
                        end = o
                        while self.data[end:end + 2] not in (b"\0\0", b""):
                            end += 2
                        return f"ustr:{self.data[o:end].decode('utf-16-le', 'replace')!r}"
                    if s["name"] == "__DATA_CONST,__cfstring":
                        # a CFString is 32 bytes: isa, flags, the characters, the length. The
                        # characters' pointer is a rebase, whose low 36 bits are the address.
                        entry = addr - (addr - s["addr"]) % 32
                        chars = struct.unpack_from("<Q", self.data, s["offset"] + entry - s["addr"] + 16)[0]
                        return f"cfstr:{name(chars & ((1 << 36) - 1))}+{addr - entry}"
                    i = bisect.bisect_right(addrs, addr) - 1
                    if s["name"] == "__TEXT,__const" and (i < 0 or addrs[i] != addr):
                        # no symbol starts here: a jump table or another constant the linker
                        # dropped the label of. The symbol before it may be another file's,
                        # whose data can change size, so it stands for its bytes, up to the next
                        # symbol: all the constant can reach, and perhaps more. A jump table's
                        # entries are relative to its own function.
                        end = self.const_extent_end(addr)
                        return f"const:{end - addr}:{hashlib.sha256(self.data[o:o + end - addr]).hexdigest()[:24]}"
                    if i >= 0 and addrs[i] >= s["addr"]:
                        return f"{defined[i][1]}+{addr - addrs[i]}"
                    # nothing to name it by: the offset in its section, which moves when that
                    # section's earlier content changes size, so this errs towards "differs"
                    return f"{s['name']}+{addr - s['addr']}"
            return "?"
        return name


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


CALL_CLOBBERED = set(range(19)) | {30}  # x0-x18 and the link register (AAPCS64)
UNKNOWN = None  # a register's value on a way where no ADRP's page is known to be in it


def written(w):
    """the general registers an instruction other than a branch, ADR(P) or a tracked use writes.
    Too many costs only tracking; too few could pair a use with a page no longer in its register."""
    rd, rn = w & 31, (w >> 5) & 31
    if w & 0x1c000000 == 0x10000000:  # data processing, immediate
        return (rd,)
    if w & 0x0e000000 == 0x0a000000:  # data processing, register
        if w & 0x1fe00000 == 0x1a400000:  # CCMP, CCMN: bits 0-3 are flags
            return ()
        return (rd,)
    if w & 0x1c000000 == 0x14000000:  # branches, system
        return (rd,) if w & 0xfff00000 == 0xd5300000 else ()  # MRS
    if w & 0x0e000000 == 0x0e000000:  # SIMD and floating point
        if w & 0x5f20fc00 == 0x1e200000 or w & 0x9fe08400 == 0x0e000400:  # to/from general, UMOV/SMOV
            return (rd,)
        return ()
    if w & 0x0a000000 == 0x08000000:  # loads and stores
        regs = []
        simd = w & 0x04000000
        pair = w & 0x38000000 == 0x28000000
        exclusive = w & 0x3f000000 == 0x08000000
        if pair or exclusive:
            load = w & 0x00400000
        elif w & 0x3b200c00 == 0x38200000 or w & 0xff200400 == 0xf8200400:
            load = True  # LSE atomics (LDADD, SWP...) and LDRAA/LDRAB write Rt whatever opc says
        else:
            load = w & 0x00c00000 and not (w >> 30 == 3 and (w >> 22) & 3 == 2)  # opc != 00, not PRFM
        if load and not simd:
            regs.append(rd)
            if pair:
                regs.append((w >> 10) & 31)
        if exclusive:
            regs.append((w >> 16) & 31)  # the status register of a store exclusive, CAS's Rs
        if (w & 0x3b200000 == 0x38000000 and w & 0x400) or (pair and w & 0x00800000):
            regs.append(rn)  # written back
        return tuple(regs)
    return (rd,)


def masked(m, resolve, addr, size):
    """the function's instructions, with what an address points to in place of the address

    An ADRP's own token is what its uses point to: the ADD, load or store that completes it, which
    may come after calls (a callee keeps x19-x28). An ADRP whose use this pass doesn't find stands
    for its page. So a changed ADRP always changes a token.

    The registers are carried along branches, along each jump table to the cases it lists, and
    round loops until nothing changes. Where ways meet, a register holds every ADRP page any way
    brings, and UNKNOWN for a way where it holds something else. A use stands for all of its
    ADRPs' targets and counts as a use of each; where UNKNOWN is among them it also keeps its own
    offset as it is, since that may be the whole address. A BR this pass can't read as a jump table
    keeps every use's offset as it is from there on. Each loss of track makes a function compare
    as changed, never as equal."""
    code = m.bytes_of(addr, size)
    end = addr + size
    words = [struct.unpack_from("<I", code, i)[0] for i in range(0, len(code) - len(code) % 4, 4)]

    def merge(ways):
        page = {}
        for r in {r for way in ways for r in way}:
            cands = []
            for way in ways:
                for c in way.get(r, (UNKNOWN,)):
                    if c not in cands:
                        cands.append(c)
            if any(c is not UNKNOWN for c in cands):
                page[r] = tuple(cands)
        return page

    def scan(back, optimistic):
        """one pass. <back> is what the last pass saw reach the targets of backward branches.
        Optimistic: code no way reaches yet (the head of a loop, before its backward branch is
        known) carries nothing to where it branches; otherwise it carries a state of no registers."""
        out, uses, ahead, behind = [], {}, {}, {}  # instruction index -> the states branches carry there
        page = {}  # register -> (candidate, ...): (page, index of its ADRP in out) or UNKNOWN
        exact = {}  # register -> the address an ADRP and its ADD put in it, for jump tables
        opaque = unreached = False

        def drop(*regs):
            for r in regs:
                page.pop(r, None)
                exact.pop(r, None)

        def branch(t):
            here = addr + len(out) * 4
            if addr <= t < end and t != here and not (optimistic and unreached):
                (ahead if t > here else behind).setdefault((t - addr) // 4, []).append(dict(page))

        def last_write(reg, before, stop):
            """the index of the last instruction in [stop, before) that writes reg, or None"""
            for j in range(before - 1, stop - 1, -1):
                if reg in written(words[j]):
                    return j
            return None

        def jump_table(i):
            """the cases of a BR at words[i], or None. Traced back from the BR's own register,
            with nothing else writing the registers in between: add xD, xB, xE, lsl #2 of an adr
            xB and an entry loaded by ldrb/ldrh/ldr(sw) xE, [xT, xI] from the table in xT, whose
            length the cmp xI, #n and b.hi or b.hs before it bound."""
            stop = max(i - 12, 0)
            d = (words[i] >> 5) & 31
            j = last_write(d, i, stop)
            w = words[j] if j is not None else 0
            if w & 0xffe0fc00 != 0x8b000800:  # ADD Xd, Xn, Xm, LSL #2
                return None
            b, e = (w >> 5) & 31, (w >> 16) & 31
            ja, jl = last_write(b, j, stop), last_write(e, j, stop)
            if ja is None or jl is None or words[ja] & 0x9f000000 != 0x10000000 or words[jl] & 0x3f200c00 != 0x38200800:
                return None
            adr, load = words[ja], words[jl]
            base = addr + ja * 4 + sext(((adr >> 5) & 0x7ffff) << 2 | (adr >> 29) & 3, 21)
            size_bits, opc, t, x = load >> 30, (load >> 22) & 3, (load >> 5) & 31, (load >> 16) & 31
            if opc == 0 or size_bits == 3 or (size_bits == 2 and opc == 3) or last_write(t, i, jl + 1) is not None:
                return None
            table = exact.get(t)
            if table is None:
                return None
            count = None
            for k in range(jl - 1, stop - 1, -1):  # the bound: cmp xI, #n, then b.hi or b.hs
                c = words[k]
                if c & 0x7fc0001f == 0x7100001f and (c >> 5) & 31 == x:
                    cond = next((words[n] & 0xf for n in range(k + 1, jl) if words[n] & 0xff000010 == 0x54000000), None)
                    if cond in (8, 2):
                        count = ((c >> 10) & 0xfff) + (cond == 8)
                    break
                if x in written(c):
                    break
            return m.jump_table(table, 1 << size_bits, opc >= 2, base, addr, end, count)

        for i, w in enumerate(words):
            pc = addr + i * 4
            rd, rn = w & 31, (w >> 5) & 31
            fall = page is not None and not (optimistic and unreached)
            ways = ([page] if fall else []) + ahead.pop(i, []) + back.get(i, [])
            if not fall:
                unreached = not ways
            page = merge(ways) if len(ways) != 1 else dict(ways[0])
            if len(ways) != 1:
                exact = {}

            def target(t, keep):
                return (keep, resolve(t)) if not addr <= t < end else w

            def use(reg, t, keep):
                cands = page[reg]
                known = [c for c in cands if c is not UNKNOWN]
                targets = tuple(resolve(p + t) for p, _ in known)
                for (_, at), name in zip(known, targets):
                    uses.setdefault(at, []).append(name)
                out.append((w if opaque or UNKNOWN in cands else keep, targets))
                return known[0][0] + t if len(cands) == 1 else None

            if w & 0x7c000000 == 0x14000000:  # B, BL
                t = pc + sext(w & 0x3ffffff, 26) * 4
                out.append(target(t, w & 0xfc000000))
                if w & 0x80000000:
                    drop(*CALL_CLOBBERED)
                else:
                    branch(t)
                    page = None
            elif w & 0xff000010 == 0x54000000 or w & 0x7e000000 == 0x34000000:  # B.cond, CBZ, CBNZ
                out.append(w)
                branch(pc + sext((w >> 5) & 0x7ffff, 19) * 4)
            elif w & 0x7e000000 == 0x36000000:  # TBZ, TBNZ
                out.append(w)
                branch(pc + sext((w >> 5) & 0x3fff, 14) * 4)
            elif w & 0x9f000000 == 0x90000000:  # ADRP
                drop(rd)
                page[rd] = (((pc & ~0xfff) + (sext(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21) << 12), len(out)),)
                uses.setdefault(len(out), [])
                out.append(None)
            elif w & 0x9f000000 == 0x10000000:  # ADR
                out.append(target(pc + sext(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21), w & 0x9f00001f))
                drop(rd)
            elif w & 0x3b000000 == 0x18000000:  # LDR (literal), PRFM (literal)
                out.append(target(pc + sext((w >> 5) & 0x7ffff, 19) * 4, w & 0xff00001f))
                if not w & 0x04000000:
                    drop(rd)
            elif w & 0xffe0ffe0 == 0xaa0003e0 and (w >> 16) & 31 in page:  # MOV Xd, Xm: a copy of a page
                out.append(w)
                copy = page[(w >> 16) & 31]
                drop(rd)
                page[rd] = copy
            elif w & 0xffc00000 == 0x91000000 and rn in page:  # ADD Xd, Xn, #imm (unshifted) after ADRP
                a = use(rn, (w >> 10) & 0xfff, w & 0xffc003ff)
                drop(rd)
                if a is not None:
                    exact[rd] = a
            elif w & 0x3b000000 == 0x39000000 and rn in page:  # load or store, unsigned offset, after ADRP
                scale = 4 if w & 0x04800000 == 0x04800000 else w >> 30  # 128-bit SIMD, or the size
                use(rn, ((w >> 10) & 0xfff) << scale, w & 0xffc003ff)
                if not w & 0x04000000 and w & 0x00c00000:  # a general register load (opc != 00)
                    drop(rd)
            else:
                out.append(w)
                drop(*written(w))
                if w & 0xfffffc1f == 0xd63f0000:  # BLR
                    drop(*CALL_CLOBBERED)
                elif w & 0xfffffc1f == 0xd61f0000:  # BR
                    targets = jump_table(i)
                    if targets is None:
                        opaque = True  # where it goes is unknown: every use keeps its offset
                    for t in targets or ():
                        branch(t)
                    # a case may start right after the BR, which branch() leaves to the fall-through
                    page = page if targets and pc + 4 in targets else None
                elif w & 0xfffffc1f == 0xd65f0000:  # RET
                    page = None
        return out, uses, behind

    # again until what reaches the targets of backward branches stops changing: first leaving out
    # code no way reaches yet, then with it as a state of no registers
    back = {}
    for optimistic in (True, False):
        for _ in range(16):
            out, uses, behind = scan(back, optimistic)
            if behind == back:
                break
            back = behind
    for at, targets in uses.items():
        w = words[at]
        if targets:
            out[at] = ("adrp", w & 31, tuple(targets))
        else:
            out[at] = ("adrp", w & 31, "page", resolve((addr + at * 4 & ~0xfff) + (sext(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21) << 12)))
    return out


S_ZEROFILL_TYPES = (0x1, 0xc, 0x12)  # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL
LC_UUID, LC_CODE_SIGNATURE = 0x1b, 0x1d
LINKEDIT_BLOBS = {0x80000034: "chained fixups", 0x80000033: "exports trie", 0x26: "function starts",
                  0x29: "data in code"}


def debug_map_free(m):
    """the symbol table without what only says where and when it was built: an N_SO entry's
    directory (Mach9's ccache made the engine's relative, build/wine/../../src) and an N_OSO
    entry's object modification time. Everything else stays, the debug map's order included."""
    out = []
    for n, t, sect, desc, value in m.syms:
        if t == N_SO and n.endswith("/"):
            n = "<source directory>"
        if t == N_OSO:
            value = 0
        out.append((n, t, sect, desc, value))
    return out


def command_bytes(m, cmd, off, size):
    """a load command's bytes, with the fields that follow the string table's length or hash the
    whole file zeroed"""
    raw = bytearray(m.data[off:off + size])
    if cmd in (LC_UUID, LC_CODE_SIGNATURE):
        raw[8:] = bytes(len(raw) - 8)  # a hash of the whole file, and the signature's place
    elif cmd == LC_SYMTAB:
        raw[20:24] = bytes(4)  # strsize
    elif cmd == LC_SEGMENT_64 and raw[8:24].rstrip(b"\0") == b"__LINKEDIT":
        raw[32:40] = bytes(8)  # vmsize
        raw[48:56] = bytes(8)  # filesize
    return bytes(raw)


def linkedit_blob(m, cmd):
    """the data a link edit command (dataoff, datasize) points to"""
    for c, o, _ in m.commands:
        if c == cmd:
            dataoff, size = struct.unpack_from("<II", m.data, o + 8)
            return m.data[dataoff:dataoff + size]
    return None


def byte_gate(mine, theirs, r):
    """the linked file against the engine's, section by section and through the link edit data;
    the names of what differs"""
    differ = []
    if [s["name"] for s in mine.segments] != [s["name"] for s in theirs.segments]:
        differ.append("the segments")
    for sm, st in zip(mine.segments, theirs.segments):
        if sm["name"] != "__LINKEDIT" and sm != st:
            differ.append(f"segment {sm['name']}'s layout")
    if [s["name"] for s in mine.sections] != [s["name"] for s in theirs.sections]:
        differ.append("the sections")
    zerofill = []
    for sm, st in zip(mine.sections, theirs.sections):
        layout = lambda s: (s["name"], s["addr"], s["size"], s["align"], s["flags"], s["reserved1"], s["reserved2"])
        if layout(sm) != layout(st):
            differ.append(f"{sm['name']}'s place or size")
        elif sm["type"] in S_ZEROFILL_TYPES:
            zerofill.append(sm["name"])
        elif mine.bytes_of(sm["addr"], sm["size"]) != theirs.bytes_of(st["addr"], st["size"]):
            differ.append(sm["name"])
    cm = [(c, command_bytes(mine, c, o, n)) for c, o, n in mine.commands]
    ct = [(c, command_bytes(theirs, c, o, n)) for c, o, n in theirs.commands]
    if cm != ct:
        differ.append("the load commands")
    for cmd, what in LINKEDIT_BLOBS.items():
        if linkedit_blob(mine, cmd) != linkedit_blob(theirs, cmd):
            differ.append(f"the {what}")
    if mine.indirect != theirs.indirect or mine.dysymtab != theirs.dysymtab:
        differ.append("the indirect symbol table")
    if debug_map_free(mine) != debug_map_free(theirs):
        differ.append("the symbol table")
    r.check(not differ,
            f"every section of every loaded segment, the load commands and the link edit data are "
            f"the engine's byte for byte. Zero-filled, so compared by place and size: {', '.join(zerofill)}. "
            f"Left out: LC_UUID and the code signature (hashes of the whole file), the string table's "
            f"length and __LINKEDIT's size, the symbol table's N_SO source directories and N_OSO times",
            f"not byte for byte the engine's: {', '.join(differ)}")
    return not differ


# objects: the patched build against my build of the engine's commit, per object file

def relocations(m, sec):
    """a section's relocations as (offset, type, pc-relative, length, target): the target by symbol
    name, or by section name for one within the file, or the addend of an ARM64_RELOC_ADDEND"""
    out = []
    for k in range(sec["nreloc"]):
        address, info = struct.unpack_from("<iI", m.data, sec["reloff"] + 8 * k)
        num, pcrel, length, ext, rtype = info & 0xffffff, info >> 24 & 1, info >> 25 & 3, info >> 27 & 1, info >> 28
        if rtype == 10:
            target = f"addend {sext(num, 24)}"
        elif ext:
            target = m.syms[num][0]
        else:
            target = m.sections[num - 1]["name"] if 0 < num <= len(m.sections) else f"section {num}"
        out.append((address, rtype, pcrel, length, target))
    return out


def canonical(m):
    """what of an object file reaches the linked file: every section outside __DWARF, with its
    bytes and relocations, and the symbols that aren't debug entries"""
    sections = []
    for sec in m.sections:
        if sec["name"].startswith("__DWARF,"):
            continue
        data = None if sec["type"] in S_ZEROFILL_TYPES else bytes(m.data[sec["offset"]:sec["offset"] + sec["size"]])
        sections.append((sec["name"], sec["size"], sec["align"], sec["flags"], data, tuple(relocations(m, sec))))
    names = [sec["name"] for sec in m.sections]
    symbols = sorted((n, t, names[sect - 1] if sect else "", desc, value)
                     for n, t, sect, desc, value in m.syms if not t & N_STAB)
    return sections, symbols


def object_functions(m):
    """name -> (bytes, relocations relative to it) for each function of an object's __text"""
    text = m.section("__TEXT,__text")
    if not text:
        return {}
    index = m.sections.index(text) + 1
    starts = sorted({(v, n) for n, t, sect, _, v in m.syms
                     if not t & N_STAB and t & N_TYPE == N_SECT and sect == index and not n.startswith(("ltmp", "l_", "L"))})
    relocs = relocations(m, text)
    out = {}
    for k, (start, name) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else text["size"]
        out[name] = (bytes(m.data[text["offset"] + start:text["offset"] + end]),
                     tuple((a - start,) + rest for a, *rest in relocs if start <= a < end))
    return out


def build_commands(log):
    """from a make log: object -> its compile command, and the objects of ntdll.so's link"""
    lines, cur = [], ""
    for line in open(log, errors="replace"):
        line = line.rstrip("\n")
        if line.endswith("\\"):
            cur += line[:-1] + " "
            continue
        lines.append(cur + line)
        cur = ""
    compiles, link = {}, None
    for line in lines:
        words = line.split()
        if "-c" in words and "-o" in words:
            target = words[words.index("-o") + 1]
            if target.startswith("dlls/ntdll/unix/") and target.endswith(".o"):
                compiles[os.path.basename(target)] = line.strip()
        elif "-o" in words and words[words.index("-o") + 1] == "dlls/ntdll/ntdll.so":
            link = sorted(os.path.basename(w) for w in words if w.endswith(".o"))
    return compiles, link


def linked_objects(m):
    """the debug map's objects: basename -> modification time"""
    return {os.path.basename(n): value for n, t, _, _, value in m.syms if t == N_OSO}


def compare_objects(r, base_file, base_dir, head_file, head_dir, base_log, head_log, since, touched):
    """the patched build's objects against my base build's: proof that both sets are what their
    files linked, then every untouched object equal through its relocations, and the functions of
    the touched ones listed"""
    base_objs = {f: os.path.join(base_dir, f) for f in os.listdir(base_dir) if f.endswith(".o")}
    head_objs = {f: os.path.join(head_dir, f) for f in os.listdir(head_dir) if f.endswith(".o")}
    base_cc, base_link = build_commands(base_log)
    head_cc, head_link = build_commands(head_log)
    for label, objs, file, link, cc in (("my base build", base_objs, base_file, base_link, base_cc),
                                       ("this build", head_objs, head_file, head_link, head_cc)):
        linked = linked_objects(MachO(file))
        stale = [f for f, p in objs.items() if int(os.stat(p).st_mtime) != linked.get(f)]
        r.check(link is not None and set(link) == set(objs) == set(linked) and not stale,
                f"{label}: the {len(objs)} objects compared are the ones its link line names and its debug "
                f"map lists, with the times the link saw",
                f"{label}: the objects compared ({len(objs)}), the link line's ({len(link or [])}) and the "
                f"debug map's ({len(linked)}) aren't the same files{'; changed since the link: ' + listing(stale) if stale else ''}")
        missing = [f for f in objs if f not in cc]
        r.check(not missing, f"{label}: the make log has the compile command of every object",
                f"{label}: no compile command in the make log for {listing(missing)}")
    old = [f for f, p in head_objs.items() if os.stat(p).st_mtime < since]
    r.check(not old, "this build compiled every object after the checkout",
            f"objects older than the checkout, so not rebuilt: {listing(old)}")

    touched_objs = {os.path.splitext(os.path.basename(p))[0] + ".o" for p in touched
                    if p.startswith("dlls/ntdll/unix/") and p.endswith(".c")}
    touched_objs.add("version.o")  # git describe's text, generated for each commit
    bad, changed, new, removed = [], [], [], []
    for f in sorted(set(base_objs) | set(head_objs)):
        if f not in head_objs:
            (removed if f in touched_objs else bad).append(f"{f} (gone)")
            continue
        if f not in base_objs:
            (new if f in touched_objs else bad).append(f"{f} (new)")
            continue
        if f not in touched_objs:
            if base_cc.get(f) != head_cc.get(f):
                bad.append(f"{f}: compiled with another command")
            elif canonical(MachO(base_objs[f])) != canonical(MachO(head_objs[f])):
                bad.append(f"{f}: code, data, relocations or symbols")
            continue
        fb, fh = object_functions(MachO(base_objs[f])), object_functions(MachO(head_objs[f]))
        label = f[:-2] + ".c"
        for name in sorted(set(fb) | set(fh)):
            short = f"{label}: {name[1:] if name.startswith('_') else name}"
            if name not in fb:
                new.append(short + " (new)")
            elif name not in fh:
                removed.append(short + " (gone)")
            elif fb[name] != fh[name]:
                changed.append(short)
    r.check(not bad, "every object of an untouched source file is my base build's: its compile command, "
                     "and its code, data, relocations and symbols",
            f"{len(bad)} objects of untouched source files differ from my base build's:")
    for b in bad:
        print(f"  - `{b}`")
    return changed, new, removed


class Report:
    def __init__(self):
        self.failed = 0

    def ok(self, text):
        print(f"- ok: {text}")

    def fail(self, text):
        print(f"- **FAIL**: {text}")
        self.failed += 1

    def check(self, cond, good, bad):
        self.ok(good) if cond else self.fail(bad)


def listing(names, limit=40):
    names = sorted(names)
    shown = ", ".join(f"`{n}`" for n in names[:limit])
    return shown + (f" and {len(names) - limit} more" if len(names) > limit else "")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class Fixture:
    """hand-made code for masked(): the words of one function at 0, and its jump tables"""

    def __init__(self, words, tables=None):
        self.code, self.tables = struct.pack(f"<{len(words)}I", *words), tables or {}

    def bytes_of(self, addr, size):
        return self.code[addr:addr + size]

    def jump_table(self, table, width, signed, base, lo, hi, count=None):
        return self.tables.get(table)


def fixtures():
    """The changes a review found the masking hid, each as two versions of a function laid out so
    that a wrong pairing of a use with an ADRP makes them look alike. Each pair has to differ."""
    def adrp(rd, pages):
        return 0x90000000 | (pages & 3) << 29 | (pages >> 2 & 0x7ffff) << 5 | rd

    def add(rd, rn, imm):
        return 0x91000000 | imm << 10 | rn << 5 | rd

    def adr(rd, off):
        return 0x10000000 | (off & 3) << 29 | (off >> 2 & 0x7ffff) << 5 | rd

    def ldrb_reg(rt, rn, rm):
        return 0x38606800 | rm << 16 | rn << 5 | rt

    def add_lsl(rd, rn, rm, sh):
        return 0x8b000000 | rm << 16 | sh << 10 | rn << 5 | rd

    def br(rn):
        return 0xd61f0000 | rn << 5

    ret = 0xd65f03c0
    failed = []

    def check(name, a, b, names_a, names_b, tables=None):
        size = len(a) * 4
        if masked(Fixture(a, tables), lambda t: names_a.get(t, f"@{t:#x}"), 0, size) == \
           masked(Fixture(b, tables), lambda t: names_b.get(t, f"@{t:#x}"), 0, size):
            failed.append(name)

    # an LSE atomic writes its Rt: x1's page is gone, and the ADD's offset is the change
    lse = lambda imm: [adrp(1, 1), 0xf8200041, add(3, 1, imm), ret]  # LDADD x0, x1, [x2]
    check("LDADD over an ADRP's register", lse(16), lse(32), {0x1010: "X"}, {0x1020: "X"})

    # nested jump tables: the outer case at 15 gets x24 from the outer BR, not the inner one
    def nested(imm):
        return [adrp(24, 1),                                   # 0: x24 = page 0x1000
                adrp(9, 2), add(9, 9, 0), adr(10, 16),          # 1-3: table 0x2000, base 0x1c
                ldrb_reg(11, 9, 8), add_lsl(10, 10, 11, 2), br(10),  # 4-6
                adrp(24, 3),                                   # 7, outer case 0: x24 = page 0x3000
                adrp(9, 4), add(9, 9, 0), adr(10, 16),          # 8-10: table 0x4000, base 0x38
                ldrb_reg(11, 9, 8), add_lsl(10, 10, 11, 2), br(10),  # 11-13
                ret,                                           # 14, the inner case
                add(0, 24, imm), ret]                          # 15-16, outer case 1
    tables = {0x2000: [7 * 4, 15 * 4], 0x4000: [14 * 4]}
    check("an outer case after an inner jump table", nested(0x20), nested(0x40),
          {0x1020: "Y", 0x3020: "Z"}, {0x1040: "W", 0x3040: "Z"}, tables)

    # a BR on a register that no table's chain fills: an ADR and an indexed load nearby don't
    # make it a jump table, and the case at 10, also reached at 8, keeps its offset
    def stray(imm):
        return [adrp(24, 1), adrp(9, 2), add(9, 9, 0), adr(10, 16), ldrb_reg(11, 9, 8),
                0xf9400014, br(20),                            # 5-6: ldr x20, [x0]; br x20
                adrp(24, 3), 0x14000002, ret,                  # 7-9: x24 = page 0x3000; b 10
                add(0, 24, imm), ret]                          # 10-11
    check("a BR that isn't a jump table's", stray(0x10), stray(0x20),
          {0x3010: "Q"}, {0x3020: "Q"}, {0x2000: [7 * 4]})

    # two arrays without symbols that start alike, each also used at +4: a use of either stands
    # for all of it, not for the 4 bytes before the other use
    const = MachO.__new__(MachO)
    const.data = bytes([1, 2, 3, 4] + [0x11] * 28 + [1, 2, 3, 4] + [0x22] * 28)
    const.sections = [dict(name="__TEXT,__const", addr=0x4000, size=64, offset=0, type=0, reserved1=0, reserved2=0)]
    const.syms, const.indirect = [], []
    const.__dict__["const_bounds"] = [0x4000, 0x4004, 0x4020, 0x4024, 0x4040]
    name = const.resolver()
    if name(0x4000) == name(0x4020):
        failed.append("arrays that start alike, cut by uses inside them")
    return failed


def fake_object(code, relocs, data, dwarf=b"", syms=(("_f", 0x0f, 1, 0, 0),)):
    """a minimal object file for canonical(): __text with relocations, __data, and __debug_info"""
    m = MachO.__new__(MachO)
    reloff = len(code) + len(data) + len(dwarf)
    m.data = code + data + dwarf + b"".join(struct.pack("<iI", a, info) for a, info in relocs)
    m.sections = [dict(name="__TEXT,__text", offset=0, size=len(code), align=2, flags=0x80000400, type=0,
                       reloff=reloff, nreloc=len(relocs), reserved1=0, reserved2=0),
                  dict(name="__DATA,__data", offset=len(code), size=len(data), align=3, flags=0, type=0,
                       reloff=0, nreloc=0, reserved1=0, reserved2=0),
                  dict(name="__DWARF,__debug_info", offset=len(code) + len(data), size=len(dwarf), align=0,
                       flags=0x02000000, type=0, reloff=0, nreloc=0, reserved1=0, reserved2=0)]
    m.syms = list(syms) + [("_a", 0x01, 0, 0, 0), ("_b", 0x01, 0, 0, 0)]
    return m


def selftest(engine_dir):
    """The gates against changes they must see, and the masking fixtures."""
    failed = []

    # the byte gate: one byte of initialised data, then only the debug map's directories and times
    theirs = MachO(os.path.join(engine_dir, "ntdll.so"))
    mine = MachO(os.path.join(engine_dir, "ntdll.so"))
    sec = mine.section("__DATA_CONST,__const")
    data = bytearray(mine.data)
    data[sec["offset"] + 8] ^= 1
    mine.data = bytes(data)
    quiet = Report()
    with open(os.devnull, "w") as null, contextlib.redirect_stdout(null):
        if byte_gate(mine, theirs, quiet):
            failed.append("byte gate: a byte of __DATA_CONST,__const")
        mine.data = theirs.data
        mine.syms = [(("/elsewhere/" if t == N_SO and n.endswith("/") else n), t, sect, desc,
                      value + 1 if t == N_OSO else value) for n, t, sect, desc, value in theirs.syms]
        if not byte_gate(mine, theirs, quiet):
            failed.append("byte gate: the debug map's source directories and object times only")
        mine.syms = [(n + "x" if t == N_OSO else n, t, sect, desc, value) for n, t, sect, desc, value in theirs.syms]
        if byte_gate(mine, theirs, quiet):
            failed.append("byte gate: an object's path in the debug map")

    # the object compare: a relocation's target, an addend, a byte of data; not a debug section
    code = struct.pack("<2I", 0x90000008, 0x91000108)  # adrp x8, _a@PAGE; add x8, x8, _a@PAGEOFF
    page, off = 3 << 28 | 1 << 27 | 2 << 25 | 1 << 24, 4 << 28 | 1 << 27 | 2 << 25  # ARM64_RELOC_PAGE21, PAGEOFF12
    base = canonical(fake_object(code, [(0, page | 1), (4, off | 1)], b"\x01\x02"))  # symbol 1 is _a
    for what, obj in (("a relocation's target", fake_object(code, [(0, page | 2), (4, off | 2)], b"\x01\x02")),
                      ("an addend", fake_object(code, [(0, 10 << 28 | 8), (0, page | 1), (4, off | 1)], b"\x01\x02")),
                      ("a byte of data", fake_object(code, [(0, page | 1), (4, off | 1)], b"\x01\x03"))):
        if canonical(obj) == base:
            failed.append(f"object compare: {what}")
    if canonical(fake_object(code, [(0, page | 1), (4, off | 1)], b"\x01\x02", b"\x07")) != base:
        failed.append("object compare: the debug information only")

    for item in fixtures():
        failed.append(f"masking fixture: {item}")
    for item in failed:
        print(f"- **FAIL**: {item}")
    print(f"selftest: {len(failed)} failed" if failed else
          "selftest: the byte gate and the object compare see every change they must, and the masking fixtures hold")
    return 1 if failed else 0


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "selftest":
        return selftest(args[1])
    if not ((len(args) == 3 and args[0] == "baseline") or (len(args) >= 9 and args[0] == "patched")):
        print("usage: ntdll-compare.py baseline <mine> <engine-unix-dir>\n"
              "       ntdll-compare.py patched <mine> <engine-unix-dir> <base ntdll.so> <base objects> "
              "<objects> <base make log> <make log> <checkout time> [<touched source>...]\n"
              "       ntdll-compare.py selftest <engine-unix-dir>", file=sys.stderr)
        return 2
    mode, mine_path, engine_dir = args[:3]
    if mode == "patched":
        base_file, base_objs, head_objs, base_log, head_log, since = args[3:9]
        since, touched = float(since), args[9:]
    mine, theirs = MachO(mine_path), MachO(os.path.join(engine_dir, "ntdll.so"))
    r = Report()

    print(f"### ntdll.so compare ({mode})\n")
    print(f"- mine: {os.path.getsize(mine_path)} bytes, sha256 `{sha256(mine_path)}`")
    print(f"- the engine's: {os.path.getsize(theirs.path)} bytes, sha256 `{sha256(theirs.path)}`")

    r.check(mine.filetype == theirs.filetype == 6, "a dylib, as the engine's", "not a dylib like the engine's")
    r.check(mine.install_name == theirs.install_name, f"install name `{mine.install_name}`",
            f"install name `{mine.install_name}`, the engine's is `{theirs.install_name}`")
    r.check(mine.rpaths == theirs.rpaths, f"rpaths {mine.rpaths}",
            f"rpaths {mine.rpaths}, the engine's are {theirs.rpaths}")
    r.check(mine.dylibs == theirs.dylibs, f"loads the engine's {len(theirs.dylibs)} libraries: {listing(theirs.dylibs)}",
            f"loads {mine.dylibs}, the engine's loads {theirs.dylibs}")
    r.check(mine.minos is not None and mine.minos == theirs.minos, f"minos {mine.minos} (SDK {mine.sdk}, the engine's {theirs.sdk})",
            f"minos {mine.minos}, the engine's is {theirs.minos}")
    r.check("pipe2" not in mine.imports, "doesn't import pipe2", "imports pipe2, which macOS 26.4 lacks")

    mine_exp, theirs_exp = set(mine.exports), set(theirs.exports)
    missing, extra = theirs_exp - mine_exp, mine_exp - theirs_exp
    if mode == "baseline":
        r.check(not missing and not extra, f"exports the engine's {len(theirs_exp)} symbols, no more",
                f"exports differ: missing {listing(missing)}; extra {listing(extra)}")
    else:
        r.check(not missing, f"exports all {len(theirs_exp)} of the engine's symbols, {len(extra)} more"
                + (f": {listing(extra)}" if extra else ""), f"lacks exports of the engine's: {listing(missing)}")
    absent = [n for n in COMPATDB if n not in mine_exp]
    r.check(not absent, "exports the seven names compatdb.so looks up", f"lacks compatdb's {listing(absent)}")
    for dep in DEPENDENTS:
        d = MachO(os.path.join(engine_dir, dep))
        wanted = {n for n, lib in d.imports.items() if lib == "@rpath/ntdll.so"}
        absent = wanted - mine_exp
        r.check(wanted and not absent, f"exports all {len(wanted)} symbols the engine's {dep} imports from it",
                f"{dep}: lacks {listing(absent)}" if absent else f"{dep} imports nothing from @rpath/ntdll.so")

    if mine.imports == theirs.imports:
        r.ok(f"imports the engine's {len(theirs.imports)} symbols from the same libraries")
    else:
        diff = {f"{n} ({mine.imports.get(n)} / {theirs.imports.get(n)})"
                for n in set(mine.imports) | set(theirs.imports) if mine.imports.get(n) != theirs.imports.get(n)}
        text = f"imports differ (mine / the engine's): {listing(diff)}"
        r.fail(text) if mode == "baseline" else r.ok(text)

    if mode == "baseline":
        if not byte_gate(mine, theirs, r):
            diagnose(mine, theirs)
    else:
        changed, new, removed = compare_objects(r, base_file, base_objs, mine_path, head_objs, base_log,
                                                head_log, since, touched)
        print(f"\n#### Changed by this branch ({', '.join(sorted(touched)) or 'no source file of ntdll.so'})\n")
        for title, items in (("changed", changed), ("new", new), ("removed", removed)):
            print(f"- {title}: {len(items)}")
            for item in items:
                print(f"  - `{item}`")

    print("\n" + ("all checks passed" if not r.failed else f"{r.failed} checks failed"))
    return 1 if r.failed else 0


def diagnose(mine, theirs):
    """where the baseline isn't the engine's: each function compared with the addresses masked,
    to name what differs. Only a listing: the byte gate has already failed."""
    tm, tt = mine.section("__TEXT,__text"), theirs.section("__TEXT,__text")
    if tm["addr"] == tt["addr"] and mine.bytes_of(tm["addr"], tm["size"]) == theirs.bytes_of(tt["addr"], tt["size"]):
        print("- __TEXT,__text is the engine's byte for byte")
        return
    fm, ft = mine.functions(), theirs.functions()
    rm, rt = mine.resolver(), theirs.resolver()
    differ = [f"{obj}.c: {name[1:] if name.startswith('_') else name}"
              + ("" if (obj, name) in fm and (obj, name) in ft else " (only in one)")
              for obj, name in sorted(set(fm) | set(ft), key=lambda k: (k[0] or "", k[1]))
              if (obj, name) not in fm or (obj, name) not in ft
              or masked(mine, rm, *fm[(obj, name)]) != masked(theirs, rt, *ft[(obj, name)])]
    print(f"- __TEXT,__text differs. With the addresses masked, {len(differ)} of {len(ft)} functions differ:")
    for d in differ:
        print(f"  - `{d}`")


if __name__ == "__main__":
    sys.exit(main())
