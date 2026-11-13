#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace griddy::harness
{

/** How a hub scenario reports back to the harness. */
struct Report
{
    std::function<void (bool, const juce::String&)> check;
    std::function<void (const juce::String&)> say;
};

void hubRecordThenPlay (const Report& report);
void hubSessions (const Report& report);
void hubResetSlot (const Report& report);
void hubUiStatus (const Report& report);

} // namespace griddy::harness
