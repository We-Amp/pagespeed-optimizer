// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// POSIX compatibility layer for Windows (MSVC).
// Include this instead of POSIX headers in cross-platform code.
//
// This header lives in lib/base/ so both lib/ and src/ can use it.
// src/worker/posix_compat.h is a thin wrapper that includes this file.
//
// IMPORTANT: Do NOT use macros to remap POSIX names — they conflict with
// MSVC STL internals (e.g., std::filebuf::open). Instead, use the ps_*
// inline wrappers provided here.

#ifndef LIB_BASE_POSIX_COMPAT_H_
#define LIB_BASE_POSIX_COMPAT_H_

#ifdef _WIN32

#include <BaseTsd.h>  // SSIZE_T
#include <direct.h>   // _mkdir
#include <fcntl.h>
#include <io.h>       // _open, _close, _write, _read, _unlink, _get_osfhandle
#include <process.h>  // _getpid
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>

#include <cerrno>

typedef SSIZE_T ssize_t;
typedef int pid_t;

// O_NOFOLLOW is a no-op on Windows; callers must use
// ps_open_exclusive_nofollow() for symlink safety.
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

// Use FlushFileBuffers() instead of _commit() for reliable durability on
// all Windows file systems (network shares, USB, etc.).
inline int fsync(int fd) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return -1;
  }
  if (!FlushFileBuffers(h)) {
    errno = EIO;
    return -1;
  }
  return 0;
}

// rename() on Windows fails if destination exists; this wrapper uses
// MoveFileExA(MOVEFILE_REPLACE_EXISTING) to match POSIX semantics.
inline int ps_rename(const char* src, const char* dst) {
  return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

// Opens a new file for writing, refusing to follow symlinks.
// Combines O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW semantics.
// Windows: CreateFileA with CREATE_NEW + FILE_FLAG_OPEN_REPARSE_POINT.
// Returns file descriptor on success, -1 on failure (errno set).
inline int ps_open_exclusive_nofollow(const char* path, int mode) {
  (void)mode;  // Windows ignores POSIX permission bits here.
  HANDLE h = CreateFileA(path, GENERIC_WRITE,
                         0,        // exclusive — no sharing
                         nullptr,  // default security
                         CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    errno = (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) ? EEXIST
                                                                      : EACCES;
    return -1;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), O_WRONLY);
  if (fd < 0) {
    CloseHandle(h);
    return -1;
  }
  return fd;
}

inline ssize_t ps_write(int fd, const void* buf, size_t count) {
  return _write(fd, buf, static_cast<unsigned int>(count));
}

inline int ps_close(int fd) { return _close(fd); }

inline int ps_unlink(const char* path) { return _unlink(path); }

#else  // !_WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

inline int ps_rename(const char* src, const char* dst) {
  return std::rename(src, dst);
}

inline int ps_open_exclusive_nofollow(const char* path, int mode) {
  return ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
}

inline ssize_t ps_write(int fd, const void* buf, size_t count) {
  return ::write(fd, buf, count);
}

inline int ps_close(int fd) { return ::close(fd); }

inline int ps_unlink(const char* path) { return ::unlink(path); }

#endif  // _WIN32

#endif  // LIB_BASE_POSIX_COMPAT_H_
