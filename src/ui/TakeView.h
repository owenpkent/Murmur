#pragma once

#include "../PluginProcessor.h"
#include "Segmented.h"
#include <okstudio/MouseOnly.h>
#include <okstudio/Theme.h>
#include <functional>

namespace murmur::ui
{
// The roll of the Take Editor (docs/TAKE_EDITOR_DESIGN.md): pitch rises up a keyboard on the
// left, time runs left to right, one lane per semitone and never shorter than minHitPx. Each
// note of the take is a blob in the lane of its targetNote; the sung curve is drawn through
// the blobs and moves with them. Left-click, left-drag and the wheel only (the wheel scrolls
// time over the roll and pitch over the keyboard; it never zooms). The verb is the mode.
class TakeRoll : public juce::Component
{
public:
    enum class Mode { move, reset, erase, drift };

    explicit TakeRoll(MurmurProcessor&);

    void setMode(Mode m);
    void zoomTime(double factor);
    void fit(); // whole take across the width, its note range centred
    bool isDragging() const { return dragIndex >= 0; }
    // Where Play starts: set by clicking the time ruler. 0 when unset or past the take's end.
    double playFromSeconds() const;
    // The view timer calls this while playing: repaints, and pages forward if the playhead
    // has run off the right edge.
    void followPlayhead();

    std::function<void(const juce::String&)> onHint;

    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void resized() override;

    static constexpr int keyW = 56;
    static constexpr int laneH = okstudio::ui::minHitPx;
    static constexpr int rulerH = okstudio::ui::minHitPx;

private:
    double xToTime(int x) const { return scrollSec + (double) (x - keyW) / pxPerSec; }
    float timeToX(double s) const { return (float) keyW + (float) ((s - scrollSec) * pxPerSec); }
    int yToLane(int y) const { return (int) std::floor(topMidi - (double) (y - rulerH) / laneH); }
    float laneTop(double midi) const { return (float) rulerH + (float) ((topMidi - midi) * laneH); }
    float midiToY(double midi) const { return laneTop(midi) + (float) laneH * 0.5f; }
    int visibleLanes() const { return juce::jmax(1, (getHeight() - rulerH) / laneH); }
    double takeDuration() const;
    void clampView();
    int snapLane(int lane) const;
    juce::String hintAt(juce::Point<int>) const;
    static juce::String driftHint(float drift);
    static inline const juce::String driftHelp = "drag left to flatten, right to keep the wobble.";
    void sendHint(const juce::String&);

    MurmurProcessor& processor;
    Mode mode = Mode::move;
    double pxPerSec = 80.0, scrollSec = 0.0;
    double topMidi = 76.0; // the lane at the top of the plot
    double startSec = 0.0; // the playback start marker

    // The drag in flight: the note picked, the lane it previews in, and the take version at
    // pick time (a take that changed under the drag is not edited).
    bool pendingFit = false; // a fit asked for while the roll had no size yet
    int dragIndex = -1, dragLane = 0, dragOriginLane = 0, dragVersion = 0;
    // Drift mode: a horizontal drag. Dragging driftDragPx to the right is the whole 0..1 range.
    float dragDrift = 1.0f, dragStartDrift = 1.0f;
    int dragStartX = 0;
    static constexpr int driftDragPx = 160;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeRoll)
};

// The Drag MIDI tile: looks like a button, but a left-drag of more than a few pixels asks
// `prepareFile` for the .mid on disk and starts a drag-and-drop of it out of the window, into
// Live. No click is needed (a drag is an allowed mouse-only gesture). Disabled: no drag.
class DragMidiTile : public juce::Component
{
public:
    DragMidiTile();

    std::function<juce::File()> prepareFile; // writes the file; a non-existent File = nothing to drag
    std::function<void(const juce::String&)> onHint;

    void paint(juce::Graphics&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void enablementChanged() override { repaint(); }

private:
    bool over = false, pressed = false, started = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DragMidiTile)
};

// The Edit view: transport row, mode and zoom row, the roll, and a hint strip. Drums mode
// shows a message instead (v1 edits melodies only). Polls the processor at 30 Hz itself.
class TakeView : public juce::Component, private juce::Timer
{
public:
    explicit TakeView(MurmurProcessor&);
    ~TakeView() override;

    void setDrumsMode(bool);

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void onRecordClicked();
    void onPlayClicked();
    void onClearClicked();
    void refreshControls();
    void setButtonText(juce::TextButton&, const juce::String&);
    juce::File writeMidiFile() const;

    MurmurProcessor& processor;
    TakeRoll roll;
    DragMidiTile dragTile;
    juce::TextButton recordButton, playButton, undoButton, snapButton, clearButton;
    juce::TextButton zoomOutButton, zoomInButton, fitButton;
    Segmented modeToggle;
    juce::Label status, hint;

    bool drums = false;
    juce::uint32 clearArmedUntil = 0; // ms counter; 0 = not armed
    int lastVersion = -1, lastSecond = -1;
    bool wasPlaying = false;
    juce::String lastAudioPath, notice;
};
} // namespace murmur::ui
