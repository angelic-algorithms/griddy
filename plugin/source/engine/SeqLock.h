#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace griddy
{

/** One writer, any number of readers, wait free for the writer, never blocks a reader for long.

    The value is stored as relaxed atomic words so there is no data race in the C++ sense
    while a reader is retrying around a write. T must be trivially copyable.
*/
template <typename T>
class SeqLock
{
    static_assert (std::is_trivially_copyable_v<T>);
    static constexpr size_t numWords = (sizeof (T) + 7) / 8;

public:
    SeqLock() { write (T {}); }

    void write (const T& value) noexcept
    {
        uint64_t tmp[numWords] = {};
        std::memcpy (tmp, &value, sizeof (T));

        const auto s = seq.load (std::memory_order_relaxed);
        seq.store (s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);

        for (size_t i = 0; i < numWords; ++i)
            words[i].store (tmp[i], std::memory_order_relaxed);

        seq.store (s + 2, std::memory_order_release);
    }

    T read() const noexcept
    {
        uint64_t tmp[numWords];

        for (;;)
        {
            const auto s1 = seq.load (std::memory_order_acquire);

            if (s1 & 1u)
                continue;

            for (size_t i = 0; i < numWords; ++i)
                tmp[i] = words[i].load (std::memory_order_relaxed);

            std::atomic_thread_fence (std::memory_order_acquire);

            if (seq.load (std::memory_order_relaxed) == s1)
                break;
        }

        T out;
        std::memcpy (&out, tmp, sizeof (T));
        return out;
    }

private:
    std::atomic<uint32_t> seq { 0 };
    std::atomic<uint64_t> words[numWords];
};

} // namespace griddy
