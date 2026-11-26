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
virtual machines and vary most between runs. These numbers were taken before
the Linux and Windows walk used several threads; see Threads on CI runners
below for the current walk.

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

### Threads on CI runners

The `bench` job of `.github/workflows/ci.yml`, run by hand with `bench=full`,
times the walk on 1, 2, 4 and 8 threads (`REAL_SIZE_TEST_THREADS`) against
the 0.1.1 binary from npm and the platform baseline: 5 warm runs after one
warm-up, then 5 cold runs on Linux and macOS, each after dropping the caches.
The generated trees are a `node_modules`-like tree of 160,023 files and one
of 1,000,013 files (not on ReFS), the hard-link trees, the deep tree, and
100,000 files in one directory. btrfs and XFS are 16 GB loopback images on
the runner's disk; ReFS is a 16 GB virtual disk. Each cell is the warm mean,
then the cold mean. Every thread count printed the same `--json` output as
0.1.1 on every tree.

These runners have 3 or 4 cores, so the numbers cannot show how the walk
scales past 4 threads on a larger machine, or on a spinning disk or a
network-backed volume.

#### macos-15-intel (4 cores), APFS

| Tree                          | 0.1.1               | 1 thread            | 2 threads           | 4 threads           | 8 threads           | du -sk              |
| ----------------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- |
| node_modules, 160,023 files   | 2.61 s / 2.54 s     | 2.61 s / 2.59 s     | 2.11 s / 1.98 s     | 1.58 s / 1.60 s     | 1.32 s / 1.54 s     | 1.63 s / 1.79 s     |
| node_modules, 1,000,013 files | 54.09 s / 44.47 s   | 43.49 s / 38.28 s   | 30.73 s / 28.17 s   | 25.30 s / 23.23 s   | 19.90 s / 19.57 s   | 33.20 s / 30.05 s   |
| hard links                    | 550.0 ms / 786.7 ms | 659.5 ms / 915.3 ms | 499.6 ms / 650.2 ms | 378.9 ms / 516.4 ms | 327.7 ms / 446.0 ms | 446.7 ms / 524.4 ms |
| hard links and store          | 1.33 s / 1.44 s     | 1.29 s / 1.49 s     | 844.5 ms / 1.03 s   | 728.1 ms / 957.1 ms | 583.6 ms / 751.5 ms | 767.0 ms / 1.02 s   |
| deep tree                     | 2.25 s / 1.91 s     | 2.17 s / 1.88 s     | 1.74 s / 1.38 s     | 1.32 s / 1.22 s     | 968.1 ms / 1.27 s   | 1.39 s / 1.50 s     |
| wide directory                | 1.43 s / 1.48 s     | 1.42 s / 1.58 s     | 1.45 s / 1.44 s     | 1.42 s / 1.47 s     | 1.37 s / 1.48 s     | 608.8 ms / 578.4 ms |

#### macos-latest, M1 (3 virtual cores), APFS

| Tree                          | 0.1.1               | 1 thread            | 2 threads           | 4 threads           | 8 threads           | du -sk              |
| ----------------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- |
| node_modules, 160,023 files   | 3.19 s / 3.41 s     | 3.04 s / 2.73 s     | 2.06 s / 2.25 s     | 1.60 s / 2.12 s     | 1.26 s / 1.70 s     | 1.59 s / 1.99 s     |
| node_modules, 1,000,013 files | 24.39 s / 27.26 s   | 23.45 s / 27.05 s   | 16.69 s / 16.41 s   | 12.14 s / 13.40 s   | 11.36 s / 12.02 s   | 20.36 s / 19.46 s   |
| hard links                    | 462.7 ms / 428.3 ms | 445.8 ms / 500.3 ms | 297.3 ms / 345.0 ms | 240.1 ms / 331.6 ms | 250.8 ms / 344.1 ms | 330.5 ms / 360.1 ms |
| hard links and store          | 813.2 ms / 660.4 ms | 773.9 ms / 741.7 ms | 567.4 ms / 534.3 ms | 417.4 ms / 605.9 ms | 418.9 ms / 441.8 ms | 449.3 ms / 404.0 ms |
| deep tree                     | 2.48 s / 2.82 s     | 1.78 s / 2.14 s     | 1.56 s / 2.13 s     | 1.00 s / 1.44 s     | 1.23 s / 1.49 s     | 1.58 s / 1.64 s     |
| wide directory                | 1.48 s / 2.20 s     | 1.25 s / 1.64 s     | 1.36 s / 1.58 s     | 1.33 s / 1.67 s     | 1.32 s / 1.60 s     | 941.6 ms / 1.26 s   |

#### ubuntu-24.04-arm (4 cores), btrfs

| Tree                          | 0.1.1               | 1 thread            | 2 threads           | 4 threads           | 8 threads           | du -sk              |
| ----------------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- |
| node_modules, 160,023 files   | 1.07 s / 2.79 s     | 1.06 s / 2.86 s     | 669.3 ms / 2.03 s   | 394.0 ms / 1.59 s   | 383.8 ms / 1.46 s   | 343.6 ms / 2.01 s   |
| node_modules, 1,000,013 files | 6.78 s / 17.72 s    | 6.68 s / 18.16 s    | 4.20 s / 12.80 s    | 2.40 s / 9.90 s     | 2.39 s / 9.19 s     | 2.15 s / 13.05 s    |
| hard links                    | 49.8 ms / 397.8 ms  | 52.3 ms / 359.8 ms  | 38.9 ms / 282.3 ms  | 30.8 ms / 239.5 ms  | 29.9 ms / 210.0 ms  | 54.3 ms / 378.0 ms  |
| hard links and store          | 251.8 ms / 691.8 ms | 262.1 ms / 691.3 ms | 224.3 ms / 525.1 ms | 209.6 ms / 471.4 ms | 208.4 ms / 446.8 ms | 94.8 ms / 539.4 ms  |
| deep tree                     | 732.1 ms / 1.47 s   | 739.3 ms / 1.59 s   | 484.5 ms / 1.16 s   | 279.6 ms / 879.3 ms | 282.9 ms / 815.7 ms | 251.0 ms / 864.3 ms |
| wide directory                | 527.3 ms / 1.14 s   | 531.0 ms / 1.12 s   | 531.6 ms / 1.12 s   | 530.0 ms / 1.13 s   | 529.8 ms / 1.12 s   | 115.1 ms / 745.7 ms |

#### ubuntu-24.04-arm (4 cores), ext4

| Tree                          | 0.1.1               | 1 thread           | 2 threads           | 4 threads          | 8 threads           | du -sk              |
| ----------------------------- | ------------------- | ------------------ | ------------------- | ------------------ | ------------------- | ------------------- |
| node_modules, 160,023 files   | 342.8 ms / 3.30 s   | 333.2 ms / 3.20 s  | 187.5 ms / 1.79 s   | 102.8 ms / 1.03 s  | 102.0 ms / 1.01 s   | 369.3 ms / 3.28 s   |
| node_modules, 1,000,013 files | 1.87 s / 20.74 s    | 1.81 s / 20.35 s   | 1.05 s / 11.10 s    | 574.4 ms / 7.83 s  | 569.7 ms / 7.91 s   | 2.18 s / 19.95 s    |
| hard links                    | 50.4 ms / 549.4 ms  | 51.5 ms / 530.1 ms | 36.4 ms / 299.3 ms  | 25.9 ms / 170.1 ms | 25.0 ms / 109.1 ms  | 56.9 ms / 543.9 ms  |
| hard links and store          | 92.7 ms / 628.6 ms  | 93.9 ms / 630.0 ms | 59.2 ms / 343.5 ms  | 39.6 ms / 193.7 ms | 38.4 ms / 128.4 ms  | 100.0 ms / 626.9 ms |
| deep tree                     | 208.3 ms / 2.43 s   | 212.1 ms / 2.36 s  | 127.4 ms / 1.33 s   | 69.4 ms / 777.8 ms | 68.6 ms / 783.5 ms  | 239.5 ms / 2.43 s   |
| wide directory                | 101.2 ms / 551.9 ms | 99.4 ms / 532.3 ms | 100.4 ms / 529.3 ms | 99.8 ms / 529.9 ms | 100.9 ms / 527.0 ms | 182.3 ms / 578.6 ms |

#### ubuntu-24.04-arm (4 cores), XFS

| Tree                          | 0.1.1               | 1 thread            | 2 threads           | 4 threads           | 8 threads           | du -sk              |
| ----------------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- | ------------------- |
| node_modules, 160,023 files   | 530.6 ms / 1.49 s   | 521.4 ms / 1.49 s   | 325.7 ms / 1.21 s   | 191.4 ms / 1.04 s   | 195.5 ms / 978.1 ms | 294.0 ms / 1.25 s   |
| node_modules, 1,000,013 files | 3.22 s / 14.07 s    | 3.16 s / 14.03 s    | 1.98 s / 10.07 s    | 1.10 s / 8.85 s     | 1.07 s / 8.63 s     | 1.80 s / 13.25 s    |
| hard links                    | 46.8 ms / 389.8 ms  | 47.8 ms / 392.1 ms  | 36.7 ms / 289.5 ms  | 29.7 ms / 258.9 ms  | 29.9 ms / 255.3 ms  | 50.3 ms / 417.1 ms  |
| hard links and store          | 143.7 ms / 610.6 ms | 145.1 ms / 580.5 ms | 119.7 ms / 421.4 ms | 105.8 ms / 366.0 ms | 105.0 ms / 353.3 ms | 88.0 ms / 547.4 ms  |
| deep tree                     | 355.4 ms / 1.08 s   | 361.6 ms / 1.20 s   | 232.6 ms / 844.0 ms | 128.8 ms / 747.1 ms | 127.8 ms / 718.8 ms | 199.9 ms / 1.04 s   |
| wide directory                | 247.6 ms / 1.03 s   | 250.0 ms / 993.9 ms | 245.9 ms / 1.01 s   | 249.7 ms / 1.00 s   | 246.4 ms / 999.9 ms | 107.6 ms / 844.2 ms |

#### ubuntu-latest, x64 (4 cores), btrfs

| Tree                          | 0.1.1              | 1 thread           | 2 threads           | 4 threads           | 8 threads           | du -sk            |
| ----------------------------- | ------------------ | ------------------ | ------------------- | ------------------- | ------------------- | ----------------- |
| node_modules, 160,023 files   | 1.74 s / 5.60 s    | 1.72 s / 6.40 s    | 899.4 ms / 4.20 s   | 570.9 ms / 3.13 s   | 562.6 ms / 2.62 s   | 753.2 ms / 4.59 s |
| node_modules, 1,000,013 files | 10.89 s / 37.75 s  | 10.73 s / 41.48 s  | 5.55 s / 28.55 s    | 3.50 s / 24.13 s    | 3.50 s / 20.24 s    | 4.91 s / 27.68 s  |
| hard links                    | 79.9 ms / 968.5 ms | 81.2 ms / 796.6 ms | 48.6 ms / 503.0 ms  | 37.1 ms / 496.8 ms  | 34.8 ms / 427.3 ms  | 108.3 ms / 1.04 s |
| hard links and store          | 390.2 ms / 1.59 s  | 386.1 ms / 1.34 s  | 324.0 ms / 958.3 ms | 301.5 ms / 774.0 ms | 298.3 ms / 879.3 ms | 178.6 ms / 1.06 s |
| deep tree                     | 1.21 s / 2.63 s    | 1.22 s / 3.24 s    | 633.3 ms / 1.92 s   | 416.6 ms / 1.32 s   | 404.8 ms / 1.35 s   | 563.6 ms / 1.88 s |
| wide directory                | 889.2 ms / 2.86 s  | 887.7 ms / 2.37 s  | 885.6 ms / 2.61 s   | 887.7 ms / 2.13 s   | 892.4 ms / 2.05 s   | 225.4 ms / 1.74 s |

#### ubuntu-latest, x64 (4 cores), ext4

| Tree                          | 0.1.1              | 1 thread           | 2 threads          | 4 threads          | 8 threads          | du -sk              |
| ----------------------------- | ------------------ | ------------------ | ------------------ | ------------------ | ------------------ | ------------------- |
| node_modules, 160,023 files   | 290.7 ms / 2.39 s  | 282.9 ms / 2.36 s  | 161.1 ms / 1.29 s  | 98.4 ms / 711.4 ms | 99.8 ms / 446.2 ms | 414.1 ms / 2.60 s   |
| node_modules, 1,000,013 files | 1.58 s / 16.19 s   | 1.57 s / 16.86 s   | 911.8 ms / 8.99 s  | 570.5 ms / 4.79 s  | 568.9 ms / 2.84 s  | 2.53 s / 18.28 s    |
| hard links                    | 46.4 ms / 454.0 ms | 47.5 ms / 452.0 ms | 34.7 ms / 247.7 ms | 25.5 ms / 136.0 ms | 25.5 ms / 85.5 ms  | 62.0 ms / 560.7 ms  |
| hard links and store          | 85.8 ms / 517.9 ms | 88.3 ms / 511.5 ms | 58.2 ms / 278.4 ms | 41.0 ms / 151.3 ms | 41.3 ms / 104.4 ms | 108.0 ms / 616.5 ms |
| deep tree                     | 176.6 ms / 1.82 s  | 183.9 ms / 1.85 s  | 111.7 ms / 1.00 s  | 68.8 ms / 522.8 ms | 68.8 ms / 299.3 ms | 292.6 ms / 2.10 s   |
| wide directory                | 98.4 ms / 453.0 ms | 98.6 ms / 450.6 ms | 98.9 ms / 456.4 ms | 98.4 ms / 453.8 ms | 98.4 ms / 450.1 ms | 134.2 ms / 568.3 ms |

#### ubuntu-latest, x64 (4 cores), XFS

| Tree                          | 0.1.1              | 1 thread           | 2 threads           | 4 threads           | 8 threads           | du -sk              |
| ----------------------------- | ------------------ | ------------------ | ------------------- | ------------------- | ------------------- | ------------------- |
| node_modules, 160,023 files   | 1.03 s / 2.66 s    | 984.5 ms / 3.04 s  | 512.4 ms / 2.13 s   | 317.1 ms / 1.50 s   | 322.4 ms / 1.42 s   | 651.2 ms / 2.30 s   |
| node_modules, 1,000,013 files | 6.10 s / 25.29 s   | 5.91 s / 26.71 s   | 3.13 s / 16.42 s    | 1.86 s / 13.21 s    | 1.85 s / 11.56 s    | 4.33 s / 25.33 s    |
| hard links                    | 70.7 ms / 794.1 ms | 77.8 ms / 836.2 ms | 45.6 ms / 426.6 ms  | 37.6 ms / 396.5 ms  | 34.1 ms / 389.3 ms  | 97.8 ms / 770.9 ms  |
| hard links and store          | 232.1 ms / 1.13 s  | 232.9 ms / 1.03 s  | 187.7 ms / 726.6 ms | 171.5 ms / 618.7 ms | 173.5 ms / 620.7 ms | 161.7 ms / 1.08 s   |
| deep tree                     | 676.2 ms / 1.71 s  | 680.7 ms / 2.26 s  | 354.0 ms / 1.14 s   | 217.9 ms / 969.0 ms | 219.7 ms / 945.6 ms | 498.3 ms / 1.68 s   |
| wide directory                | 470.2 ms / 1.25 s  | 467.6 ms / 1.32 s  | 471.3 ms / 1.27 s   | 469.3 ms / 1.38 s   | 473.0 ms / 1.30 s   | 203.7 ms / 962.5 ms |

#### windows-11-arm (4 cores), NTFS

| Tree                          | 0.1.1    | 1 thread | 2 threads | 4 threads | 8 threads | robocopy /L |
| ----------------------------- | -------- | -------- | --------- | --------- | --------- | ----------- |
| node_modules, 160,023 files   | 3.50 s   | 2.91 s   | 2.02 s    | 1.19 s    | 1.14 s    | 1.46 s      |
| node_modules, 1,000,013 files | 22.98 s  | 17.88 s  | 13.55 s   | 7.66 s    | 12.48 s   | 10.25 s     |
| hard links                    | 310.0 ms | 289.4 ms | 236.8 ms  | 156.8 ms  | 154.4 ms  | 169.4 ms    |
| hard links and store          | 519.5 ms | 516.6 ms | 400.8 ms  | 254.8 ms  | 255.7 ms  | 194.2 ms    |
| deep tree                     | 1.58 s   | 1.46 s   | 1.30 s    | 752.6 ms  | 726.3 ms  | 1.49 s      |
| wide directory                | 555.3 ms | 549.8 ms | 554.2 ms  | 554.8 ms  | 557.8 ms  | 83.3 ms     |

#### windows-11-arm (4 cores), ReFS

| Tree                        | 0.1.1    | 1 thread | 2 threads | 4 threads | 8 threads | robocopy /L |
| --------------------------- | -------- | -------- | --------- | --------- | --------- | ----------- |
| node_modules, 160,023 files | 5.09 s   | 4.56 s   | 3.13 s    | 1.76 s    | 1.70 s    | 1.57 s      |
| hard links                  | 759.7 ms | 739.5 ms | 513.4 ms  | 293.9 ms  | 286.7 ms  | 383.4 ms    |
| hard links and store        | 1.43 s   | 1.40 s   | 964.8 ms  | 552.4 ms  | 534.9 ms  | 410.0 ms    |
| deep tree                   | 3.16 s   | 3.09 s   | 2.23 s    | 1.18 s    | 1.20 s    | 1.47 s      |
| wide directory              | 2.14 s   | 2.15 s   | 2.14 s    | 2.16 s    | 2.15 s    | 353.0 ms    |

#### windows-latest, x64 (4 cores), NTFS

| Tree                          | 0.1.1    | 1 thread | 2 threads | 4 threads | 8 threads | robocopy /L |
| ----------------------------- | -------- | -------- | --------- | --------- | --------- | ----------- |
| node_modules, 160,023 files   | 1.95 s   | 1.86 s   | 1.07 s    | 690.4 ms  | 665.2 ms  | 883.5 ms    |
| node_modules, 1,000,013 files | 10.64 s  | 10.67 s  | 6.20 s    | 4.31 s    | 4.43 s    | 4.94 s      |
| hard links                    | 252.6 ms | 232.7 ms | 159.2 ms  | 144.8 ms  | 138.8 ms  | 89.2 ms     |
| hard links and store          | 450.8 ms | 427.2 ms | 280.0 ms  | 239.1 ms  | 249.9 ms  | 104.7 ms    |
| deep tree                     | 1.04 s   | 969.5 ms | 634.1 ms  | 504.0 ms  | 487.5 ms  | 411.4 ms    |
| wide directory                | 482.4 ms | 493.6 ms | 471.4 ms  | 476.0 ms  | 471.5 ms  | 320.6 ms    |

#### windows-latest, x64 (4 cores), ReFS

| Tree                        | 0.1.1  | 1 thread | 2 threads | 4 threads | 8 threads | robocopy /L |
| --------------------------- | ------ | -------- | --------- | --------- | --------- | ----------- |
| node_modules, 160,023 files | 6.11 s | 6.10 s   | 3.36 s    | 2.62 s    | 2.56 s    | 1.80 s      |
| hard links                  | 1.20 s | 1.22 s   | 611.8 ms  | 474.0 ms  | 469.5 ms  | 157.1 ms    |
| hard links and store        | 2.20 s | 2.21 s   | 1.14 s    | 888.0 ms  | 890.3 ms  | 495.4 ms    |
| deep tree                   | 4.49 s | 4.55 s   | 2.58 s    | 2.22 s    | 2.19 s    | 1.45 s      |
| wide directory              | 3.62 s | 3.73 s   | 3.66 s    | 3.60 s    | 3.74 s    | 604.7 ms    |

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
- **Parallelism, Linux and Windows.** The same pool lists directories on one
  thread per CPU, at most 4. On the 4-core runners, 4 threads were about
  1.3x to 3x faster than one, warm, on every tree with many directories, on
  ext4, btrfs, XFS, NTFS and ReFS, and 8 threads were no faster than 4. Cold,
  on Linux, 4 threads were faster than 0.1.1 on every tree with many
  directories, and 8 threads were faster still on ext4, which suggests more
  threads could help a cold walk on a machine with more cores; that is not
  measured, so the cap stays 4. One thread was up to 1.3x slower than 0.1.1
  cold on btrfs and XFS. A single directory of
  100,000 files is listed by one thread, so it does not change. On Linux,
  subdirectories are handed out in listing order: handing them out last
  first made one thread about 2x slower than 0.1.1 cold on XFS.
