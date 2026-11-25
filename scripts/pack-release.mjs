import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { chmodSync, existsSync } from "node:fs";
import { binaryName, targets } from "./targets.mjs";

const executables = [
  "bin/real-size",
  ...targets.map((target) => `prebuilds/${target}/${binaryName(target)}`),
];
for (const file of executables) {
  assert.ok(existsSync(file), `Missing ${file}`);
  chmodSync(file, 0o755);
}
const packed = spawnSync(
  process.execPath,
  [process.env.npm_execpath, "pack", "--json", "--ignore-scripts"],
  { encoding: "utf8", timeout: 60000 },
);
assert.equal(packed.status, 0, packed.stderr);
const [archive] = JSON.parse(packed.stdout);
const modes = new Map(archive.files.map(({ path, mode }) => [path, mode]));
for (const file of [
  ...executables,
  "skill/SKILL.md",
  "index.mjs",
  "index.d.mts",
])
  assert.ok(modes.has(file), `Tarball missing ${file}`);
for (const file of executables)
  assert.equal(modes.get(file) & 0o111, 0o111, `${file} is not executable`);
console.log(archive.filename);
