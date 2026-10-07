import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from functools import cache
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = Path(os.environ.get("HW_ORACLE_CACHE") or Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "anyps5-hw-oracle")
ROWS_PER_DISPATCH = 1024


def rocm_root():
    for root in (os.environ.get("ROCM_PATH"), "/opt/rocm"):
        if root and (Path(root) / "include" / "hsa" / "hsa.h").exists():
            return Path(root)
    return None


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    root = rocm_root()
    if root and (root / "llvm" / "bin" / name).exists():
        return str(root / "llvm" / "bin" / name)
    sys.exit(f"{name} not found: install LLVM with the AMDGPU target or set ROCM_PATH")


def oracle():
    binary = CACHE / "oracle"
    source = HERE / "oracle.c"
    if binary.exists() and binary.stat().st_mtime >= source.stat().st_mtime:
        return binary
    CACHE.mkdir(parents=True, exist_ok=True)
    command = [os.environ.get("CC", "cc"), "-O1", str(source), "-o", str(binary)]
    root = rocm_root()
    if root:
        lib = root / "lib"
        command += [f"-I{root / 'include'}", f"-L{lib}", f"-Wl,-rpath,{lib}"]
    subprocess.run(command + ["-lhsa-runtime64"], check=True)
    return binary


@cache
def target():
    return os.environ.get("HW_ORACLE_TARGET") or subprocess.run([oracle(), "--target"], capture_output=True, text=True, check=True).stdout.strip()


def assemble(body, work, wave64, ieee, denorm32, denorm16):
    text = (HERE / "template.s").read_text()
    for key, value in (("@TARGET@", target()), ("@WAVE32@", "0" if wave64 else "1"), ("@WAVESIZE@", "64" if wave64 else "32"),
                       ("@DENORM@", str(denorm32)), ("@DENORM16@", str(denorm16)), ("@IEEE@", str(ieee)), ("@BODY@", body)):
        text = text.replace(key, value)
    (work / "k.s").write_text(text)
    subprocess.run([tool("clang"), "-x", "assembler", "-target", "amdgcn-amd-amdhsa", f"-mcpu={target()}", "-c", str(work / "k.s"), "-o", str(work / "k.o")], check=True)
    subprocess.run([tool("ld.lld"), "-shared", str(work / "k.o"), "-o", str(work / "k.co")], check=True)
    return work / "k.co"


def run(body, rows, extra=b"", wave64=False, ieee=1, denorm32=3, denorm16=3, coarse=False):
    rows = [tuple(r) for r in rows]
    wave = 64 if wave64 else 32
    padded = rows + [(0, 0, 0, 0)] * (-len(rows) % wave)
    env = dict(os.environ)
    env.pop("COARSE", None)
    if coarse:
        env["COARSE"] = "1"
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        code = assemble(body, work, wave64, ieee, denorm32, denorm16)
        for start in range(0, len(padded), ROWS_PER_DISPATCH):
            chunk = padded[start:start + ROWS_PER_DISPATCH]
            (work / "in.bin").write_bytes(b"".join(struct.pack("<4I", *r) for r in chunk) + extra)
            subprocess.run([oracle(), str(code), str(len(chunk)), str(work / "in.bin"), str(work / "out.bin"), "16"], check=True, env=env)
            data = (work / "out.bin").read_bytes()
            out += [struct.unpack_from("<16I", data, lane * 64) for lane in range(len(chunk))]
    return out[:len(rows)]


def main():
    parser = argparse.ArgumentParser(description="Run an RDNA kernel body on the local AMD GPU, one input row per lane.")
    parser.add_argument("body", type=Path, help="assembly inserted into template.s")
    parser.add_argument("rows", type=Path, help="one row per line: 4 u32 values (decimal or 0x hex)")
    parser.add_argument("--wave64", action="store_true")
    parser.add_argument("--ieee", type=int, choices=(0, 1), default=1)
    parser.add_argument("--denorm32", type=int, choices=range(4), default=3)
    parser.add_argument("--denorm16", type=int, choices=range(4), default=3)
    parser.add_argument("--coarse", action="store_true", help="input and output in coarse-grained GPU memory")
    parser.add_argument("--extra", type=Path, help="bytes appended after the rows")
    parser.add_argument("--outs", type=int, choices=range(1, 17), default=16, metavar="N", help="print v10..v(10+N-1)")
    args = parser.parse_args()
    rows = [tuple(int(v, 0) for v in line.split()) for line in args.rows.read_text().splitlines() if line.strip()]
    if any(len(r) != 4 for r in rows):
        sys.exit("every row needs 4 values")
    extra = args.extra.read_bytes() if args.extra else b""
    for result in run(args.body.read_text(), rows, extra, args.wave64, args.ieee, args.denorm32, args.denorm16, args.coarse):
        print(" ".join(f"{v:08x}" for v in result[:args.outs]))


if __name__ == "__main__":
    main()
