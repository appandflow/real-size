import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { linkSync, mkdirSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import {
  cli,
  diskImage,
  MiB,
  measure,
  near,
  scratch,
  write,
} from "./helpers.mjs";

const darwin = process.platform === "darwin";

test(
  "APFS: a clone of a file outside the tree frees nothing",
  { skip: !darwin },
  (t) => {
    const root = scratch(t);
    write(join(root, "outside", "source"), 2 * MiB);
    write(join(root, "tree", "regular"));
    execFileSync("cp", [
      "-c",
      join(root, "outside", "source"),
      join(root, "tree", "clone"),
    ]);
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 2);
    near(size.allocated, 3 * MiB);
    near(size.private, MiB);
    assert.equal(size.exact, false);
  },
);

test(
  "APFS: two clones inside the tree report a lower bound",
  { skip: !darwin },
  (t) => {
    const root = scratch(t);
    write(join(root, "tree", "a"), 2 * MiB);
    execFileSync("cp", [
      "-c",
      join(root, "tree", "a"),
      join(root, "tree", "b"),
    ]);
    const size = measure(join(root, "tree"));
    near(size.allocated, 4 * MiB);
    near(size.private, 0);
    assert.equal(size.exact, false);
  },
);

test(
  "APFS: hard links count once and free bytes only when every link is inside",
  { skip: !darwin },
  (t) => {
    const root = scratch(t);
    write(join(root, "outside", "shared"), 2 * MiB);
    write(join(root, "tree", "own"));
    write(join(root, "tree", "sub", "regular"));
    linkSync(
      join(root, "outside", "shared"),
      join(root, "tree", "shared-link"),
    );
    linkSync(join(root, "tree", "own"), join(root, "tree", "sub", "own-link"));
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 3);
    near(size.allocated, 4 * MiB);
    near(size.private, 2 * MiB);
    assert.equal(size.exact, true);
  },
);

test(
  "macOS: a volume without private sizes exits 2",
  { skip: !darwin },
  (t) => {
    const disk = diskImage(t);
    const mount = join(disk.root, "mount");
    if (!disk.attach(mount)) {
      t.skip("could not attach an HFS+ image");
      return;
    }
    write(join(mount, "tree", "file"), 10);
    mkdirSync(join(mount, "empty"));
    for (const path of [join(mount, "tree"), join(mount, "empty")]) {
      const run = cli(["--json", path]);
      assert.equal(run.status, 2, run.stderr);
      assert.equal(run.stdout, "");
      assert.match(run.stderr, /cannot report/);
    }
  },
);

test(
  "macOS: a volume without private sizes inside the tree is skipped",
  { skip: !darwin },
  (t) => {
    const disk = diskImage(t);
    const tree = join(disk.root, "tree");
    const mount = join(tree, "sub", "mount");
    write(join(tree, "a"));
    write(join(tree, "sub", "b"));
    mkdirSync(mount);
    const without = measure(tree);
    if (!disk.attach(mount)) {
      t.skip("could not attach an HFS+ image");
      return;
    }
    write(join(mount, "file"), 10);
    write(join(mount, "deeper", "file"), 10);
    const run = cli(["--json", tree]);
    assert.equal(run.status, 1, run.stderr);
    assert.equal(
      run.stderr,
      `real-size: ${mount}: another filesystem that cannot report private size\n`,
    );
    const size = JSON.parse(run.stdout).paths[0];
    assert.equal(size.skipped, 1);
    assert.equal(size.exact, false);
    assert.equal(size.files, without.files);
    assert.equal(size.allocated, without.allocated);
    assert.equal(size.private, without.private);
  },
);
