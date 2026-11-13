#include <juce_core/juce_core.h>

#include "engine/SessionStore.h"
#include "engine/TakeFormat.h"

using namespace griddy;

namespace
{
/** A take folder as TakeHistory leaves it: take.json, audio and a frame. */
void makeTake (const juce::File& session, int slot, int32_t id)
{
    const auto folder = session.getChildFile ("slot" + juce::String (slot + 1)).getChildFile (juce::String (id) + ".gtake");
    folder.getChildFile ("frames").createDirectory();
    folder.getChildFile ("frames").getChildFile ("000000.jpg").replaceWithText ("jpeg " + juce::String (id));
    folder.getChildFile ("audio.wav").replaceWithText ("wav " + juce::String (id));

    TakeMeta meta;
    meta.takeId = id;
    meta.slot = slot;
    meta.state = TakeMeta::State::review;
    writeTakeMeta (folder, meta);
    session.getChildFile ("counter.txt").replaceWithText (juce::String (id));
}
} // namespace

struct SessionTests : juce::UnitTest
{
    SessionTests() : UnitTest ("Sessions", "Griddy") {}

    void runTest() override
    {
        const auto root = juce::File::createTempFile ("griddy-sessions");
        root.createDirectory();
        const auto sessions = root.getChildFile ("Sessions");
        const juce::String template_ = "b541fe4e-dbc2-452e-98c5-b4bd80d9dc93";
        const auto folder = sessions.getChildFile (template_);
        folder.createDirectory();

        beginTest ("take lists survive the saved state");
        expect (session::decodeTakeIds (session::encodeTakeIds ({ 40, 41, 3 })) == std::vector<int32_t> { 3, 40, 41 });
        expect (session::decodeTakeIds ("").empty());

        beginTest ("a template saved with no takes opens empty after another set recorded into its session");
        // 12 November in Live: the template named b541fe4e, an unsaved set made from it recorded takes 40 and 41 there.
        makeTake (folder, 0, 40);
        makeTake (folder, 1, 41);
        {
            const auto c = session::choose (sessions, template_, std::vector<int32_t> {});
            expect (c.kind == SessionChoice::Kind::fresh, c.reason);
            expect (c.id != template_);
            expect (session::takeIdsOnDisk (sessions.getChildFile (c.id)).empty(), "the new session has no takes");
        }

        beginTest ("a template saved before take lists existed opens empty");
        {
            const auto c = session::choose (sessions, template_, std::nullopt);
            expect (c.kind == SessionChoice::Kind::fresh && c.id != template_, c.reason);
        }

        beginTest ("a saved project reopens its own takes");
        {
            const auto c = session::choose (sessions, template_, std::vector<int32_t> { 40, 41 });
            expect (c.kind == SessionChoice::Kind::reopened && c.id == template_, c.reason);
        }

        beginTest ("a copy of a project that the original has since recorded over gets the takes it saved, in a session of its own");
        makeTake (folder, 0, 42); // the original recorded take 42 after the copy was made
        {
            const auto c = session::choose (sessions, template_, std::vector<int32_t> { 40, 41 });
            expect (c.kind == SessionChoice::Kind::forked && c.id != template_, c.reason);
            expectEquals (c.linkedTakes, 2);

            const auto copy = sessions.getChildFile (c.id);
            expect (session::takeIdsOnDisk (copy) == std::vector<int32_t> { 40, 41 });
            expectEquals (copy.getChildFile ("counter.txt").loadFileAsString().getIntValue(), 42, "take numbers carry on");

            // Editing the copy's take never touches the original's.
            const auto mine = copy.getChildFile ("slot1").getChildFile ("40.gtake");
            TakeMeta meta;
            expect (readTakeMeta (mine, meta));
            meta.slipSeconds = 0.25;
            expect (writeTakeMeta (mine, meta));
            TakeMeta original;
            expect (readTakeMeta (folder.getChildFile ("slot1").getChildFile ("40.gtake"), original));
            expectEquals (original.slipSeconds, 0.0);
            expectEquals (mine.getChildFile ("frames").getChildFile ("000000.jpg").loadFileAsString(), juce::String ("jpeg 40"));
        }

        beginTest ("nothing is deleted");
        expect (session::takeIdsOnDisk (folder) == std::vector<int32_t> { 40, 41, 42 });

        beginTest ("a discarded take does not count as a take of the session");
        folder.getChildFile ("slot1").getChildFile ("42.gtake").moveFileTo (folder.getChildFile ("slot1").getChildFile ("42.discarded"));
        expect (session::takeIdsOnDisk (folder) == std::vector<int32_t> { 40, 41 });

        beginTest ("a project whose session folder is gone starts that session again");
        {
            const auto c = session::choose (sessions, "0c225229-932a-4fa7-869a-e1f6259c0f89", std::vector<int32_t> {});
            expect (c.kind == SessionChoice::Kind::reopened && c.id == "0c225229-932a-4fa7-869a-e1f6259c0f89", c.reason);
        }

        beginTest ("a state with a strange session name is not used as a path");
        {
            const auto c = session::choose (sessions, "../../escape", std::vector<int32_t> {});
            expect (c.kind == SessionChoice::Kind::fresh && c.id != "../../escape");
        }

        root.deleteRecursively();
    }
};

static SessionTests sessionTests;
