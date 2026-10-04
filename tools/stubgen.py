#!/usr/bin/env python3
"""stubgen.py - generate AnyPS5 Export.cpp skeletons from missing NIDs.

Stdlib only. No third-party imports.

CLI:
    stubgen.py --module libSceAmpr --nids missing.txt [--db names.csv] [--out Export.cpp]

NIDs file: one 11-char NID per line; trailing ``#X#Y`` suffixes are stripped.
DB file: ``NID name`` per line (whitespace separated).
"""
import argparse
import re
import sys


def strip_nid_token(raw):
    """Strip trailing # suffixes and whitespace; return NID token or ''."""
    s = raw.lstrip("\ufeff").strip()
    if not s:
        return ""
    if s.startswith("#"):
        return ""
    # cut trailing #X#Y suffixes
    if "#" in s:
        s = s.split("#", 1)[0].strip()
    if not s:
        return ""
    # one token per line: take first whitespace-separated token
    tok = s.split()[0]
    return tok


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
            # strip inline # comments only if they look like suffixes?
            # DB format is "NID name"; names never contain '#', so cut at '#'
            # only for the NID part. Handle carefully:
            parts = s.split()
            if len(parts) < 2:
                continue
            nid_raw = parts[0]
            nid = nid_raw.split("#", 1)[0].strip().lstrip("\ufeff")
            name = parts[1].strip().lstrip("\ufeff")
            # name may itself have trailing # suffix? strip it
            name = name.split("#", 1)[0].strip()
            if not nid or not name:
                continue
            # keep first mapping only
            if nid not in mapping:
                mapping[nid] = name
    return mapping


def sanitize_identifier(name):
    """Sanitize to a valid C identifier."""
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if not s:
        return "_"
    if s[0].isdigit():
        s = "_" + s
    return s


def module_prefix(module):
    """Derive function prefix from module name, matching repo conventions.

    libSceAmpr -> sceAmpr, libc -> libc, libkernel -> libkernel.
    Rule: strip leading 'lib' only if next char is uppercase, then
    lowercase the first letter. Result is sanitized to a valid identifier.
    """
    base = module
    if base.startswith("lib") and len(base) > 3 and base[3].isupper():
        base = base[3:]
        base = base[0].lower() + base[1:] if base else module
    return sanitize_identifier(base)


def camel_part(real_name):
    """Convert real CRT name to CamelCase part: strtok_s -> StrtokS.

    Strips leading underscores, splits on non-alphanumeric runs,
    capitalizes each chunk. Falls back to sanitized identifier.
    """
    stripped = re.sub(r"^_+", "", real_name)
    if not stripped:
        return "Anon"
    chunks = re.split(r"[^0-9A-Za-z]+", stripped)
    chunks = [c for c in chunks if c]
    if not chunks:
        return "Anon"
    out = ""
    for c in chunks:
        out += c[0].upper() + c[1:]
    # ensure it starts with a letter or underscore (prefix guarantees this
    # for the full name, but keep part clean anyway)
    out = sanitize_identifier(out)
    if out[0].isdigit():
        out = "_" + out
    return out


# ---------------------------------------------------------------------------
# Hardcoded CRT signature table.
#
# Each entry: real_name -> (return_type, param_list, body_lines list)
# Body lines are C++ statements (without surrounding braces).
# All bodies implement real standard semantics (no throws, no NotImplemented).
# ---------------------------------------------------------------------------
def crt_table():
    T = {}
    T["strtok_s"] = (
        "char*",
        "char* str, const char* delim, char** context",
        [
            "if (!delim || !context) return nullptr;",
            "char* cursor = str ? str : *context;",
            "if (!cursor) return nullptr;",
            "cursor += std::strspn(cursor, delim);",
            "if (*cursor == '\\0') {",
            "    *context = cursor;",
            "    return nullptr;",
            "}",
            "char* token = cursor;",
            "cursor += std::strcspn(cursor, delim);",
            "if (*cursor != '\\0') {",
            "    *cursor = '\\0';",
            "    *context = cursor + 1;",
            "} else {",
            "    *context = cursor;",
            "}",
            "return token;",
        ],
    )
    T["localeconv"] = (
        "std::lconv*",
        "void",
        [
            "return std::localeconv();",
        ],
    )
    T["wcscpy_s"] = (
        "int",
        "wchar_t* dest, std::size_t size, const wchar_t* src",
        [
            "if (!dest || !src || size == 0) return EINVAL;",
            "std::size_t i = 0;",
            "while (i + 1 < size && src[i] != L'\\0') {",
            "    dest[i] = src[i];",
            "    ++i;",
            "}",
            "dest[i] = L'\\0';",
            "return src[i] == L'\\0' ? 0 : ERANGE;",
        ],
    )
    T["ctime_s"] = (
        "int",
        "char* buffer, std::size_t size, const std::time_t* time",
        [
            'static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};',
            'static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};',
            "if (!buffer || size < 26 || !time) return EINVAL;",
            "const std::tm* parts = std::localtime(time);",
            "if (!parts || parts->tm_wday < 0 || parts->tm_wday > 6 || parts->tm_mon < 0 || parts->tm_mon > 11) return EINVAL;",
            'std::snprintf(buffer, size, "%.3s %.3s %2d %.2d:%.2d:%.2d %4d\\n", days[parts->tm_wday], months[parts->tm_mon],',
            "    parts->tm_mday, parts->tm_hour, parts->tm_min, parts->tm_sec, 1900 + parts->tm_year);",
            "return 0;",
        ],
    )
    T["mbstowcs"] = (
        "std::size_t",
        "wchar_t* dest, const char* src, std::size_t size",
        [
            "return std::mbstowcs(dest, src, size);",
        ],
    )
    T["asctime_s"] = (
        "int",
        "char* buffer, std::size_t size, const std::tm* time",
        [
            'static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};',
            'static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};',
            "if (!buffer || size < 26 || !time) return EINVAL;",
            "if (time->tm_wday < 0 || time->tm_wday > 6 || time->tm_mon < 0 || time->tm_mon > 11) return EINVAL;",
            'std::snprintf(buffer, size, "%.3s %.3s %2d %.2d:%.2d:%.2d %4d\\n", days[time->tm_wday], months[time->tm_mon],',
            "    time->tm_mday, time->tm_hour, time->tm_min, time->tm_sec, 1900 + time->tm_year);",
            "return 0;",
        ],
    )
    T["div"] = (
        "std::div_t",
        "int numer, int denom",
        [
            "return std::div(numer, denom);",
        ],
    )
    T["abs"] = (
        "int",
        "int j",
        [
            "return std::abs(j);",
        ],
    )
    T["strnlen"] = (
        "std::size_t",
        "const char* s, std::size_t maxlen",
        [
            "std::size_t i = 0;",
            "while (i < maxlen && s && s[i] != '\\0') ++i;",
            "return i;",
        ],
    )
    T["vsnprintf"] = (
        "int",
        "char* s, std::size_t n, const char* format, std::va_list arg",
        [
            "return std::vsnprintf(s, n, format, arg);",
        ],
    )
    T["vsscanf"] = (
        "int",
        "const char* s, const char* format, std::va_list arg",
        [
            "return std::vsscanf(s, format, arg);",
        ],
    )
    T["snprintf"] = (
        "int",
        "char* s, std::size_t n, const char* format, ...",
        [
            "std::va_list ap;",
            "va_start(ap, format);",
            "int r = std::vsnprintf(s, n, format, ap);",
            "va_end(ap);",
            "return r;",
        ],
    )
    T["sprintf"] = (
        "int",
        "char* s, const char* format, ...",
        [
            "std::va_list ap;",
            "va_start(ap, format);",
            "int r = std::vsprintf(s, format, ap);",
            "va_end(ap);",
            "return r;",
        ],
    )
    T["vsprintf"] = (
        "int",
        "char* s, const char* format, std::va_list arg",
        [
            "return std::vsprintf(s, format, arg);",
        ],
    )
    T["memcmp"] = (
        "int",
        "const void* s1, const void* s2, std::size_t n",
        [
            "return std::memcmp(s1, s2, n);",
        ],
    )
    T["memcpy"] = (
        "void*",
        "void* dest, const void* src, std::size_t n",
        [
            "return std::memcpy(dest, src, n);",
        ],
    )
    T["memmove"] = (
        "void*",
        "void* dest, const void* src, std::size_t n",
        [
            "return std::memmove(dest, src, n);",
        ],
    )
    T["memset"] = (
        "void*",
        "void* s, int c, std::size_t n",
        [
            "return std::memset(s, c, n);",
        ],
    )
    T["strlen"] = (
        "std::size_t",
        "const char* s",
        [
            "return std::strlen(s);",
        ],
    )
    T["strcmp"] = (
        "int",
        "const char* s1, const char* s2",
        [
            "return std::strcmp(s1, s2);",
        ],
    )
    T["_Mtx_init"] = (
        "int",
        "void** mtx, int type",
        [
            "(void)type;",
            "if (!mtx) return EINVAL;",
            "*mtx = new (std::nothrow) std::mutex();",
            "if (!*mtx) return ENOMEM;",
            "return 0;",
        ],
    )
    T["_Mtx_lock"] = (
        "int",
        "void** mtx",
        [
            "if (!mtx || !*mtx) return EINVAL;",
            "static_cast<std::mutex*>(*mtx)->lock();",
            "return 0;",
        ],
    )
    T["_Mtx_unlock"] = (
        "int",
        "void** mtx",
        [
            "if (!mtx || !*mtx) return EINVAL;",
            "static_cast<std::mutex*>(*mtx)->unlock();",
            "return 0;",
        ],
    )
    T["_Mtx_destroy"] = (
        "void",
        "void** mtx",
        [
            "if (mtx && *mtx) {",
            "    delete static_cast<std::mutex*>(*mtx);",
            "    *mtx = nullptr;",
            "}",
        ],
    )
    T["_Cnd_init"] = (
        "int",
        "void** cond",
        [
            "if (!cond) return EINVAL;",
            "*cond = new (std::nothrow) std::condition_variable();",
            "if (!*cond) return ENOMEM;",
            "return 0;",
        ],
    )
    T["_Cnd_wait"] = (
        "int",
        "void** cond, void** mtx",
        [
            "if (!cond || !*cond || !mtx || !*mtx) return EINVAL;",
            "auto* c = static_cast<std::condition_variable*>(*cond);",
            "auto* m = static_cast<std::mutex*>(*mtx);",
            "std::unique_lock<std::mutex> lk(*m, std::adopt_lock);",
            "c->wait(lk);",
            "lk.release();",
            "return 0;",
        ],
    )
    T["_Cnd_broadcast"] = (
        "int",
        "void** cond",
        [
            "if (!cond || !*cond) return EINVAL;",
            "static_cast<std::condition_variable*>(*cond)->notify_all();",
            "return 0;",
        ],
    )
    T["_Cnd_destroy"] = (
        "void",
        "void** cond",
        [
            "if (cond && *cond) {",
            "    delete static_cast<std::condition_variable*>(*cond);",
            "    *cond = nullptr;",
            "}",
        ],
    )
    T["quick_exit"] = (
        "[[noreturn]] void",
        "int status",
        [
            "std::quick_exit(status);",
            "std::abort();",
            "for (;;) {}",
        ],
    )
    T["exit"] = (
        "[[noreturn]] void",
        "int status",
        [
            "std::exit(status);",
            "std::abort();",
            "for (;;) {}",
        ],
    )
    T["abort"] = (
        "[[noreturn]] void",
        "void",
        [
            "std::abort();",
            "for (;;) {}",
        ],
    )
    T["__udivti3"] = (
        "unsigned __int128",
        "unsigned __int128 dividend, unsigned __int128 divisor",
        [
            "if (divisor == 0) return 0;",
            "unsigned __int128 quotient = 0;",
            "unsigned __int128 remainder = 0;",
            "for (int i = 127; i >= 0; --i) {",
            "    remainder = (remainder << 1) | ((dividend >> i) & 1);",
            "    if (remainder >= divisor) {",
            "        remainder -= divisor;",
            "        quotient |= (unsigned __int128)1 << i;",
            "    }",
            "}",
            "return quotient;",
        ],
    )
    return T


def generate(module, nids, db):
    prefix = module_prefix(module)
    table = crt_table()
    used = set()
    unknown_counter = 0
    chunks = []
    for nid in nids:
        real = db.get(nid)
        entry = table.get(real) if real else None
        if real is not None and entry is not None:
            ret, params, body = entry
            func = prefix + camel_part(real)
            # avoid collisions with previously emitted names
            base = func
            dup = 2
            while func in used:
                func = "%s_%d" % (base, dup)
                dup += 1
            used.add(func)
            chunks.append("")
            chunks.append('APS5_EXPORT("%s", %s);' % (nid, func))
            chunks.append("%s APS5_VABI %s(%s) {" % (ret, func, params))
            for line in body:
                chunks.append("    " + line)
            chunks.append("}")
        else:
            # generic throwing stub; sequential NN avoids collisions because
            # the counter only moves forward and every emitted name is tracked
            # in `used` (typed names included), so an UnknownNN can never
            # equal a typed name or a previous UnknownNN.
            while True:
                func = "%sUnknown%02d" % (prefix, unknown_counter)
                unknown_counter += 1
                if func not in used:
                    break
            used.add(func)
            chunks.append("")
            if real is not None:
                chunks.append("// %s" % sanitize_identifier(real))
            chunks.append('APS5_EXPORT("%s", %s);' % (nid, func))
            chunks.append("int APS5_VABI %s(void) {" % func)
            chunks.append('    NotImplemented_nid_no_patch("%s");' % nid)
            chunks.append("    return 0;")
            chunks.append("}")
    header = []
    header.append("#include <cstdint>")
    header.append("#include <cstddef>")
    header.append("#include <cstdio>")
    header.append("#include <cstdarg>")
    header.append("#include <cstdlib>")
    header.append("#include <cstring>")
    header.append("#include <ctime>")
    header.append("#include <clocale>")
    header.append("#include <cwchar>")
    header.append("#include <cerrno>")
    header.append("#include <mutex>")
    header.append("#include <condition_variable>")
    header.append("#include <new>")
    header.append('#include "SceTypes.hpp"')
    header.append('#include "prx/libc/include/General.hpp"')
    header.append("")
    header.append('extern "C" {')
    footer = ["", "}"]
    return "\n".join(header + chunks + footer) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Generate AnyPS5 Export.cpp skeleton")
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
