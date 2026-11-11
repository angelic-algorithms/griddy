#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace griddy
{

/** Single producer, single consumer ring of trivially copyable items.

    The producer may be the audio thread: write() never allocates or locks. If the
    consumer falls behind, write() drops what does not fit and counts it, because
    dropping analysis audio is better than blocking the audio thread.
    Positions are absolute counts since construction, so the consumer can tell
    how many items were lost.
*/
template <typename T>
class SpscRing
{
public:
    explicit SpscRing (size_t minimumCapacity)
    {
        size_t c = 16;
        while (c < minimumCapacity)
            c <<= 1;

        buffer.resize (c);
        mask = c - 1;
    }

    size_t capacity() const noexcept { return buffer.size(); }

    /** Producer. Returns how many items were stored (the rest were dropped). */
    size_t write (const T* src, size_t n) noexcept
    {
        const auto w = head.load (std::memory_order_relaxed);
        const auto r = tail.load (std::memory_order_acquire);
        const auto freeSpace = buffer.size() - (w - r);
        const auto stored = n < freeSpace ? n : freeSpace;

        for (size_t i = 0; i < stored; ++i)
            buffer[(w + i) & mask] = src[i];

        head.store (w + stored, std::memory_order_release);

        if (stored < n)
            dropped.fetch_add (n - stored, std::memory_order_relaxed);

        return stored;
    }

    /** Consumer. Returns how many items were copied out. */
    size_t read (T* dst, size_t n) noexcept
    {
        const auto r = tail.load (std::memory_order_relaxed);
        const auto w = head.load (std::memory_order_acquire);
        const auto avail = w - r;
        const auto count = n < avail ? n : avail;

        for (size_t i = 0; i < count; ++i)
            dst[i] = buffer[(r + i) & mask];

        tail.store (r + count, std::memory_order_release);
        return count;
    }

    size_t available() const noexcept
    {
        return head.load (std::memory_order_acquire) - tail.load (std::memory_order_relaxed);
    }

    uint64_t numDropped() const noexcept { return dropped.load (std::memory_order_relaxed); }
    uint64_t numWritten() const noexcept { return head.load (std::memory_order_relaxed); }

private:
    std::vector<T> buffer;
    size_t mask = 0;
    std::atomic<size_t> head { 0 }, tail { 0 };
    std::atomic<uint64_t> dropped { 0 };
};

} // namespace griddy
