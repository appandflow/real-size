import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { test } from "node:test";

test(
  "the packed tarball ships the skill, the launcher and the API",
  { skip: !process.env.npm_execpath && "run through npm test" },
  () => {
    const packed = spawnSync(
      process.execPath,
      [
        process.env.npm_execpath ?? "",
        "pack",
        "--dry-run",
        "--json",
        "--ignore-scripts",
      ],
      { encoding: "utf8", timeout: 60000 },
    );
    assert.equal(packed.status, 0, packed.stderr);
    const files = new Map(
      JSON.parse(packed.stdout)[0].files.map(({ path, mode }) => [path, mode]),
    );
    for (const file of [
      "skill/SKILL.md",
      "bin/real-size",
      "index.mjs",
      "index.d.mts",
      "LICENSE",
      "README.md",
    ])
      assert.ok(files.has(file), `missing ${file}`);
    if (process.platform !== "win32")
      assert.equal(files.get("bin/real-size") & 0o111, 0o111);
  },
);

test("the skill's frontmatter has a name and a description", () => {
  const text = readFileSync(
    new URL("../skill/SKILL.md", import.meta.url),
    "utf8",
  );
  const match = /^---\n([\s\S]*?)\n---\n/.exec(text);
  assert.ok(match, "no frontmatter");
  const fields = Object.fromEntries(
    match[1].split("\n").map((line) => {
      const colon = line.indexOf(":");
      return [line.slice(0, colon).trim(), line.slice(colon + 1).trim()];
    }),
  );
  assert.equal(fields.name, "real-size");
  assert.ok(fields.description?.length > 0);
});
