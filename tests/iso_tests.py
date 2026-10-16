import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

BLOCK = 2048


def both32(value):
    return struct.pack('<I', value) + struct.pack('>I', value)


def record(name, block, size, flags=0):
    name = name if isinstance(name, bytes) else name.encode('ascii')
    length = 33 + len(name) + (len(name) % 2 == 0)
    data = bytearray(length)
    data[0] = length
    data[2:10] = both32(block)
    data[10:18] = both32(size)
    data[25] = flags
    data[28:32] = b'\x01\x00\x00\x01'
    data[32] = len(name)
    data[33:33 + len(name)] = name
    return data


def fixture():
    image = bytearray(48 * BLOCK)
    pvd = bytearray(BLOCK)
    pvd[:7] = b'\x01CD001\x01'
    pvd[80:88] = both32(48)
    pvd[128:132] = b'\x00\x08\x08\x00'
    root = record(b'\x00', 20, BLOCK, 2)
    pvd[156:156 + len(root)] = root
    image[16 * BLOCK:17 * BLOCK] = pvd
    image[17 * BLOCK:17 * BLOCK + 7] = b'\xffCD001\x01'

    def directory(block, entries):
        data = b''.join(entries)
        image[block * BLOCK:block * BLOCK + len(data)] = data

    directory(20, [record('casper', 21, BLOCK, 2), record('.disk', 22, BLOCK, 2), record('efi', 23, BLOCK, 2)])
    directory(21, [record('vmlinuz', 30, 100), record('initrd', 31, 100)])
    label = b'Kubuntu 24.04.4 LTS "Noble Numbat" - Release amd64\n'
    directory(22, [record('info', 32, len(label))])
    directory(23, [record('boot', 24, BLOCK, 2)])
    directory(24, [record('bootx64.efi', 33, 512), record('grubx64.efi', 34, 512)])
    image[32 * BLOCK:32 * BLOCK + len(label)] = label
    for block in (33, 34):
        efi = bytearray(512)
        efi[:2] = b'MZ'
        struct.pack_into('<I', efi, 60, 64)
        efi[64:68] = b'PE\0\0'
        struct.pack_into('<H', efi, 68, 0x8664)
        struct.pack_into('<H', efi, 84, 240)
        struct.pack_into('<H', efi, 88, 0x20b)
        struct.pack_into('<H', efi, 156, 10)
        image[block * BLOCK:block * BLOCK + len(efi)] = efi
    return image


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='diskboot-iso-') as raw:
        directory = pathlib.Path(raw)
        count = 0

        def inspect(data, success):
            nonlocal count
            path = directory / f'case-{count}.iso'
            path.write_bytes(data)
            result = subprocess.run([str(executable), 'inspect', str(path)], capture_output=True, text=True, timeout=30)
            assert (result.returncode == 0) == success, result.stderr
            count += 1
            if success:
                info = json.loads(result.stdout)
                assert info['sha256'] == hashlib.sha256(data).hexdigest()
                return info

        info = inspect(fixture(), True)
        assert info['candidate'] and info['distribution'] == 'Kubuntu'
        assert 'not been validated' in info['limitation']
        data = fixture()
        data[22 * BLOCK:23 * BLOCK] = bytes(BLOCK)
        trailing_dot = record('INFO.;1', 32, len(b'Kubuntu 24.04.4 LTS "Noble Numbat" - Release amd64\n'))
        data[22 * BLOCK:22 * BLOCK + len(trailing_dot)] = trailing_dot
        assert inspect(data, True)['candidate']
        data = fixture()
        struct.pack_into('<H', data, 33 * BLOCK + 68, 0xaa64)
        inspect(data, False)
        inspect(b'not an iso', False)
        data = fixture()
        data[16 * BLOCK + 84] ^= 1
        inspect(data, False)
        data = fixture()
        data[20 * BLOCK:21 * BLOCK] = bytes(BLOCK)
        malicious = record('../escape', 30, 100)
        data[20 * BLOCK:20 * BLOCK + len(malicious)] = malicious
        inspect(data, False)
        data = fixture()
        data[20 * BLOCK:21 * BLOCK] = bytes(BLOCK)
        cycle = record('cycle', 20, BLOCK, 2)
        data[20 * BLOCK:20 * BLOCK + len(cycle)] = cycle
        inspect(data, False)
        data = fixture()
        data[20 * BLOCK + 2:20 * BLOCK + 10] = both32(1000)
        inspect(data, False)
        data = fixture()
        data[24 * BLOCK + 25] = 0x80
        inspect(data, False)
        data = fixture()
        first = record('large', 35, BLOCK, 0x80)
        second = record('large', 36, BLOCK)
        data[21 * BLOCK:22 * BLOCK] = bytes(BLOCK)
        data[21 * BLOCK:21 * BLOCK + len(first + second)] = first + second
        assert not inspect(data, True)['candidate']
        data = fixture()
        data[24 * BLOCK + 33] = ord('x')
        assert not inspect(data, True)['candidate']
        result = subprocess.run([str(executable), 'prepare', '--iso', str(directory / 'missing.iso'), '--disk', 'a' * 64, '--size', '500GB'], capture_output=True, text=True)
        assert result.returncode != 0 and 'Physical preparation is disabled' in result.stderr
        print(f'{count} ISO cases and physical execution refusal passed')


if __name__ == '__main__':
    main()
