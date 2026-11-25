import assert from "node:assert/strict";
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { chmodSync, mkdirSync, rmSync } from "node:fs";
import { join, sep } from "node:path";
import { test } from "node:test";
import { realSize } from "../index.mjs";
import { binary, cli, scratch, write } from "./helpers.mjs";

const windows = process.platform === "win32";
const root = process.getuid?.() === 0;

/** Makes `directory` unlistable for this user, and returns a function that undoes it. */
function lock(directory) {
  if (windows) {
    execFileSync("icacls", [directory, "/deny", "*S-1-1-0:(RD)"]);
    return () => execFileSync("icacls", [directory, "/remove:d", "*S-1-1-0"]);
  }
  chmodSync(directory, 0o000);
  return () => chmodSync(directory, 0o755);
}

/** Runs `body` with each directory locked, and unlocks them before the scratch directory is removed. */
function withLocked(directories, body) {
  const unlocks = directories.map(lock);
  try {
    return body();
  } finally {
    for (const unlock of unlocks) unlock();
  }
}

test("a directory deleted during the walk is skipped without a message", async (t) => {
  const dir = scratch(t);
  write(join(dir, "tree", "a", "file"), 10);
  write(join(dir, "tree", "gone", "file"), 10);
  write(join(dir, "tree", "gone", "deeper", "file"), 10);
  write(join(dir, "tree", "z", "file"), 10);
  const child = spawn(binary, ["--json", "tree"], {
    cwd: dir,
    env: { ...process.env, REAL_SIZE_TEST_PAUSE_AT: "gone" },
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8").on("data", (chunk) => (stdout += chunk));
  child.stderr.setEncoding("utf8").on("data", (chunk) => {
    stderr += chunk;
    if (stderr.includes("\n") && !child.stdin.writableEnded) {
      rmSync(join(dir, "tree", "gone"), { recursive: true, force: true });
      child.stdin.end("\n");
    }
  });
  const status = await new Promise((resolve) => child.on("close", resolve));
  assert.equal(
    stderr.replace(/\r\n/g, "\n"),
    `real-size: test pause at tree${sep}gone\n`,
  );
  assert.equal(status, 0);
  const result = JSON.parse(stdout).paths[0];
  assert.equal(result.files, 2);
  assert.equal(result.skipped, 0);
  assert.equal(result.exact, true);
});

test(
  "an unreadable directory is skipped, named on stderr, and the result printed with exit 1",
  { skip: root && "root reads every directory" },
  (t) => {
    const dir = scratch(t);
    write(join(dir, "tree", "readable", "file"), 10);
    write(join(dir, "tree", "locked", "file"), 10);
    withLocked([join(dir, "tree", "locked")], () => {
      for (const arg of ["tree", `tree${sep}`]) {
        const json = cli(["--json", arg], { cwd: dir });
        assert.equal(json.status, 1, json.stderr);
        assert.match(
          json.stderr,
          new RegExp(`^real-size: tree\\${sep}locked: .+\\r?\\n$`),
        );
        const result = JSON.parse(json.stdout);
        assert.deepEqual(
          [
            result.paths[0].path,
            result.paths[0].files,
            result.paths[0].skipped,
            result.paths[0].exact,
          ],
          [arg, 1, 1, false],
        );
        assert.equal(result.total.skipped, 1);
        assert.equal(result.total.exact, false);
      }
      const plain = cli(["tree"], { cwd: dir });
      assert.equal(plain.status, 1);
      assert.match(
        plain.stdout,
        /^tree: at least \d+(\.\d)? k?B private, .+ allocated, 1 files \(1 entries could not be read\)\n$/,
      );
    });
  },
);

test(
  "realSize resolves with skipped entries instead of rejecting",
  { skip: root && "root reads every directory" },
  async (t) => {
    const dir = scratch(t);
    write(join(dir, "tree", "file"), 10);
    mkdirSync(join(dir, "tree", "locked"));
    const unlock = lock(join(dir, "tree", "locked"));
    try {
      const result = await realSize(join(dir, "tree"));
      assert.equal(result.paths[0].files, 1);
      assert.equal(result.paths[0].skipped, 1);
      assert.equal(result.total.skipped, 1);
      assert.equal(result.total.exact, false);
    } finally {
      unlock();
    }
  },
);

test(
  "past ten unreadable entries stderr gives the rest as a count",
  { skip: root && "root reads every directory" },
  (t) => {
    const dir = scratch(t);
    const locked = [];
    for (let i = 0; i < 12; i++) {
      locked.push(join(dir, "tree", `locked-${i}`));
      mkdirSync(locked.at(-1), { recursive: true });
    }
    withLocked(locked, () => {
      const run = cli(["--json", "tree"], { cwd: dir });
      assert.equal(run.status, 1, run.stderr);
      const lines = run.stderr.trim().split("\n");
      assert.equal(lines.length, 11);
      assert.ok(lines.slice(0, 10).every((line) => line.includes("locked-")));
      assert.equal(
        lines[10],
        "real-size: and 2 more entries could not be read",
      );
      assert.equal(JSON.parse(run.stdout).total.skipped, 12);
    });
  },
);

test(
  "an unreadable path itself exits 1 with nothing on stdout",
  { skip: root && "root reads every directory" },
  (t) => {
    const dir = scratch(t);
    write(join(dir, "tree", "file"), 10);
    withLocked([join(dir, "tree")], () => {
      const run = cli(["--json", "tree"], { cwd: dir });
      assert.equal(run.status, 1);
      assert.equal(run.stdout, "");
      assert.match(run.stderr, /^real-size: tree: .+\r?\n$/);
    });
  },
);

test("directories nested past the depth limit are skipped and named", (t) => {
  const dir = scratch(t);
  const deep = join(dir, "tree", ...Array(260).fill("d"));
  write(join(deep, "file"), 10);
  write(join(dir, "tree", "file"), 10);
  const run = cli(["--json", "tree"], { cwd: dir });
  assert.equal(run.status, 1, run.stderr);
  assert.match(
    run.stderr,
    new RegExp(
      `^real-size: tree(\\${sep}d){256}: more than 256 nested directories\\r?\\n$`,
    ),
  );
  const result = JSON.parse(run.stdout).paths[0];
  assert.deepEqual([result.files, result.skipped], [1, 1]);
});

const python = !windows && spawnSync("python3", ["--version"]).status === 0;

/** Runs the binary with stderr on a pseudo-terminal `columns` wide and stdout on a pipe. */
function onTerminal(args, env, columns = 60) {
  const helper = `
import fcntl, json, os, struct, subprocess, sys, termios
master, slave = os.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, ${columns}, 0, 0))
child = subprocess.Popen(sys.argv[1:], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=slave)
os.close(slave)
stderr = b""
while True:
    try:
        data = os.read(master, 65536)
    except OSError:
        break
    if not data:
        break
    stderr += data
stdout = child.stdout.read()
print(json.dumps({"status": child.wait(), "stdout": stdout.decode(), "stderr": stderr.decode()}))
`;
  const run = spawnSync("python3", ["-c", helper, binary, ...args], {
    encoding: "utf8",
    env: { ...process.env, ...env },
  });
  assert.equal(run.status, 0, run.stderr);
  return JSON.parse(run.stdout);
}

test(
  "progress shows on a terminal only, fits its width, and is cleared",
  {
    skip: windows
      ? "no pseudo-terminal helper on Windows"
      : !python && "needs python3 for a pseudo-terminal",
  },
  (t) => {
    const dir = scratch(t);
    const long = join(dir, "x".repeat(80));
    write(join(long, "a", "file"), 10);
    const env = { REAL_SIZE_TEST_PROGRESS_AFTER_MS: "0" };
    for (const json of [false, true]) {
      const shown = onTerminal(json ? ["--json", long] : [long], env);
      assert.equal(shown.status, 0, shown.stderr);
      const lines = shown.stderr.split("\r").filter((line) => line.trim());
      assert.ok(lines.length >= 1, JSON.stringify(shown.stderr));
      for (const line of lines) {
        assert.match(line, /^\d+ files, \.\.\.x+/);
        assert.ok(line.length <= 59, line);
      }
      assert.match(shown.stderr, /\r +\r$/);
      if (json) assert.equal(JSON.parse(shown.stdout).paths[0].files, 1);
      else assert.match(shown.stdout, /1 files\n$/);
    }
    const off = onTerminal(["--no-progress", long], env);
    assert.equal(off.status, 0);
    assert.equal(off.stderr, "");
    const piped = cli([long], { env: { ...process.env, ...env } });
    assert.equal(piped.status, 0);
    assert.equal(piped.stderr, "");
  },
);
