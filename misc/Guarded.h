#ifndef MISC_GUARDED_H
#define MISC_GUARDED_H

#include <memory>
#include <mutex>

namespace misc
{
// The lock around a Guarded: it holds the mutex for as long as this object is alive and gives access to T through
// it. operator bool says whether there is anything to read, because a Guarded can hold a null pointer.
//
// It cannot be copied: a second guard over the same mutex would unlock it twice. It cannot be moved either, so a
// guard stays where it was made, and the Guarded it points into has to outlive it - the reference to the mutex is
// what makes that a condition rather than something the type can check.
template <typename T> class LockGuard
{
  public:
    LockGuard(T *inner, std::mutex &m) : inner(inner), m(m)
    {
        m.lock();
    }

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

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

// A T with a mutex around it: this is not a lock, it is the object plus the mutex that guards it, which is why it
// is not called Mutex - that name is what std::mutex already holds. lock() gives a LockGuard valid only while that
// guard is alive, so the caller reads through it and the mutex is released when the guard goes out of scope.
//
// The T is held as a unique_ptr, so ownership is on the type: what was handed to replace() as a raw pointer could
// be owned by two places at once, and this cannot be. Guarded is not copyable or movable, because std::mutex is
// neither, so it has to live behind an indirection (a shared_ptr, not a std::vector<Guarded<T>>).
//
// Nothing here proves that the caller touches T while the lock is held: get() hands out T* and a caller can keep it
// past the guard. That part is a convention of the call sites, not something this type enforces.
//
// This header is in misc because it knows nothing about the client or the protocol - memory and mutex are all it
// needs, so a template that only guards a pointer can be used by any target that links fb_misc.
template <typename T> class Guarded
{
  public:
    Guarded() : inner(nullptr)
    {
    }

    explicit Guarded(std::unique_ptr<T> inner) : inner(std::move(inner))
    {
    }

    LockGuard<T> lock()
    {
        return LockGuard<T>(inner.get(), m);
    }

    // The T this was holding is freed by the assignment; the caller cannot keep a handle to it through this type.
    void replace(std::unique_ptr<T> other)
    {
        const std::lock_guard<std::mutex> lock(m);
        inner = std::move(other);
    }

  private:
    std::unique_ptr<T> inner;
    std::mutex m;
};
} // namespace misc

#endif
