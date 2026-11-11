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
N_FUN, N_SO, N_OSO = 0x24, 0x64, 0x66
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

    def resolver(self):
        """address -> what it points to, independent of where the linker put it"""
        defined = sorted((v, n) for n, t, sect, _, v in self.syms
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


def masked(m, resolve, addr, size):
    """the function's instructions, with what an address points to in place of the address

    An ADRP's own token is what its uses point to: the ADD, load or store that completes it, which
    may come after calls (a callee keeps x19-x28). An ADRP whose page this pass loses track of
    stands for its page instead. So a changed ADRP always changes a token, and the cost of lost
    tracking is a function reported changed, never one reported equal. The pass carries the
    registers along branches. Where ways meet, a register may hold the page of more than one
    ADRP, and a use then stands for all of their targets and counts as a use of each."""
    code = m.bytes_of(addr, size)
    end = addr + size

    def scan(back):
        out, uses, ahead, behind = [], {}, {}, {}  # instruction index -> the states branches carry there
        page = {}  # register -> ((page, index of its ADRP in out), ...); None where nothing falls through

        def drop(*regs):
            for r in regs:
                page.pop(r, None)

        def branch(t):
            if addr <= t < end and t != addr + len(out) * 4:
                (ahead if t > addr + len(out) * 4 else behind).setdefault((t - addr) // 4, []).append(dict(page))

        for i in range(0, len(code) - len(code) % 4, 4):
            pc, w = addr + i, struct.unpack_from("<I", code, i)[0]
            rd, rn = w & 31, (w >> 5) & 31
            ways = ([page] if page is not None else []) + ahead.pop(i // 4, []) + back.get(i // 4, [])
            page = {}
            for way in ways:  # every ADRP that may reach here, the fall-through's first
                for r, v in way.items():
                    page[r] = page.get(r, ()) + tuple(c for c in v if c not in page.get(r, ()))

            def target(t, keep):
                return (keep, resolve(t)) if not addr <= t < end else w

            def use(reg, t, keep):
                targets = tuple(resolve(p + t) for p, _ in page[reg])
                for (_, at), name in zip(page[reg], targets):
                    uses.setdefault(at, []).append(name)
                out.append((keep, targets))
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
            elif w & 0xffc00000 == 0x91000000 and rn in page:  # ADD Xd, Xn, #imm (unshifted) after ADRP
                use(rn, (w >> 10) & 0xfff, w & 0xffc003ff)
                drop(rd)
            elif w & 0x3b000000 == 0x39000000 and rn in page:  # load or store, unsigned offset, after ADRP
                scale = 4 if w & 0x04800000 == 0x04800000 else w >> 30  # 128-bit SIMD, or the size
                use(rn, ((w >> 10) & 0xfff) << scale, w & 0xffc003ff)
                if not w & 0x04000000 and w & 0x00c00000:  # a general register load (opc != 00)
                    drop(rd)
            else:
                out.append(w)
                if w & 0x0a000000 == 0x08000000:  # loads and stores
                    if not w & 0x04000000:
                        drop(rd)  # Rt, which only a load writes; dropping a stored one loses tracking only
                        if w & 0x38000000 == 0x28000000:
                            drop((w >> 10) & 31)  # a pair's Rt2
                    if w & 0x3f000000 == 0x08000000:
                        drop((w >> 16) & 31)  # an exclusive's status register Rs
                    if (w & 0x3b200000 == 0x38000000 and w & 0x400) or (w & 0x3a000000 == 0x28000000 and w & 0x00800000):
                        drop(rn)  # written back
                else:
                    drop(rd)
                if w & 0xfffffc1f == 0xd63f0000:  # BLR
                    drop(*CALL_CLOBBERED)
                elif w & 0xfffffc1f in (0xd65f0000, 0xd61f0000):  # RET, BR: no fall-through
                    page = None
        return out, uses, behind

    # twice: the second pass also has what the first saw reach a backward branch's target
    out, uses, _ = scan(scan({})[2])
    for at, targets in uses.items():
        w = struct.unpack_from("<I", code, at * 4)[0]
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


def selftest(engine_dir):
    """Change one address-bearing instruction at a time in the engine's file, in memory, and
    require that the function's masked code changes: every ADRP's page, the offset of every ADD,
    load or store that completes one, every branch out of its function, every ADR and literal
    load."""
    m = MachO(os.path.join(engine_dir, "ntdll.so"))
    m.data = bytearray(m.data)
    resolve, text = m.resolver(), m.section("__TEXT,__text")
    tried, missed = {}, []
    for key, (addr, size) in sorted(m.functions().items(), key=lambda kv: kv[1]):
        want = masked(m, resolve, addr, size)
        for i in range(0, size - size % 4, 4):
            o = text["offset"] + addr - text["addr"] + i
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
            elif isinstance(want[i // 4], tuple):  # an ADD, load or store that completes an ADRP
                kind, new = "imm12", w ^ (1 << 10)
            else:
                continue
            struct.pack_into("<I", m.data, o, new)
            if masked(m, resolve, addr, size) == want:
                missed.append(f"{key[0]}.c: {key[1]} +{i:#x} ({kind})")
            struct.pack_into("<I", m.data, o, w)
            tried[kind] = tried.get(kind, 0) + 1
    print("selftest: changed " + ", ".join(f"{n} {k}" for k, n in sorted(tried.items())) + " instructions one at a time")
    for item in missed:
        print(f"- **FAIL**: hidden by the masking: {item}")
    print("selftest: every change showed" if not missed else f"selftest: {len(missed)} changes hidden")
    return 1 if missed else 0


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
