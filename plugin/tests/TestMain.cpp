#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Runs every juce::UnitTest in the "Griddy" category and exits non zero on failure,
// so `ctest` or CI can gate on it.
int main()
{
    juce::ScopedJuceInitialiser_GUI init; // fonts and images need the message manager set up

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("Griddy");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    std::cout << (failures == 0 ? "ALL GRIDDY TESTS PASSED" : "GRIDDY TESTS FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
