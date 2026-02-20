#pragma once
#include <string>

// RAII singleton lock. Ensures only one tccp instance runs at a time.
// Uses flock() on Unix, LockFileEx() on Windows.
// Lock is automatically released when the process exits (even on crash).
class SingletonLock {
public:
    // Attempts to acquire the lock. Check held() after construction.
    explicit SingletonLock(const std::string& lock_path);
    ~SingletonLock();

    SingletonLock(const SingletonLock&) = delete;
    SingletonLock& operator=(const SingletonLock&) = delete;

    // Returns true if this instance holds the lock.
    bool held() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};
