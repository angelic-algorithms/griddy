#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

namespace griddy
{

/** Serves the built web UI (ui/dist, embedded into the binary at build time)
    to the WebView. url is the path part only, e.g. "/" or "/app.js". */
std::optional<juce::WebBrowserComponent::Resource> getEmbeddedUiResource (const juce::String& url);

/** Mime type from a file extension, for the resource provider. */
juce::String mimeTypeForPath (const juce::String& path);

} // namespace griddy
