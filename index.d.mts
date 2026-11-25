export interface TreeSize {
  /** Bytes the regular files occupy on disk, each file counted once. */
  allocated: number;
  /** Bytes deleting the tree would free. A lower bound when `exact` is false. */
  private: number;
  /** Regular files, each counted once however many links it has in the tree. */
  files: number;
  /**
   * False when `private` is a lower bound: some counted file shares blocks with another file, or some entries could
   * not be read (`skipped` above 0).
   */
  exact: boolean;
  /**
   * Entries the walk could not read, for example a directory without read permission, and left out of every other
   * field. When above 0 every size is a lower bound; the binary names the entries on stderr and exits with status 1.
   * Entries deleted during the walk are not counted here.
   */
  skipped: number;
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
   * would free (exit status 2). `timeout`: `timeoutMs` ran out. `failed`: a usage error or a path that cannot be read (exit
   * status 1 without a result), or the binary could not run.
   */
  readonly code: RealSizeErrorCode;
  /** The binary's exit status, null when it did not exit on its own. */
  readonly exitCode: number | null;
}

/**
 * Measures each directory in its own walk, run in a child process. Resolves when every path was measured, including
 * when some entries below a path could not be read (`total.skipped` above 0). Rejects when a path itself cannot be
 * read.
 */
export declare function realSize(
  paths: string | readonly string[],
  options?: RealSizeOptions,
): Promise<RealSizeResult>;

/** The absolute path of the prebuilt binary for this platform, or null when the package has none. */
export declare function binaryPath(): string | null;
