#pragma once

#include <atomic>
#include <cstdint>

namespace griddy
{

/** What the DAW told us about its transport on the most recent audio block.

    The audio thread writes it, the video thread and the UI read it. Every
    field is a plain value so a whole snapshot can be copied in one go.
*/
struct TransportSnapshot
{
    double bpm = 120.0;
    double ppqPosition = 0.0;         // song position in quarter notes
    double ppqLastBarStart = 0.0;     // quarter note position where the current bar began
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    bool isPlaying = false;
    bool isRecording = false;
    bool hostProvidedPosition = false; // false when the host gave us no play head at all
    int64_t updateCounter = 0;         // bumps every audio block, so readers can tell it's fresh
};

/** A single writer, many reader snapshot using a sequence lock.

    Why not a mutex? The audio thread must never block. A seqlock lets the
    writer always go straight through, and a reader simply retries if it
    happened to read while a write was in progress.
*/
class TransportState
{
public:
    void write (const TransportSnapshot& s) noexcept
    {
        const auto seq = sequence.load (std::memory_order_relaxed);
        sequence.store (seq + 1, std::memory_order_relaxed);   // odd: write in progress
        std::atomic_thread_fence (std::memory_order_release);

        storeFields (s);

        std::atomic_thread_fence (std::memory_order_release);
        sequence.store (seq + 2, std::memory_order_relaxed);   // even: stable
    }

    TransportSnapshot read() const noexcept
    {
        for (;;)
        {
            const auto before = sequence.load (std::memory_order_acquire);

            if ((before & 1) != 0)
                continue;

            auto s = loadFields();
            std::atomic_thread_fence (std::memory_order_acquire);

            if (sequence.load (std::memory_order_relaxed) == before)
                return s;
        }
    }

private:
    // Each field is its own relaxed atomic so the copy is free of data races
    // even when a reader overlaps a writer (the sequence check throws that copy away).
    void storeFields (const TransportSnapshot& s) noexcept
    {
        bpm.store (s.bpm, std::memory_order_relaxed);
        ppq.store (s.ppqPosition, std::memory_order_relaxed);
        barStart.store (s.ppqLastBarStart, std::memory_order_relaxed);
        num.store (s.timeSigNumerator, std::memory_order_relaxed);
        den.store (s.timeSigDenominator, std::memory_order_relaxed);
        playing.store (s.isPlaying, std::memory_order_relaxed);
        recording.store (s.isRecording, std::memory_order_relaxed);
        hasPos.store (s.hostProvidedPosition, std::memory_order_relaxed);
        counter.store (s.updateCounter, std::memory_order_relaxed);
    }

    TransportSnapshot loadFields() const noexcept
    {
        TransportSnapshot s;
        s.bpm = bpm.load (std::memory_order_relaxed);
        s.ppqPosition = ppq.load (std::memory_order_relaxed);
        s.ppqLastBarStart = barStart.load (std::memory_order_relaxed);
        s.timeSigNumerator = num.load (std::memory_order_relaxed);
        s.timeSigDenominator = den.load (std::memory_order_relaxed);
        s.isPlaying = playing.load (std::memory_order_relaxed);
        s.isRecording = recording.load (std::memory_order_relaxed);
        s.hostProvidedPosition = hasPos.load (std::memory_order_relaxed);
        s.updateCounter = counter.load (std::memory_order_relaxed);
        return s;
    }

    std::atomic<uint64_t> sequence { 0 };
    std::atomic<double> bpm { 120.0 }, ppq { 0.0 }, barStart { 0.0 };
    std::atomic<int> num { 4 }, den { 4 };
    std::atomic<bool> playing { false }, recording { false }, hasPos { false };
    std::atomic<int64_t> counter { 0 };
};

} // namespace griddy
