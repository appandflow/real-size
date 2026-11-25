import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
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
