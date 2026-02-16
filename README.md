# OBMAFS3

A FUSE filesystem for deduplicated storage of disk image files, implemented in C99.

## Overview

OBMAFS3 deduplicates data at the sector level of disk images stored inside it, while
allowing efficient random access and storage of accompanying metadata files.

**Tools included:**

| Binary         | Description                          |
|----------------|--------------------------------------|
| `mkobmafs`     | Create a new OBMAFS3 filesystem      |
| `mount.obmafs` | Mount an OBMAFS3 filesystem via FUSE |
| `obmafsck`     | Check / validate an OBMAFS3 volume   |

## Prerequisites (Linux)

```bash
# Debian / Ubuntu
sudo apt-get install build-essential cmake pkg-config libfuse3-dev git

# Fedora / RHEL
sudo dnf install gcc cmake pkgconfig fuse3-devel git

# Arch
sudo pacman -S base-devel cmake pkgconf fuse3 git
```

## Building

xxHash and zstd are fetched automatically from Git via CMake `FetchContent`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Binaries are placed under `build/src/*/`.

## Quick Test

```bash
# Create a 10 MiB filesystem image
./build/src/mkobmafs/mkobmafs -s 10485760 -l TestFS /tmp/test.obmafs

# Check the filesystem
./build/src/obmafsck/obmafsck /tmp/test.obmafs

# Mount (foreground, for debugging)
mkdir -p /tmp/mnt
./build/src/mount/mount.obmafs --device=/tmp/test.obmafs -f /tmp/mnt
```

## VS Code Remote SSH Workflow

1. Install the **Remote - SSH** extension in VS Code.
2. Connect to your Linux machine via `Remote-SSH: Connect to Host…`.
3. Open the `obmafs3` folder on the remote machine.
4. Install the recommended extensions when prompted (CMake Tools, C/C++).
5. CMake Tools will auto-detect the project — select a kit and configure.
6. Use **Ctrl+Shift+B** to build, or run tasks from the Command Palette.
7. Debug configurations for all three binaries are provided in `.vscode/launch.json`.

## Project Structure

```
CMakeLists.txt              Root CMake (fetches xxHash + zstd, finds FUSE3)
src/
  include/
    superblock.h             Superblock on-disk structure
    btree.h                  B+Tree structures (catalog, inode nodes)
    block.h                  Data block header
    defs.h                   Dedup, extent, tree-list structures
    enums.h                  Filesystem enumerations
    obmafs.h                 Library public API
  lib/
    superblock.c             Superblock read/write/validate
    btree.c                  B+Tree traversal and lookup
    block.c                  Block I/O, compression, file data reading
    checksum.c               XXH64 checksum operations
    io.c                     Context management, creation, checking
  mkobmafs/
    main.c                   mkobmafs entry point
  mount/
    main.c                   FUSE mount entry point
    fuse_ops.h/.c            FUSE operation callbacks
  obmafsck/
    main.c                   Filesystem checker entry point
.vscode/
  settings.json              CMake & editor settings
  tasks.json                 Build & test tasks
  launch.json                Debug configurations
  c_cpp_properties.json      IntelliSense configuration
  extensions.json            Recommended extensions
```

## License

TBD
