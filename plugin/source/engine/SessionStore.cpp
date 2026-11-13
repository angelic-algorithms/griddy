#include "SessionStore.h"

#include <algorithm>
#include <set>

#if ! JUCE_WINDOWS
 #include <unistd.h>
#endif

namespace griddy::session
{

std::vector<int32_t> takeIdsOnDisk (const juce::File& sessionFolder)
{
    std::vector<int32_t> ids;

    for (const auto& slot : sessionFolder.findChildFiles (juce::File::findDirectories, false, "slot*"))
        for (const auto& take : slot.findChildFiles (juce::File::findDirectories, false, "*.gtake"))
            if (const auto id = take.getFileNameWithoutExtension().getIntValue(); id > 0)
                ids.push_back (id);

    std::sort (ids.begin(), ids.end());
    return ids;
}

juce::String encodeTakeIds (const std::vector<int32_t>& ids)
{
    juce::StringArray parts;
    for (auto id : ids)
        parts.add (juce::String (id));
    return parts.joinIntoString (",");
}

std::vector<int32_t> decodeTakeIds (const juce::String& text)
{
    std::vector<int32_t> ids;
    for (const auto& part : juce::StringArray::fromTokens (text, ",", ""))
        if (const auto id = part.trim().getIntValue(); id > 0)
            ids.push_back (id);

    std::sort (ids.begin(), ids.end());
    ids.erase (std::unique (ids.begin(), ids.end()), ids.end());
    return ids;
}

static bool linkOrCopy (const juce::File& from, const juce::File& to)
{
   #if ! JUCE_WINDOWS
    if (::link (from.getFullPathName().toRawUTF8(), to.getFullPathName().toRawUTF8()) == 0)
        return true;
   #endif
    return from.copyFileTo (to);
}

bool linkTake (const juce::File& from, const juce::File& to)
{
    if (! from.isDirectory() || ! to.createDirectory())
        return false;

    bool ok = true;
    for (const auto& item : juce::RangedDirectoryIterator (from, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto& f = item.getFile();
        const auto target = to.getChildFile (f.getFileName());

        if (item.isDirectory())
            ok = linkTake (f, target) && ok;
        else if (f.getFileName() == "take.json")
            ok = f.copyFileTo (target) && ok;
        else if (! f.getFileName().endsWith (".tmp") && ! f.getFileName().contains (".part"))
            ok = linkOrCopy (f, target) && ok;
    }

    return ok;
}

SessionChoice choose (const juce::File& sessionsRoot, const juce::String& savedId,
                      const std::optional<std::vector<int32_t>>& savedTakes)
{
    SessionChoice c;
    const auto fresh = [&c] (const juce::String& why)
    {
        c.id = juce::Uuid().toDashedString();
        c.kind = SessionChoice::Kind::fresh;
        c.reason = why;
        return c;
    };

    if (savedId.isEmpty() || juce::File::createLegalFileName (savedId) != savedId)
        return fresh ("the saved state names no session");

    if (! savedTakes.has_value())
        return fresh ("the state was saved before Griddy remembered its takes, so it could be a template");

    const auto folder = sessionsRoot.getChildFile (savedId);
    const auto onDisk = takeIdsOnDisk (folder);

    if (onDisk == *savedTakes)
    {
        c.id = savedId;
        c.kind = SessionChoice::Kind::reopened;
        c.reason = onDisk.empty() ? "the project had no takes yet and nothing else wrote there"
                                  : "the session folder holds exactly the takes the project saved";
        return c;
    }

    // Someone else wrote into that folder after this state was saved. Start a new session with the takes this state knew.
    const std::set<int32_t> wanted (savedTakes->begin(), savedTakes->end());
    if (wanted.empty())
        return fresh ("the state had no takes, and another set has since recorded into its session (a template)");

    c.id = juce::Uuid().toDashedString();
    c.kind = SessionChoice::Kind::forked;
    c.reason = "another set changed the session folder after this project was saved";
    const auto target = sessionsRoot.getChildFile (c.id);

    for (const auto& slot : folder.findChildFiles (juce::File::findDirectories, false, "slot*"))
        for (const auto& take : slot.findChildFiles (juce::File::findDirectories, false, "*.gtake"))
            if (wanted.count (take.getFileNameWithoutExtension().getIntValue()) > 0)
                if (linkTake (take, target.getChildFile (slot.getFileName()).getChildFile (take.getFileName())))
                    ++c.linkedTakes;

    // Keep counting take numbers from where the old session is, so the plain .mov names stay unique.
    if (const auto counter = folder.getChildFile ("counter.txt"); counter.existsAsFile())
    {
        target.createDirectory();
        counter.copyFileTo (target.getChildFile ("counter.txt"));
    }

    return c;
}

} // namespace griddy::session
