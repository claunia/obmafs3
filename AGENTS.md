# Project Guidelines

## Overview

OBMAFS3 is a FUSE-based Linux filesystem for deduplicated storage of disk images (CDs, DVDs, floppies, hard drives, etc.). Written in C99, licensed GPLv3+. See [docs/DESIGN.md](docs/DESIGN.md) for the on-disk format specification.

## Code Style

A `.clang-format` file is present at the repo root — use it. Key conventions:

- **Naming**: `snake_case` for functions, variables, types. Public API prefixed `obmafs3_`. Enums use `kCamelCase` (e.g. `kCompressionZstd`). Macros/constants use `OBMAFS3_UPPER_SNAKE`.
- **Indentation**: 4 spaces, no tabs. Allman brace style (opening brace on its own line).
- **Line length**: 120 columns.
- **Pointer alignment**: Right-aligned (`char *ptr`).
- **Space before parens**: Never — `if(condition)`, not `if (condition)`.
- **Header guards**: `#ifndef OBMAFS3_FILENAME_H` / `#define` / `#endif`.
- **On-disk structs**: Always `__attribute__((packed))`, all fields little-endian.
- **Comments**: Doxygen `/** ... */` with `@param`/`@return` for public APIs. `///` for struct field annotations.

## Architecture

- `src/lib/` — Core static library (`obmafs`) linked by all executables. All B+Tree, dedup, bitmap, block I/O, and filesystem logic lives here.
- `src/include/` — Shared headers (`obmafs.h`, `defs.h`, `enums.h`, `btree.h`, `superblock.h`, `tags.h`, etc.).
- `src/mkobmafs/` — Filesystem creation tool.
- `src/mount/` — FUSE3 mount daemon (`mount.obmafs`).
- `src/obmafsck/` — Filesystem checker.
- `src/import-aif/` — Aaru Image Format importer.
- `src/import-ngcw/` — Nintendo GameCube/Wii disc importer.
- `src/import-wiiu/` — Wii U disc importer.
- `src/import-ps3/` — PlayStation 3 disc importer.
- `src/obmafs-query/` — Metadata query tool.
- `src/defrag/` — Defragmentation tool.
- `3rdparty/lzma2600/` — Vendored LZMA SDK.
- `docs/DESIGN.md` — On-disk format specification.

## Error Handling

Functions return `int` — `OBMAFS3_OK` (0) on success, `OBMAFS3_ERR_*` negative codes on failure (defined in `src/include/obmafs.h`). Use `DBG_RETURN()` / `DBG_RETURN_ERRNO()` macros from `src/include/debug.h` to log and return errors. No goto-cleanup — use early returns with inline resource cleanup.

## Build and Test

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Integration test (create + check a filesystem):
```bash
build/src/mkobmafs/mkobmafs -s 10485760 -l TestFS /tmp/test.obmafs
build/src/obmafsck/obmafsck /tmp/test.obmafs
```

No unit test framework is used. Correctness is validated via `obmafsck`.

## Dependencies

Fetched automatically by CMake: xxHash, zstd, libaaruformat. System requirements: FUSE 3 (≥3.0), pthreads, libreadline.

## Conventions

- All multi-byte on-disk values are little-endian.
- Deduplication granularity matches native media sector size (512, 2048, or 2352 bytes).
- Every source file includes the standard GPLv3+ license header with author and component metadata.
- File headers in `src/include/` are shared across all tools — changes there affect the entire project.
