import assert from "node:assert/strict";
import { mkdirSync, readFileSync, symlinkSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { cli, measure, MiB, near, scratch, write } from "./helpers.mjs";

const { version } = JSON.parse(
  readFileSync(new URL("../package.json", import.meta.url), "utf8"),
);

test("--version prints the package version", () => {
  const run = cli(["--version"]);
  assert.equal(run.status, 0);
  assert.equal(run.stdout.trim(), version);
});

test("--help prints usage and exits 0", () => {
  const run = cli(["--help"]);
  assert.equal(run.status, 0);
  assert.match(run.stdout, /^usage: real-size/);
});

test("usage errors exit 1 with a message on stderr and nothing on stdout", () => {
  for (const args of [[], ["--bogus", "."]]) {
    const run = cli(args);
    assert.equal(run.status, 1, args.join(" "));
    assert.equal(run.stdout, "");
    assert.match(run.stderr, /usage: real-size/);
  }
});

test("a missing path or a file exits 1 and names the path", (t) => {
  const root = scratch(t);
  write(join(root, "file"), 10);
  for (const path of [join(root, "missing"), join(root, "file")]) {
    const run = cli(["--json", path]);
    assert.equal(run.status, 1);
    assert.equal(run.stdout, "");
    assert.ok(run.stderr.includes(path), run.stderr);
  }
});

test("--json prints one document with every path and a total", (t) => {
  const root = scratch(t);
  write(join(root, "a", "one"));
  write(join(root, "b", "two"));
  write(join(root, "b", "nested", "three"));
  const run = cli(["--json", join(root, "a"), join(root, "b")]);
  assert.equal(run.status, 0, run.stderr);
  const result = JSON.parse(run.stdout);
  assert.deepEqual(Object.keys(result), ["paths", "total"]);
  assert.deepEqual(
    result.paths.map((entry) => Object.keys(entry)),
    [
      ["path", "allocated", "private", "files", "exact"],
      ["path", "allocated", "private", "files", "exact"],
    ],
  );
  assert.deepEqual(
    result.paths.map((entry) => [entry.path, entry.files, entry.exact]),
    [
      [join(root, "a"), 1, true],
      [join(root, "b"), 2, true],
    ],
  );
  near(result.paths[1].allocated, 2 * MiB);
  near(result.paths[1].private, 2 * MiB);
  assert.deepEqual(Object.keys(result.total), [
    "allocated",
    "private",
    "files",
    "exact",
  ]);
  assert.equal(result.total.files, 3);
  assert.equal(
    result.total.allocated,
    result.paths[0].allocated + result.paths[1].allocated,
  );
});

test("plain output prints one line per path and a total", (t) => {
  const root = scratch(t);
  write(join(root, "a", "one"));
  mkdirSync(join(root, "b"));
  const run = cli([join(root, "a"), join(root, "b")]);
  assert.equal(run.status, 0, run.stderr);
  const lines = run.stdout.trim().split(/\r?\n/);
  assert.equal(lines.length, 3);
  assert.match(lines[0], /^.+: 1\.\d MB private, 1\.\d MB allocated, 1 files$/);
  assert.match(lines[1], /: 0 B private, 0 B allocated, 0 files$/);
  assert.match(lines[2], /^total: 1\.\d MB private/);
});

test("symbolic links are not followed, inside the tree or as the path itself", (t) => {
  const root = scratch(t);
  write(join(root, "outside", "big"), 4 * MiB);
  write(join(root, "tree", "small"));
  const type = process.platform === "win32" ? "junction" : "dir";
  symlinkSync(join(root, "outside"), join(root, "tree", "link"), type);
  const size = measure(join(root, "tree"));
  assert.equal(size.files, 1);
  near(size.allocated, MiB);
  const run = cli(["--json", join(root, "tree", "link")]);
  assert.equal(run.status, 1);
  assert.equal(run.stdout, "");
});
