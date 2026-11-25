# Changelog

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

## 0.1.0

- First release.
