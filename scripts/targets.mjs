export const targets = [
  "darwin-arm64",
  "darwin-x64",
  "linux-x64",
  "linux-arm64",
  "win32-x64",
  "win32-arm64",
];

export function binaryName(target) {
  return target.startsWith("win32-") ? "real-size.exe" : "real-size";
}
