import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { test } from "node:test";
import { binaryPath, realSize, RealSizeError } from "../index.mjs";
import { binary, measure, scratch, write } from "./helpers.mjs";

test("binaryPath returns the prebuilt binary for this platform", () => {
  assert.ok(existsSync(binaryPath() ?? ""));
});

test("realSize returns what the CLI prints", async (t) => {
  const root = scratch(t);
  write(join(root, "tree", "file"));
  const result = await realSize(join(root, "tree"));
  assert.deepEqual(result.paths[0], measure(join(root, "tree")));
  assert.equal(result.total.files, 1);
});

test("realSize passes paths that start with a dash as paths", async (t) => {
  const root = scratch(t);
  write(join(root, "-tree", "file"), 10);
  const result = await inside(root, () => realSize(["-tree"]));
  assert.equal(result.paths[0].path, "-tree");
});

test("a missing path rejects with a failed error", async (t) => {
  const root = scratch(t);
  await assert.rejects(realSize(join(root, "missing")), (error) => {
    assert.ok(error instanceof RealSizeError);
    assert.equal(error.code, "failed");
    assert.equal(error.exitCode, 1);
    assert.ok(error.message.includes("missing"));
    return true;
  });
});

test("an aborted signal rejects with its reason", async () => {
  const reason = new Error("stop");
  await assert.rejects(
    realSize(".", { signal: AbortSignal.abort(reason) }),
    reason,
  );
});

/** Runs `start` with `directory` as the working directory, so relative paths stay short. */
async function inside(directory, start) {
  const cwd = process.cwd();
  process.chdir(directory);
  try {
    return await start();
  } finally {
    process.chdir(cwd);
  }
}

function running() {
  if (process.platform === "win32") {
    const list = spawnSync(
      "tasklist",
      ["/FI", "IMAGENAME eq real-size.exe", "/FO", "CSV", "/NH"],
      {
        encoding: "utf8",
      },
    );
    return list.stdout.includes("real-size.exe");
  }
  const list = spawnSync("ps", ["-A", "-o", "command="], { encoding: "utf8" });
  return list.stdout.split("\n").some((line) => line.startsWith(binary));
}

test("timeoutMs kills the walk and rejects with a timeout error", async (t) => {
  const root = scratch(t);
  for (let i = 0; i < 300; i++)
    write(join(root, "t", String(i % 10), String(i)), 10);
  const started = Date.now();
  const walk = inside(root, () =>
    realSize(Array(3000).fill("t"), { timeoutMs: 50 }),
  );
  await assert.rejects(walk, (error) => {
    assert.ok(error instanceof RealSizeError);
    assert.equal(error.code, "timeout");
    return true;
  });
  assert.ok(Date.now() - started < 5000);
  for (let tries = 0; running() && tries < 40; tries++) await sleep(50);
  assert.equal(running(), false, "the real-size child is still running");
});
