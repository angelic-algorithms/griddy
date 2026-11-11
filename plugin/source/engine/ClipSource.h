#pragma once

#include <cstdint>

namespace griddy
{

/** How a position was found, best first. The SyncController prefers the highest kind that is locked. */
enum class SyncKind : uint8_t
{
    none = 0,
    hostPosition, // the take is assumed to sit where it was recorded
    fingerprint,  // audio matched against the take's reference audio
    embedded      // Griddy's own sync signal decoded from the audio
};

/** "At host time hostSeconds, the audio you hear is takeSeconds into take takeId."

    Between fixes the SyncController extrapolates with the host clock. Everything is in
    seconds, so a fix survives sample rate and tempo changes.
*/
struct ClipFix
{
    int32_t takeId = -1;
    double takeSeconds = 0.0;
    double hostSeconds = 0.0;
    float confidence = 0.0f; // 0..1, source specific
    SyncKind kind = SyncKind::none;
    bool locked = false;
    bool warped = false; // evidence that the audio is time stretched; see the design note
    uint32_t serial = 0; // changes whenever a new fix is published
};

/** Something that can work out where in a take the audio of one slot currently is.

    feed() is called from the audio thread: it must copy the samples into a lock free ring
    and return. process() does the actual analysis on the sync worker thread.
*/
class ClipSource
{
public:
    virtual ~ClipSource() = default;

    virtual SyncKind kind() const noexcept = 0;

    /** Audio thread. mono is the slot's audio mixed down to one channel.
        hostSample is the host's time in samples of mono[0]; playing is the transport state. */
    virtual void feed (const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept = 0;

    /** Sync worker thread. Runs whatever analysis is pending. */
    virtual void process() = 0;

    /** Any thread. The newest fix, or a fix with locked == false. */
    virtual ClipFix latest() const noexcept = 0;
};

} // namespace griddy
