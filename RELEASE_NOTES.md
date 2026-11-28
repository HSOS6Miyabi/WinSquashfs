# WinSquashfs 0.2.0

- Add native random-access reading and creation of DwarFS and EROFS images alongside SquashFS.
- Unified browser, extraction, verification, drive mounts and UNC access.
- Redesigned format-specific creation workspace with multiple files and folders, drag-and-drop and eight compression presets.
- Extreme compression uses high-ratio format-specific settings.
- Explorer multiselect creation command and DwarFS/EROFS file associations.
- English and Simplified Chinese interfaces.

DwarFS creation uses mkdwarfs 0.12.4; the reader supports filesystem versions 2.3–2.5 and null, Zstandard, LZ4 and LZMA codecs. EROFS reading supports single-device raw images.

See the README for installation, supported formats and creation options.
