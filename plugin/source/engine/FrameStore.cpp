#include "FrameStore.h"

namespace griddy
{

void FrameStore::push (const juce::String& sourceId, juce::Image image, double captureTimeSeconds)
{
    // Build the new frame outside the lock; only the pointer swap happens under it.
    auto frame = std::make_shared<VideoFrame>();
    frame->image = std::move (image);
    frame->captureTimeSeconds = captureTimeSeconds;

    std::shared_ptr<const VideoFrame> old;

    {
        const std::scoped_lock sl (lock);
        auto& slot = frames[sourceId];
        frame->serial = slot != nullptr ? slot->serial + 1 : 1;
        old = std::exchange (slot, std::move (frame));
    }

    // old is released here, outside the lock, so freeing a big image never blocks a reader.
}

std::shared_ptr<const VideoFrame> FrameStore::latest (const juce::String& sourceId) const
{
    const std::scoped_lock sl (lock);

    if (const auto it = frames.find (sourceId); it != frames.end())
        return it->second;

    return nullptr;
}

void FrameStore::remove (const juce::String& sourceId)
{
    std::shared_ptr<const VideoFrame> old;

    {
        const std::scoped_lock sl (lock);

        if (const auto it = frames.find (sourceId); it != frames.end())
        {
            old = std::move (it->second);
            frames.erase (it);
        }
    }
}

} // namespace griddy
