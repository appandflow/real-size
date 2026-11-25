# real-size

How many bytes deleting a folder would actually free, as opposed to what `du`
shows. A command-line tool and a Node library for macOS, Linux and Windows. It
ships an [agent skill](#agent-skill) so coding agents reach for it instead of
`du`.

```console
$ du -sh node_modules
4.4G	node_modules
$ npx real-size node_modules
node_modules: at least 90.3 MB private, 4.8 GB allocated, 168433 files
```

## Why `du` misleads

`du` adds up the blocks of every file under a path. That is the space the files
occupy, but not the space their deletion returns, because modern filesystems
share blocks between files:

- **Clones.** On APFS, `cp -c`, Finder copies and tools such as pnpm and
  Xcode create copy-on-write clones. A clone shares every block with its
  original until one of them changes. Deleting the clone frees nothing.
- **Hard links.** A package store (pnpm, Nix, Bazel caches) links one file
  into many folders. Deleting one folder removes a link, not the data, while
  another link exists outside it.
- **Reflinks and block clones.** Btrfs and XFS (`cp --reflink`) and ReFS or
  Dev Drive on Windows share extents between files that each have a single
  link, which a link count does not show.

`real-size` walks the tree and counts only the bytes no file outside it
shares.

## Install

```bash
npm install --global real-size
```

Or run it without installing: `npx real-size <path>`.

The package ships prebuilt binaries and has no install scripts and no runtime
dependencies.

On macOS and Linux the `real-size` command is a `#!/bin/sh` script that finds
the binary for the machine and `exec`s it, so no Node process is involved and
it adds a few milliseconds to the binary's start-up. It works when installed
with npm, pnpm, Yarn classic and Yarn Berry's `node-modules` linker, locally
or globally, through `npx`, `pnpm dlx` and `yarn dlx`, and with BusyBox `sh`
on Alpine. `yarn real-size` under Yarn Berry, including Plug'n'Play, runs the
command through Node; the same file is also a Node script that starts the
binary.

On Windows the command is **`real-size-windows`**, with the same arguments:

```bat
npm install --global real-size
real-size-windows node_modules
npx --package real-size real-size-windows node_modules
```

npm and pnpm generate Windows launchers from a command's `#!` line, so the
`real-size` command's `#!/bin/sh` needs an `sh` on Windows (Git Bash has one;
cmd.exe and PowerShell do not). `real-size-windows` is a batch file with no
`#!` line, which the launchers start directly in cmd.exe, Windows PowerShell
and PowerShell, without Node. It runs the arm64 binary on Windows on Arm and
the x64 binary otherwise, and returns its exit status. `real-size --version`
prints the version and the binary's platform, for example `0.1.0
(win32-arm64)`.

## Command line

```text
real-size [--json] <path>...
```

Each path is measured on its own. Plain output prints one line per path, and a
total when several paths are given:

```console
$ real-size build packages
build: at least 24.6 kB private, 167.9 kB allocated, 3 files
packages: at least 94.7 MB private, 19.8 GB allocated, 68181 files
total: at least 94.7 MB private, 19.8 GB allocated, 68184 files
```

Sizes use decimal units (1 MB is 1,000,000 bytes). "at least" marks a result
that is not exact.

`--json` prints exactly one JSON document on stdout:

```json
{
  "paths": [
    {
      "path": "node_modules",
      "allocated": 4767227904,
      "private": 90292224,
      "files": 168433,
      "exact": false
    }
  ],
  "total": {
    "allocated": 4767227904,
    "private": 90292224,
    "files": 168433,
    "exact": false
  }
}
```

| Exit status | Meaning                                                                               |
| ----------- | ------------------------------------------------------------------------------------- |
| 0           | Every path was measured.                                                              |
| 1           | A usage error, or a path could not be read. The reason is on stderr, stdout is empty. |
| 2           | The filesystem cannot report what deleting a path would free. Stdout is empty.        |

`--version` prints the version and `--help` prints usage.

## Library

```js
import { realSize, binaryPath, RealSizeError } from "real-size";

const { paths, total } = await realSize(["node_modules", "ios/Pods"], {
  timeoutMs: 30_000,
});
console.log(`${total.private} bytes reclaimable`);
```

`realSize(paths, { signal, timeoutMs })` runs the binary in a child process
and resolves with the same document `--json` prints. The walk runs in its own
process so that a slow or hung filesystem cannot block the caller: at
`timeoutMs`, or when `signal` aborts, the child is killed with `SIGKILL` and
the promise rejects at once. It rejects with a `RealSizeError` whose `code` is:

- `unsupported`: no prebuilt binary for this platform, or exit status 2.
- `timeout`: `timeoutMs` ran out.
- `failed`: exit status 1 or the binary could not start. `message` holds the
  binary's error.

An aborted `signal` rejects with the signal's reason.

`binaryPath()` returns the absolute path of the prebuilt binary for this
platform, or `null` when the package has none, so you can spawn it yourself,
for example at a lower CPU or I/O priority.

TypeScript types are included. Node `^20.19.4 || >=22.12.0`.

## Output fields

- `allocated`: the bytes the tree's regular files occupy on disk. A file with
  several hard links in the tree counts once.
- `private`: the bytes deleting the tree would free.
- `files`: the regular files counted, each hard-linked file once.
- `exact`: `true` when `private` is the exact answer, `false` when it is a
  lower bound.

Directories, symbolic links and metadata are not counted. Symbolic links are
never followed, and neither are Windows junctions.

A file with more links than the tree holds is shared with a path outside the
tree and contributes nothing to `private`. That rule is exact on every
platform.

`exact` is `false` when some counted file shares blocks with another file
(a clone, reflink or block clone) and the other file could be inside the tree
too. Each copy then reports only its unshared bytes, so deleting both may free
more than the sum. The answer is a lower bound and the true figure lies
between `private` and `allocated`.

## Platforms

| Platform                    | Prebuilt   | Hard links | Clones and shared extents         | `exact`                           |
| --------------------------- | ---------- | ---------- | --------------------------------- | --------------------------------- |
| macOS, APFS                 | arm64, x64 | Exact      | APFS private size per file        | `false` when a file shares blocks |
| macOS, other volumes        |            |            |                                   | Exit status 2                     |
| Linux, ext2/3/4, tmpfs      | x64, arm64 | Exact      | Not possible on these filesystems | Always `true`                     |
| Linux, btrfs, XFS           | x64, arm64 | Exact      | `FIEMAP` shared extent flag       | `false` when an extent is shared  |
| Linux, other filesystems    |            |            |                                   | Exit status 2                     |
| Windows, NTFS, FAT, exFAT   | x64, arm64 | Exact      | Not possible on these volumes     | Always `true`                     |
| Windows, ReFS and Dev Drive | x64, arm64 | Exact      | Per-extent reference counts       | `false` when an extent is shared  |
| Windows, network shares     |            |            |                                   | Exit status 2                     |

The Linux binaries are statically linked, so they run on glibc and musl
distributions alike.

### macOS

The walk uses `getattrlistbulk` and reads each file's
`ATTR_CMNEXT_PRIVATESIZE`, which APFS reports as the bytes the file does not
share with a clone. On a volume that does not report it (HFS+, FAT, SMB) the
exit status is 2. The walk descends into volumes mounted inside the tree.

APFS also reports a private size below the allocation for transparently
compressed files (decmpfs, for example files installed by some system tools),
so a tree that holds them reports `exact: false` and a lower bound even
without clones.

### Linux

The walk uses `fstatat`. `allocated` is `st_blocks` times 512. The filesystem
is identified by its `statfs` `f_type`, again for each volume mounted inside
the tree:

- ext2, ext3, ext4 and tmpfs share blocks only through hard links, so the
  result is exact.
- Btrfs and XFS can share extents between files. Each file is opened
  read-only and mapped with the `FS_IOC_FIEMAP` ioctl; extents flagged
  `FIEMAP_EXTENT_SHARED` do not count as private, and any shared extent sets
  `exact: false`. A file on these filesystems that cannot be opened, or whose
  mapping fails, counts as entirely shared: it adds to `allocated`, nothing to
  `private`, and sets `exact: false`.
- Any other filesystem (overlayfs, NFS, ZFS, bcachefs, FUSE mounts, sysfs
  and so on) exits with status 2, because it may share blocks in ways the
  walk cannot see. Container users meet this first: a Docker container's root
  is overlayfs, while a bind-mounted host folder on ext4 works.

### Windows

The walk uses `FindFirstFileExW` with `\\?\` long paths, so paths longer than
`MAX_PATH` and any Unicode name work. Each file is opened for its attributes
only. `allocated` is the allocation size from `FILE_STANDARD_INFO`, or for a
compressed or sparse NTFS file its compressed size from
`FILE_COMPRESSION_INFO`, whichever is smaller. Hard links use
`nNumberOfLinks` with the volume serial number and the 128-bit `FILE_ID_INFO`
file id, which is unique on ReFS where the 64-bit index is not.

Reparse points whose tag is a name surrogate (symbolic links, junctions,
volume mount points) are not followed, so the walk never leaves the volume.

On a volume that supports block cloning (`FILE_SUPPORTS_BLOCK_REFCOUNTING`:
ReFS, and Dev Drive), each file's extents are read with
[`FSCTL_GET_RETRIEVAL_POINTERS_AND_REFCOUNT`](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/80ac720b-374c-40b7-847c-e57874260b64).
An extent whose
[reference count](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/dd2be29e-4f60-4bb4-b68a-20f2c34ac0d2)
is above 1 is shared and does not count as private, and sets
`exact: false`. ReFS supports this control code from Windows 10 1703 and
Windows Server 2019
([product notes](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fsa/4e3695bd-7574-4f24-a223-b4679c065b63)).
Microsoft documents it in the protocol specifications only, and the count it
reports is the count of the cluster each extent starts at. A file whose
extents cannot be read counts as entirely shared, as on Linux.

Cloud placeholders (OneDrive and other Cloud Files providers) are not read:
the walk opens each file for its attributes only, which Microsoft's
placeholder guidance treats as an attribute-only open that should not recall
data, though no page guarantees it. A file marked
`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN` or `OFFLINE`
contributes its local allocation, which for a dehydrated placeholder is close
to zero. Data Deduplication on Windows Server stores optimized files in a
chunk store that only a garbage collection job reclaims; those files report
their own allocation, so `private` overstates what deleting them frees at
once. Network shares (UNC paths and mapped drives) exit with status 2.

## Speed

Warm-cache wall time of the bare binary against `du -sk` (macOS, Linux) or
`robocopy /L` (Windows), 10 runs each. Method, cold-cache runs and every
runner are in [docs/benchmarks.md](docs/benchmarks.md).

| Machine                        | Filesystem | Tree                                  | real-size | Baseline | Ratio |
| ------------------------------ | ---------- | ------------------------------------- | --------- | -------- | ----- |
| Apple M4 Pro, 14 cores         | APFS       | pnpm `node_modules`, 168,433 files    | 1.01 s    | 876.4 ms | 1.15x |
| Apple M4 Pro                   | APFS       | 30,000 files hard-linked from a store | 184.4 ms  | 123.8 ms | 1.49x |
| Apple M4 Pro                   | APFS       | 117,186 small files, 7 levels         | 754.7 ms  | 551.9 ms | 1.37x |
| GitHub ubuntu-latest, 4 cores  | ext4       | 30,000 hard-linked files              | 91.4 ms   | 123.8 ms | 0.74x |
| GitHub ubuntu-latest           | ext4       | 117,186 small files                   | 352.0 ms  | 594.5 ms | 0.59x |
| GitHub ubuntu-latest           | btrfs      | 117,186 small files                   | 1.47 s    | 619.8 ms | 2.37x |
| GitHub ubuntu-latest           | XFS        | 117,186 small files                   | 747.1 ms  | 521.4 ms | 1.43x |
| GitHub windows-latest, 4 cores | NTFS       | 30,000 hard-linked files              | 337.8 ms  | 135.1 ms | 2.50x |
| GitHub windows-latest          | NTFS       | 117,186 small files                   | 1.66 s    | 690.4 ms | 2.40x |

The extra time is the per-file question `du` does not ask: the private size
on APFS, the extent map on btrfs and XFS, and the link count on Windows.

## Limitations

- Each path is measured on its own and the total is their sum. Two paths that
  hard-link or clone each other's files each report those bytes as shared, so
  the total can be lower than what deleting both would free.
- The macOS and Linux walks stop at 256 nested directories and the Windows
  walk at 256 as well; deeper trees exit with status 1.
- A path that cannot be read, such as a directory without read permission,
  exits with status 1 rather than reporting a partial size.
- Files changed during the walk may be counted in either state.
- The plain output's sizes are rounded; use `--json` for bytes.
- On Linux, filesystems other than ext2/3/4, tmpfs, btrfs and XFS exit with
  status 2 even where an answer could be given, for example ZFS without
  block cloning.
- On Windows the command is `real-size-windows`, not `real-size`, in cmd.exe
  and PowerShell (see [Install](#install)). Yarn classic's Windows launchers
  are not guaranteed.
- On macOS and Linux, `real-size-windows` is installed too; run there, it
  prints that the command is `real-size` and exits with status 2.
- On Windows, files compressed by the Windows Overlay Filter (`compact
/exe`) report their main stream's allocation.

## Agent skill

The package includes a skill at [`skill/SKILL.md`](skill/SKILL.md) that tells
coding agents to use `real-size` rather than `du` when the question is how
much space deleting something would free. Install it with:

```bash
npx skills add appandflow/real-size
```

Or copy the `skill` folder into your agent's skills directory under the name
`real-size`, for example `~/.claude/skills/real-size/SKILL.md` for Claude
Code. The installed npm package also contains it at
`node_modules/real-size/skill/SKILL.md`.

## Development

Building needs a C11 compiler: `cc` on macOS and Linux (a static glibc for the
Linux build), MSVC `cl` on Windows.

```sh
npm ci --ignore-scripts
npm run build
npm run typecheck
npm run format:check
npm test
```

CI builds and tests every platform, then assembles the release tarball with
`npm run pack:release`. See [RELEASING.md](RELEASING.md).

## License

MIT
