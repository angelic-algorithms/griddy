#include "CaptureSource.h"

namespace griddy
{

bool hostAllowsCameraAccess (juce::String& reason)
{
    // Real camera capture is only implemented for macOS so far. Windows would use
    // Media Foundation and Linux V4L2; until then, test patterns only.
    reason = "Camera capture isn't built for this platform yet, so only test patterns are available.";
    return false;
}

} // namespace griddy
