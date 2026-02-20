#include "singleton.hpp"
#include <filesystem>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#else
#  include <sys/file.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

SingletonLock::SingletonLock(const std::string& lock_path) {
    // Ensure parent directory exists
    std::filesystem::create_directories(
        std::filesystem::path(lock_path).parent_path());

#ifdef _WIN32
    fd_ = _open(lock_path.c_str(), _O_CREAT | _O_RDWR, 0644);
    if (fd_ < 0) return;
    HANDLE h = (HANDLE)_get_osfhandle(fd_);
    OVERLAPPED ov = {};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &ov)) {
        _close(fd_);
        fd_ = -1;
    }
#else
    fd_ = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) return;
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        close(fd_);
        fd_ = -1;
    }
#endif
}

SingletonLock::~SingletonLock() {
    if (fd_ < 0) return;
#ifdef _WIN32
    _close(fd_);
#else
    close(fd_);
#endif
    // flock is released automatically when fd is closed
}
