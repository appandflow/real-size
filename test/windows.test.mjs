import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { linkSync, mkdirSync, symlinkSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { cli, MiB, measure, near, scratch, write } from "./helpers.mjs";

const windows = process.platform === "win32";

test(
  "Windows: hard links count once and free bytes only when every link is inside",
  { skip: !windows },
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
  "Windows: junctions and symbolic links are not followed",
  { skip: !windows },
  (t) => {
    const root = scratch(t);
    write(join(root, "outside", "big"), 4 * MiB);
    write(join(root, "tree", "small"));
    symlinkSync(
      join(root, "outside"),
      join(root, "tree", "junction"),
      "junction",
    );
    symlinkSync(
      join(root, "outside"),
      join(root, "tree", "directory-link"),
      "dir",
    );
    symlinkSync(
      join(root, "outside", "big"),
      join(root, "tree", "file-link"),
      "file",
    );
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 1);
    near(size.allocated, MiB);
  },
);

test(
  "Windows: paths longer than MAX_PATH are walked",
  { skip: !windows },
  (t) => {
    const root = scratch(t);
    let deep = join(root, "tree");
    while (deep.length < 400)
      deep = join(deep, "a-long-directory-name-to-pass-max-path");
    write(join(deep, "file"));
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 1);
    near(size.allocated, MiB);
  },
);

test(
  "Windows: non-ASCII names round-trip through the arguments and the JSON",
  { skip: !windows },
  (t) => {
    const root = scratch(t);
    const tree = join(root, "données-日本-🙂");
    write(join(tree, "fichier-é"));
    const size = measure(tree);
    assert.equal(size.path, tree);
    assert.equal(size.files, 1);
  },
);

const refs = process.env.REAL_SIZE_REFS_DIR;
const cloneTool = process.env.REAL_SIZE_CLONE_TOOL;
test(
  "Windows ReFS: a block clone of a file outside the tree frees nothing",
  {
    skip:
      !windows || !refs || !cloneTool
        ? "set REAL_SIZE_REFS_DIR and REAL_SIZE_CLONE_TOOL"
        : false,
  },
  (t) => {
    const root = scratch(t, refs);
    write(join(root, "outside", "source"), 2 * MiB);
    write(join(root, "tree", "regular"));
    execFileSync(cloneTool, [
      join(root, "outside", "source"),
      join(root, "tree", "clone"),
    ]);
    const size = measure(join(root, "tree"));
    assert.equal(size.files, 2);
    near(size.allocated, 3 * MiB);
    near(size.private, MiB);
    assert.equal(size.exact, false);
    const plain = measure(join(root, "outside"));
    near(plain.private, 0);
    mkdirSync(join(root, "fresh"));
    write(join(root, "fresh", "only"));
    const fresh = measure(join(root, "fresh"));
    assert.equal(fresh.private, fresh.allocated);
    assert.equal(fresh.exact, true);
    assert.equal(cli(["--json", root]).status, 0);
  },
);
