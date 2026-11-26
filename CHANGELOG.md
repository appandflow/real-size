# Changelog

## 0.3.0

- On Linux and Windows the walk lists directories on several threads, as on
  macOS: one per CPU, at most 4. On 4-core GitHub-hosted runners, 4 threads
  were about 1.3x to 3x faster than 0.1.1 and 0.2.0 on trees of many
  directories, warm, on ext4, btrfs, XFS, NTFS and ReFS, and faster cold on
  ext4, btrfs and XFS. A single directory of many files walks at the same
  speed as before.
- On Linux and Windows, a tree that changes during the walk can give a
  different result than 0.2.0 did, as on macOS since 0.2.0. Directories are
  opened by path, so a parent directory replaced by a symbolic link during
  the walk is followed. When a parent directory is renamed during the walk,
  the directories queued below it are left out without a message, like
  deleted entries. When more than ten entries are skipped, which ten are
  named on stderr can vary between runs; the counts and the result do not.

## 0.2.0

- A volume mounted inside the measured tree whose filesystem cannot report
  private sizes no longer fails the whole measurement with exit status 2. It
  is skipped as one entry, named on stderr with its path and the reason
  "another filesystem that cannot report private size", and nothing in it is
  counted. The result is printed, `exact` is `false` and the exit status is 1.
  On macOS 0.1.1 walked until it met a file on such a volume, for example the
  FSKit volume under `~/Library/Developer/CoreDevice/DeviceFS`, and then
  stopped with no result, so `real-size ~` could run for minutes and print
  only an error. Linux had the same rule for nested mounts such as FUSE or
  overlayfs; it now skips them too. On macOS `/dev` (devfs) is now one of
  these skipped volumes, so `real-size /` exits with status 1.
- On macOS a measured path that is not on APFS exits with status 2 at once,
  before any walk. A directory on such a volume that holds no regular files,
  which 0.1.1 reported as 0 bytes with exit status 0, now exits with status 2
  as well.
- On macOS the walk lists directories on several threads: one per CPU, at
  most 4. Linux and Windows stay single-threaded. On a 14-core Apple silicon
  Mac this made a warm `node_modules` about 2x faster and trees of millions
  of files about 4x faster.
- On macOS, a tree that changes during the walk can give a different result
  than 0.1.1 did. Directories are opened by path, so a parent directory
  replaced by a symbolic link during the walk is followed. When a parent
  directory is renamed during the walk, the directories queued below it are
  left out without a message, like deleted entries. Also, when more than ten
  entries are skipped, which ten are named on stderr can vary between runs,
  because the walk order does; the counts and the result do not.

## 0.1.1

- An entry that cannot be read, such as a directory without read permission,
  no longer fails the whole measurement. It is skipped and named on stderr
  with its own path and the reason (the first ten, then a count), and the
  result is still printed. The new `skipped` field counts these entries in
  each path and in the total, `exact` is `false` when it is above 0, and the
  plain output adds "(N entries could not be read)". The exit status is then
  1 with the result on stdout, as with `du`.
- An entry deleted while the walk runs is left out without a message. 0.1.0
  stopped with an error that named the measured path, for example
  `real-size: .: No such file or directory`.
- A directory nested deeper than 256 levels is skipped and counted in
  `skipped` instead of failing the measurement.
- `realSize()` resolves with the result when entries were skipped, instead of
  rejecting. It still rejects when a path itself cannot be read.
- On a terminal, a walk that takes more than two seconds shows a progress
  line on stderr. `--no-progress` turns it off.
- On Linux, entries that the directory listing reports as symbolic links,
  sockets, FIFOs or devices are skipped without an `fstatat` call. Results do
  not change. A tree of 30,000 symbolic links walks about four times faster.

## 0.1.0

- First release.
