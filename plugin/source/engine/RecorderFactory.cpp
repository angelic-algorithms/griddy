#include "ImageSequenceRecorder.h"

namespace griddy
{

#if JUCE_MAC
std::unique_ptr<Recorder> createAVFoundationRecorder(); // AVFoundationRecorder.mm
#endif

std::unique_ptr<Recorder> createRecorder()
{
   #if JUCE_MAC
    return createAVFoundationRecorder();
   #else
    return std::make_unique<ImageSequenceRecorder>();
   #endif
}

} // namespace griddy
