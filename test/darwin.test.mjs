import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { linkSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { cli, MiB, measure, near, scratch, write } from "./helpers.mjs";

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
    const root = scratch(t);
    const image = join(root, "hfs.dmg");
    const mount = join(root, "mount");
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
      execFileSync("hdiutil", [
        "attach",
        "-quiet",
        "-nobrowse",
        "-mountpoint",
        mount,
        image,
      ]);
    } catch (error) {
      t.skip(`could not attach an HFS+ image: ${error.message}`);
      return;
    }
    try {
      write(join(mount, "tree", "file"), 10);
      const run = cli(["--json", join(mount, "tree")]);
      assert.equal(run.status, 2, run.stderr);
      assert.equal(run.stdout, "");
      assert.match(run.stderr, /cannot report/);
    } finally {
      execFileSync("hdiutil", ["detach", "-quiet", "-force", mount]);
    }
  },
);
