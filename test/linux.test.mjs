import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { linkSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { cli, MiB, measure, near, scratch, write } from "./helpers.mjs";

const linux = process.platform === "linux";

test(
  "Linux: hard links count once and free bytes only when every link is inside",
  { skip: !linux },
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

test("Linux: a filesystem that cannot tell exits 2", { skip: !linux }, () => {
  const run = cli(["--json", "/sys/kernel"]);
  assert.equal(run.status, 2, run.stderr);
  assert.equal(run.stdout, "");
  assert.match(run.stderr, /cannot report/);
});

const reflinkRoots = (process.env.REAL_SIZE_REFLINK_DIRS ?? "")
  .split(":")
  .filter(Boolean);
if (linux && reflinkRoots.length === 0)
  test(
    "Linux reflinks",
    { skip: "set REAL_SIZE_REFLINK_DIRS to writable btrfs or XFS mounts" },
    () => {},
  );

for (const mount of linux ? reflinkRoots : []) {
  test(`Linux reflinks on ${mount}: shared extents are not private`, (t) => {
    const root = scratch(t, mount);
    write(join(root, "outside", "source"), 2 * MiB);
    write(join(root, "tree", "regular"));
    execFileSync("cp", [
      "--reflink=always",
      join(root, "outside", "source"),
      join(root, "tree", "clone"),
    ]);
    execFileSync("sync");
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 2);
    near(size.allocated, 3 * MiB);
    near(size.private, MiB);
    assert.equal(size.exact, false);
  });

  test(`Linux reflinks on ${mount}: hard-linked files are mapped once every link is seen`, (t) => {
    const root = scratch(t, mount);
    write(join(root, "outside", "source"), 2 * MiB);
    write(join(root, "tree", "own"));
    execFileSync("cp", [
      "--reflink=always",
      join(root, "outside", "source"),
      join(root, "tree", "clone"),
    ]);
    linkSync(join(root, "tree", "own"), join(root, "tree", "own-link"));
    linkSync(join(root, "tree", "clone"), join(root, "tree", "clone-link"));
    write(join(root, "outside", "linked"));
    linkSync(join(root, "outside", "linked"), join(root, "tree", "linked-out"));
    execFileSync("sync");
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 3);
    near(size.allocated, 4 * MiB);
    near(size.private, MiB);
    assert.equal(size.exact, false);
  });

  test(`Linux reflinks on ${mount}: a tree without shared extents is exact`, (t) => {
    const root = scratch(t, mount);
    write(join(root, "tree", "a"));
    write(join(root, "tree", "sub", "b"));
    execFileSync("sync");
    const size = measure(join(root, "tree"));
    near(size.allocated, 2 * MiB);
    assert.equal(size.private, size.allocated);
    assert.equal(size.exact, true);
  });
}
