#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv) {
  if (argc != 3) {
    fputs("usage: clone <source> <target>\n", stderr);
    return 1;
  }
  HANDLE source = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  HANDLE target = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
  LARGE_INTEGER size;
  if (source == INVALID_HANDLE_VALUE || target == INVALID_HANDLE_VALUE || !GetFileSizeEx(source, &size)) {
    fprintf(stderr, "clone: open failed: %lu\n", GetLastError());
    return 1;
  }
  FILE_END_OF_FILE_INFO end = {size};
  DUPLICATE_EXTENTS_DATA extents = {source, {0}, {0}, size};
  DWORD bytes;
  if (!SetFileInformationByHandle(target, FileEndOfFileInfo, &end, sizeof end) ||
      !DeviceIoControl(target, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &extents, sizeof extents, NULL, 0, &bytes, NULL)) {
    fprintf(stderr, "clone: FSCTL_DUPLICATE_EXTENTS_TO_FILE failed: %lu\n", GetLastError());
    return 1;
  }
  CloseHandle(target);
  CloseHandle(source);
  return 0;
}
