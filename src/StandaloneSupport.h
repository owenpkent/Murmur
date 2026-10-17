#pragma once

namespace murmur::standalone
{
// The Standalone app mutes its audio input by default (JUCE's guard against a mic feeding
// back through the speakers), which leaves Murmur deaf: it has nothing to analyse or record.
// Murmur instead silences the voice on the way OUT in the Standalone (MurmurProcessor and
// MurmurHostProcessor processBlock), so the feedback loop cannot form, and unmutes the
// input here. Message thread. Does nothing outside the Standalone app.
void unmuteInput();
} // namespace murmur::standalone
