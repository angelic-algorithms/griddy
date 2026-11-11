#include "TestPatternSource.h"

namespace griddy
{

#if JUCE_MAC
// AVFoundationCapture.mm
std::vector<CaptureDeviceInfo> listAVFoundationDevices();
std::unique_ptr<CaptureSource> createAVFoundationSource (const juce::String& uniqueId, FrameStore& store);
#endif

static constexpr int numTestPatterns = 4;

std::vector<CaptureDeviceInfo> listCaptureDevices()
{
    std::vector<CaptureDeviceInfo> devices;

    for (int i = 1; i <= numTestPatterns; ++i)
        devices.push_back ({ TestPatternSource::idFor (i), "Test pattern " + juce::String (i), "test" });

   #if JUCE_MAC
    // Only enumerate real cameras when the host allows it: even listing them can
    // trigger the permission check on some macOS versions.
    if (juce::String reason; hostAllowsCameraAccess (reason))
        for (auto& d : listAVFoundationDevices())
            devices.push_back (std::move (d));
   #endif

    return devices;
}

std::unique_ptr<CaptureSource> createCaptureSource (const juce::String& id, FrameStore& store)
{
    if (id.startsWith ("test:"))
    {
        const auto n = id.fromFirstOccurrenceOf (":", false, false).getIntValue();

        if (n >= 1 && n <= numTestPatterns)
            return std::make_unique<TestPatternSource> (store, n);

        return nullptr;
    }

   #if JUCE_MAC
    if (id.startsWith ("av:"))
    {
        if (juce::String reason; ! hostAllowsCameraAccess (reason))
            return nullptr;

        return createAVFoundationSource (id.fromFirstOccurrenceOf ("av:", false, false), store);
    }
   #endif

    return nullptr;
}

} // namespace griddy
