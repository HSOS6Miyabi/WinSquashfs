// SPDX-License-Identifier: GPL-3.0-only
use dwarfs::{Archive, ArchiveIndex, AsChunks};
use dwarfs::archive::{Config, IsInode};
use dwarfs::positioned_io::{ReadAt, Size};
use std::{ffi::c_void, io, panic::{catch_unwind, AssertUnwindSafe}, ptr, slice};

type SourceRead = extern "C" fn(u64, *mut u8, usize) -> i32;
type Visit = extern "C" fn(u64, *const u8, usize, u32, u64, u64) -> i32;

struct Source { length: u64, read: SourceRead }
impl ReadAt for Source {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> io::Result<usize> {
        let len = buffer.len().min(self.length.saturating_sub(offset).min(usize::MAX as u64) as usize);
        if len == 0 { return Ok(0); }
        if (self.read)(offset, buffer.as_mut_ptr(), len) != 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "image read failed"));
        }
        Ok(len)
    }
}
impl Size for Source {
    fn size(&self) -> io::Result<Option<u64>> { Ok(Some(self.length)) }
}
struct Image { index: ArchiveIndex, archive: Archive<Source> }
fn guarded(f: impl FnOnce() -> Result<i32, Box<dyn std::error::Error>>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) { Ok(Ok(n)) => n, _ => -1 }
}
#[no_mangle]
pub unsafe extern "C" fn wa_open(length: u64, reader: SourceRead, out: *mut *mut c_void,
    root: *mut u64, block: *mut u32, mtime: *mut u64) -> i32 {
    if out.is_null() || root.is_null() || block.is_null() || mtime.is_null() { return -1; }
    *out = ptr::null_mut();
    guarded(|| {
        let mut config = Config::default();
        config.metadata_size_limit(usize::MAX).section_index_size_limit(usize::MAX);
        let (index, archive) = Archive::new_with_config(Source { length, read: reader }, &config)?;
        *root = 0;
        *block = 0; // DwarFS can use varying section/chunk sizes.
        *mtime = index.root().metadata().mtime();
        *out = Box::into_raw(Box::new(Image { index, archive })).cast();
        Ok(0)
    })
}
#[no_mangle]
pub unsafe extern "C" fn wa_close(image: *mut c_void) {
    if !image.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(image.cast::<Image>())))); }
}
#[no_mangle]
pub unsafe extern "C" fn wa_list(image: *mut c_void, id: u64, visit: Visit) -> i32 {
    guarded(|| {
        let im = &*image.cast::<Image>();
        let dir = im.index.get_inode(u32::try_from(id)?).and_then(|i|i.as_dir()).ok_or("invalid directory")?;
        for entry in dir.entries() {
            let inode = entry.inode();
            let meta = inode.metadata();
            let mode = meta.file_type_mode().0;
            let size = inode.as_file().map(|f| f.as_chunks().total_size()).unwrap_or(0);
            let name = entry.name().as_bytes();
            if visit(inode.inode_num() as u64, name.as_ptr(), name.len(), mode, size, meta.mtime()) != 0 {
                return Err("entry callback failed".into());
            }
        }
        Ok(0)
    })
}
#[no_mangle]
pub unsafe extern "C" fn wa_read(image: *mut c_void, id: u64, offset: u64,
    buffer: *mut u8, length: u32) -> i32 {
    if length > i32::MAX as u32 { return -1; }
    if length == 0 { return 0; }
    guarded(|| {
        let im = &mut *image.cast::<Image>();
        let file = im.index.get_inode(u32::try_from(id)?).and_then(|i|i.as_file()).ok_or("invalid file")?;
        let out = slice::from_raw_parts_mut(buffer, length as usize);
        let mut skip = offset;
        let mut written = 0;
        for chunk in file.as_chunks() {
            if skip >= chunk.size() as u64 { skip -= chunk.size() as u64; continue; }
            let bytes = chunk.read_cached(&mut im.archive)?;
            let start = skip as usize;
            let n = (bytes.len() - start).min(out.len() - written);
            out[written..written+n].copy_from_slice(&bytes[start..start+n]);
            written += n; skip = 0;
            if written == out.len() { break; }
        }
        Ok(written as i32)
    })
}
