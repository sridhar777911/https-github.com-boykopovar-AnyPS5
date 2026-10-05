#!/usr/bin/env python3
import argparse
import re
import sys


def strip_nid_token(raw):
    s = raw.lstrip("\ufeff").strip()
    if not s:
        return ""
    if s.startswith("#"):
        return ""
    if "#" in s:
        s = s.split("#", 1)[0].strip()
    if not s:
        return ""
    return s.split()[0]


def parse_nids(path):
    nids = []
    seen = set()
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            nid = strip_nid_token(line)
            if not nid:
                continue
            if nid in seen:
                continue
            seen.add(nid)
            nids.append(nid)
    return nids


def parse_db(path):
    mapping = {}
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            s = line.lstrip("\ufeff").strip()
            if not s or s.startswith("#"):
                continue
            parts = s.split()
            if len(parts) < 2:
                continue
            nid = parts[0].split("#", 1)[0].strip().lstrip("\ufeff")
            name = parts[1].split("#", 1)[0].strip().lstrip("\ufeff")
            if not nid or not name:
                continue
            if nid not in mapping:
                mapping[nid] = name
    return mapping


def sanitize_identifier(name):
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if not s:
        return "_"
    if s[0].isdigit():
        s = "_" + s
    return s


def module_prefix(module):
    base = module
    if base.startswith("lib") and len(base) > 3 and base[3].isupper():
        base = base[3:]
        base = base[0].lower() + base[1:] if base else module
    return sanitize_identifier(base)


def camel_part(real_name):
    stripped = re.sub(r"^_+", "", real_name)
    if not stripped:
        return "Anon"
    chunks = [c for c in re.split(r"[^0-9A-Za-z]+", stripped) if c]
    if not chunks:
        return "Anon"
    out = "".join(c[0].upper() + c[1:] for c in chunks)
    out = sanitize_identifier(out)
    if out[0].isdigit():
        out = "_" + out
    return out


def generate(module, nids, db):
    prefix = module_prefix(module)
    used = set()
    unknown_counter = 0
    chunks = []
    for nid in nids:
        real = db.get(nid)
        if real is not None:
            func = prefix + camel_part(real)
            base = func
            dup = 2
            while func in used:
                func = "%s_%d" % (base, dup)
                dup += 1
        else:
            while True:
                func = "%sUnknown%02d" % (prefix, unknown_counter)
                unknown_counter += 1
                if func not in used:
                    break
        used.add(func)
        chunks.append("")
        chunks.append('APS5_EXPORT("%s", %s);' % (nid, func))
        chunks.append("int APS5_VABI %s(void) {" % func)
        chunks.append('    NotImplemented_nid_no_patch("%s");' % nid)
        chunks.append("    return 0;")
        chunks.append("}")
    header = []
    header.append("#include <cstdint>")
    header.append("#include <cstddef>")
    header.append('#include "SceTypes.hpp"')
    header.append('#include "prx/libc/include/General.hpp"')
    header.append("")
    header.append('extern "C" {')
    footer = ["", "}"]
    return "\n".join(header + chunks + footer) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate AnyPS5 Export.cpp skeletons")
    ap.add_argument("--module", required=True, help="target module name, e.g. libSceAmpr")
    ap.add_argument("--nids", required=True, help="path to missing-NIDs file")
    ap.add_argument("--db", required=False, default=None, help="path to NID->name CSV")
    ap.add_argument("--out", required=False, default=None, help="output file (default stdout)")
    args = ap.parse_args(argv)
    nids = parse_nids(args.nids)
    db = {}
    if args.db:
        db = parse_db(args.db)
    text = generate(args.module, nids, db)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
