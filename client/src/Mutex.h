#ifndef CLIENT_SRC_MUTEX_H
#define CLIENT_SRC_MUTEX_H

#include <mutex>
#include "spdlog/spdlog.h"

template <typename T> class LockGuard
{
  public:
    LockGuard(T *inner, std::mutex &m) noexcept : inner(inner), m(m)
    {
        m.lock();
    }

    ~LockGuard()
    {
        m.unlock();
    }

    explicit operator bool() const noexcept
    {
        return get() != nullptr;
    }

    T *operator*() const noexcept
    {
        return get();
    }

    T *operator->() const noexcept
    {
        return get();
    }

    T *get() const noexcept
    {
        return inner;
    }

  private:
    T *inner;
    std::mutex &m;
};

template <typename T> class Mutex
{
  public:
    Mutex() noexcept : inner(nullptr)
    {
    }

    Mutex(T *inner) noexcept : inner(inner)
    {
    }

    ~Mutex()
    {
        if (inner != nullptr)
        {
            delete inner;
        }
    }

    LockGuard<T> lock() noexcept
    {
        return LockGuard<T>(inner, m);
    }

    void replace(T *other) noexcept
    {
        const std::lock_guard<std::mutex> lock(m);
        if (inner != nullptr)
        {
            delete inner;
        }
        inner = other;
    }

  private:
    T *inner;
    std::mutex m;
};

#endif
