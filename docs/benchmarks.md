# Benchmarks

`scripts/bench.mjs` times the bare `real-size` binary against the platform's
own tool for the same walk:

- macOS and Linux: `du -sk`.
- Windows: `robocopy <path> NUL /L /S /BYTES`, `dir /s /a` and PowerShell
  `Get-ChildItem -Recurse | Measure-Object`. `robocopy /L` is the fastest of
  the three on every tree except a handful of large files, where `dir` wins,
  so the ratio is taken against whichever was fastest for that tree.

Each tool runs twice to warm the cache, then 10 more times; the table shows
the mean, standard deviation, minimum and maximum. With `--cold` the script
also drops the caches once and runs each tool again. That needs `sudo`
without a password (`purge` on macOS, `/proc/sys/vm/drop_caches` on Linux), so
cold numbers come from CI runners; Windows has no cold run.

```sh
npm run build
node scripts/bench.mjs [--runs 10] [--warmup 2] [--cold] [--generate <dir>] [<path>...]
```

`--generate <dir>` builds three trees under `<dir>` and removes them after:

- **hard links**: 30,000 small files in a store, each hard-linked into
  1,500 package folders, as pnpm does. The measured folder holds only links
  whose other link is outside it.
- **deep tree**: 117,186 small files, six per directory, seven levels deep.
- **large files**: four 256 MB files.

Paths given on the command line are measured as they are, read only.

On Windows the script also times `real-size` with
`REAL_SIZE_NO_STAT_BY_NAME=1`, which makes it open a handle per file the way
it does on Windows releases older than Windows 11 24H2 and Windows Server 2025.

CI runs the script on every platform in the `bench` job, which never fails
the build.

## Results

### This Mac

Apple M4 Pro, 14 cores, 48 GB, macOS 27.0, internal SSD, APFS. Warm cache, 10
runs, with the walk on 4 threads. Other work ran on the machine.

| Tree                                                       | Files   | real-size | du -sk   | Ratio |
| ---------------------------------------------------------- | ------- | --------- | -------- | ----- |
| A pnpm project's `node_modules`, mostly clones (read only) | 147,077 | 482.6 ms  | 864.7 ms | 0.56x |
| hard links                                                 | 30,000  | 148.1 ms  | 138.0 ms | 1.07x |
| deep tree                                                  | 117,186 | 579.5 ms  | 618.7 ms | 0.94x |
| large files                                                | 4       | 2.8 ms    | 1.8 ms   | 1.51x |

The deep tree varied most: 485.5 ms to 1.23 s.

### CI runners

GitHub-hosted runners, warm cache unless marked cold. macOS runners are
virtual machines and vary most between runs.

| Runner                                | Filesystem | Tree        | real-size | Baseline            | Ratio | Cold real-size / baseline |
| ------------------------------------- | ---------- | ----------- | --------- | ------------------- | ----- | ------------------------- |
| ubuntu-latest (Xeon 6973P-C, 4 cores) | ext4       | hard links  | 91.4 ms   | 123.8 ms (du)       | 0.74x | 623.9 ms / 732.9 ms       |
| ubuntu-latest                         | ext4       | deep tree   | 352.0 ms  | 594.5 ms (du)       | 0.59x | 3.06 s / 3.59 s           |
| ubuntu-latest                         | ext4       | large files | 1.5 ms    | 3.6 ms (du)         | 0.42x | 5.4 ms / 36.2 ms          |
| ubuntu-latest                         | btrfs      | hard links  | 90.1 ms   | 123.5 ms (du)       | 0.73x | 582.1 ms / 652.2 ms       |
| ubuntu-latest                         | btrfs      | deep tree   | 1.47 s    | 619.8 ms (du)       | 2.37x | 2.71 s / 1.86 s           |
| ubuntu-latest                         | XFS        | hard links  | 73.9 ms   | 105.4 ms (du)       | 0.70x | 384.8 ms / 465.8 ms       |
| ubuntu-latest                         | XFS        | deep tree   | 747.1 ms  | 521.4 ms (du)       | 1.43x | 1.54 s / 1.38 s           |
| ubuntu-24.04-arm (4 cores)            | ext4       | hard links  | 59.7 ms   | 57.6 ms (du)        | 1.04x | 491.5 ms / 521.5 ms       |
| ubuntu-24.04-arm                      | ext4       | deep tree   | 199.4 ms  | 233.7 ms (du)       | 0.85x | 2.78 s / 2.89 s           |
| ubuntu-24.04-arm                      | btrfs      | hard links  | 58.8 ms   | 56.3 ms (du)        | 1.04x | 370.0 ms / 391.2 ms       |
| ubuntu-24.04-arm                      | btrfs      | deep tree   | 728.0 ms  | 244.1 ms (du)       | 2.98x | 1.23 s / 792.0 ms         |
| ubuntu-24.04-arm                      | XFS        | hard links  | 50.9 ms   | 50.9 ms (du)        | 1.00x | 233.6 ms / 245.8 ms       |
| ubuntu-24.04-arm                      | XFS        | deep tree   | 353.0 ms  | 199.1 ms (du)       | 1.77x | 666.7 ms / 593.3 ms       |
| macos-latest (M1, 3 virtual cores)    | APFS       | hard links  | 369.1 ms  | 233.9 ms (du)       | 1.58x | 423.9 ms / 269.3 ms       |
| macos-latest                          | APFS       | deep tree   | 3.61 s    | 1.60 s (du)         | 2.26x | 3.81 s / 3.95 s           |
| macos-15-intel (i7-8700B, 4 cores)    | APFS       | hard links  | 1.00 s    | 280.7 ms (du)       | 3.56x | 1.26 s / 355.2 ms         |
| macos-15-intel                        | APFS       | deep tree   | 2.28 s    | 1.05 s (du)         | 2.17x | 2.08 s / 1.14 s           |
| windows-latest (Xeon 8573C, 4 cores)  | NTFS       | hard links  | 337.8 ms  | 135.1 ms (robocopy) | 2.50x |                           |
| windows-latest                        | NTFS       | deep tree   | 1.66 s    | 690.4 ms (robocopy) | 2.40x |                           |
| windows-11-arm (Cobalt 100, 4 cores)  | NTFS       | hard links  | 302.5 ms  | 152.2 ms (robocopy) | 1.99x |                           |
| windows-11-arm                        | NTFS       | deep tree   | 1.36 s    | 1.27 s (robocopy)   | 1.07x |                           |

The four large files take 1 to 10 ms everywhere and `real-size` is at or below
the baseline on every runner.

## What the profiles showed and what changed

- **macOS.** `sample` on the hard-link tree shows 85% of the time inside
  `getattrlistbulk`, and the hard-link table and the rest of the program
  below 1%. A test walker with the same attributes minus
  `ATTR_CMNEXT_PRIVATESIZE` runs at `du`'s speed (about 120 ms on the hard-link
  tree and 650 ms on `node_modules`); adding the private size costs 75% on the
  hard-link tree and 30% on `node_modules`. The buffer size (8 KB to 256 KB)
  and the other attributes make no measurable difference. The private size is
  the answer, so this cost stays. The walk now lists directories on several
  threads instead (see Parallelism below).
- **Hard-link table.** It is a hash table on device and inode (and the
  128-bit file id on Windows) from the first version; the reference helper's
  linear scan was never used here. A test measures a tree with 5,000 linked
  files.
- **Linux, ext4 and tmpfs.** `fdopendir` and `fstatat(AT_SYMLINK_NOFOLLOW)`
  with no per-file `open`; `real-size` is at or below `du`.
- **Linux, btrfs and XFS.** The per-file `open` and `FS_IOC_FIEMAP` are the
  cost. A file with several links is now mapped after the walk, and only when
  every link was inside the tree, so a pnpm-style tree of links to an outside
  store maps nothing: on btrfs that took the hard-link tree from 1.78x to
  0.73x on x64 and from 3.57x to 1.04x on arm64, and on XFS from 1.35x to
  0.70x and from 2.02x to 1.00x. Files with one link still need the `open` and
  the ioctl, which is the 1.4x to 3x on the deep tree of small files.
- **Windows.** Opening a handle per file to read its link count, allocation
  and file id is the cost; the directory listing has no link count. On
  Windows 11 24H2 and Windows Server 2025 and later, `GetFileInformationByName`
  returns all three without a handle, which made the walk 1.8x to 2.3x faster
  (hard links 745.6 ms to 337.8 ms, deep tree 3.16 s to 1.66 s on
  windows-latest). Older releases keep the handle per file. `robocopy` reads
  only the directory listing, so the per-file query is what remains between
  the two. On ReFS and Dev Drive volumes, and for compressed, sparse or
  reparse-point files, the handle is still opened to read the extents or the
  compressed size.
- **Parallelism, macOS.** Most of the wall time of a large walk is spent
  waiting in `getattrlistbulk`: on a tree of 7.8 million files the
  single-threaded walk took 221 s of wall time for 88 s of CPU. The walk now
  lists directories on one thread per CPU, at most 4. On this Mac, warm,
  4 threads were about 2x faster than one on `node_modules` trees of about
  150,000 files, and about 3.5x to 4x faster on a source tree of 1.9 million
  files (46 s to 14 s) and on the 7.8 million file tree (221 s to 60 s). Past
  4 to 8 threads the walk got slower and its CPU time, nearly all system
  time, climbed steeply (802 s of CPU at 14 threads on the 7.8 million file
  tree), so the count is capped at 4.
- **Parallelism, Linux and Windows** was not added. Linux is at or below
  `du` where no per-file query is needed.
