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

struct worker {
  struct totals totals;
  wchar_t path[PATH_CAPACITY];
};

struct item {
  size_t length;
  int depth;
  wchar_t *path;
};

static wchar_t measured[PATH_CAPACITY];
static DWORD last_error;
static int refcounted;
static uint64_t cluster_size;

static int fail(DWORD error) {
  last_error = error;
  return -1;
}

static const char *error_message(DWORD error) {
  static char message[512];
  wchar_t wide[256];
  DWORD length =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, wide, 256, NULL);
  while (length > 0 && (wide[length - 1] == L'\r' || wide[length - 1] == L'\n' || wide[length - 1] == L'.'))
    wide[--length] = 0;
  if (length == 0 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, message, sizeof message, NULL, NULL))
    snprintf(message, sizeof message, "error %lu", (unsigned long)error);
  return message;
}

/* A file deleted while a handle to it is open stays listed until the last handle closes, and opening it fails with
 * ERROR_ACCESS_DENIED; only the thread's last NTSTATUS, STATUS_DELETE_PENDING, tells the two apart. ntdll exports
 * RtlGetLastNtStatus, which is undocumented; without it such an entry is reported as unreadable. */
typedef LONG(NTAPI *nt_status_function)(void);
static nt_status_function last_nt_status;

/* Call right after the failing call, before any other API call replaces the thread's last status. */
static int vanished(DWORD error) {
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_DELETE_PENDING) return 1;
  return error == ERROR_ACCESS_DENIED && last_nt_status && last_nt_status() == (LONG)0xC0000056L;
}

static const char *root_utf8;
static size_t root_length;
static char shown[PATH_CAPACITY * 3 + 512];

static size_t append_utf8(size_t used, const wchar_t *text, size_t length) {
  if (length == 0 || used >= sizeof shown - 1) return used;
  int written = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, shown + used, (int)(sizeof shown - 1 - used), NULL,
                                    NULL);
  return written > 0 ? used + (size_t)written : used;
}

static size_t append_separator(size_t used) {
  if (used > 0 && shown[used - 1] != '\\' && shown[used - 1] != '/' && shown[used - 1] != ':' && used < sizeof shown - 1)
    shown[used++] = '\\';
  return used;
}

/* The path of `name` in the directory path[0..length] holds, or of that directory when `name` is NULL, as the user
 * wrote it. Called with the output lock held. */
static const char *entry_path(const wchar_t *path, size_t length, const wchar_t *name) {
  int written = snprintf(shown, sizeof shown, "%s", root_utf8);
  size_t used = written > 0 && (size_t)written < sizeof shown ? (size_t)written : 0;
  size_t start = root_length, end = length;
  while (start < end && path[start] == L'\\') start++;
  while (end > start && path[end - 1] == L'\\') end--;
  if (end > start) used = append_utf8(append_separator(used), path + start, end - start);
  if (name) used = append_utf8(append_separator(used), name, wcslen(name));
  shown[used] = 0;
  return shown;
}

static void skip(struct worker *worker, size_t length, const wchar_t *name, DWORD error, const char *reason) {
  output_begin();
  if (skip_entry(&worker->totals))
    skip_message(entry_path(worker->path, length, name), reason ? reason : error_message(error));
  output_end();
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

static void add_entry_by_name(struct worker *worker, size_t length, const wchar_t *name) {
  struct stat_basic info;
  BOOL found = stat_by_name(worker->path, 3, &info, sizeof info);
  DWORD error = found ? 0 : GetLastError();
  int gone = !found && vanished(error);
  worker->path[length] = 0;
  if (!found) {
    if (!gone) skip(worker, length, name, error, NULL);
    return;
  }
  uint64_t ino, ino_high, allocated = (uint64_t)info.allocation_size.QuadPart;
  memcpy(&ino, info.file_id_128, sizeof ino);
  memcpy(&ino_high, info.file_id_128 + 8, sizeof ino_high);
  count_file(&worker->totals, (uint64_t)info.volume_serial_number.QuadPart, ino, ino_high, info.number_of_links,
             allocated, allocated, 0, NULL);
}

static void add_entry(struct worker *worker, size_t length, const wchar_t *name, DWORD attributes) {
  if (stat_by_name && !refcounted &&
      !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))) {
    add_entry_by_name(worker, length, name);
    return;
  }
  HANDLE file = CreateFileW(worker->path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  worker->path[length] = 0;
  if (file == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (!vanished(error)) skip(worker, length, name, error, NULL);
    return;
  }
  BY_HANDLE_FILE_INFORMATION info;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandle(file, &info) ||
      !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard)) {
    DWORD error = GetLastError();
    int gone = vanished(error);
    CloseHandle(file);
    if (!gone) skip(worker, length, name, error, NULL);
    return;
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
  count_file(&worker->totals, volume, ino, ino_high, info.nNumberOfLinks, allocated, private_size, shared, NULL);
}

/* Reparse points whose tag is a name surrogate (symbolic links, junctions, volume mount points) name another file
 * and are not followed. Other reparse points, such as cloud placeholders, are ordinary files and folders here. Files
 * are opened for FILE_READ_ATTRIBUTES only, an attribute-only open that placeholder filters are expected not to turn
 * into a recall: https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/placeholders_guidance
 * Subdirectories are queued by their full path for any worker to list. */
static int walk(struct worker *worker, size_t length, int depth) {
  wchar_t *path = worker->path;
  if (length + 2 >= PATH_CAPACITY) return fail(ERROR_FILENAME_EXCED_RANGE);
  if (path[length - 1] != L'\\') path[length++] = L'\\';
  path[length] = L'*';
  path[length + 1] = 0;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW(path, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
  path[length] = 0;
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return 0;
    if (depth == 0) return fail(error);
    if (!vanished(error)) skip(worker, length, NULL, error, NULL);
    return 0;
  }
  if (progress_begin()) progress_end(entry_path(path, length, NULL));
  uint64_t files = 0;
  do {
    const wchar_t *name = data.cFileName;
    if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
    DWORD attributes = data.dwFileAttributes;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(data.dwReserved0)) continue;
    size_t name_length = wcslen(name);
    if (length + name_length + 3 >= PATH_CAPACITY) {
      skip(worker, length, name, ERROR_FILENAME_EXCED_RANGE, NULL);
      continue;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      memcpy(path + length, name, (name_length + 1) * sizeof *name);
      add_entry(worker, length, name, attributes);
      files++;
    } else if (depth + 1 >= MAX_DEPTH)
      skip(worker, length, name, 0, "more than " MAX_DEPTH_TEXT " nested directories");
    else {
      if (pause_at) {
        char utf8[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL) && strcmp(utf8, pause_at) == 0) {
          output_begin();
          test_pause(entry_path(path, length, name));
          output_end();
        }
      }
      size_t child_length = length + name_length;
      struct item *item = malloc(sizeof *item + (child_length + 1) * sizeof *path);
      if (!item) out_of_memory();
      item->path = (wchar_t *)(item + 1);
      item->length = child_length;
      item->depth = depth + 1;
      memcpy(item->path, path, length * sizeof *path);
      memcpy(item->path + length, name, (name_length + 1) * sizeof *name);
      pool_push(item);
    }
  } while (FindNextFileW(find, &data));
  DWORD error = GetLastError();
  int gone = error != ERROR_NO_MORE_FILES && vanished(error);
  FindClose(find);
  pool_seen(files);
  if (error == ERROR_NO_MORE_FILES) return 0;
  if (depth == 0) return fail(error);
  if (!gone) skip(worker, length, NULL, error, NULL);
  return 0;
}

static int walk_queued(void *argument, void *data) {
  struct worker *worker = argument;
  struct item *item = data;
  memcpy(worker->path, item->path, (item->length + 1) * sizeof *item->path);
  return walk(worker, item->length, item->depth);
}

static size_t extended_path(const char *utf8) {
  wchar_t given[PATH_CAPACITY], full[PATH_CAPACITY];
  if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, given, PATH_CAPACITY)) return (size_t)fail(GetLastError());
  if (wcsncmp(given, L"\\\\?\\", 4) == 0) {
    wcscpy(measured, given);
    return wcslen(measured);
  }
  DWORD length = GetFullPathNameW(given, PATH_CAPACITY, full, NULL);
  if (length == 0 || length >= PATH_CAPACITY - 8) return (size_t)fail(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
  if (full[0] == L'\\' && full[1] == L'\\') {
    wcscpy(measured, L"\\\\?\\UNC\\");
    wcscat(measured, full + 2);
  } else {
    wcscpy(measured, L"\\\\?\\");
    wcscat(measured, full);
  }
  return wcslen(measured);
}

int measure(const char *utf8, struct totals *totals) {
  size_t length = extended_path(utf8);
  if (length == (size_t)-1) return -1;
  while (length > 7 && measured[length - 1] == L'\\' && measured[length - 2] != L':') measured[--length] = 0;
  root_utf8 = utf8;
  root_length = length;
  HANDLE root = CreateFileW(measured, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
  if (wcsncmp(measured, L"\\\\?\\UNC\\", 8) == 0) return UNSUPPORTED;
  if (!GetVolumePathNameW(measured, volume, MAX_PATH + 1)) return fail(GetLastError());
  if (GetDriveTypeW(volume) == DRIVE_REMOTE) return UNSUPPORTED;
  refcounted = (flags & FILE_SUPPORTS_BLOCK_REFCOUNTING) != 0;
  if (!stat_by_name && !getenv("REAL_SIZE_NO_STAT_BY_NAME"))
    stat_by_name = (stat_by_name_function)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                         "GetFileInformationByName");
  if (!last_nt_status)
    last_nt_status =
        (nt_status_function)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetLastNtStatus");
  if (refcounted) {
    DWORD sectors, bytes, free_clusters, clusters;
    if (!GetDiskFreeSpaceW(volume, &sectors, &bytes, &free_clusters, &clusters)) return fail(GetLastError());
    cluster_size = (uint64_t)sectors * bytes;
  }
  int count = pool_threads();
  struct worker *workers = calloc((size_t)count, sizeof *workers);
  if (!workers) out_of_memory();
  for (int i = 0; i < count; i++) workers[i].totals.exact = 1;
  memcpy(workers[0].path, measured, (length + 1) * sizeof *measured);
  pool_begin(totals);
  int result = walk(&workers[0], length, 0);
  result = pool_run(workers, sizeof *workers, count, walk_queued, result);
  free(workers);
  return result;
}

const char *measure_error(void) { return error_message(last_error); }

const char *environment(const char *name) { return getenv(name); }

int stderr_is_terminal(void) {
  DWORD mode;
  return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
}

int terminal_width(void) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) return 0;
  return info.srWindow.Right - info.srWindow.Left + 1;
}

uint64_t monotonic_ms(void) { return GetTickCount64(); }

static UINT console_code_page;

static void restore_console(void) { SetConsoleOutputCP(console_code_page); }

#if defined(_M_X64) && !defined(_M_ARM64EC)
/* The npm and pnpm launchers of `real-size-windows` start the x64 binary. On an arm64 machine it starts the arm64
 * binary next to it, prebuilds\win32-arm64\real-size.exe, with the same arguments, and returns its exit status.
 * IsWow64Process2 reports the machine's native architecture to an emulated x64 process:
 * https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-iswow64process2 */
typedef BOOL(WINAPI *wow64_function)(HANDLE, USHORT *, USHORT *);

static int run_native(DWORD *status) {
  wow64_function wow64 =
      (wow64_function)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
  USHORT process_machine, native_machine;
  if (!wow64 || !wow64(GetCurrentProcess(), &process_machine, &native_machine) ||
      native_machine != IMAGE_FILE_MACHINE_ARM64)
    return 0;
  static wchar_t target[PATH_CAPACITY];
  DWORD length = GetModuleFileNameW(NULL, target, PATH_CAPACITY);
  const wchar_t suffix[] = L"\\win32-x64\\real-size.exe";
  size_t suffix_length = wcslen(suffix);
  if (length == 0 || length >= PATH_CAPACITY || length < suffix_length ||
      _wcsicmp(target + length - suffix_length, suffix) != 0)
    return 0;
  wcscpy(target + length - suffix_length, L"\\win32-arm64\\real-size.exe");
  if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) return 0;
  const wchar_t *arguments = GetCommandLineW();
  if (*arguments == L'"') {
    arguments++;
    while (*arguments && *arguments != L'"') arguments++;
    if (*arguments) arguments++;
  } else
    while (*arguments && *arguments != L' ' && *arguments != L'\t') arguments++;
  size_t size = wcslen(target) + wcslen(arguments) + 3;
  wchar_t *command = malloc(size * sizeof *command);
  if (!command) return 0;
  swprintf(command, size, L"\"%ls\"%ls", target, arguments);
  STARTUPINFOW startup = {sizeof startup};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION child;
  if (!CreateProcessW(target, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) return 0;
  WaitForSingleObject(child.hProcess, INFINITE);
  if (!GetExitCodeProcess(child.hProcess, status)) *status = 1;
  return 1;
}
#endif

int wmain(int argc, wchar_t **wide) {
#if defined(_M_X64) && !defined(_M_ARM64EC)
  DWORD status;
  if (run_native(&status)) return (int)status;
#endif
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
