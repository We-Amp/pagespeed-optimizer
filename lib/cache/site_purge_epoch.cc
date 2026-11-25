// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/cache/site_purge_epoch.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>

namespace pagespeed {

namespace {

constexpr std::string_view kPrefix = "pspe1 ";
constexpr size_t kMaxFileBytes = 64;

// fcntl record locks belong to the PROCESS, so two threads of one process
// both "hold" the lock; this mutex is what serialises them.
std::mutex& ProcessEpochMutex() {
  static std::mutex m;
  return m;
}

std::string Render(uint64_t epoch) {
  std::string out(kPrefix);
  out += std::to_string(epoch);
  out += '\n';
  return out;
}

#ifdef _WIN32

std::wstring Wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                      static_cast<int>(s.size()), w.data(), n);
  return w;
}

// The file's content; `*missing` is set when there is no file.
std::expected<std::string, SitePurgeEpochError> ReadAll(const std::string& path,
                                                        bool* missing) {
  *missing = false;
  const std::wstring wpath = Wide(path);
  if (wpath.empty()) return std::unexpected(SitePurgeEpochError::kIo);
  // FILE_SHARE_DELETE so a writer's rename over the file is not blocked.
  HANDLE h = CreateFileW(
      wpath.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      *missing = true;
      return std::string();
    }
    return std::unexpected(SitePurgeEpochError::kIo);
  }
  char buf[kMaxFileBytes + 1];
  DWORD got = 0;
  const BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
  CloseHandle(h);
  if (!ok) return std::unexpected(SitePurgeEpochError::kIo);
  return std::string(buf, got);
}

class FileLock {
 public:
  explicit FileLock(const std::string& path) {
    const std::wstring wpath = Wide(path);
    if (wpath.empty()) return;
    h_ = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                     nullptr);
    if (h_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    if (!LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov)) {
      CloseHandle(h_);
      h_ = INVALID_HANDLE_VALUE;
      return;
    }
    locked_ = true;
  }
  ~FileLock() {
    if (h_ == INVALID_HANDLE_VALUE) return;
    if (locked_) {
      OVERLAPPED ov{};
      UnlockFileEx(h_, 0, 1, 0, &ov);
    }
    CloseHandle(h_);
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  bool locked() const { return locked_; }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  bool locked_ = false;
};

bool WriteTempAndReplace(const std::string& tmp, const std::string& path,
                         const std::string& content) {
  const std::wstring wtmp = Wide(tmp);
  const std::wstring wpath = Wide(path);
  if (wtmp.empty() || wpath.empty()) return false;
  HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const BOOL wrote_ok =
      WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &wrote,
                nullptr) &&
      wrote == content.size() && FlushFileBuffers(h);
  CloseHandle(h);
  if (!wrote_ok) {
    DeleteFileW(wtmp.c_str());
    return false;
  }
  // A reader opens with FILE_SHARE_DELETE, so the replace is not blocked by
  // one; retry briefly on a transient sharing conflict from anything else.
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (MoveFileExW(wtmp.c_str(), wpath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    const DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
    Sleep(5);
  }
  DeleteFileW(wtmp.c_str());
  return false;
}

#else  // !_WIN32

std::expected<std::string, SitePurgeEpochError> ReadAll(const std::string& path,
                                                        bool* missing) {
  *missing = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      *missing = true;
      return std::string();
    }
    return std::unexpected(SitePurgeEpochError::kIo);
  }
  std::string out;
  char buf[kMaxFileBytes + 1];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return std::unexpected(SitePurgeEpochError::kIo);
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > kMaxFileBytes) break;  // not ours; Parse refuses it
  }
  ::close(fd);
  return out;
}

class FileLock {
 public:
  explicit FileLock(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                 0660);
    if (fd_ < 0) return;
    // Exact mode for a peer in the cache group; fails harmlessly when this
    // process did not create the file.
    (void)::fchmod(fd_, 0660);
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    while (::fcntl(fd_, F_SETLKW, &fl) != 0) {
      if (errno != EINTR) return;
    }
    locked_ = true;
  }
  ~FileLock() {
    if (fd_ < 0) return;
    if (locked_) {
      struct flock fl{};
      fl.l_type = F_UNLCK;
      fl.l_whence = SEEK_SET;
      (void)::fcntl(fd_, F_SETLK, &fl);
    }
    ::close(fd_);
  }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  bool locked() const { return locked_; }

 private:
  int fd_ = -1;
  bool locked_ = false;
};

bool WriteTempAndReplace(const std::string& tmp, const std::string& path,
                         const std::string& content) {
  // A stale .tmp is a crashed writer's; we hold the lock, so it is not live.
  ::unlink(tmp.c_str());
  const int fd = ::open(tmp.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        0660);
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, 0660) == 0;
  size_t written = 0;
  while (ok && written < content.size()) {
    const ssize_t n =
        ::write(fd, content.data() + written, content.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    written += static_cast<size_t>(n);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  // Make the rename itself durable.
  const std::string::size_type slash = path.rfind('/');
  const std::string dir =
      slash == std::string::npos ? std::string(".") : path.substr(0, slash);
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

#endif  // _WIN32

std::expected<uint64_t, SitePurgeEpochError> ReadValue(
    const std::string& path) {
  bool missing = false;
  auto content = ReadAll(path, &missing);
  if (!content.has_value()) return std::unexpected(content.error());
  if (missing) return 0;
  return ParseSitePurgeEpoch(*content);
}

}  // namespace

bool IsSitePurgeEpochSite(std::string_view site) {
  if (site.size() != 64) return false;
  for (char c : site) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

std::string SitePurgeEpochPath(std::string_view volume_path,
                               std::string_view site) {
  if (volume_path.empty() || !IsSitePurgeEpochSite(site)) return std::string();
  std::string out(volume_path);
  out += ".purge-epoch.";
  out += site;
  return out;
}

std::expected<uint64_t, SitePurgeEpochError> ParseSitePurgeEpoch(
    std::string_view content) {
  if (content.size() > kMaxFileBytes || content.size() < kPrefix.size() + 2 ||
      content.substr(0, kPrefix.size()) != kPrefix || content.back() != '\n') {
    return std::unexpected(SitePurgeEpochError::kCorrupted);
  }
  const std::string_view digits =
      content.substr(kPrefix.size(), content.size() - kPrefix.size() - 1);
  if (digits.empty() || (digits.size() > 1 && digits[0] == '0')) {
    return std::unexpected(SitePurgeEpochError::kCorrupted);
  }
  uint64_t value = 0;
  const auto [end, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc() || end != digits.data() + digits.size()) {
    return std::unexpected(SitePurgeEpochError::kCorrupted);
  }
  return value;
}

std::expected<uint64_t, SitePurgeEpochError> ReadSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site) {
  const std::string path = SitePurgeEpochPath(volume_path, site);
  if (path.empty()) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  // No lock: the value file is only ever replaced whole, by a rename.
  return ReadValue(path);
}

std::expected<void, SitePurgeEpochError> WriteSitePurgeEpochFile(
    std::string_view volume_path, std::string_view site,
    uint64_t epoch_seconds, uint64_t now_seconds) {
  const std::string path = SitePurgeEpochPath(volume_path, site);
  if (path.empty()) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  if (epoch_seconds > now_seconds + kSitePurgeEpochSkewSeconds) {
    return std::unexpected(SitePurgeEpochError::kInvalidArgument);
  }
  std::lock_guard in_process(ProcessEpochMutex());
  FileLock lock(path + ".lock");
  if (!lock.locked()) return std::unexpected(SitePurgeEpochError::kIo);
  auto current = ReadValue(path);
  uint64_t to_write = epoch_seconds;
  if (!current.has_value()) {
    if (current.error() != SitePurgeEpochError::kCorrupted) {
      return std::unexpected(current.error());
    }
    // The stored value is unknowable, so it may have been anything up to
    // now: replace it with the larger of the caller's value and the present,
    // which fences every entry stored so far (fail closed) and heals the
    // file instead of leaving the site fenced on it forever.
    to_write = epoch_seconds > now_seconds ? epoch_seconds : now_seconds;
  } else if (epoch_seconds <= *current) {
    return {};
  }
  if (!WriteTempAndReplace(path + ".tmp", path, Render(to_write))) {
    return std::unexpected(SitePurgeEpochError::kIo);
  }
  return {};
}

}  // namespace pagespeed
