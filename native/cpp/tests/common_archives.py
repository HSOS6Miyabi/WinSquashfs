# SPDX-License-Identifier: GPL-3.0-only
import bz2
import gzip
import io
import lzma
import pathlib
import random
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

exe = pathlib.Path(sys.argv[1]).resolve()

def run(*args, ok=True):
    result = subprocess.run([str(exe), '--lang', 'en-US', *map(str, args)], capture_output=True, timeout=30)
    assert (result.returncode == 0) == ok, (result.args, result.returncode, result.stderr)
    return result.stdout.decode('utf-8').strip()

def read(image, name, offset, count, expected):
    assert run('read', image, name, offset, count) == expected.hex().upper(), (image, name, offset)

def add_tar(tar, name, data):
    entry = tarfile.TarInfo(name)
    entry.size = len(data)
    tar.addfile(entry, io.BytesIO(data))

def cpio_entry(name, data, inode, links=1, mode=0o100644):
    name = name.encode() + b'\0'
    values = (inode, mode, 0, 0, links, 0, len(data), 0, 0, 0, 0, len(name), 0)
    header = b'070701' + b''.join(f'{n:08x}'.encode() for n in values) + name
    return header + b'\0' * (-len(header) % 4) + data + b'\0' * (-len(data) % 4)

with tempfile.TemporaryDirectory(prefix='archivefs-common-') as tmp:
    root = pathlib.Path(tmp)
    data = random.Random(2026).randbytes(384 * 1024) + b'abcd' * 32768
    name = '目录/文件😀.bin'
    long_name = 'long/' + 'a' * 180 + '.txt'
    tar_bytes = io.BytesIO()
    with tarfile.open(fileobj=tar_bytes, mode='w', format=tarfile.PAX_FORMAT) as tar:
        add_tar(tar, name, data)
        add_tar(tar, long_name, b'pax long filename')
        add_tar(tar, 'empty', b'')
        for link, kind, target in [('hard',tarfile.LNKTYPE,name),('symlink',tarfile.SYMTYPE,name)]:
            entry=tarfile.TarInfo(link);entry.type=kind;entry.linkname=target;tar.addfile(entry)
    raw = tar_bytes.getvalue()
    for suffix, blob in [('tar', raw), ('tar.gz',gzip.compress(raw)), ('tar.bz2',bz2.compress(raw)), ('tar.xz',lzma.compress(raw))]:
        image = root / ('archive.' + suffix)
        image.write_bytes(blob)
        assert run('info',image).startswith('TAR ')
        assert '目录' in run('list',image)
        assert run('readlink',image,'symlink') == name
        for offset,count in [(65529,127),(len(data)-17,64),(0,1),(131071,513)]:
            read(image,name,offset,count,data[offset:offset+count])
        read(image,'hard',123,64,data[123:187])
        read(image,long_name,0,32,b'pax long filename')
        output=root/('extracted-'+suffix)
        run('unpack',image,output)
        assert (output/name).read_bytes()==data
        assert (output/'empty').stat().st_size==0
    for compression in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED,zipfile.ZIP_BZIP2,zipfile.ZIP_LZMA):
        image=root/f'archive-{compression}.zip'
        with zipfile.ZipFile(image,'w',compression=compression,allowZip64=True) as z:
            with z.open(name,'w',force_zip64=True) as member:member.write(data)
            z.writestr('empty',b'')
        assert run('info',image).startswith('ZIP ')
        read(image,name,65529,127,data[65529:65656])
        read(image,name,len(data)-17,64,data[-17:])
        assert run('verify',image)==f'Verified 2 files, {len(data)} bytes.'
    image=root/'links.cpio'
    image.write_bytes(cpio_entry('a',b'',11,2)+cpio_entry('b',data,11,2)+cpio_entry('TRAILER!!!',b'',0,1,0))
    assert run('info',image).startswith('CPIO ')
    for member in ('a','b'):
        read(image,member,333,127,data[333:460])
    assert run('verify',image)==f'Verified 2 files, {len(data)*2} bytes.'
    for name,blob in [('payload.bin.gz',gzip.compress(data)),('payload.bin.bz2',bz2.compress(data)),('payload.bin.xz',lzma.compress(data))]:
        image=root/name
        image.write_bytes(blob)
        read(image,'payload.bin',65529,127,data[65529:65656])
        assert run('verify',image)==f'Verified 1 files, {len(data)} bytes.'
    empty=root/'empty.txt.gz';empty.write_bytes(gzip.compress(b''))
    assert 'empty.txt' in run('list',empty)
    bad=root/'traversal.zip'
    with zipfile.ZipFile(bad,'w') as z:z.writestr('../escape.txt','invalid')
    run('info',bad,ok=False)
    bad=root/'invalid.tar.gz';bad.write_bytes(gzip.compress(b'not a tar archive'))
    run('info',bad,ok=False)
    bad=root/'unknown.bin';bad.write_bytes(b'not an archive')
    run('info',bad,ok=False)
    # A packed sparse TAR header with two extents and a 64-bit logical EOF.
    logical=2**32+8192
    sparse=root/'sparse.tar'
    entry=tarfile.TarInfo('sparse.bin');entry.size=1024;entry.type=tarfile.GNUTYPE_SPARSE
    header=bytearray(entry.tobuf(format=tarfile.GNU_FORMAT))
    for idx,(offset,length) in enumerate([(0,512),(2**32+4096,512)]):
        base=386+idx*24
        header[base:base+12]=f'{offset:011o}\0'.encode()
        header[base+12:base+24]=f'{length:011o}\0'.encode()
    header[483:495]=f'{logical:011o}\0'.encode()
    header[148:156]=b' '*8
    header[148:156]=f'{sum(header):06o}\0 '.encode()
    sparse.write_bytes(header+b'A'*512+b'B'*512+b'\0'*1024)
    read(sparse,'sparse.bin',508,12,b'AAAA'+b'\0'*8)
    read(sparse,'sparse.bin',2**32+4092,12,b'\0'*4+b'B'*8)
    read(sparse,'sparse.bin',logical-8,16,b'\0'*8)
print('PASS common archives, Unicode, PAX, ZIP64, CPIO links, sparse 64-bit TAR and invalid input')
