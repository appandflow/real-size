export interface TreeSize {
  /** Bytes the regular files occupy on disk, each file counted once. */
  allocated: number;
  /** Bytes deleting the tree would free. A lower bound when `exact` is false. */
  private: number;
  /** Regular files, each counted once however many links it has in the tree. */
  files: number;
  /** False when `private` is a lower bound because some counted file shares blocks with another file. */
  exact: boolean;
}

export interface PathSize extends TreeSize {
  /** The path as it was passed. */
  path: string;
}

export interface RealSizeResult {
  paths: PathSize[];
  /** The sum of `paths`, each measured on its own. */
  total: TreeSize;
}

export interface RealSizeOptions {
  /** Aborting kills the walk and rejects with the signal's reason. */
  signal?: AbortSignal;
  /** Kills the walk and rejects with a `timeout` error after this many milliseconds. */
  timeoutMs?: number;
}

export type RealSizeErrorCode = "unsupported" | "timeout" | "failed";

export declare class RealSizeError extends Error {
  constructor(
    code: RealSizeErrorCode,
    message: string,
    options?: { cause?: unknown; exitCode?: number | null },
  );
  readonly name: "RealSizeError";
  /**
   * `unsupported`: no prebuilt binary for this platform, or the filesystem cannot report the bytes deleting the path
   * would free (exit status 2). `timeout`: `timeoutMs` ran out. `failed`: a usage or I/O error (exit status 1), or the
   * binary could not run.
   */
  readonly code: RealSizeErrorCode;
  /** The binary's exit status, null when it did not exit on its own. */
  readonly exitCode: number | null;
}

/** Measures each directory in its own walk, run in a child process. */
export declare function realSize(
  paths: string | readonly string[],
  options?: RealSizeOptions,
): Promise<RealSizeResult>;

/** The absolute path of the prebuilt binary for this platform, or null when the package has none. */
export declare function binaryPath(): string | null;
