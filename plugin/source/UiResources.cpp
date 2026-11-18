#include "UiResources.h"

#include <GriddyUiData.h>

namespace griddy
{

juce::String mimeTypeForPath (const juce::String& path)
{
    static const std::map<juce::String, juce::String> types {
        { "html", "text/html" },        { "js", "text/javascript" }, { "css", "text/css" },
        { "json", "application/json" }, { "svg", "image/svg+xml" },  { "png", "image/png" },
        { "jpg", "image/jpeg" },        { "woff2", "font/woff2" },   { "map", "application/json" }
    };

    const auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();

    if (const auto it = types.find (ext); it != types.end())
        return it->second;

    return "application/octet-stream";
}

std::optional<juce::WebBrowserComponent::Resource> getEmbeddedUiResource (const juce::String& url)
{
    // "/" means the page itself. Everything else is a flat file name: the UI
    // build emits index.html, app.js and app.css with no sub folders, because
    // juce_add_binary_data keeps only file names, not paths.
    auto wanted = url == "/" || url.isEmpty() ? juce::String ("index.html")
                                               : url.fromLastOccurrenceOf ("/", false, false);
    wanted = wanted.upToFirstOccurrenceOf ("?", false, false);

    const auto findFile = [] (const juce::String& fileName) -> std::optional<juce::WebBrowserComponent::Resource>
    {
        for (int i = 0; i < GriddyUiData::namedResourceListSize; ++i)
        {
            if (fileName != GriddyUiData::originalFilenames[i])
                continue;

            int size = 0;

            if (const auto* data = GriddyUiData::getNamedResource (GriddyUiData::namedResourceList[i], size))
            {
                std::vector<std::byte> bytes ((size_t) size);
                std::memcpy (bytes.data(), data, (size_t) size);
                return juce::WebBrowserComponent::Resource { std::move (bytes), mimeTypeForPath (fileName) };
            }
        }

        return std::nullopt;
    };

    if (auto r = findFile (wanted))
        return r;

    // No built UI embedded: show the fallback page that says how to build it.
    if (wanted == "index.html")
        return findFile ("fallback.html");

    return std::nullopt;
}

} // namespace griddy
