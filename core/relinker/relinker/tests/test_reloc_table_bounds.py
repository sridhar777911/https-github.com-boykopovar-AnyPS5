from pathlib import Path
import struct
import subprocess
import sys
import tempfile


DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_RELA = 7
DT_RELASZ = 8
DT_RELAENT = 9
DT_STRSZ = 10
DT_SYMENT = 11
DT_OS_STRTAB = 0x61000035
DT_OS_STRSZ = 0x61000037
DT_OS_SYMTAB = 0x61000039
DT_OS_SYMENT = 0x6100003B
DT_OS_SYMTABSZ = 0x6100003F
DT_OS_RELA = 0x6100002F
DT_OS_RELASZ = 0x61000031
DT_OS_RELAENT = 0x61000033


def fixture(rela=0x700, relasz=24, os_tags=True):
    image = bytearray(0x1000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0x200, 64, 0, 0, 64, 56, 3, 64, 0, 0)
    image[0x200:0x206] = b"\xff\x25\xfa\x00\x00\x00"
    tags = [
        (DT_NEEDED, 1),
        (DT_OS_STRTAB if os_tags else DT_STRTAB, 0x600),
        (DT_OS_STRSZ if os_tags else DT_STRSZ, 16),
        (DT_OS_SYMTAB if os_tags else DT_SYMTAB, 0x620),
        (DT_OS_SYMENT if os_tags else DT_SYMENT, 24),
        (DT_OS_SYMTABSZ, 48),
        (DT_OS_RELA if os_tags else DT_RELA, rela),
        (DT_OS_RELASZ if os_tags else DT_RELASZ, relasz),
        (DT_OS_RELAENT if os_tags else DT_RELAENT, 24),
        (0, 0),
    ]
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 7, 0, 0, 0, len(image), len(image), 0x1000)
    struct.pack_into("<IIQQQQQQ", image, 120,
                     2, 6, 0x400, 0x400, 0x400, len(tags) * 16, len(tags) * 16, 8)
    struct.pack_into("<IIQQQQQQ", image, 176,
                     0x61000000, 0, 0, 0, 0, len(image), len(image), 1)
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x400 + index * 16, *tag)
    struct.pack_into("<QQq", image, 0x700, 0x300, (1 << 32) | 6, 0)
    struct.pack_into("<I", image, 0x620 + 24, 8)
    image[0x600:0x610] = b"\x00lib.so\x00symbol\x00\x00"[:16]
    return image


def run(relinker, work, name, image, expected_error=None):
    source = work / (name + ".elf")
    output = work / (name + ".out")
    source.write_bytes(image)
    result = subprocess.run([str(relinker), "--skip-sce-module", "--windows", str(source), str(output)],
                            capture_output=True, text=True, timeout=20)
    if expected_error is None:
        assert result.returncode == 0 and output.exists(), (name, result.stdout, result.stderr)
    else:
        assert result.returncode == 2 and expected_error in result.stderr and not output.exists(), (
            name, result.returncode, result.stdout, result.stderr)


def main():
    relinker = Path(sys.argv[1]).resolve()
    error = "Relocation table is out of bounds"
    with tempfile.TemporaryDirectory(prefix="anyps5-reloc-table-") as directory:
        work = Path(directory)
        run(relinker, work, "sysv-valid", fixture(os_tags=False))
        run(relinker, work, "os-valid", fixture())
        run(relinker, work, "empty-table", fixture(relasz=0))
        run(relinker, work, "offset-past-eof-empty-table",
            fixture(rela=0x2000, relasz=0), error)
        run(relinker, work, "offset-max-empty-table",
            fixture(rela=0xFFFFFFFFFFFFFFFF, relasz=0), error)
        run(relinker, work, "range-past-eof",
            fixture(rela=0xFF0, relasz=0x100), error)
        run(relinker, work, "mapped-range-past-eof",
            fixture(rela=0xFF0, relasz=0x100, os_tags=False), error)
        run(relinker, work, "huge-size",
            fixture(rela=0x700, relasz=0xFFFFFFFFFFFFFFFF), error)
    print("Relocation table bounds tests passed")


if __name__ == "__main__":
    main()
