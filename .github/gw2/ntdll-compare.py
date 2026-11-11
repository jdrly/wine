#!/usr/bin/env python3
# gw2 (jdrly/gw2-on-mac#40): compare my ntdll.so with the one in Mach9's engine. It reads both
# files and runs nothing.
#
#   ntdll-compare.py baseline <mine> <engine-unix-dir>
#   ntdll-compare.py patched <mine> <engine-unix-dir> [<touched source>...]
#
# <engine-unix-dir> is the engine's lib/wine/aarch64-unix. Prints a Markdown report and exits 1
# when a check fails.
#
# baseline: my build of the engine's commit. Its exports, imports, libraries, install name,
#   rpaths and minos equal the engine's, and every function's code is the engine's with the
#   addresses masked. The report also says whether __TEXT,__text is the engine's byte for byte.
# patched: my build of a later commit. Its exports are a superset of the engine's and hold the
#   seven names compatdb.so looks up and every symbol the engine's win32u.so, winemac.so and
#   winemetal.so import from ntdll.so; its libraries, install name, rpaths and minos are the
#   engine's; nothing imports pipe2. Every function of an object whose source file isn't in
#   <touched source> (paths as git prints them) is the engine's, and the report lists the changed,
#   new and removed functions of the touched ones.
#
# A function is a name in the debug map (the N_FUN stabs the linker writes from the objects'
# DWARF), keyed by its object file, since two files may each have a static function of one name.
# Its size is the stab's too. Masking: an instruction that encodes an address outside its own
# function (B, BL, ADR, ADRP and the ADD or load or store that completes an ADRP, LDR literal) is
# compared by what it points to: a stub or GOT slot by the imported name, a C string by its text,
# another symbol by name and offset, anything else by its section. Branches inside the function
# stay as they are, since they don't move when the function does.
import bisect
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
        self.sections, self.dylibs, self.rpaths = [], [], []
        self.install_name = self.minos = self.sdk = None
        symtab = dysymtab = None
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            if cmd == LC_SEGMENT_64:
                nsects = struct.unpack_from("<I", d, off + 64)[0]
                for i in range(nsects):
                    s = off + 72 + i * 80
                    sect, seg = (d[s:s + 16].rstrip(b"\0").decode(), d[s + 16:s + 32].rstrip(b"\0").decode())
                    addr, sz, foff = struct.unpack_from("<QQI", d, s + 32)
                    flags, r1, r2 = struct.unpack_from("<III", d, s + 64)
                    self.sections.append(dict(name=f"{seg},{sect}", addr=addr, size=sz, offset=foff,
                                              type=flags & 0xff, reserved1=r1, reserved2=r2))
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


def selftest(engine_dir):
    """Change one address-bearing instruction at a time in the engine's file, in memory, and
    require that the function's masked code changes: every ADRP's page, the offset of every ADD,
    load or store that completes one, every branch out of its function, every ADR and literal
    load, and every ADRP and ADD pair that points into __TEXT,__const pointed at the pieces next
    to its own and at others that start with the same bytes. Then the hand-made fixtures."""
    m = MachO(os.path.join(engine_dir, "ntdll.so"))
    m.data = bytearray(m.data)
    resolve, text, const = m.resolver(), m.section("__TEXT,__text"), m.section("__TEXT,__const")
    bounds = m.const_bounds[:-1]
    by_prefix = {}
    for b in bounds:
        by_prefix.setdefault(bytes(m.bytes_of(b, min(16, m.const_end(b) - b))), []).append(b)
    tried, missed = {}, []

    def mutate(key, addr, size, want, changes, kind, where):
        for o, new in changes:
            struct.pack_into("<I", m.data, o, new)
        if masked(m, resolve, addr, size) == want:
            missed.append(f"{key[0]}.c: {key[1]} +{where:#x} ({kind})")
        tried[kind] = tried.get(kind, 0) + 1

    for key, (addr, size) in sorted(m.functions().items(), key=lambda kv: kv[1]):
        want = masked(m, resolve, addr, size)
        base = text["offset"] + addr - text["addr"]
        for i in range(0, size - size % 4, 4):
            o = base + i
            w = struct.unpack_from("<I", m.data, o)[0]
            if w & 0x9f000000 == 0x90000000:
                kind, new = "adrp", w ^ (1 << 29)  # the page moves by one
            elif w & 0x9f000000 == 0x10000000:
                kind, new = "adr", w ^ (1 << 29)
            elif w & 0x7c000000 == 0x14000000:
                t = addr + i + sext(w & 0x3ffffff, 26) * 4
                if addr <= t < addr + size:
                    continue
                kind, new = "bl" if w & 0x80000000 else "b", w ^ 1
            elif w & 0x3b000000 == 0x18000000:
                kind, new = "literal", w ^ (1 << 5)
            elif isinstance(want[i // 4], tuple) and want[i // 4][0] != "adrp":  # completes an ADRP
                kind, new = "imm12", w ^ (1 << 10)
            else:
                continue
            mutate(key, addr, size, want, [(o, new)], kind, i)
            struct.pack_into("<I", m.data, o, w)

            # an ADRP and the ADD after it, into __TEXT,__const: another piece of it instead
            nxt = struct.unpack_from("<I", m.data, o + 4)[0] if i + 4 < size else 0
            if kind != "adrp" or nxt & 0xffc00000 != 0x91000000 or (nxt >> 5) & 31 != w & 31:
                continue
            pc = addr + i
            t = (pc & ~0xfff) + (sext(((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3, 21) << 12) + ((nxt >> 10) & 0xfff)
            if not const or not const["addr"] <= t < const["addr"] + const["size"]:
                continue
            j = bisect.bisect_left(bounds, t)
            others = {bounds[k] for k in (j - 1, j + 1) if 0 <= k < len(bounds)}
            others |= set(by_prefix.get(bytes(m.bytes_of(t, min(16, m.const_end(t) - t))), [])[:4])
            for alt in sorted(others - {t}):
                if resolve(alt) == resolve(t) and resolve(t).startswith("const:"):
                    continue  # the same bytes, as long: no change the code could see
                pages = ((alt & ~0xfff) - (pc & ~0xfff)) >> 12
                new_adrp = (w & 0x9f00001f) | (pages & 3) << 29 | (pages >> 2 & 0x7ffff) << 5
                new_add = (nxt & ~(0xfff << 10)) | (alt & 0xfff) << 10
                mutate(key, addr, size, want, [(o, new_adrp), (o + 4, new_add)], "const", i)
                struct.pack_into("<I", m.data, o, w)
                struct.pack_into("<I", m.data, o + 4, nxt)
    print("selftest: changed " + ", ".join(f"{n} {k}" for k, n in sorted(tried.items())) + " one at a time")
    for item in missed:
        print(f"- **FAIL**: hidden by the masking: {item}")
    failed = fixtures()
    for item in failed:
        print(f"- **FAIL**: hidden by the masking: fixture: {item}")
    print(f"selftest: {len(missed) + len(failed)} changes hidden" if missed or failed else
          "selftest: every change showed, and every fixture")
    return 1 if missed or failed else 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "selftest":
        return selftest(sys.argv[2])
    if len(sys.argv) < 4 or sys.argv[1] not in ("baseline", "patched"):
        print(__doc__ or "usage: ntdll-compare.py baseline|patched <mine> <engine-unix-dir> [<touched source>...]",
              file=sys.stderr)
        return 2
    mode, mine_path, engine_dir, touched = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
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

    text_mine, text_theirs = mine.section("__TEXT,__text"), theirs.section("__TEXT,__text")
    same_bytes = (text_mine["addr"] == text_theirs["addr"]
                  and mine.bytes_of(text_mine["addr"], text_mine["size"])
                  == theirs.bytes_of(text_theirs["addr"], text_theirs["size"]))
    print(f"- __TEXT,__text: mine {text_mine['size']:#x} bytes at {text_mine['addr']:#x}, the engine's "
          f"{text_theirs['size']:#x} at {text_theirs['addr']:#x}: "
          + ("**byte for byte the engine's**" if same_bytes else "not byte for byte the engine's"))

    fm, ft = mine.functions(), theirs.functions()
    if not fm or not ft:
        r.fail(f"no debug map to read functions from (mine: {len(fm)}, the engine's: {len(ft)})")
        return 1
    rm, rt = mine.resolver(), theirs.resolver()
    touched_objs = {os.path.splitext(os.path.basename(p))[0] for p in touched
                    if p.startswith("dlls/ntdll/unix/") and p.endswith(".c")}
    changed, new, removed, bad, equal = [], [], [], [], 0
    for key in sorted(set(fm) | set(ft), key=lambda k: (k[0] or "", k[1])):
        obj, name = key
        label = f"{obj}.c: {name[1:] if name.startswith('_') else name}"
        if key not in ft:
            (new if obj in touched_objs else bad).append(label + " (new)")
        elif key not in fm:
            (removed if obj in touched_objs else bad).append(label + " (gone)")
        elif masked(mine, rm, *fm[key]) != masked(theirs, rt, *ft[key]):
            (changed if obj in touched_objs else bad).append(label)
        else:
            equal += 1
    print(f"- functions: mine {len(fm)}, the engine's {len(ft)}, {equal} equal with addresses masked")
    if mode == "baseline":
        r.check(not bad and not changed and not new and not removed,
                "0 functions differ from the engine's", f"{len(bad)} functions differ from the engine's:")
    else:
        r.check(not bad, "every function of an untouched source file is the engine's",
                f"{len(bad)} functions of untouched source files differ from the engine's:")
    for b in bad:
        print(f"  - `{b}`")
    if mode == "patched":
        print(f"\n#### Changed by this branch ({', '.join(sorted(touched)) or 'no source file of ntdll.so'})\n")
        for title, items in (("changed", changed), ("new", new), ("removed", removed)):
            print(f"- {title}: {len(items)}")
            for item in items:
                print(f"  - `{item}`")

    print("\n" + ("all checks passed" if not r.failed else f"{r.failed} checks failed"))
    return 1 if r.failed else 0


if __name__ == "__main__":
    sys.exit(main())
