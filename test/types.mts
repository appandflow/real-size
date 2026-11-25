import { binaryPath, realSize, RealSizeError } from "real-size";
import type { RealSizeResult } from "real-size";

const result: RealSizeResult = await realSize(["."], {
  timeoutMs: 1000,
  signal: AbortSignal.timeout(2000),
});
const reclaimable: number = result.paths[0]!.private;
const exact: boolean = result.total.exact;
const binary: string | null = binaryPath();
try {
  await realSize("/");
} catch (error) {
  if (error instanceof RealSizeError) {
    const code: "unsupported" | "timeout" | "failed" = error.code;
    void code;
  }
}
void reclaimable;
void exact;
void binary;
