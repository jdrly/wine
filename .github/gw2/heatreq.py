#!/usr/bin/env python3
# gw2 (jdrly/gw2-on-mac#60): writes dlls/ntdll/unix/heatreq.h's list of the server's requests from
# enum request in include/wine/server_protocol.h, or checks that the list is that enum's.
#
#   heatreq.py write <server_protocol.h> <heatreq.h>
#   heatreq.py check <server_protocol.h> <heatreq.h>
#
# write keeps everything in heatreq.h outside the HEAT_REQUESTS macro. check prints a diff and
# exits 1 when write would change the file. It reads files and runs nothing.
import difflib
import re
import sys

WIDTH = 100  # of a list line before its " \"
MACRO = "#define HEAT_REQUESTS(R) \\\n"


def requests(protocol):
    text = open(protocol, encoding="utf-8").read()
    m = re.search(r"^enum request\n\{\n(.*?)^\};", text, re.S | re.M)
    if not m:
        sys.exit(f"{protocol}: no enum request")
    names = re.findall(r"^\s*REQ_(\w+),?\s*$", m.group(1), re.M)
    if not names or names[-1] != "NB_REQUESTS":
        sys.exit(f"{protocol}: enum request doesn't end in REQ_NB_REQUESTS")
    return names[:-1]


def render(old, names):
    start = old.find(MACRO)
    if start < 0:
        sys.exit("heatreq.h: no HEAT_REQUESTS macro")
    body_start = start + len(MACRO)
    # the macro ends at its first line without a trailing backslash
    end = body_start
    while True:
        nl = old.index("\n", end)
        line = old[end:nl]
        end = nl + 1
        if not line.endswith("\\"):
            break
    lines, cur = [], "   "
    for name in names:
        item = f" R({name})"
        if len(cur) + len(item) > WIDTH and cur.strip():
            lines.append(cur)
            cur = "   "
        cur += item
    lines.append(cur)
    return old[:body_start] + " \\\n".join(lines) + "\n" + old[end:]


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("write", "check"):
        sys.exit("usage: heatreq.py write|check <server_protocol.h> <heatreq.h>")
    mode, protocol, header = sys.argv[1:]
    old = open(header, encoding="utf-8").read()
    new = render(old, requests(protocol))
    if mode == "write":
        if new != old:
            open(header, "w", encoding="utf-8").write(new)
        return 0
    if new == old:
        print(f"{header}: the list is enum request's")
        return 0
    sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), new.splitlines(True), header, header + " (from the enum)"))
    print(f"::error::{header} isn't enum request's list: run .github/gw2/heatreq.py write {protocol} {header}")
    return 1


sys.exit(main())
