import { spawnSync } from "node:child_process";
import {
  existsSync,
  linkSync,
  mkdirSync,
  mkdtempSync,
  openSync,
  closeSync,
  writeSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { cpus, tmpdir, totalmem } from "node:os";
import { join, resolve } from "node:path";
import { parseArgs } from "node:util";
import { binaryPath } from "../index.mjs";

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    runs: { type: "string", default: "10" },
    warmup: { type: "string", default: "2" },
    generate: { type: "string" },
    cold: { type: "boolean", default: false },
    binary: { type: "string" },
    keep: { type: "boolean", default: false },
  },
});
const runs = Number(values.runs);
const warmup = Number(values.warmup);
const binary = values.binary ? resolve(values.binary) : binaryPath();
if (!binary) throw new Error("run npm run build first");
const windows = process.platform === "win32";

/** Generated trees: a pnpm-style store hard-linked into a project, a deep tree of small files, a few large files. */
function generate(root) {
  const trees = [];
  const store = join(root, "store");
  const project = join(root, "hard-links", "node_modules");
  for (let i = 0; i < 30000; i++) {
    const bucket = String(i % 256).padStart(3, "0");
    mkdirSync(join(store, bucket), { recursive: true });
    const file = join(store, bucket, `${i}.js`);
    writeFileSync(file, `module.exports = ${i};\n`.repeat(1 + (i % 40)));
    const target = join(project, `package-${i % 1500}`, "lib", `${i}.js`);
    mkdirSync(join(project, `package-${i % 1500}`, "lib"), { recursive: true });
    linkSync(file, target);
  }
  trees.push({
    name: "hard links (30,000 files linked from a store)",
    path: project,
  });
  const deep = join(root, "deep");
  let count = 0;
  const fill = (dir, depth) => {
    mkdirSync(dir, { recursive: true });
    for (let f = 0; f < 6; f++)
      writeFileSync(join(dir, `f${f}.txt`), `${count++}\n`);
    if (depth < 6)
      for (let d = 0; d < 5; d++) fill(join(dir, `d${d}`), depth + 1);
  };
  fill(deep, 0);
  trees.push({
    name: `deep tree (${count.toLocaleString("en-US")} small files, 7 levels)`,
    path: deep,
  });
  const large = join(root, "large");
  mkdirSync(large);
  const chunk = Buffer.alloc(16 * 1024 * 1024, 7);
  for (let f = 0; f < 4; f++) {
    const fd = openSync(join(large, `large-${f}.bin`), "w");
    for (let c = 0; c < 16; c++) writeSync(fd, chunk);
    closeSync(fd);
  }
  trees.push({ name: "large files (4 x 256 MB)", path: large });
  return trees;
}

function baselines(path) {
  if (!windows) return [{ name: "du -sk", command: "du", args: ["-sk", path] }];
  return [
    {
      name: "robocopy /L",
      command: "robocopy",
      args: [
        path,
        "NUL",
        "/L",
        "/S",
        "/NJH",
        "/BYTES",
        "/NFL",
        "/NDL",
        "/NC",
        "/R:0",
        "/W:0",
      ],
      ok: (status) => status < 8,
    },
    {
      name: "dir /s /a",
      command: "cmd.exe",
      args: ["/d", "/s", "/c", `"dir /s /a "${path}""`],
      verbatim: true,
    },
    {
      name: "Get-ChildItem",
      command: "powershell.exe",
      args: [
        "-NoProfile",
        "-Command",
        `(Get-ChildItem -LiteralPath '${path}' -Recurse -Force -File | Measure-Object -Property Length -Sum).Sum`,
      ],
    },
  ];
}

function once(tool) {
  const started = process.hrtime.bigint();
  const run = spawnSync(tool.command, tool.args, {
    stdio: ["ignore", "ignore", "pipe"],
    maxBuffer: 1 << 26,
    windowsVerbatimArguments: tool.verbatim,
    env: tool.env ? { ...process.env, ...tool.env } : process.env,
  });
  const ms = Number(process.hrtime.bigint() - started) / 1e6;
  const ok = tool.ok ? tool.ok(run.status) : run.status === 0;
  if (!ok)
    throw new Error(`${tool.name} exited with ${run.status}: ${run.stderr}`);
  return ms;
}

function dropCaches() {
  if (process.platform === "linux")
    return (
      spawnSync("sudo", ["sh", "-c", "sync; echo 3 > /proc/sys/vm/drop_caches"])
        .status === 0
    );
  if (process.platform === "darwin")
    return spawnSync("sudo", ["-n", "purge"]).status === 0;
  return false;
}

function measure(tool) {
  for (let i = 0; i < warmup; i++) once(tool);
  const times = [];
  for (let i = 0; i < runs; i++) times.push(once(tool));
  const mean = times.reduce((a, b) => a + b, 0) / times.length;
  const sd = Math.sqrt(
    times.reduce((a, b) => a + (b - mean) ** 2, 0) / times.length,
  );
  let cold = null;
  if (values.cold && dropCaches()) cold = once(tool);
  return { mean, sd, min: Math.min(...times), max: Math.max(...times), cold };
}

const scratch = values.generate ? resolve(values.generate) : null;
let trees = positionals.map((path) => ({ name: path, path: resolve(path) }));
if (scratch) {
  if (existsSync(scratch) && !values.keep)
    rmSync(scratch, { recursive: true, force: true });
  mkdirSync(scratch, { recursive: true });
  const root = mkdtempSync(join(scratch, "trees-"));
  trees = [...trees, ...generate(root)];
}

const format = (ms) =>
  ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${ms.toFixed(1)} ms`;
console.log(
  `${process.platform}-${process.arch}, ${cpus()[0]?.model.trim()}, ${cpus().length} cores, ${Math.round(totalmem() / 2 ** 30)} GB; ${runs} warm runs after ${warmup} warm-up runs`,
);
console.log("");
console.log(
  "| Tree | Files | Tool | Mean | ± | Min | Max | Cold | vs baseline |",
);
console.log("| --- | --- | --- | --- | --- | --- | --- | --- | --- |");
for (const tree of trees) {
  const json = spawnSync(binary, ["--json", tree.path], {
    encoding: "utf8",
    maxBuffer: 1 << 26,
  });
  const files =
    json.status === 0
      ? JSON.parse(json.stdout).total.files.toLocaleString("en-US")
      : `exit ${json.status}`;
  const ours = {
    name: "real-size",
    command: binary,
    args: [tree.path],
    ok: (s) => s === 0 || s === 2,
  };
  const tools = [
    ours,
    ...(windows
      ? [
          {
            ...ours,
            name: "real-size, a handle per file",
            env: { REAL_SIZE_NO_STAT_BY_NAME: "1" },
          },
        ]
      : []),
    ...baselines(tree.path),
  ];
  const results = tools.map((tool) => ({ tool, ...measure(tool) }));
  const fastestBaseline = Math.min(
    ...results
      .filter((r) => !r.tool.name.startsWith("real-size"))
      .map((r) => r.mean),
  );
  for (const r of results)
    console.log(
      `| ${tree.name} | ${files} | ${r.tool.name} | ${format(r.mean)} | ${format(r.sd)} | ${format(r.min)} | ${format(r.max)} | ${r.cold === null ? "n/a" : format(r.cold)} | ${(r.mean / fastestBaseline).toFixed(2)}x |`,
    );
}
if (scratch && !values.keep) rmSync(scratch, { recursive: true, force: true });
