import { spawnSync } from "node:child_process";
import { chmodSync, mkdirSync, readFileSync } from "node:fs";

const { version } = JSON.parse(readFileSync("package.json", "utf8"));
const target = `${process.platform}-${process.arch}`;
const directory = `prebuilds/${target}`;
mkdirSync(directory, { recursive: true });

function run(command, args) {
  console.log([command, ...args].join(" "));
  const result = spawnSync(command, args, { stdio: "inherit" });
  if (result.status !== 0) process.exit(result.status ?? 1);
}

const warnings = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"];
if (process.platform === "darwin") {
  run("cc", [
    ...warnings,
    "-mmacosx-version-min=11.0",
    `-DREAL_SIZE_VERSION=${version}`,
    `-DREAL_SIZE_TARGET=${target}`,
    "src/main.c",
    "src/darwin.c",
    "src/posix.c",
    "-o",
    `${directory}/real-size`,
  ]);
} else if (process.platform === "linux") {
  run("cc", [
    ...warnings,
    "-static",
    "-s",
    `-DREAL_SIZE_VERSION=${version}`,
    `-DREAL_SIZE_TARGET=${target}`,
    "src/main.c",
    "src/linux.c",
    "src/posix.c",
    "-o",
    `${directory}/real-size`,
  ]);
} else if (process.platform === "win32") {
  mkdirSync("build", { recursive: true });
  run("cl", [
    "/nologo",
    "/std:c11",
    "/O2",
    "/W4",
    "/WX",
    "/MT",
    "/utf-8",
    `/DREAL_SIZE_VERSION=${version}`,
    `/DREAL_SIZE_TARGET=${target}`,
    "src\\main.c",
    "src\\windows.c",
    "/Fobuild\\",
    `/Fe${directory}\\real-size.exe`,
  ]);
} else {
  console.error(`real-size does not build on ${process.platform}`);
  process.exit(1);
}
if (process.platform !== "win32") chmodSync(`${directory}/real-size`, 0o755);
