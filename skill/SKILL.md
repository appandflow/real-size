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
- `exact`: `false` means `private` is a lower bound: some file shares blocks with another file, possibly one inside the same tree, or some entries could not be read. Deleting the whole tree may free more. Say "at least".
- `skipped`: entries that could not be read (no permission, an I/O error, nesting deeper than 256 directories) and were left out of every number.

Exit status 1 with a result on stdout means some entries were skipped: stderr names each one with its reason (the first ten, then a count). Still use the result: report `private` as "at least", give the number of entries that could not be read and the paths stderr names, and suggest rerunning with the needed permissions if the user wants the full figure. Exit status 1 with nothing on stdout is a usage error or a path that cannot be read at all, with the reason on stderr.

Exit status 2 means the filesystem the path is on cannot report this (for example NFS, overlayfs, a network share, or a non-APFS volume on macOS). Then say real-size cannot tell on this filesystem, fall back to `du -sh`, and present that number as an upper bound. A volume like that mounted inside the folder does not cause exit 2: it is one of the skipped entries of exit status 1, named on stderr as "another filesystem that cannot report private size", and nothing in it is counted.

Entries deleted while the walk runs are left out without a message; they free nothing.

Example, a pnpm worktree's `node_modules` on macOS:

```
$ du -sh node_modules            4.4G   node_modules
$ npx real-size node_modules     node_modules: at least 90.3 MB private, 4.8 GB allocated, 168433 files
```

Deleting it gets back about 90 MB, not 4.4 GB. Symbolic links and Windows junctions are never followed. On Windows, NTFS hard links are exact; ReFS and Dev Drive block clones are read from the volume's per-extent reference counts and give `exact: false`, and network shares exit 2.
