import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  realpathSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const windows = process.platform === "win32";
const root = realpathSync(mkdtempSync(join(tmpdir(), "real-size-package-")));

function npm(args, cwd) {
  const result = spawnSync(
    process.execPath,
    [process.env.npm_execpath, ...args],
    { cwd, encoding: "utf8", timeout: 120000 },
  );
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
}

function check(run, label) {
  assert.equal(run.status, 0, `${label}: ${run.stderr}`);
  const result = JSON.parse(run.stdout);
  assert.equal(result.paths[0].files, 1, label);
  console.log(`${label}: ok`);
}

try {
  const tree = join(root, "tree");
  mkdirSync(tree);
  writeFileSync(join(tree, "file"), "real-size");
  let tarball = process.argv[2] && resolve(process.argv[2]);
  if (!tarball) {
    const [packed] = JSON.parse(
      npm(
        ["pack", "--json", "--ignore-scripts", "--pack-destination", root],
        process.cwd(),
      ),
    );
    tarball = join(root, packed.filename);
  }
  const project = join(root, "project");
  mkdirSync(project);
  npm(
    [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
      "--package-lock=false",
      tarball,
    ],
    project,
  );

  const api = spawnSync(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      `import { realSize, binaryPath } from "real-size";
       if (!binaryPath()) throw new Error("no binary");
       const result = await realSize(${JSON.stringify(tree)});
       process.stdout.write(JSON.stringify(result));`,
    ],
    { cwd: project, encoding: "utf8", timeout: 30000 },
  );
  check(api, "API");

  if (windows) {
    check(
      spawnSync(
        "cmd.exe",
        [
          "/d",
          "/s",
          "/c",
          `"node_modules\\.bin\\real-size-windows.cmd --json "${tree}""`,
        ],
        { cwd: project, encoding: "utf8", windowsVerbatimArguments: true },
      ),
      "node_modules/.bin/real-size-windows.cmd",
    );
  } else {
    const tools = join(root, "tools");
    mkdirSync(tools);
    for (const tool of ["sh", "uname", "readlink", "dirname"])
      symlinkSync(
        execFileSync("/bin/sh", ["-c", `command -v ${tool}`], {
          encoding: "utf8",
        }).trim(),
        join(tools, tool),
      );
    writeFileSync(join(tools, "node"), "#!/bin/sh\nexit 99\n", { mode: 0o755 });
    check(
      spawnSync(
        join(project, "node_modules", ".bin", "real-size"),
        ["--json", tree],
        { encoding: "utf8", env: { PATH: tools } },
      ),
      "node_modules/.bin/real-size without Node",
    );
  }

  const npx = spawnSync(
    process.execPath,
    [
      process.env.npm_execpath.replace(/npm-cli\.js$/, "npx-cli.js"),
      "--yes",
      "--package",
      tarball,
      "--",
      windows ? "real-size-windows" : "real-size",
      "--json",
      tree,
    ],
    { cwd: root, encoding: "utf8", timeout: 120000 },
  );
  check(npx, "npx");
} finally {
  rmSync(root, { recursive: true, force: true });
}
