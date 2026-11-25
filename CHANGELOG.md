# Changelog

## 0.1.2

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
