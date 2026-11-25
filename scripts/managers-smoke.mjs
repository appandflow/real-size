import { spawnSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  realpathSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { delimiter, join, resolve } from "node:path";

const [manager, tarballArgument] = process.argv.slice(2);
const managers = ["npm", "pnpm", "yarn-classic", "yarn-berry"];
if (!managers.includes(manager) || !tarballArgument) {
  console.error(`usage: managers-smoke.mjs <${managers.join("|")}> <tarball>`);
  process.exit(1);
}
const tarball = resolve(tarballArgument);
const windows = process.platform === "win32";
const root = realpathSync(mkdtempSync(join(tmpdir(), "real-size-managers-")));
const tree = join(root, "tree");
mkdirSync(tree);
writeFileSync(join(tree, "file"), "real-size");

const basePath = windows
  ? (process.env.PATH ?? "")
      .split(delimiter)
      .filter((dir) => dir && !existsSync(join(dir, "sh.exe")))
      .join(delimiter)
  : (process.env.PATH ?? "");
const fakeNode = join(root, "fake-node");
if (!windows) {
  mkdirSync(fakeNode);
  writeFileSync(
    join(fakeNode, "node"),
    "#!/bin/sh\necho node was started >&2\nexit 99\n",
    { mode: 0o755 },
  );
}

const dlxCommand = windows ? "real-size-windows" : "real-size";
const results = [];
let failed = false;

function tool(command, args, cwd, env = {}) {
  const run = spawnSync(command, args, {
    cwd,
    encoding: "utf8",
    shell: windows,
    timeout: 300000,
    env: { ...process.env, COREPACK_ENABLE_DOWNLOAD_PROMPT: "0", ...env },
  });
  if (run.status !== 0)
    throw new Error(
      `${command} ${args.join(" ")} exited with ${run.status ?? run.error}\n${run.stdout}\n${run.stderr}`,
    );
  return run.stdout;
}

/** Runs `command` the way a user would and records whether it printed the tree's JSON. */
function attempt(label, required, run) {
  let outcome;
  try {
    const result = run();
    const last = result.stdout?.trim().split(/\r?\n/).at(-1) ?? "";
    const parsed = result.status === 0 ? JSON.parse(last) : null;
    outcome =
      parsed?.paths?.[0]?.files === 1
        ? "ok"
        : `exit ${result.status}: ${(result.stderr || result.stdout || String(result.error)).trim().split(/\r?\n/)[0]}`;
  } catch (error) {
    outcome = `error: ${error.message.split("\n")[0]}`;
  }
  if (required && outcome !== "ok") failed = true;
  results.push({ label, required, outcome });
}

/** The command on PATH, as typed in a shell: sh on macOS and Linux with a failing `node` first on PATH; cmd.exe and PowerShell on Windows. */
function shells(scope, binDirectory, command, required) {
  if (!windows) {
    attempt(`${scope}: ${command} (sh, no Node)`, required, () =>
      spawnSync("sh", ["-c", `${command} --json "$1"`, "sh", tree], {
        encoding: "utf8",
        env: {
          PATH: [fakeNode, binDirectory, basePath].join(delimiter),
        },
      }),
    );
    attempt(`${scope}: ${command} (spawn without a shell)`, required, () =>
      spawnSync(join(binDirectory, command), ["--json", tree], {
        encoding: "utf8",
        env: { PATH: [fakeNode, basePath].join(delimiter) },
      }),
    );
    return;
  }
  const env = {
    ...process.env,
    PATH: [binDirectory, basePath].join(delimiter),
  };
  if (command === "real-size-windows") {
    const expected = [
      process.env.PROCESSOR_ARCHITECTURE,
      process.env.PROCESSOR_ARCHITEW6432,
    ].includes("ARM64")
      ? "win32-arm64"
      : "win32-x64";
    for (const [shell, args] of [
      ["cmd.exe", ["/d", "/s", "/c", `"${command} --version"`]],
      [
        "pwsh.exe",
        ["-NoProfile", "-Command", `${command} --version; exit $LASTEXITCODE`],
      ],
    ]) {
      const run = spawnSync(shell, args, {
        encoding: "utf8",
        env,
        windowsVerbatimArguments: shell === "cmd.exe",
      });
      const outcome =
        run.status === 0 && run.stdout.includes(`(${expected})`)
          ? "ok"
          : `exit ${run.status}: ${(run.stdout + run.stderr).trim()}`;
      if (required && outcome !== "ok") failed = true;
      results.push({
        label: `${scope}: ${command} --version is ${expected} (${shell})`,
        required,
        outcome,
      });
    }
  }
  attempt(`${scope}: ${command} (cmd.exe)`, required, () =>
    spawnSync("cmd.exe", ["/d", "/s", "/c", `"${command} --json "${tree}""`], {
      encoding: "utf8",
      env,
      windowsVerbatimArguments: true,
    }),
  );
  attempt(`${scope}: ${command} (Windows PowerShell)`, required, () =>
    spawnSync(
      "powershell.exe",
      [
        "-NoProfile",
        "-Command",
        `${command} --json '${tree}'; exit $LASTEXITCODE`,
      ],
      { encoding: "utf8", env },
    ),
  );
  attempt(`${scope}: ${command} (pwsh)`, required, () =>
    spawnSync(
      "pwsh.exe",
      [
        "-NoProfile",
        "-Command",
        `${command} --json '${tree}'; exit $LASTEXITCODE`,
      ],
      { encoding: "utf8", env },
    ),
  );
}

function project(name, packageJson = {}) {
  const directory = join(root, name);
  mkdirSync(directory);
  writeFileSync(
    join(directory, "package.json"),
    JSON.stringify({ name: "scratch", private: true, ...packageJson }),
  );
  return directory;
}

const commands = windows ? ["real-size", "real-size-windows"] : ["real-size"];
const required = (command) =>
  !windows ||
  (command === "real-size-windows" &&
    (manager === "npm" || manager === "pnpm"));

try {
  if (manager === "npm") {
    const local = project("local");
    tool("npm", ["install", "--no-audit", "--no-fund", tarball], local);
    for (const command of commands)
      shells(
        "local",
        join(local, "node_modules", ".bin"),
        command,
        required(command),
      );
    const prefix = join(root, "global");
    tool("npm", ["install", "--global", "--prefix", prefix, tarball], root);
    for (const command of commands)
      shells(
        "global",
        windows ? prefix : join(prefix, "bin"),
        command,
        required(command),
      );
    attempt(`npx ${dlxCommand}`, true, () =>
      spawnSync(
        "npx",
        ["--yes", "--package", tarball, "--", dlxCommand, "--json", tree],
        { encoding: "utf8", shell: windows, cwd: root },
      ),
    );
  } else if (manager === "pnpm") {
    const local = project("local");
    tool("pnpm", ["add", tarball], local);
    for (const command of commands)
      shells(
        "local",
        join(local, "node_modules", ".bin"),
        command,
        required(command),
      );
    const home = join(root, "pnpm-home");
    mkdirSync(home);
    const env = {
      PNPM_HOME: home,
      PATH: [home, join(home, "bin"), process.env.PATH].join(delimiter),
    };
    tool("pnpm", ["add", "--global", tarball], root, env);
    const globalBin = tool("pnpm", ["bin", "--global"], root, env).trim();
    for (const command of commands)
      shells("global", globalBin, command, required(command));
    attempt(`pnpm dlx ${dlxCommand}`, true, () =>
      spawnSync(
        "pnpm",
        [`--package=${tarball}`, "dlx", dlxCommand, "--json", tree],
        { encoding: "utf8", shell: windows, cwd: root },
      ),
    );
  } else if (manager === "yarn-classic") {
    const local = project("local");
    tool("yarn", ["add", `real-size@file:${tarball}`], local);
    for (const command of commands)
      shells(
        "local",
        join(local, "node_modules", ".bin"),
        command,
        required(command),
      );
    const prefix = join(root, "yarn-prefix");
    tool(
      "yarn",
      [
        "global",
        "add",
        `real-size@file:${tarball}`,
        "--prefix",
        prefix,
        "--global-folder",
        join(root, "yarn-global"),
      ],
      root,
    );
    for (const command of commands)
      shells(
        "global",
        windows ? prefix : join(prefix, "bin"),
        command,
        required(command),
      );
  } else {
    for (const linker of ["node-modules", "pnp"]) {
      const local = project(linker, { packageManager: "yarn@4.12.0" });
      writeFileSync(join(local, ".yarnrc.yml"), `nodeLinker: ${linker}\n`);
      writeFileSync(join(local, "yarn.lock"), "");
      tool("corepack", ["yarn", "add", `real-size@file:${tarball}`], local);
      if (linker === "node-modules")
        for (const command of commands)
          shells(
            linker,
            join(local, "node_modules", ".bin"),
            command,
            required(command),
          );
      attempt(`${linker}: yarn real-size (through Node)`, true, () =>
        spawnSync("corepack", ["yarn", "real-size", "--json", tree], {
          encoding: "utf8",
          shell: windows,
          cwd: local,
          env: { ...process.env, COREPACK_ENABLE_DOWNLOAD_PROMPT: "0" },
        }),
      );
    }
    const dlx = project("dlx", { packageManager: "yarn@4.12.0" });
    writeFileSync(join(dlx, "yarn.lock"), "");
    attempt("yarn dlx", true, () =>
      spawnSync(
        "corepack",
        [
          "yarn",
          "dlx",
          "-p",
          `real-size@file:${tarball}`,
          "real-size",
          "--json",
          tree,
        ],
        {
          encoding: "utf8",
          shell: windows,
          cwd: dlx,
          env: { ...process.env, COREPACK_ENABLE_DOWNLOAD_PROMPT: "0" },
        },
      ),
    );
  }
} catch (error) {
  failed = true;
  results.push({ label: "setup", required: true, outcome: error.message });
} finally {
  rmSync(root, { recursive: true, force: true });
}

for (const { label, required, outcome } of results)
  console.log(
    `RESULT ${process.platform}-${process.arch} ${manager} | ${label} | ${outcome}${required ? "" : " (not required)"}`,
  );
if (failed) process.exit(1);
