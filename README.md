# OBMAFS3 — The Object Based Media Archival File System

> *A purpose-built filesystem for preserving the world's disk images — CDs, DVDs, Blu-rays, hard drives, floppies, tapes, and everything in between — with unmatched storage efficiency and rich, queryable metadata.*

---

## Why Another Filesystem?

You probably already have a perfectly good filesystem. Ext4, XFS, ZFS, btrfs — they all work fine for everyday files, and nobody is asking you to replace them. But if you have ever tried to maintain a large collection of disk images — say, a few thousand CD-ROM ISOs, some floppy archives, a shelf of hard drive forensic captures — you have already noticed a problem that none of those filesystems were designed to solve.

**Disk images are enormous, repetitive, and full of duplicated data.**

A retail game CD from 1998 and a demo disc from the same year might share 90% of their raw sectors. Two slightly different pressings of the same album, or two revisions of the same operating system installer, can differ by only a handful of sectors out of millions. Store them as plain files on a conventional filesystem and every single byte gets written to disk twice — or three times, or fifty times, depending on how many copies you have.

OBMAFS3 exists to solve exactly this problem.

It stores disk images at the **sector level** rather than the file level. When two images share a sector — down to 512 bytes for a floppy or hard drive, or 2048 bytes for optical media — that sector is stored only once. The savings are not marginal. In real-world collections, OBMAFS3 routinely achieves 40–80% space reduction compared to storing the same images as flat files, even before its built-in Zstandard compression kicks in.

And it does all of this while keeping every image instantly accessible as a normal file. You can mount an OBMAFS3 volume, `cd` into it, and read any image with standard Linux tools — no special extraction step, no waiting for decompression, no proprietary viewer. It just works.

---

## A Decade of Design

OBMAFS has been in active design since **2015**. The filesystem you see here is the **third and final iteration** — the result of years of learning what works and what does not in the specialized domain of media image archival.

- **Version 1** was the proof of concept: could sector-level deduplication work at all inside a usable filesystem? The answer was yes, but the on-disk format had serious limitations.
- **Version 2** refined the B+Tree structures and introduced metadata support, but the deduplication model still had rough edges and the format was not flexible enough for the long term.
- **Version 3** is the culmination of everything we learned. The on-disk format is clean, extensible, and designed to last. The deduplication engine is robust. The metadata system is rich enough to build real tools on top of. This is the version we intend to maintain and finalize.

OBMAFS3 is still **experimental** — we have not yet reached a 1.0 release and the on-disk format may still change. That said, the filesystem has been under heavy development and testing, and it should be **relatively stable for evaluation and non-critical use** today. We are past the stage of fundamental design changes; what remains is hardening, optimizing, and expanding tooling.

---

## Features at a Glance

### Sector-Level Deduplication

This is the core reason OBMAFS3 exists. Unlike general-purpose filesystems that deduplicate at a block level of 4 KB, 16 KB, or even 128 KB, OBMAFS3 deduplicates at the **native sector size** of the media being stored. For a CD image, that means 2048-byte sectors. For a floppy or hard drive image, 512 bytes. For optical media with raw sectors, 2352 bytes.

Why does this matter? Because disk images differ at the sector boundary, not at arbitrary 4K or 128K boundaries. A single changed sector in a 700 MB CD image would force ZFS or btrfs to re-store an entire 128K block. OBMAFS3 stores only the one sector that changed — potentially saving 64x the space for that single difference. Multiply that across thousands of images and the savings become dramatic.

On top of deduplication, all stored data is compressed with **Zstandard (zstd)**, giving you an additional layer of space savings with minimal performance overhead.

### Rich, Queryable Metadata

Every image stored in OBMAFS3 can carry **arbitrary key-value metadata** — artist, title, year, genre, platform, serial number, barcode, mastering information, or anything else you can think of. This is not just a filename convention or a sidecar text file that can get separated from its image; the metadata lives inside the filesystem itself, inseparable from the data it describes.

But OBMAFS3 goes further than just storing metadata. It maintains a **reverse index** that lets you search across your entire collection:

- Find all images where `artist = "Iron Maiden"`
- Find all images where `year > "1995" AND platform = "PlayStation"`
- Find all images where *any* metadata field `CONTAINS "maiden"` (wildcard key search)
- Find all images where a particular key `EXISTS`, regardless of its value

Queries support nine operators — equals, not-equals, greater-than, less-than, greater-or-equal, less-or-equal, contains, starts-with, and exists — and can combine up to four conditions with AND or OR logic. The included `obmafs-query` tool gives you an interactive prompt to run these queries on a live mount, with the option to export results as plain text or JSON.

If you have ever tried to find "that one disc" in a collection of thousands, you will appreciate having a built-in search engine that operates at the filesystem level.

### Native CD/DVD/Blu-ray Awareness

OBMAFS3 understands optical media at a level that no general-purpose filesystem does. CD images are not just opaque blobs of bytes — they have structure. Each raw 2352-byte sector contains a 16-byte sync/header prefix, 2048 bytes of user data, and 288 bytes of error correction (ECC/EDC) suffix. Some discs also carry 96 bytes of subchannel data per sector.

OBMAFS3 **splits CD sectors into their constituent parts** and deduplicates each component separately. The prefix, user data, suffix, and subchannel data each go into their own dedicated B+Tree. Since ECC/EDC data is deterministic (it can be recomputed from the user data), and sync headers are highly repetitive, this decomposition achieves far better deduplication ratios than treating each sector as an indivisible unit.

When you read a CD image back, OBMAFS3 transparently reassembles the full raw sector from its stored components. The image you get out is byte-for-byte identical to the one you put in.

### Media Tags

Disk images often come with out-of-band information that does not fit into the sector data itself — things like ATIP (Absolute Time In Pregroove) data from CDs, PFI/DMI structures from DVDs, or SCSI mode pages captured during imaging. OBMAFS3 stores these as **media tags**: binary blobs keyed by a well-defined tag type, attached to individual images. This preserves forensic-grade detail about the original media that would otherwise be lost.

### FUSE Integration

OBMAFS3 mounts as a standard Linux filesystem via FUSE3. Once mounted, your images appear as regular files in a regular directory tree. You can:

- Browse with `ls`, copy with `cp`, read with `dd` or `hexdump`
- Use any application that reads files — no special tools required
- Access images over NFS, Samba, or any other network filesystem layered on top

There is no import/export dance, no proprietary container to unpack. Your data is always accessible.

### Filesystem Integrity Checking

The `obmafsck` tool performs comprehensive integrity checking of OBMAFS3 volumes:

- Superblock validation (magic numbers, checksums, field ranges)
- Backup superblock consistency (with automatic recovery)
- Allocation bitmap verification
- Full B+Tree traversal for all tree structures (catalog, inode, overflow, metadata, dedup, refcount, media tag, CD prefix/suffix/subchannel)
- Cross-reference validation between trees
- Optional data block scrubbing (verify every stored checksum)
- Optional deduplication and compression statistics
- Optional B+Tree defragmentation

### Aaru Image Format Import

If you already have a collection of disk images in the [Aaru](https://aaru.app) format (`.aif` files), OBMAFS3 includes `import-aif`, a dedicated import tool that transfers everything — sector data, media tags, metadata, CICM XML sidecars, and dump hardware information — into an OBMAFS3 volume in a single operation.

The importer handles all the details: it converts Aaru's internal compression and hashing to OBMAFS3's native formats, maps media tag types, translates UTF-16LE metadata to UTF-8, and generates CUE sheet sidecars for CD images. It is the fastest way to migrate an existing Aaru collection to OBMAFS3.

**Aaru export** (converting OBMAFS3 images back to `.aif` files) is not yet implemented, but it is planned. The on-disk format preserves all the information needed for lossless round-tripping, so this is a matter of writing the tooling, not a fundamental limitation.

### Interactive Query Tool

The `obmafs-query` tool connects to a live OBMAFS3 mount and lets you search your collection interactively:

```
$ obmafs-query /mnt/archive
obmafs-query: connected to /mnt/archive
Type 'help' for available commands, 'quit' to exit.

> artist = "Iron Maiden"
  /metal/iron_maiden_1982.iso
  /metal/iron_maiden_1984.iso
  /metal/iron_maiden_1988.iso

3 result(s)
Export? [txt <path> / json <path> / enter to skip]: json /tmp/results.json
Exported 3 result(s) to /tmp/results.json
```

You can use `*` as a wildcard key to search across all metadata fields at once:

```
> * CONTAINS "maiden"
```

Results can be exported to `.txt` (one path per line) or `.json` (structured output with a count and a results array) for integration with scripts and other tools.

---

## Advantages in Detail

### Unmatched Space Efficiency for Media Collections

This is the headline feature. No other filesystem deduplicates at the sector level, and the difference is not subtle. Consider a collection of 500 Sega Saturn disc images. Many of those games share the same system area, the same BIOS bootstrap, the same audio codec libraries. On a conventional filesystem, you store all of that redundancy hundreds of times over. On OBMAFS3, each unique sector exists exactly once, regardless of how many images contain it.

The savings compound further because OBMAFS3 also compresses the deduplicated data with Zstandard. You get deduplication *and* compression, applied in the right order (deduplicate first, then compress the unique blocks), which is the most effective approach.

### Your Data Stays Accessible

Some archival solutions require you to pack data into proprietary containers or tarballs that must be fully extracted before you can read anything. OBMAFS3 takes the opposite approach: your images are always live, always mountable, always readable with standard tools. Need to grab a single file from a CD image stored in your archive? Mount the OBMAFS3 volume, mount the ISO inside it, and copy what you need. No waiting, no temporary space, no special software.

### Metadata That Travels With Your Data

Metadata stored in filenames, spreadsheets, or separate database files is fragile. Rename a file and you lose the connection. Move your collection to a new machine and the database might not come along. OBMAFS3 stores metadata *inside* the filesystem, physically inseparable from the images it describes. Back up the volume and the metadata comes with it. Restore the volume and the metadata is there.

The reverse-index query system means you do not need external tools to search your collection. The filesystem *is* the database.

### Built for Archival Longevity

OBMAFS3 uses well-understood, proven data structures (B+Trees with explicit checksums) and a simple on-disk layout documented in exhaustive detail. Every block on disk has a magic number and a checksum. The superblock is backed up at the end of the volume. The filesystem checker can detect and repair a wide range of corruption scenarios. This is a filesystem designed with the assumption that data will be stored for decades.

---

## Disadvantages — Honestly

We believe in being upfront about limitations. OBMAFS3 is not the right tool for every job.

### Linux Only

OBMAFS3 currently runs only on Linux. This was a deliberate choice, not an oversight: our primary targets are **NAS appliances and backend archive servers**, which overwhelmingly run Linux. A headless server in a closet, serving images over the network to whatever desktop OS you prefer, is the intended deployment model.

That said, the FUSE3 API is the only OS-specific dependency. A macOS port via macFUSE or a FreeBSD port via FUSE for FreeBSD is theoretically possible, but it is not a priority at this time.

### Write-Optimized, Not Delete-Optimized

OBMAFS3 is designed for archival workloads: you write data in, and it stays there. When you delete a file, the directory entry is removed, but the underlying deduplicated sectors are **not freed**. This is intentional — in a deduplicated filesystem, a sector might be shared by hundreds of images, and reference-counted garbage collection adds significant complexity and risk to a system whose primary job is to keep data safe forever.

For the intended use case (building and maintaining a media archive), this is rarely a problem. You are adding images to a collection, not churning through temporary files. But if you need a filesystem that efficiently reclaims space on deletion, OBMAFS3 is not the right choice today.

### Write Performance

Deduplication is not free. Every sector written must be hashed, looked up in the dedup tree, and either stored as new data or recorded as a reference to existing data. This means write throughput is lower than a conventional filesystem, particularly for data that does not benefit from deduplication (e.g., small non-image files, highly unique data).

Read performance, on the other hand, is essentially limited by your storage device — the B+Tree lookups add negligible overhead compared to the I/O cost of reading the actual data.

### Single Developer

OBMAFS3 is developed and maintained by a single person. This means development moves at a human pace, bug fixes may take time, and the bus factor is exactly one. We mention this not as an excuse but as an honest assessment: if you are evaluating OBMAFS3 for a critical deployment, you should understand the support model.

Contributions are welcome and encouraged.

---

## How OBMAFS3 Compares

### vs. ZFS / btrfs

ZFS and btrfs are excellent general-purpose filesystems with built-in deduplication. However, their deduplication operates at the **filesystem block level** — typically 4 KB to 128 KB. For disk images, this is the wrong granularity. A single changed 512-byte sector in a hard drive image forces ZFS to re-store an entire 128 KB record. OBMAFS3 stores only the 512 bytes that actually changed.

ZFS deduplication also has a well-known drawback: it requires keeping the entire dedup table (DDT) in RAM, which can consume enormous amounts of memory for large pools. OBMAFS3's dedup structures live on disk in B+Trees and do not require large amounts of RAM.

On the other hand, ZFS and btrfs are battle-tested, production-grade, multi-platform filesystems with decades of development behind them. OBMAFS3 is none of those things. If you need a general-purpose filesystem with some dedup capability, use ZFS or btrfs. If you need the best possible deduplication for media images specifically, that is where OBMAFS3 shines.

### vs. Storing Images in Archives (tar, zip, 7z, rar)

Compressed archives can achieve good ratios, but they sacrifice **random access**. To read one file from a tar.zst archive, you must decompress from the beginning. To read one sector from an image inside that archive, you must decompress the entire image first. OBMAFS3 provides instant random access to any sector of any image, at any time, with no extraction step.

Archives also offer no deduplication across files. Two nearly identical ISOs in the same tar archive are stored in full, twice. OBMAFS3 deduplicates across the entire volume.

### vs. Aaru Image Format (.aif)

Aaru is a wonderful disk imaging tool and its image format is excellent for capturing and preserving individual disc images with full fidelity. OBMAFS3 is designed to complement Aaru, not replace it — in fact, `import-aif` exists specifically to bring Aaru images into OBMAFS3 for long-term, deduplicated storage.

The key difference is scope: Aaru operates on individual images (one file per disc), while OBMAFS3 operates on entire collections (thousands of images in a single volume with cross-image deduplication). They solve different problems and work well together.

### vs. MAME CHD

CHD (Compressed Hunks of Data) is a per-file compression format. Each CHD file is self-contained with no deduplication across files. OBMAFS3 deduplicates across your entire collection. If you have 200 variants of the same arcade board ROM, OBMAFS3 will store the shared sectors only once; with CHD, each file stores them independently.

---

## Included Tools

| Binary | Description |
|---|---|
| `mkobmafs` | Create a new OBMAFS3 filesystem image |
| `mount.obmafs` | Mount an OBMAFS3 filesystem via FUSE3 |
| `obmafsck` | Check, verify, and repair an OBMAFS3 volume |
| `import-aif` | Import Aaru Image Format (.aif) archives into OBMAFS3 |
| `obmafs-query` | Interactive metadata query tool for live mounts |

---

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

xxHash, zstd, and BLAKE3 are fetched automatically from Git via CMake `FetchContent`. No manual dependency management needed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Binaries are placed under `build/src/*/`.

## Quick Start

```bash
# Create a 10 GiB filesystem image
./build/src/mkobmafs/mkobmafs -s 10737418240 -l "My Archive" /path/to/archive.obmafs

# Check the filesystem
./build/src/obmafsck/obmafsck /path/to/archive.obmafs

# Mount
mkdir -p /mnt/archive
./build/src/mount/mount.obmafs --device=/path/to/archive.obmafs /mnt/archive

# Import an Aaru image
./build/src/import-aif/import-aif /path/to/image.aif /mnt/archive/my_disc.iso

# Query metadata
./build/src/obmafs-query/obmafs-query /mnt/archive
```

## Development with VS Code

1. Install the **Remote - SSH** extension and connect to your Linux machine.
2. Open the `obmafs3` folder on the remote machine.
3. Install the recommended extensions when prompted (CMake Tools, C/C++).
4. CMake Tools will auto-detect the project — select a kit and configure.
5. Use **Ctrl+Shift+B** to build, or run tasks from the Command Palette.
6. Debug configurations for all binaries are provided in `.vscode/launch.json`.

---

## License

GPLv3+ — see [LICENSE](LICENSE) for details.
