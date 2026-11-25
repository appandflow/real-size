import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

export class RealSizeError extends Error {
  /**
   * @param {"unsupported" | "timeout" | "failed"} code
   * @param {string} message
   * @param {{ cause?: unknown; exitCode?: number | null }} [options]
   */
  constructor(code, message, options = {}) {
    super(message, { cause: options.cause });
    this.name = "RealSizeError";
    this.code = code;
    this.exitCode = options.exitCode ?? null;
  }
}

/** @returns {string | null} */
export function binaryPath() {
  const name = process.platform === "win32" ? "real-size.exe" : "real-size";
  const file = fileURLToPath(
    new URL(
      `./prebuilds/${process.platform}-${process.arch}/${name}`,
      import.meta.url,
    ),
  );
  return existsSync(file) ? file : null;
}

/**
 * @param {string | readonly string[]} paths
 * @param {{ signal?: AbortSignal; timeoutMs?: number }} [options]
 * @returns {Promise<import("./index.d.mts").RealSizeResult>}
 */
export function realSize(paths, options = {}) {
  const { signal, timeoutMs } = options;
  const list = typeof paths === "string" ? [paths] : [...paths];
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(signal.reason);
    if (list.length === 0)
      return reject(new TypeError("realSize needs at least one path"));
    const binary = binaryPath();
    if (!binary)
      return reject(
        new RealSizeError(
          "unsupported",
          `real-size has no prebuilt binary for ${process.platform}-${process.arch}`,
        ),
      );
    const child = spawn(binary, ["--json", "--", ...list], {
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true,
    });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8").on("data", (chunk) => (stdout += chunk));
    child.stderr.setEncoding("utf8").on("data", (chunk) => (stderr += chunk));
    let settled = false;
    /** @type {NodeJS.Timeout | undefined} */
    let timer;
    const onAbort = () => {
      child.kill("SIGKILL");
      finish(() => reject(signal?.reason));
    };
    /** @param {() => void} settle */
    const finish = (settle) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
      settle();
    };
    if (timeoutMs !== undefined)
      timer = setTimeout(() => {
        child.kill("SIGKILL");
        finish(() =>
          reject(
            new RealSizeError(
              "timeout",
              `real-size did not finish within ${timeoutMs} ms`,
            ),
          ),
        );
      }, timeoutMs);
    signal?.addEventListener("abort", onAbort, { once: true });
    child.on("error", (error) =>
      finish(() =>
        reject(new RealSizeError("failed", error.message, { cause: error })),
      ),
    );
    child.on("close", (exitCode, exitSignal) =>
      finish(() => {
        const message = (stderr.trim().split(/\r?\n/).at(-1) ?? "").replace(
          /^real-size: /,
          "",
        );
        const partial = exitCode === 1 ? parsePartial(stdout) : null;
        if (partial) resolve(partial);
        else if (exitCode === 0) {
          try {
            resolve(JSON.parse(stdout));
          } catch (error) {
            reject(
              new RealSizeError("failed", "real-size printed invalid JSON", {
                cause: error,
                exitCode,
              }),
            );
          }
        } else if (exitCode === 2)
          reject(new RealSizeError("unsupported", message, { exitCode }));
        else
          reject(
            new RealSizeError(
              "failed",
              message || `real-size exited with ${exitCode ?? exitSignal}`,
              { exitCode },
            ),
          );
      }),
    );
  });
}

/**
 * Exit status 1 with a document on stdout means some entries could not be read and were skipped.
 * @param {string} stdout
 * @returns {import("./index.d.mts").RealSizeResult | null}
 */
function parsePartial(stdout) {
  try {
    const result = JSON.parse(stdout);
    return result?.total?.skipped > 0 ? result : null;
  } catch {
    return null;
  }
}
