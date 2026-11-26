import { spawnSync } from "node:child_process";
import {
  appendFileSync,
  existsSync,
  linkSync,
  mkdirSync,
  mkdtempSync,
  openSync,
  closeSync,
  writeSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { cpus, totalmem } from "node:os";
import { join, resolve } from "node:path";
import { parseArgs } from "node:util";
import { binaryPath } from "../index.mjs";

const { values, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    runs: { type: "string", default: "10" },
    warmup: { type: "string", default: "2" },
    generate: { type: "string" },
    trees: { type: "string" },
    threads: { type: "string" },
    baseline: { type: "string", multiple: true, default: [] },
    cold: { type: "boolean", default: false },
    binary: { type: "string" },
    variant: { type: "string", multiple: true, default: [] },
    keep: { type: "boolean", default: false },
  },
});
const runs = Number(values.runs);
const warmup = Number(values.warmup);
const binary = values.binary ? resolve(values.binary) : binaryPath();
if (!binary) throw new Error("run npm run build first");
const windows = process.platform === "win32";
const threads = values.threads ? values.threads.split(",") : [];

/** A tree shaped like an npm `node_modules`: packages of 8 to 47 small files in a few folders, a quarter of them
 * with nested dependencies, down to six levels. Seeded, so every runner builds the same tree. */
function nodeModules(root, target) {
  let seed = 1;
  const random = (n) => {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    return seed % n;
  };
  const content = Buffer.alloc(32 * 1024, "x");
  let files = 0;
  const pkg = (dir, depth) => {
    const folders = ["", "lib", join("lib", "internal"), "dist"];
    for (const folder of folders)
      mkdirSync(join(dir, folder), { recursive: true });
    writeFileSync(join(dir, "package.json"), `{"name":"${files}"}\n`);
    const count = 8 + random(40);
    for (let f = 0; f < count; f++) {
      const size = 1 + (random(8) === 0 ? random(32 * 1024) : random(2048));
      writeFileSync(
        join(dir, folders[random(4)], `file-${f}.js`),
        content.subarray(0, size),
      );
    }
    files += count + 1;
    if (depth < 5 && random(4) === 0) {
      const nested = 1 + random(5);
      for (let n = 0; n < nested && files < target; n++)
        pkg(join(dir, "node_modules", `dep-${n}`), depth + 1);
    }
  };
  for (let i = 0; files < target; i++) pkg(join(root, `package-${i}`), 0);
  return files;
}

/** Generated trees: a pnpm-style store hard-linked into a project, a deep tree of small files, a few large files,
 * and so on. `--trees` picks some by key; by default the first six are built. */
const generators = {
  "hard-links": (root) => {
    const base = join(root, "hard-links");
    const store = join(base, "store");
    const project = join(base, "node_modules");
    for (let i = 0; i < 30000; i++) {
      const bucket = String(i % 256).padStart(3, "0");
      mkdirSync(join(store, bucket), { recursive: true });
      const file = join(store, bucket, `${i}.js`);
      writeFileSync(file, `module.exports = ${i};\n`.repeat(1 + (i % 40)));
      const target = join(project, `package-${i % 1500}`, "lib", `${i}.js`);
      mkdirSync(join(project, `package-${i % 1500}`, "lib"), {
        recursive: true,
      });
      linkSync(file, target);
    }
    return [
      { name: "hard links (30,000 files linked from a store)", path: project },
      { name: "hard links with their store inside", path: base },
    ];
  },
  deep: (root) => {
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
    return [
      {
        name: `deep tree (${count.toLocaleString("en-US")} small files, 7 levels)`,
        path: deep,
      },
    ];
  },
  large: (root) => {
    const large = join(root, "large");
    mkdirSync(large);
    const chunk = Buffer.alloc(16 * 1024 * 1024, 7);
    for (let f = 0; f < 4; f++) {
      const fd = openSync(join(large, `large-${f}.bin`), "w");
      for (let c = 0; c < 16; c++) writeSync(fd, chunk);
      closeSync(fd);
    }
    return [{ name: "large files (4 x 256 MB)", path: large }];
  },
  wide: (root) => {
    const wide = join(root, "wide");
    mkdirSync(wide);
    for (let i = 0; i < 100000; i++)
      writeFileSync(join(wide, `file-${i}`), `${i}\n`);
    return [
      { name: "wide directory (100,000 files in one directory)", path: wide },
    ];
  },
  symlinks: (root) => {
    const links = join(root, "symlinks");
    for (let d = 0; d < 300; d++) {
      const dir = join(links, `package-${d}`);
      mkdirSync(dir, { recursive: true });
      writeFileSync(join(dir, "index.js"), `${d}\n`);
      for (let l = 0; l < 100; l++)
        symlinkSync(
          windows ? join(dir, "index.js") : "index.js",
          join(dir, `link-${l}`),
        );
    }
    return [{ name: "symbolic links (30,000 links, 300 files)", path: links }];
  },
  "node-modules": (root) => {
    const path = join(root, "node-modules");
    const files = nodeModules(path, 160000);
    return [
      {
        name: `node_modules-like (${files.toLocaleString("en-US")} files)`,
        path,
      },
    ];
  },
  million: (root) => {
    const path = join(root, "million");
    const files = nodeModules(path, 1000000);
    return [
      {
        name: `node_modules-like, large (${files.toLocaleString("en-US")} files)`,
        path,
      },
    ];
  },
};

function generate(root) {
  const keys = values.trees
    ? values.trees.split(",")
    : ["hard-links", "deep", "large", "wide", "symlinks"];
  return keys.flatMap((key) => {
    if (!generators[key]) throw new Error(`no generated tree ${key}`);
    const started = Date.now();
    const trees = generators[key](root);
    console.error(`generated ${key} in ${Date.now() - started} ms`);
    return trees;
  });
}

function baselines(path) {
  const all = !windows
    ? [{ name: "du -sk", command: "du", args: ["-sk", path] }]
    : [
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
  if (values.baseline.length === 0) return all;
  return all.filter((tool) =>
    values.baseline.some((name) => tool.name.startsWith(name)),
  );
}

function spawn(tool, args, stdout = "pipe") {
  return spawnSync(tool.command, args, {
    encoding: "utf8",
    stdio: ["ignore", stdout, "pipe"],
    maxBuffer: 1 << 26,
    windowsVerbatimArguments: tool.verbatim,
    env: tool.env ? { ...process.env, ...tool.env } : process.env,
  });
}

function once(tool) {
  const started = process.hrtime.bigint();
  const run = spawn(tool, tool.args, "ignore");
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

function stats(times) {
  if (times.length === 0) return null;
  const mean = times.reduce((a, b) => a + b, 0) / times.length;
  const sd = Math.sqrt(
    times.reduce((a, b) => a + (b - mean) ** 2, 0) / times.length,
  );
  return { mean, sd, min: Math.min(...times), max: Math.max(...times) };
}

/** Runs every tool in turn, round after round, so drift on the runner spreads over all of them. */
function measure(tools) {
  for (let i = 0; i < warmup; i++) for (const tool of tools) once(tool);
  const warm = tools.map(() => []);
  for (let r = 0; r < runs; r++)
    tools.forEach((tool, i) => warm[i].push(once(tool)));
  const cold = tools.map(() => []);
  if (values.cold)
    for (let r = 0; r < runs; r++)
      tools.forEach((tool, i) => {
        if (dropCaches()) cold[i].push(once(tool));
      });
  return tools.map((tool, i) => ({
    tool,
    warm: stats(warm[i]),
    cold: stats(cold[i]),
  }));
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

const lines = [];
const print = (line) => {
  console.log(line);
  lines.push(line);
};
const format = (ms) =>
  ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${ms.toFixed(1)} ms`;
print(
  `${process.platform}-${process.arch}, ${cpus()[0]?.model.trim()}, ${cpus().length} cores, ${Math.round(totalmem() / 2 ** 30)} GB; ${runs} warm runs after ${warmup} warm-up runs${values.cold ? `, ${runs} cold runs` : ""}, tools interleaved`,
);
print("");
print(
  "| Tree | Files | Tool | Mean | ± | Min | Max | Cold mean | Cold ± | vs baseline |",
);
print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |");
let mismatches = 0;
for (const tree of trees) {
  const ours = {
    name: "real-size",
    command: binary,
    args: [tree.path],
    ok: (s) => s === 0 || s === 2,
  };
  const tools = [
    ...values.variant.map((variant) => {
      const [label, path] = variant.split("=");
      return { ...ours, name: `real-size (${label})`, command: resolve(path) };
    }),
    ...(threads.length
      ? threads.map((count) => ({
          ...ours,
          name: `real-size (${count} thread${count === "1" ? "" : "s"})`,
          env: { REAL_SIZE_TEST_THREADS: count },
        }))
      : [ours]),
    ...(windows && !threads.length
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
  const ourTools = tools.filter((tool) => tool.name.startsWith("real-size"));
  const outputs = ourTools.map((tool) => {
    const run = spawn(tool, ["--json", tree.path]);
    return { tool, status: run.status, stdout: run.stdout };
  });
  const reference = outputs[0];
  for (const output of outputs.slice(1))
    if (
      output.status !== reference.status ||
      output.stdout !== reference.stdout
    ) {
      mismatches++;
      console.error(
        `${tree.name}: ${output.tool.name} differs from ${reference.tool.name}:\n${reference.status} ${reference.stdout}\n${output.status} ${output.stdout}`,
      );
    }
  const files =
    reference.status === 0
      ? JSON.parse(reference.stdout).total.files.toLocaleString("en-US")
      : `exit ${reference.status}`;
  const results = measure(tools);
  const fastestBaseline = Math.min(
    ...results
      .filter((r) => !r.tool.name.startsWith("real-size"))
      .map((r) => r.warm.mean),
  );
  for (const r of results)
    print(
      `| ${tree.name} | ${files} | ${r.tool.name} | ${format(r.warm.mean)} | ${format(r.warm.sd)} | ${format(r.warm.min)} | ${format(r.warm.max)} | ${r.cold ? format(r.cold.mean) : "n/a"} | ${r.cold ? format(r.cold.sd) : "n/a"} | ${(r.warm.mean / fastestBaseline).toFixed(2)}x |`,
    );
}
print("");
print(
  mismatches
    ? `${mismatches} outputs differ from the first real-size tool's --json output`
    : "Every real-size tool printed the same --json output and exit status.",
);
if (process.env.GITHUB_STEP_SUMMARY)
  appendFileSync(process.env.GITHUB_STEP_SUMMARY, `${lines.join("\n")}\n\n`);
if (scratch && !values.keep) rmSync(scratch, { recursive: true, force: true });
if (mismatches) process.exitCode = 1;
