import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  realpathSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { randomBytes } from "node:crypto";
import { binaryPath } from "../index.mjs";

export const MiB = 1024 * 1024;

export const binary = binaryPath();
assert.ok(binary, "run npm run build first");

/** A fresh directory under `parent` (the system temporary directory by default), removed after the test. */
export function scratch(t, parent = tmpdir()) {
  const directory = realpathSync(mkdtempSync(join(parent, "real-size-")));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  return directory;
}

/** Writes `size` random bytes, so no filesystem can compress or deduplicate them. */
export function write(file, size = MiB) {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, randomBytes(size));
}

export function cli(args, options = {}) {
  return spawnSync(binary, args, {
    encoding: "utf8",
    timeout: 60000,
    ...options,
  });
}

export function measure(path) {
  const run = cli(["--json", path]);
  assert.equal(run.status, 0, run.stderr);
  return JSON.parse(run.stdout).paths[0];
}

/** Asserts `actual` is within `tolerance` bytes of `expected`: allocation rounds each file up to whole blocks. */
export function near(actual, expected, tolerance = 64 * 1024) {
  assert.ok(
    Math.abs(actual - expected) <= tolerance,
    `expected about ${expected} bytes, got ${actual}`,
  );
}

function attached(mount) {
  return execFileSync("hdiutil", ["info"], { encoding: "utf8" })
    .split("\n")
    .some((line) => line.endsWith(`\t${mount}`));
}

function detach(mount, device) {
  for (let attempt = 0; attempt < 5 && attached(mount); attempt++) {
    if (attempt > 0)
      Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1000);
    spawnSync("hdiutil", ["detach", "-force", device ?? mount], {
      stdio: "ignore",
    });
  }
  if (attached(mount))
    throw new Error(`could not detach the image at ${mount}`);
}

/**
 * A fresh directory with an HFS+ image file in it, for macOS tests. `attach(mount)` attaches the image at `mount`
 * and returns false when it cannot. After the test one hook detaches the image, retrying, and only then removes the
 * directory, so a removal never runs into a volume that is still attached.
 */
export function diskImage(t) {
  const root = realpathSync(mkdtempSync(join(tmpdir(), "real-size-")));
  const image = join(root, "hfs.dmg");
  let mounted;
  let device;
  t.after(() => {
    if (mounted) detach(mounted, device);
    rmSync(root, { recursive: true, force: true });
  });
  return {
    root,
    attach(mount) {
      try {
        execFileSync("hdiutil", [
          "create",
          "-quiet",
          "-size",
          "8m",
          "-fs",
          "HFS+",
          "-volname",
          "real-size",
          image,
        ]);
        mkdirSync(mount, { recursive: true });
        mounted = mount;
        const output = execFileSync(
          "hdiutil",
          ["attach", "-nobrowse", "-mountpoint", mount, image],
          { encoding: "utf8" },
        );
        device = output.match(/^\/dev\/disk\d+/m)?.[0];
        return true;
      } catch {
        return false;
      }
    },
  };
}
