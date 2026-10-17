#include "StandaloneSupport.h"

#include <juce_audio_utils/juce_audio_utils.h>

#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace murmur::standalone
{
void unmuteInput()
{
#if JucePlugin_Build_Standalone
    JUCE_ASSERT_MESSAGE_THREAD
    // Also hides the "Audio input is muted" banner, and is saved with the app's settings.
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->getMuteInputValue().setValue(false);
#endif
}
} // namespace murmur::standalone
