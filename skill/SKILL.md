---
name: real-size
description: Use real-size to measure how much disk space a folder really uses or how much deleting it would free. Use it when deciding what to delete to free space, when sizing node_modules, build outputs, caches, worktrees or clones on macOS (APFS clones), Linux (hard links, btrfs and XFS reflinks) or Windows (NTFS hard links, ReFS block clones), and any time `du` or `du -sh` is about to be used to answer "how much will I get back".
---

# real-size

`du` adds up every file's blocks. On a copy-on-write or hard-linked tree that is the wrong answer to "what would deleting this free": an APFS clone, a reflink copy, a ReFS block clone or a pnpm store hard link shares its blocks with a file elsewhere, and deleting it frees nothing. Use `real-size` for that question. `du` stays the right tool for apparent size or for how much a tree occupies regardless of sharing.

```bash
npx real-size <path>...          # one line per path, plus a total
npx real-size --json <path>...   # one JSON document: { paths: [...], total }
```

On Windows in cmd.exe or PowerShell the command is `real-size-windows` (`npx --package real-size real-size-windows <path>`).

Fields:

- `allocated`: bytes the tree's files occupy on disk, each file counted once.
- `private`: bytes deleting the tree would free. Report this one as reclaimable.
- `files`: regular files counted.
- `exact`: `false` means `private` is a lower bound: some file shares blocks with another file, possibly one inside the same tree, so deleting the whole tree may free more. Say "at least".

Exit status 2 means the filesystem cannot report this (for example NFS, overlayfs, a network share, or a non-APFS volume on macOS). Then say real-size cannot tell on this filesystem, fall back to `du -sh`, and present that number as an upper bound. Exit status 1 is a usage or I/O error, with the reason on stderr.

Example, a pnpm worktree's `node_modules` on macOS:

```
$ du -sh node_modules            4.4G   node_modules
$ npx real-size node_modules     node_modules: at least 90.3 MB private, 4.8 GB allocated, 168433 files
```

Deleting it gets back about 90 MB, not 4.4 GB. Symbolic links and Windows junctions are never followed. On Windows, NTFS hard links are exact; ReFS and Dev Drive block clones are read from the volume's per-extent reference counts and give `exact: false`, and network shares exit 2.
