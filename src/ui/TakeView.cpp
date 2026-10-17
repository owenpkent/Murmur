#include "TakeView.h"
#include "../take/TakeMidi.h"
#include <okstudio/Scales.h>

namespace murmur::ui
{
namespace
{
    constexpr int hitH = okstudio::ui::minHitPx + 10; // as in PluginEditor.cpp

    juce::String noteName(int midi) { return juce::MidiMessage::getMidiNoteName(midi, true, true, 3); }

    bool isBlackKey(int midi)
    {
        const int pc = midi % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    juce::String clock(int totalSeconds)
    {
        return juce::String(totalSeconds / 60) + ":" + juce::String(totalSeconds % 60).paddedLeft('0', 2);
    }

    float boolParam(MurmurProcessor& p, const char* id) { return p.apvts.getRawParameterValue(id)->load(); }
} // namespace

// ---- TakeRoll ----------------------------------------------------------------------------

TakeRoll::TakeRoll(MurmurProcessor& p) : processor(p)
{
    okstudio::ui::makeMouseOnly(*this);
}

void TakeRoll::setMode(Mode m)
{
    mode = m;
    sendHint({});
}

double TakeRoll::takeDuration() const
{
    const auto& t = processor.getTake();
    double d = 0.0;
    for (const auto& n : t.notes)
        d = juce::jmax(d, n.end);
    if (! t.curve.empty())
        d = juce::jmax(d, t.curve.back().seconds);
    return d;
}

void TakeRoll::clampView()
{
    scrollSec = juce::jlimit(0.0, juce::jmax(0.0, takeDuration()), scrollSec);
    const double lowest = juce::jmin(127.0, (double) visibleLanes() - 1.0);
    topMidi = juce::jlimit(lowest, 127.0, topMidi);
}

double TakeRoll::playFromSeconds() const
{
    return startSec < takeDuration() ? startSec : 0.0;
}

void TakeRoll::followPlayhead()
{
    const double head = processor.getTakePlayheadSeconds();
    if (timeToX(head) > (float) getWidth() - 12.0f || head < scrollSec)
    {
        scrollSec = head - 24.0 / pxPerSec; // the playhead lands just inside the left edge
        clampView();
    }
    repaint();
}

void TakeRoll::resized()
{
    if (pendingFit)
        fit();
    clampView();
}

void TakeRoll::zoomTime(double factor)
{
    // Zoom about the middle of the plot so what you are looking at stays put.
    const double mid = xToTime(keyW + (getWidth() - keyW) / 2);
    pxPerSec = juce::jlimit(8.0, 2000.0, pxPerSec * factor);
    scrollSec = mid - (double) ((getWidth() - keyW) / 2) / pxPerSec;
    clampView();
    repaint();
}

void TakeRoll::fit()
{
    const auto& t = processor.getTake();
    const double dur = takeDuration();
    if (dur <= 0.0)
        return;
    if (getWidth() <= keyW)
    {
        pendingFit = true;
        return;
    }
    pendingFit = false;
    pxPerSec = juce::jlimit(8.0, 2000.0, (double) (getWidth() - keyW - 24) / dur);
    scrollSec = 0.0;

    double centre = 60.0;
    if (! t.notes.empty())
    {
        int lo = 127, hi = 0;
        for (const auto& n : t.notes)
        {
            lo = juce::jmin(lo, n.targetNote);
            hi = juce::jmax(hi, n.targetNote);
        }
        centre = 0.5 * (double) (lo + hi);
    }
    topMidi = centre + (double) visibleLanes() * 0.5 - 0.5;
    clampView();
    repaint();
}

int TakeRoll::snapLane(int lane) const
{
    lane = juce::jlimit(0, 127, lane);
    if (boolParam(processor, "scaleLock") < 0.5f)
        return lane;
    return juce::jlimit(0, 127, okstudio::scales::snapToScale(lane, (int) boolParam(processor, "root"),
                                                              (int) boolParam(processor, "scale")));
}

void TakeRoll::sendHint(const juce::String& s)
{
    if (onHint)
        onHint(s.isEmpty() ? juce::String("Scroll to move through time.") : s);
}

juce::String TakeRoll::hintAt(juce::Point<int> p) const
{
    if (p.x < keyW)
        return "Scroll to move through pitch.";
    if (p.y < rulerH)
        return "Click to start playback here. Scroll to move through time.";

    const auto& t = processor.getTake();
    const int lane = yToLane(p.y);
    const int i = take::noteAt(t, xToTime(p.x), lane);
    if (i < 0)
    {
        const char* verb = mode == Mode::move ? "Move" : mode == Mode::reset ? "Reset" : "Erase";
        return noteName(juce::jlimit(0, 127, lane)) + ". Empty lane. Nothing to do here in " + verb + " mode.";
    }
    const auto& n = t.notes[(size_t) i];
    const int sung = juce::roundToInt(n.sungMidi);
    const int cents = juce::roundToInt((n.sungMidi - (float) sung) * 100.0f);
    juce::String s = noteName(n.targetNote) + " (sung " + noteName(sung) + " " + juce::String::formatted("%+d", cents)
                     + " cents). ";
    switch (mode)
    {
        case Mode::move: s += "Drag up or down to move it."; break;
        case Mode::reset: s += "Click to put it back to " + noteName(n.detectedNote) + "."; break;
        case Mode::erase: s += "Click to delete it."; break;
    }
    return s;
}

void TakeRoll::mouseMove(const juce::MouseEvent& e)
{
    sendHint(hintAt(e.getPosition()));
}

void TakeRoll::mouseExit(const juce::MouseEvent&)
{
    if (! isDragging())
        sendHint({});
}

void TakeRoll::mouseDown(const juce::MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown() || e.x < keyW)
        return;
    if (e.y < rulerH)
    {
        startSec = juce::jmax(0.0, xToTime(e.x));
        repaint();
        return;
    }
    const auto& t = processor.getTake();
    const int i = take::noteAt(t, xToTime(e.x), yToLane(e.y));
    if (i < 0)
        return;

    switch (mode)
    {
        case Mode::move:
            dragIndex = i;
            dragOriginLane = dragLane = t.notes[(size_t) i].targetNote;
            dragVersion = processor.getTakeVersion();
            beginDragAutoRepeat(50); // mouseDrag keeps firing while held, for the edge scroll
            sendHint("Moving to " + noteName(dragLane) + ". Release to place it.");
            repaint();
            break;
        case Mode::reset:
            processor.editTake([i](take::Take& tk) { take::resetNote(tk, i); });
            break;
        case Mode::erase:
            processor.editTake([i](take::Take& tk) { take::deleteNote(tk, i); });
            break;
    }
}

void TakeRoll::mouseDrag(const juce::MouseEvent& e)
{
    if (dragIndex < 0)
        return;
    // Only about nine lanes fit, so holding the drag near the top or bottom edge scrolls
    // pitch: a note can travel an octave or more in one drag with no wheel needed.
    const int edge = laneH / 2;
    const bool scrollUp = e.y < rulerH + edge, scrollDown = e.y > getHeight() - edge;
    if (scrollUp || scrollDown)
    {
        topMidi += scrollUp ? 0.25 : -0.25;
        clampView();
        repaint();
    }
    const int lane = snapLane(juce::jlimit(0, 127, yToLane(juce::jlimit(rulerH, getHeight() - 1, e.y))));
    if (lane != dragLane)
    {
        dragLane = lane;
        sendHint("Moving to " + noteName(dragLane) + ". Release to place it.");
        repaint();
    }
}

void TakeRoll::mouseUp(const juce::MouseEvent& e)
{
    if (dragIndex < 0)
        return;
    const int i = dragIndex, lane = dragLane;
    const bool changed = lane != dragOriginLane && dragVersion == processor.getTakeVersion()
                         && i < (int) processor.getTake().notes.size();
    dragIndex = -1;
    if (changed)
        processor.editTake([i, lane](take::Take& tk)
        {
            if (i < (int) tk.notes.size())
                tk.notes[(size_t) i].targetNote = lane;
        });
    repaint();
    sendHint(hintAt(e.getPosition()));
}

void TakeRoll::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Never zoom on the wheel. Over the keyboard it scrolls pitch, anywhere else time.
    if (e.x < keyW)
        topMidi += (double) w.deltaY * 10.0;
    else
        scrollSec -= ((double) w.deltaY - (double) w.deltaX) * 300.0 / pxPerSec;
    clampView();
    repaint();
}

void TakeRoll::paint(juce::Graphics& g)
{
    namespace th = okstudio::theme;
    const auto& t = processor.getTake();
    const int w = getWidth(), h = getHeight();
    const bool lock = boolParam(processor, "scaleLock") > 0.5f;
    const int root = (int) boolParam(processor, "root"), scaleIdx = (int) boolParam(processor, "scale");
    const auto plot = juce::Rectangle<int>(keyW, rulerH, juce::jmax(0, w - keyW), juce::jmax(0, h - rulerH));

    g.fillAll(th::background);

    // ---- Lanes and keyboard --------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(0, rulerH, w, h - rulerH);
        const int top = juce::jlimit(0, 127, (int) std::ceil(topMidi) + 1);
        const int bottom = juce::jlimit(0, 127, top - visibleLanes() - 2);
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        for (int m = top; m >= bottom; --m)
        {
            const float y = laneTop(m);
            const auto lane = juce::Rectangle<float>((float) keyW, y, (float) (w - keyW), (float) laneH);
            g.setColour(isBlackKey(m) ? th::background.brighter(0.04f) : th::panel);
            g.fillRect(lane);
            if (lock && ! okstudio::scales::isInScale(m, root, scaleIdx))
            {
                g.setColour(juce::Colours::black.withAlpha(0.4f));
                g.fillRect(lane);
            }
            g.setColour(th::outline.withAlpha(m % 12 == 0 ? 0.9f : 0.4f));
            g.drawHorizontalLine((int) (y + (float) laneH - 1.0f), (float) keyW, (float) w);

            const auto key = juce::Rectangle<float>(0.0f, y, (float) keyW, (float) laneH);
            g.setColour(isBlackKey(m) ? th::background : th::textDim.brighter(0.3f));
            g.fillRect(key.withTrimmedBottom(1.0f));
            if (m % 12 == 0)
            {
                g.setColour(th::background);
                g.drawText(noteName(m), key.withTrimmedRight(6.0f), juce::Justification::centredRight);
            }
        }
    }

    // ---- Ruler ---------------------------------------------------------------------------
    {
        g.setColour(th::panel);
        g.fillRect(0, 0, w, rulerH);
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(plot.withY(0).withHeight(h));
        static const double steps[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 };
        double step = steps[std::size(steps) - 1];
        for (double s : steps)
            if (s * pxPerSec >= 70.0)
            {
                step = s;
                break;
            }
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        for (double s = std::floor(scrollSec / step) * step; timeToX(s) < (float) w; s += step)
        {
            const int x = juce::roundToInt(timeToX(s));
            g.setColour(th::outline.withAlpha(0.6f));
            g.drawVerticalLine(x, (float) rulerH, (float) h);
            g.setColour(th::textDim);
            g.drawVerticalLine(x, (float) rulerH * 0.55f, (float) rulerH);
            const juce::String label = step < 1.0 ? juce::String(s, 1) + "s" : juce::String(juce::roundToInt(s)) + "s";
            g.drawText(label, x + 4, 0, 60, rulerH - 4, juce::Justification::centredLeft);
        }
        g.setColour(th::outline);
        g.drawHorizontalLine(rulerH - 1, 0.0f, (float) w);

        // The start marker: a small flag on the ruler and a thin line down the roll.
        const float sx = timeToX(startSec);
        if (sx >= (float) keyW && sx <= (float) w)
        {
            g.setColour(th::accent);
            g.drawVerticalLine(juce::roundToInt(sx), (float) rulerH, (float) h);
            juce::Path flag;
            flag.addTriangle(sx, (float) rulerH - 2.0f, sx, (float) rulerH - 14.0f, sx + 10.0f, (float) rulerH - 8.0f);
            g.fillPath(flag);
        }
        if (processor.isTakePlaying())
        {
            const float px = timeToX(processor.getTakePlayheadSeconds());
            if (px >= (float) keyW && px <= (float) w)
            {
                g.setColour(th::text);
                g.fillRect(px - 1.0f, 0.0f, 2.0f, (float) rulerH);
            }
        }
    }

    // ---- Notes and curve -----------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(plot);

        auto targetOf = [this, &t](int i)
        {
            return i == dragIndex ? dragLane : t.notes[(size_t) i].targetNote;
        };

        // The origin of a dragged blob stays as a faint ghost.
        if (dragIndex >= 0 && dragIndex < (int) t.notes.size() && dragLane != dragOriginLane)
        {
            const auto& n = t.notes[(size_t) dragIndex];
            const float x1 = timeToX(n.start), x2 = juce::jmax(x1 + 4.0f, timeToX(n.end));
            g.setColour(th::accent.withAlpha(0.35f));
            g.drawRoundedRectangle(x1, laneTop(dragOriginLane) + 2.0f, x2 - x1, (float) laneH - 4.0f, 5.0f, 1.5f);
        }

        for (int i = 0; i < (int) t.notes.size(); ++i)
        {
            const auto& n = t.notes[(size_t) i];
            const float x1 = timeToX(n.start), x2 = juce::jmax(x1 + 4.0f, timeToX(n.end));
            if (x2 < (float) keyW || x1 > (float) w)
                continue;
            const auto r = juce::Rectangle<float>(x1, laneTop(targetOf(i)) + 2.0f, x2 - x1, (float) laneH - 4.0f);
            g.setColour(th::accent.withAlpha(0.5f + 0.5f * juce::jlimit(0.0f, 1.0f, n.velocity)));
            g.fillRoundedRectangle(r, 5.0f);
            if (targetOf(i) != n.detectedNote)
            {
                g.setColour(th::warn);
                g.drawRoundedRectangle(r.reduced(1.0f), 5.0f, 2.0f);
            }
        }

        if (! t.curve.empty() && ! t.notes.empty())
        {
            // Points inside a note ride with its blob; the rest stay where they were sung.
            const double secLeft = xToTime(keyW), secRight = xToTime(w);
            auto first = std::lower_bound(t.curve.begin(), t.curve.end(), secLeft - 0.1,
                                          [](const take::PitchPoint& p, double s) { return p.seconds < s; });
            size_t j = (size_t) (std::partition_point(t.notes.begin(), t.notes.end(),
                                                      [secLeft](const take::TakeNote& n) { return n.end <= secLeft; })
                                 - t.notes.begin());
            juce::Path inPath, outPath;
            bool prevVoiced = false, prevIn = false;
            double prevSec = 0.0;
            for (auto it = first; it != t.curve.end() && it->seconds <= secRight + 0.1; ++it)
            {
                while (j < t.notes.size() && t.notes[j].end <= it->seconds)
                    ++j;
                const bool in = j < t.notes.size() && t.notes[j].start <= it->seconds;
                if (it->midi <= 0.0f)
                {
                    prevVoiced = false;
                    continue;
                }
                const double shift = in ? (double) (targetOf((int) j) - t.notes[j].detectedNote) : 0.0;
                const float x = timeToX(it->seconds), y = midiToY((double) it->midi + shift);
                auto& path = in ? inPath : outPath;
                if (prevVoiced && prevIn == in && it->seconds - prevSec < 0.1)
                    path.lineTo(x, y);
                else
                    path.startNewSubPath(x, y);
                prevVoiced = true;
                prevIn = in;
                prevSec = it->seconds;
            }
            g.setColour(th::textDim.withAlpha(0.45f));
            g.strokePath(outPath, juce::PathStrokeType(1.2f));
            g.setColour(th::text);
            g.strokePath(inPath, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (processor.isTakePlaying())
        {
            const float x = timeToX(processor.getTakePlayheadSeconds());
            g.setColour(th::text);
            g.drawVerticalLine(juce::roundToInt(x), (float) rulerH, (float) h);
        }

        auto& capture = processor.getCapture();
        if (capture.isRecording())
        {
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRect(plot);
            g.setColour(th::warn);
            g.fillEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre({ (float) plot.getCentreX() - 70.0f,
                                                                           (float) plot.getCentreY() }));
            g.setColour(th::text);
            g.setFont(juce::Font(juce::FontOptions(28.0f, juce::Font::bold)));
            g.drawText("Recording " + clock((int) capture.secondsRecorded()),
                       plot.withTrimmedLeft(24), juce::Justification::centred);
        }
        else if (t.notes.empty())
        {
            g.setColour(th::textDim);
            g.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
            g.drawText(processor.isAnalysingTake() ? "Analysing..." : "Record a take to see its notes here.", plot,
                       juce::Justification::centred);
        }
    }
}
} // namespace murmur::ui

// ---- DragMidiTile ------------------------------------------------------------------------

namespace murmur::ui
{
DragMidiTile::DragMidiTile()
{
    okstudio::ui::makeMouseOnly(*this);
    setTitle("Drag MIDI");
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void DragMidiTile::paint(juce::Graphics& g)
{
    const bool on = isEnabled();
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(! on ? okstudio::theme::panel
                     : pressed ? okstudio::theme::surfaceHi
                               : over ? okstudio::theme::surface : okstudio::theme::panel);
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(on ? okstudio::theme::accent : okstudio::theme::outline);
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
    g.setColour(on ? okstudio::theme::text : okstudio::theme::textDim.withAlpha(0.5f));
    g.setFont(juce::Font(juce::FontOptions(15.0f)));
    g.drawText("Drag MIDI", getLocalBounds(), juce::Justification::centred);
}

void DragMidiTile::mouseEnter(const juce::MouseEvent&)
{
    over = true;
    if (onHint)
        onHint("Drag this into Live to drop the take as a MIDI clip.");
    repaint();
}

void DragMidiTile::mouseExit(const juce::MouseEvent&)
{
    over = false;
    if (onHint)
        onHint({});
    repaint();
}

void DragMidiTile::mouseDown(const juce::MouseEvent& e)
{
    pressed = isEnabled() && e.mods.isLeftButtonDown();
    started = false;
    repaint();
}

void DragMidiTile::mouseDrag(const juce::MouseEvent& e)
{
    if (! pressed || started || e.getDistanceFromDragStart() <= 6 || ! prepareFile)
        return;
    started = true;
    const auto file = prepareFile();
    if (file.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles({ file.getFullPathName() }, false, this);
}

void DragMidiTile::mouseUp(const juce::MouseEvent&)
{
    pressed = false;
    repaint();
}

// ---- TakeView ----------------------------------------------------------------------------

TakeView::TakeView(MurmurProcessor& p)
    : processor(p), roll(p),
      modeToggle({ "Move", "Reset", "Erase" }, 15.0f,
                 [this](int i) { roll.setMode(static_cast<TakeRoll::Mode>(i)); })
{
    okstudio::ui::makeMouseOnly(*this);

    for (auto* b : { &recordButton, &playButton, &undoButton, &snapButton, &clearButton, &zoomOutButton, &zoomInButton, &fitButton })
    {
        okstudio::ui::makeMouseOnly(*b);
        addAndMakeVisible(*b);
    }
    setButtonText(recordButton, "Record");
    setButtonText(playButton, "Play");
    setButtonText(undoButton, "Undo");
    setButtonText(snapButton, "Snap to scale");
    setButtonText(clearButton, "Clear");
    setButtonText(zoomOutButton, "-");
    setButtonText(zoomInButton, "+");
    setButtonText(fitButton, "Fit");

    recordButton.onClick = [this] { onRecordClicked(); };
    playButton.onClick = [this] { onPlayClicked(); };
    undoButton.onClick = [this] { processor.undoTake(); };
    snapButton.onClick = [this]
    {
        const int root = (int) processor.apvts.getRawParameterValue("root")->load();
        const int scale = (int) processor.apvts.getRawParameterValue("scale")->load();
        processor.editTake([root, scale](take::Take& t) { take::snapAllToScale(t, root, scale); });
    };
    clearButton.onClick = [this] { onClearClicked(); };
    zoomOutButton.onClick = [this] { roll.zoomTime(1.0 / 1.5); };
    zoomInButton.onClick = [this] { roll.zoomTime(1.5); };
    fitButton.onClick = [this] { roll.fit(); };

    status.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    status.setColour(juce::Label::textColourId, okstudio::theme::textDim);
    status.setJustificationType(juce::Justification::centredLeft);
    status.setInterceptsMouseClicks(false, false);
    hint.setFont(juce::Font(juce::FontOptions(13.0f)));
    hint.setColour(juce::Label::textColourId, okstudio::theme::textDim);
    hint.setText("Scroll to move through time.", juce::dontSendNotification);
    hint.setInterceptsMouseClicks(false, false);
    roll.onHint = [this](const juce::String& s) { hint.setText(s, juce::dontSendNotification); };
    dragTile.prepareFile = [this] { return writeMidiFile(); };
    dragTile.onHint = [this](const juce::String& s)
    { hint.setText(s.isEmpty() ? juce::String("Scroll to move through time.") : s, juce::dontSendNotification); };

    addAndMakeVisible(dragTile);
    addAndMakeVisible(modeToggle);
    addAndMakeVisible(status);
    addAndMakeVisible(hint);
    addAndMakeVisible(roll);

    refreshControls();
    startTimerHz(30);
}

TakeView::~TakeView()
{
    stopTimer();
}

void TakeView::setButtonText(juce::TextButton& b, const juce::String& text)
{
    if (b.getButtonText() == text)
        return;
    b.setButtonText(text);
    b.setTitle(text); // the accessible name is the visible text
}

void TakeView::setDrumsMode(bool d)
{
    if (d == drums)
        return;
    drums = d;
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &playButton, &undoButton, &snapButton,
                                                            &clearButton, &dragTile, &zoomOutButton, &zoomInButton, &fitButton, &modeToggle,
                                                            &status, &hint, &roll })
        c->setVisible(! d);
    repaint();
}

// <takes folder>/MIDI/<take wav name>.mid, overwritten each time. A non-existent File if the
// take has no notes or the file could not be written.
juce::File TakeView::writeMidiFile() const
{
    const auto& t = processor.getTake();
    if (t.notes.empty())
        return {};
    const auto name = t.audioPath.isEmpty() ? juce::String("take")
                                            : juce::File(t.audioPath).getFileNameWithoutExtension();
    const auto file = processor.getTakesFolder().getChildFile("MIDI").getChildFile(name + ".mid");
    if (! file.getParentDirectory().createDirectory())
        return {};
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (! out.openedOk())
        return {};
    take::toMidiFile(t).writeTo(out);
    return file;
}

void TakeView::onRecordClicked()
{
    notice.clear();
    if (processor.getCapture().isRecording())
        processor.stopRecording();
    else if (! processor.startRecording())
        notice = "Could not start recording. Is audio running?";
    refreshControls();
}

void TakeView::onPlayClicked()
{
    notice.clear();
    if (processor.isTakePlaying())
        processor.stopTake();
    else
        processor.playTake(roll.playFromSeconds());
    refreshControls();
    roll.repaint();
}

void TakeView::onClearClicked()
{
    if (clearArmedUntil != 0 && juce::Time::getMillisecondCounter() <= clearArmedUntil)
    {
        clearArmedUntil = 0;
        processor.clearTake();
    }
    else
    {
        clearArmedUntil = juce::Time::getMillisecondCounter() + 3000;
    }
    refreshControls();
}

void TakeView::refreshControls()
{
    const bool recording = processor.getCapture().isRecording();
    const bool analysing = processor.isAnalysingTake();
    const auto& t = processor.getTake();

    if (clearArmedUntil != 0 && juce::Time::getMillisecondCounter() > clearArmedUntil)
        clearArmedUntil = 0;

    setButtonText(recordButton,
                  recording ? "Stop " + clock((int) processor.getCapture().secondsRecorded()) : juce::String("Record"));
    const bool playing = processor.isTakePlaying();
    setButtonText(playButton, playing ? "Stop" : "Play");
    playButton.setEnabled(playing || (! recording && ! analysing && ! t.notes.empty()));
    setButtonText(clearButton, clearArmedUntil != 0 ? "Click again to clear" : "Clear");
    recordButton.setColour(juce::TextButton::buttonColourId,
                           recording ? okstudio::theme::warn : okstudio::theme::panel);
    recordButton.setColour(juce::TextButton::textColourOffId,
                           recording ? okstudio::theme::background : okstudio::theme::text);

    recordButton.setEnabled(recording || ! analysing);
    undoButton.setEnabled(! recording && processor.canUndoTake());
    snapButton.setEnabled(! recording && ! t.notes.empty());
    dragTile.setEnabled(! recording && ! analysing && ! t.notes.empty());
    clearButton.setEnabled(! recording && ! analysing && ! t.isEmpty());

    juce::String s;
    if (recording)
        s = "Recording";
    else if (analysing)
        s = "Analysing...";
    else if (notice.isNotEmpty())
        s = notice;
    else if (processor.getTakeError().isNotEmpty())
        s = processor.getTakeError();
    else
        s = juce::String((int) t.notes.size()) + (t.notes.size() == 1 ? " note" : " notes");
    status.setText(s, juce::dontSendNotification);
}

void TakeView::timerCallback()
{
    processor.pollCapture();

    const int version = processor.getTakeVersion();
    if (version != lastVersion)
    {
        lastVersion = version;
        const auto& t = processor.getTake();
        if (t.audioPath != lastAudioPath)
        {
            lastAudioPath = t.audioPath;
            if (! t.notes.empty())
                roll.fit(); // a new take: frame all of it
        }
        roll.repaint();
    }

    const bool recording = processor.getCapture().isRecording();
    const int sec = recording ? (int) processor.getCapture().secondsRecorded() : -1;
    if (sec != lastSecond)
    {
        lastSecond = sec;
        roll.repaint();
    }
    if (processor.isTakePlaying())
        roll.followPlayhead();
    else if (wasPlaying)
        roll.repaint(); // the playhead line goes away
    wasPlaying = processor.isTakePlaying();
    refreshControls();
}

void TakeView::paint(juce::Graphics& g)
{
    if (! drums)
        return;
    g.setColour(okstudio::theme::textDim);
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("The take editor works in Melody mode", getLocalBounds(), juce::Justification::centred);
}

void TakeView::resized()
{
    auto area = getLocalBounds();

    auto row = area.removeFromTop(hitH);
    recordButton.setBounds(row.removeFromLeft(120));
    row.removeFromLeft(8);
    playButton.setBounds(row.removeFromLeft(90));
    row.removeFromLeft(8);
    undoButton.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(8);
    snapButton.setBounds(row.removeFromLeft(130));
    row.removeFromLeft(8);
    clearButton.setBounds(row.removeFromLeft(170));
    row.removeFromLeft(8);
    dragTile.setBounds(row.removeFromLeft(110));

    area.removeFromTop(8);
    row = area.removeFromTop(hitH);
    modeToggle.setBounds(row.removeFromLeft(juce::jmin(300, row.getWidth() / 2)));
    row.removeFromLeft(16);
    for (auto* b : { &zoomOutButton, &zoomInButton })
    {
        b->setBounds(row.removeFromLeft(56));
        row.removeFromLeft(8);
    }
    fitButton.setBounds(row.removeFromLeft(70));
    row.removeFromLeft(16);
    status.setBounds(row); // the transport row is full; this one has room for "Analysing..."

    area.removeFromTop(8);
    hint.setBounds(area.removeFromBottom(24));
    area.removeFromBottom(4);
    roll.setBounds(area);
}
} // namespace murmur::ui
