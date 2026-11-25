#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real-size.h"

#ifndef FILE_SUPPORTS_BLOCK_REFCOUNTING
#define FILE_SUPPORTS_BLOCK_REFCOUNTING 0x08000000
#endif

/* MS-FSCC 2.3.31 FSCTL_GET_RETRIEVAL_POINTERS_AND_REFCOUNT, ReFS only (Windows 10 1703, Server 2019 and later):
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/80ac720b-374c-40b7-847c-e57874260b64
 * Each extent carries the reference count of the cluster it starts at, 1 when no other file uses it:
 * https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/dd2be29e-4f60-4bb4-b68a-20f2c34ac0d2 */
#define REFCOUNT_FSCTL CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 244, METHOD_NEITHER, FILE_ANY_ACCESS)

struct refcount_extent {
  LONGLONG next_vcn;
  LONGLONG lcn;
  DWORD reference_count;
};

struct refcount_buffer {
  DWORD extent_count;
  DWORD unused;
  LONGLONG starting_vcn;
  struct refcount_extent extents[64];
};

#define PATH_CAPACITY 32768

static wchar_t path[PATH_CAPACITY];
static DWORD last_error;
static int refcounted;
static uint64_t cluster_size;

static int fail(DWORD error) {
  last_error = error;
  return -1;
}

static uint64_t private_bytes(HANDLE file, uint64_t allocated, int *shared) {
  STARTING_VCN_INPUT_BUFFER input = {0};
  struct refcount_buffer output;
  uint64_t unshared = 0;
  *shared = 0;
  for (;;) {
    DWORD bytes = 0;
    BOOL done = DeviceIoControl(file, REFCOUNT_FSCTL, &input, sizeof input, &output, sizeof output, &bytes, NULL);
    DWORD error = done ? 0 : GetLastError();
    if (!done && error == ERROR_HANDLE_EOF) break;
    if (!done && error != ERROR_MORE_DATA) {
      *shared = 1;
      return 0;
    }
    LONGLONG vcn = output.starting_vcn;
    for (DWORD i = 0; i < output.extent_count && i < 64; i++) {
      const struct refcount_extent *extent = &output.extents[i];
      if (extent->lcn != -1) {
        if (extent->reference_count > 1) *shared = 1;
        else unshared += (uint64_t)(extent->next_vcn - vcn) * cluster_size;
      }
      vcn = extent->next_vcn;
    }
    if (done || output.extent_count == 0) break;
    input.StartingVcn.QuadPart = vcn;
  }
  if (!*shared) return allocated;
  return unshared < allocated ? unshared : allocated;
}

/* GetFileInformationByName with FileStatBasicByNameInfo (3) returns FILE_STAT_BASIC_INFORMATION, link count included,
 * without opening a handle. Kernel32 exports it from Windows 11 24H2 and Windows Server 2025:
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyname
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-file_stat_basic_information */
struct stat_basic {
  LARGE_INTEGER file_id;
  LARGE_INTEGER creation_time;
  LARGE_INTEGER last_access_time;
  LARGE_INTEGER last_write_time;
  LARGE_INTEGER change_time;
  LARGE_INTEGER allocation_size;
  LARGE_INTEGER end_of_file;
  ULONG file_attributes;
  ULONG reparse_tag;
  ULONG number_of_links;
  ULONG device_type;
  ULONG device_characteristics;
  ULONG reserved;
  LARGE_INTEGER volume_serial_number;
  BYTE file_id_128[16];
};

typedef BOOL(WINAPI *stat_by_name_function)(PCWSTR, int, PVOID, ULONG);
static stat_by_name_function stat_by_name;

static int add_entry_by_name(size_t length, struct totals *totals) {
  struct stat_basic info;
  BOOL found = stat_by_name(path, 3, &info, sizeof info);
  DWORD error = found ? 0 : GetLastError();
  path[length] = 0;
  if (!found) return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : fail(error);
  uint64_t ino, ino_high, allocated = (uint64_t)info.allocation_size.QuadPart;
  memcpy(&ino, info.file_id_128, sizeof ino);
  memcpy(&ino_high, info.file_id_128 + 8, sizeof ino_high);
  add_file(totals, (uint64_t)info.volume_serial_number.QuadPart, ino, ino_high, info.number_of_links, allocated,
           allocated, 0);
  return 0;
}

static int add_entry(size_t length, DWORD attributes, struct totals *totals) {
  if (stat_by_name && !refcounted &&
      !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)))
    return add_entry_by_name(length, totals);
  HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  path[length] = 0;
  if (file == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : fail(error);
  }
  BY_HANDLE_FILE_INFORMATION info;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandle(file, &info) ||
      !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard)) {
    DWORD error = GetLastError();
    CloseHandle(file);
    return fail(error);
  }
  uint64_t ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow, ino_high = 0;
  uint64_t volume = info.dwVolumeSerialNumber;
  FILE_ID_INFO id;
  if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof id)) {
    volume = id.VolumeSerialNumber;
    memcpy(&ino, id.FileId.Identifier, sizeof ino);
    memcpy(&ino_high, id.FileId.Identifier + 8, sizeof ino_high);
  }
  uint64_t allocated = (uint64_t)standard.AllocationSize.QuadPart;
  FILE_COMPRESSION_INFO compression;
  if ((attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) &&
      GetFileInformationByHandleEx(file, FileCompressionInfo, &compression, sizeof compression) &&
      (uint64_t)compression.CompressedFileSize.QuadPart < allocated)
    allocated = (uint64_t)compression.CompressedFileSize.QuadPart;
  uint64_t private_size = allocated;
  int shared = 0;
  if (refcounted && allocated > 0 &&
      !(attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)))
    private_size = private_bytes(file, allocated, &shared);
  CloseHandle(file);
  add_file(totals, volume, ino, ino_high, info.nNumberOfLinks, allocated, private_size, shared);
  return 0;
}

/* Reparse points whose tag is a name surrogate (symbolic links, junctions, volume mount points) name another file
 * and are not followed. Other reparse points, such as cloud placeholders, are ordinary files and folders here. Files
 * are opened for FILE_READ_ATTRIBUTES only, an attribute-only open that placeholder filters are expected not to turn
 * into a recall: https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/placeholders_guidance */
static int walk(size_t length, struct totals *totals, int depth) {
  if (length + 2 >= PATH_CAPACITY) return fail(ERROR_FILENAME_EXCED_RANGE);
  if (path[length - 1] != L'\\') path[length++] = L'\\';
  path[length] = L'*';
  path[length + 1] = 0;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW(path, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  path[length] = 0;
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? 0 : fail(error);
  }
  int result = 0;
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
    DWORD attributes = data.dwFileAttributes;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(data.dwReserved0)) continue;
    size_t name_length = wcslen(name);
    if (length + name_length + 3 >= PATH_CAPACITY) {
      result = fail(ERROR_FILENAME_EXCED_RANGE);
      break;
    }
    memcpy(path + length, name, (name_length + 1) * sizeof *name);
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth + 1 >= MAX_DEPTH) {
        result = fail(ERROR_CANT_RESOLVE_FILENAME);
        break;
      }
      result = walk(length + name_length, totals, depth + 1);
      path[length] = 0;
    } else result = add_entry(length, attributes, totals);
    if (result < 0) break;
  } while (FindNextFileW(find, &data));
  if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES) result = fail(GetLastError());
  FindClose(find);
  return result;
}

static size_t extended_path(const char *utf8) {
  wchar_t given[PATH_CAPACITY], full[PATH_CAPACITY];
  if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, given, PATH_CAPACITY)) return (size_t)fail(GetLastError());
  if (wcsncmp(given, L"\\\\?\\", 4) == 0) {
    wcscpy(path, given);
    return wcslen(path);
  }
  DWORD length = GetFullPathNameW(given, PATH_CAPACITY, full, NULL);
  if (length == 0 || length >= PATH_CAPACITY - 8) return (size_t)fail(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
  if (full[0] == L'\\' && full[1] == L'\\') {
    wcscpy(path, L"\\\\?\\UNC\\");
    wcscat(path, full + 2);
  } else {
    wcscpy(path, L"\\\\?\\");
    wcscat(path, full);
  }
  return wcslen(path);
}

int measure(const char *utf8, struct totals *totals) {
  size_t length = extended_path(utf8);
  if (length == (size_t)-1) return -1;
  while (length > 7 && path[length - 1] == L'\\' && path[length - 2] != L':') path[--length] = 0;
  HANDLE root = CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (root == INVALID_HANDLE_VALUE) return fail(GetLastError());
  FILE_ATTRIBUTE_TAG_INFO tag;
  DWORD flags = 0;
  wchar_t volume[MAX_PATH + 1];
  BOOL described = GetFileInformationByHandleEx(root, FileAttributeTagInfo, &tag, sizeof tag) &&
                   GetVolumeInformationByHandleW(root, NULL, 0, NULL, NULL, &flags, NULL, 0);
  DWORD error = GetLastError();
  CloseHandle(root);
  if (!described) return fail(error);
  if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return fail(ERROR_DIRECTORY);
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(tag.ReparseTag))
    return fail(ERROR_CANT_ACCESS_FILE);
  if (wcsncmp(path, L"\\\\?\\UNC\\", 8) == 0) return UNSUPPORTED;
  if (!GetVolumePathNameW(path, volume, MAX_PATH + 1)) return fail(GetLastError());
  if (GetDriveTypeW(volume) == DRIVE_REMOTE) return UNSUPPORTED;
  refcounted = (flags & FILE_SUPPORTS_BLOCK_REFCOUNTING) != 0;
  if (!stat_by_name && !getenv("REAL_SIZE_NO_STAT_BY_NAME"))
    stat_by_name = (stat_by_name_function)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                         "GetFileInformationByName");
  if (refcounted) {
    DWORD sectors, bytes, free_clusters, clusters;
    if (!GetDiskFreeSpaceW(volume, &sectors, &bytes, &free_clusters, &clusters)) return fail(GetLastError());
    cluster_size = (uint64_t)sectors * bytes;
  }
  return walk(length, totals, 0);
}

const char *measure_error(void) {
  static char message[512];
  wchar_t wide[256];
  DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_error, 0, wide,
                                256, NULL);
  while (length > 0 && (wide[length - 1] == L'\r' || wide[length - 1] == L'\n' || wide[length - 1] == L'.'))
    wide[--length] = 0;
  if (length == 0 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, message, sizeof message, NULL, NULL))
    snprintf(message, sizeof message, "error %lu", (unsigned long)last_error);
  return message;
}

static UINT console_code_page;

static void restore_console(void) { SetConsoleOutputCP(console_code_page); }

int wmain(int argc, wchar_t **wide) {
  char **argv = calloc((size_t)argc + 1, sizeof *argv);
  if (!argv) return 1;
  for (int i = 0; i < argc; i++) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
    argv[i] = malloc((size_t)size);
    if (!argv[i] || !WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, argv[i], size, NULL, NULL)) return 1;
  }
  console_code_page = GetConsoleOutputCP();
  if (console_code_page && SetConsoleOutputCP(CP_UTF8)) atexit(restore_console);
  return real_size_main(argc, argv);
}
