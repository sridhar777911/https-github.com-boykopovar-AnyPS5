from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import elf_loads
from test_guest_module_directories import needed_libraries
from test_guest_symbol_names import module_symbols
from test_windows_import_modules import executable


def guest(identity=None, module_name=None, missing_import=False, module_info_tag=0x6100000d):
    image = bytearray(0x3000)
    image[:16] = b'\x7fELF\x02\x01\x01' + bytes(9)
    struct.pack_into('<HHIQQQIHHHHHH', image, 16,
                     3, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', image, 64, 1, 7, 0, 0, 0, len(image), len(image), 0x1000)
    struct.pack_into('<IIQQQQQQ', image, 176, 0x6474e551, 6, 0, 0, 0, 0, 0, 16)
    strings = b'\0shared#A#B\0' + (b'absent' if missing_import else b'Record') + b'\0'
    dependency = len(strings)
    strings += b'libc.prx\0'
    tags = [(5, 0x2600), (10, 0), (6, 0x2400), (11, 24), (4, 0x2500),
            (7, 0x2700), (8, 72), (9, 24), (1, dependency),
            (12, 0x1010), (25, 0x2800), (27, 8),
            (26, 0x2808), (28, 8), (13, 0x1070)]
    module_names = module_name if isinstance(module_name, tuple) else (module_name,)
    for index, (name, tag) in enumerate([(identity, 14)] + [(name, module_info_tag) for name in module_names]):
        if name:
            tags.append((tag, len(strings) | (((0x101 << 32) | (index << 48)) if tag in (0x6100000d, 0x61000043) else 0)))
            strings += name.encode() + b'\0'
    tags[1] = (10, len(strings))
    tags.append((0, 0))
    struct.pack_into('<IIQQQQQQ', image, 120, 2, 6, 0x2000, 0x2000, 0x2000,
                     len(tags) * 16, len(tags) * 16, 8)
    for index, tag in enumerate(tags):
        struct.pack_into('<qQ', image, 0x2000 + index * 16, *tag)
    image[0x2600:0x2600 + len(strings)] = strings
    struct.pack_into('<IBBHQQ', image, 0x2418, 1, 0x12, 0, 1, 0x1000, 6)
    struct.pack_into('<IBBHQQ', image, 0x2430, 12, 0x12, 0, 0, 0, 0)
    struct.pack_into('<II', image, 0x2500, 1, 3)
    struct.pack_into('<QQq', image, 0x2700, 0x2820, (2 << 32) | 6, 0)
    struct.pack_into('<QQq', image, 0x2718, 0x2800, 8, 0x1030)
    struct.pack_into('<QQq', image, 0x2730, 0x2808, 8, 0x1050)
    image[0x1000:0x1006] = b'\xb8\x2a\0\0\0\xc3'
    for address, event in ((0x1010, 'I'), (0x1030, 'A'), (0x1050, 'a'), (0x1070, 'i')):
        code = b'\xbf' + struct.pack('<I', ord(event)) + b'\x48\x83\xec\x08\xff\x15'
        code += struct.pack('<i', 0x2820 - (address + 15)) + b'\x48\x83\xc4\x08\xc3'
        image[address:address + len(code)] = code
    return image


def lifecycle_tags(image):
    phoff, = struct.unpack_from('<Q', image, 32)
    phsize, count = struct.unpack_from('<HH', image, 54)
    headers = [struct.unpack_from('<IIQQQQQQ', image, phoff + index * phsize) for index in range(count)]
    dynamic = next(header for header in headers if header[0] == 2)
    return {tag for position in range(dynamic[2], dynamic[2] + dynamic[5], 16)
            for tag, value in [struct.unpack_from('<qQ', image, position)] if tag in (12, 13, 25, 26, 27, 28)}


def consumer(owner):
    image = guest()
    struct.pack_into('<IBBHQQ', image, 0x2418, 1, 0x12, 0, 0, 0, 0)
    struct.pack_into('<QQq', image, 0x2700, 0x2820, (1 << 32) | 6, 0)
    code = b'\x48\x83\xec\x08\xff\x15' + struct.pack('<i', 0x2820 - 0x101a)
    code += b'\x48\x83\xc4\x08\x83\xf8\x2a\x74\x02\x0f\x0b\xc3'
    image[0x1010:0x1010 + len(code)] = code
    tags = []
    for position in range(0x2000, 0x2200, 16):
        tag, value = struct.unpack_from('<qQ', image, position)
        if tag == 0:
            break
        if tag in (25, 26, 27, 28, 13):
            continue
        if tag == 10 and owner != 'libc.prx':
            value += len(owner) + 1
        tags.append((tag, value))
    size, = struct.unpack_from('<Q', image, 0x2018)
    if owner != 'libc.prx':
        image[0x2600 + size:0x2600 + size + len(owner) + 1] = owner.encode() + b'\0'
        tags.append((1, size))
    tags.append((0, 0))
    for index, tag in enumerate(tags):
        struct.pack_into('<qQ', image, 0x2000 + index * 16, *tag)
    struct.pack_into('<QQ', image, 120 + 32, len(tags) * 16, len(tags) * 16)
    return image


def main():
    if sys.argv[1] == '--load':
        import ctypes
        import _ctypes
        library = ctypes.CDLL(sys.argv[2])
        second = ctypes.CDLL(sys.argv[2])
        assert getattr(library, 'shared#guest')() == 42
        assert getattr(second, 'shared#guest')() == 42
        before = Path(os.environ['ANYPS5_LIFECYCLE_EVENTS']).read_text()
        _ctypes.dlclose(library._handle)
        assert Path(os.environ['ANYPS5_LIFECYCLE_EVENTS']).read_text() == before
        _ctypes.dlclose(second._handle)
        return
    relinker = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix='anyps5-guest-lifecycle-') as directory:
        work = Path(directory)
        for windows in (False, True):
            for name, identity, module_name, replacement, module_info_tag in (
                    ('libc.prx', None, None, 'libc.prx', 0x6100000d),
                    ('renamed.prx', 'libc.prx', None, 'libc.prx', 0x6100000d),
                    ('renamed.prx', 'libc', None, 'libc.prx', 0x6100000d),
                    ('renamed.prx', None, 'libc', 'libc.prx', 0x6100000d),
                    ('renamed.prx', 'ordinary.prx', 'libc', 'libc.prx', 0x61000043),
                    ('renamed.prx', None, ('custom', 'libc'), 'libc.prx', 0x6100000d),
                    ('ordinary.prx', None, ('custom', 'other'), None, 0x6100000d),
                    ('renamed.prx', None, 'libSceFont_native', 'libSceFont.native.prx', 0x6100000d),
                    ('libkernel.prx', None, None, 'libkernel.prx', 0x6100000d),
                    ('ordinary.prx', None, None, None, 0x6100000d),
                    ('unimplemented.prx', 'libNotReplaced.prx', None, None, 0x6100000d)):
                suppressed = replacement is not None
                case = work / f'{windows}-{name}-{identity}-{module_name}'
                modules = case / 'sce_module'
                modules.mkdir(parents=True)
                original = guest(identity, module_name, module_info_tag=module_info_tag)
                (modules / name).write_bytes(original)
                (modules / 'consumer.prx').write_bytes(consumer(name))
                source = case / 'input.elf'
                source.write_bytes(executable(name))
                output = case / ('output.exe' if windows else 'output.elf')
                result = subprocess.run([str(relinker), *(['--windows'] if windows else []),
                                         '--skip-syscall-check', str(source), str(output)],
                                        capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (result.stdout, result.stderr)
                converted = case / 'app0' / 'sce_module' / (name + '.guest.prx')
                image = converted.read_bytes()
                if not windows:
                    assert 'shared#guest' in module_symbols(image)
                    assert 'shared#guest' in module_symbols((converted.parent / 'consumer.prx.guest.prx').read_bytes())
                    assert lifecycle_tags(image) == (set() if suppressed else {12, 13, 25, 26, 27, 28}), name
                    for header in elf_loads(image):
                        if header[3] <= 0x1000 < header[3] + header[5]:
                            start = header[2] + 0x1000 - header[3]
                            assert image[start:start + 0x100] == original[0x1000:0x1100]
                    needed = needed_libraries(image)
                    assert 'libc.prx' in needed
                    if replacement:
                        assert replacement in needed
                native = (os.name == 'nt') if windows else (sys.platform == 'linux' and os.uname().machine == 'x86_64')
                if native and not fixture:
                    raise AssertionError('Native lifecycle test requires the replacement fixture')
                if native and fixture:
                    libraries = case / 'libs'
                    libraries.mkdir()
                    shutil.copyfile(fixture, libraries / 'libc.prx')
                    if replacement and replacement != 'libc.prx':
                        shutil.copyfile(fixture, libraries / replacement)
                    events = case / 'events.txt'
                    output.chmod(0o755)
                    command = [str(output)] if windows else [sys.executable, __file__, '--load',
                        str(converted.parent / 'consumer.prx.guest.prx')]
                    run = subprocess.run(command, env={**os.environ, 'ANYPS5_LIFECYCLE_EVENTS': str(events)},
                                         capture_output=True, text=True, timeout=30)
                    assert run.returncode == (42 if windows else 0), (name, run.returncode, run.stdout, run.stderr)
                    actual = events.read_text()
                    assert actual == ('HHhh' if replacement and replacement != 'libc.prx' else 'Hh' if suppressed else 'HIAaih'), (name, actual)
            case = work / f'{windows}-missing-import'
            modules = case / 'sce_module'
            modules.mkdir(parents=True)
            (modules / 'libc.prx').write_bytes(guest(missing_import=True))
            source = case / 'input.elf'
            source.write_bytes(executable('libc.prx'))
            output = case / ('output.exe' if windows else 'output.elf')
            result = subprocess.run([str(relinker), *(['--windows'] if windows else []),
                                     '--skip-syscall-check', str(source), str(output)],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (result.stdout, result.stderr)
            converted = case / 'app0' / 'sce_module' / 'libc.prx.guest.prx'
            if not windows:
                assert 'absent' in module_symbols(converted.read_bytes())
                assert lifecycle_tags(converted.read_bytes()) == set()
            if native:
                libraries = case / 'libs'
                libraries.mkdir()
                shutil.copyfile(fixture, libraries / 'libc.prx')
                output.chmod(0o755)
                command = [str(output)] if windows else [sys.executable, __file__, '--load', str(converted)]
                run = subprocess.run(command, env={**os.environ, 'ANYPS5_LIFECYCLE_EVENTS': str(case / 'events.txt')},
                                     capture_output=True, text=True, timeout=30)
                assert run.returncode != 0 and 'absent' in run.stderr, (run.returncode, run.stderr)
        print('Guest lifecycle ownership conversion tests passed')
        if os.name != 'nt' and (sys.platform != 'linux' or os.uname().machine != 'x86_64'):
            print('Native lifecycle execution skipped: Linux x86-64 or Windows required')


if __name__ == '__main__':
    main()
