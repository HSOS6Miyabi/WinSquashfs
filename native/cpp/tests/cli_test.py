# SPDX-License-Identifier: GPL-3.0-only
import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
modules = pathlib.Path(sys.argv[2]).resolve()


def run(*args, success=True):
    result = subprocess.run([str(exe), '--decoder-directory', str(modules), *map(str, args)],
                            capture_output=True, timeout=15)
    assert (result.returncode == 0) == success, (result.args, result.returncode, result.stderr)
    return result.stdout.decode('utf-8').strip()


with tempfile.TemporaryDirectory(prefix='archivefs-cli-') as work:
    directory = pathlib.Path(work)
    for name in ['SquashFS', 'DwarFS', 'EROFS']:
        image = directory / f'{name}.img'
        data = bytes((i * 17 + 10) % 251 for i in range(8193))
        header = bytearray(1028)
        if name == 'SquashFS':
            header[:4] = b'hsqs'
        elif name == 'DwarFS':
            header[:6] = b'DWARFS'
        else:
            header[1024:] = bytes.fromhex('e2e1f5e0')
        image.write_bytes(header + data)
        assert run('info', image).startswith(name + ' ')
        assert 'File\t8193\tdata.bin' in run('list', image)
        assert 'File\t20\tsmall.txt' in run('list', image, 'folder')
        assert run('search', image, 'SMALL.TXT') == 'File\t20\t/folder/small.txt'
        assert run('search', image, 'DATA', 'folder') == ''
        assert run('search', image, 'data') == 'File\t8193\t/data.bin'
        assert run('search', image, 'not-found') == ''
        run('search', image, '', success=False)
        run('search', image, 'txt', 'missing', success=False)
        assert '文件\t8193\tdata.bin' in run('--lang', 'zh-CN', 'list', image)
        assert run('read', image, 'data.bin', 1000, 97) == data[1000:1097].hex().upper()
        assert run('read', image, 'data.bin', 8190, 20) == data[8190:].hex().upper()
        assert run('verify', image) == 'Verified 2 files, 8213 bytes.'
        output = directory / (name + '-extracted')
        run('unpack', image, output)
        assert (output / 'data.bin').read_bytes() == data
        assert (output / 'folder' / 'small.txt').read_bytes() == data[:20]
        run('unpack', image, output, success=False)
        run('read', image, 'data.bin', -1, 1, success=False)
        run('read', image, 'data.bin', 0, 16777217, success=False)
    run('info', directory / 'missing.img', success=False)
print('PASS CLI format dispatch, Unicode output, random reads, verification, extraction and argument validation')
