#pragma once

#include "DrumFeatures.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <array>
#include <cmath>

// DrumClassifier: k-nearest-neighbour over per-user calibration examples.
//
// Why this algorithm: every mouth makes a different "boom" and "ka", and a trained model
// would need a corpus we do not have. kNN needs no training step: the user performs each
// sound a few times, we store the feature vectors, and a new hit is labelled by its three
// nearest stored examples (inverse-distance weighted). Adding or clearing an example is
// instant and cannot destabilise anything. Until a class has its own examples, built-in
// prototypes (hand-chosen centroids for "boom", "ka"/"psh", "ts", "tss") stand in for it,
// so the plugin works out of the box. A class's defaults are dropped as soon as the user
// records at least one example for it.
//
// Real-time notes: addExample / clearClass / classify never allocate (fixed storage,
// ring-overwrite when a class is full). Serialisation to/from juce::ValueTree allocates
// and belongs on the message thread; the caller is responsible for not mutating the
// classifier on the message thread while classify() runs on the audio thread (build a copy
// and swap, or guard externally).
//
// Upgrade path (rejected for v1): a small MLP or Gist MFCC features trained on a shared
// corpus could replace the kNN behind the same classify() signature.
namespace murmur::dsp
{
class DrumClassifier
{
public:
    enum class Class { Kick = 0, Snare, HatClosed, HatOpen };
    static constexpr int numClasses = 4;
    static constexpr int maxExamplesPerClass = 32;
    static constexpr int k = 3;

    struct Result
    {
        Class cls = Class::Kick;
        float confidence = 0.0f; // 0..1
    };

    // General MIDI / Ableton Drum Rack convention.
    static int defaultMidiNote(Class c)
    {
        switch (c)
        {
            case Class::Kick: return 36;
            case Class::Snare: return 38;
            case Class::HatClosed: return 42;
            case Class::HatOpen: return 46;
        }
        return 36;
    }

    static const char* className(Class c)
    {
        switch (c)
        {
            case Class::Kick: return "Kick";
            case Class::Snare: return "Snare";
            case Class::HatClosed: return "HatClosed";
            case Class::HatOpen: return "HatOpen";
        }
        return "Kick";
    }

    DrumClassifier() { buildDefaults(); }

    void prepare(double, int) {}
    void reset() {}

    void addExample(Class c, const FeatureVector& f)
    {
        auto& s = store[(size_t) c];
        s.ex[(size_t) s.next] = f;
        s.next = (s.next + 1) % maxExamplesPerClass;
        s.count = std::min(s.count + 1, maxExamplesPerClass);
    }

    void clearClass(Class c)
    {
        auto& s = store[(size_t) c];
        s.count = 0;
        s.next = 0;
    }

    void clearAll()
    {
        for (int c = 0; c < numClasses; ++c)
            clearClass((Class) c);
    }

    int getNumExamples(Class c) const { return store[(size_t) c].count; }

    Result classify(const FeatureVector& f) const
    {
        struct Nb { float d = 1.0e30f; int cls = 0; };
        std::array<Nb, k> nb {};
        for (int c = 0; c < numClasses; ++c)
        {
            const auto& s = store[(size_t) c];
            const FeatureVector* src = s.count > 0 ? s.ex.data() : defaults[(size_t) c].data();
            const int cnt = s.count > 0 ? s.count : (int) defaults[(size_t) c].size();
            for (int i = 0; i < cnt; ++i)
            {
                const float d = distance(f, src[i]);
                if (d < nb[k - 1].d)
                {
                    nb[k - 1] = { d, c };
                    for (int j = k - 1; j > 0 && nb[(size_t) j].d < nb[(size_t) (j - 1)].d; --j)
                        std::swap(nb[(size_t) j], nb[(size_t) (j - 1)]);
                }
            }
        }
        std::array<float, numClasses> votes {};
        float total = 0.0f, nearest = 1.0e30f;
        for (const auto& n : nb)
        {
            if (n.d >= 1.0e29f) continue;
            const float w = 1.0f / (n.d + 0.1f);
            votes[(size_t) n.cls] += w;
            total += w;
            nearest = std::min(nearest, n.d);
        }
        Result r;
        if (total <= 0.0f)
            return r;
        int best = 0;
        for (int c = 1; c < numClasses; ++c)
            if (votes[(size_t) c] > votes[(size_t) best]) best = c;
        r.cls = (Class) best;
        // Vote share, discounted when even the nearest example is far away.
        r.confidence = juce::jlimit(0.0f, 1.0f, (votes[(size_t) best] / total) / (1.0f + 0.25f * nearest * nearest));
        return r;
    }

    // ---- Persistence (message thread only: allocates) ----
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree t("DrumCalibration");
        for (int c = 0; c < numClasses; ++c)
        {
            const auto& s = store[(size_t) c];
            for (int i = 0; i < s.count; ++i)
            {
                juce::ValueTree e("Example");
                e.setProperty("class", c, nullptr);
                juce::StringArray parts;
                for (float x : s.ex[(size_t) i].v)
                    parts.add(juce::String(x, 6));
                e.setProperty("f", parts.joinIntoString(","), nullptr);
                t.appendChild(e, nullptr);
            }
        }
        return t;
    }

    void fromValueTree(const juce::ValueTree& t)
    {
        clearAll();
        if (! t.isValid() || ! t.hasType("DrumCalibration"))
            return;
        for (int i = 0; i < t.getNumChildren(); ++i)
        {
            const auto e = t.getChild(i);
            const int c = e.getProperty("class", -1);
            if (c < 0 || c >= numClasses)
                continue;
            juce::StringArray parts;
            parts.addTokens(e.getProperty("f").toString(), ",", "");
            if (parts.size() != FeatureVector::size)
                continue;
            FeatureVector f;
            for (int j = 0; j < FeatureVector::size; ++j)
                f.v[(size_t) j] = parts[j].getFloatValue();
            addExample((Class) c, f);
        }
    }

private:
    struct ClassStore
    {
        std::array<FeatureVector, maxExamplesPerClass> ex {};
        int count = 0, next = 0;
    };

    static float distance(const FeatureVector& a, const FeatureVector& b)
    {
        // Per-feature weights: loudness varies with mic gain, so it counts least.
        static constexpr std::array<float, FeatureVector::size> w { 1.0f, 0.7f, 0.7f, 1.0f, 0.7f, 1.0f, 0.3f };
        float s = 0.0f;
        for (int i = 0; i < FeatureVector::size; ++i)
        {
            const float d = a.v[(size_t) i] - b.v[(size_t) i];
            s += w[(size_t) i] * d * d;
        }
        return std::sqrt(s);
    }

    void buildDefaults()
    {
        // raw: centroid Hz, flatness, zcr, low, mid, high, level dB
        struct P { float hz, flat, zcr, lo, mid, hi, db; };
        static constexpr P kick[]  { { 120, 0.02f, 0.02f, 0.92f, 0.07f, 0.01f, -18 }, { 200, 0.04f, 0.04f, 0.80f, 0.18f, 0.02f, -22 }, { 150, 0.03f, 0.03f, 0.88f, 0.11f, 0.01f, -26 } };
        static constexpr P snare[] { { 2200, 0.25f, 0.20f, 0.10f, 0.60f, 0.30f, -22 }, { 3000, 0.30f, 0.25f, 0.08f, 0.50f, 0.42f, -26 }, { 1800, 0.20f, 0.15f, 0.20f, 0.60f, 0.20f, -24 } };
        static constexpr P hatC[]  { { 7000, 0.45f, 0.40f, 0.00f, 0.10f, 0.90f, -30 }, { 6000, 0.40f, 0.35f, 0.01f, 0.15f, 0.84f, -34 }, { 8000, 0.50f, 0.45f, 0.00f, 0.06f, 0.94f, -32 } };
        static constexpr P hatO[]  { { 5500, 0.50f, 0.40f, 0.01f, 0.15f, 0.84f, -24 }, { 6500, 0.55f, 0.45f, 0.00f, 0.10f, 0.90f, -27 }, { 5000, 0.45f, 0.38f, 0.02f, 0.20f, 0.78f, -26 } };
        const auto fill = [this](Class c, const P* p) {
            for (int i = 0; i < 3; ++i)
                defaults[(size_t) c][(size_t) i] = drumnorm::normalise({ std::log(p[i].hz), p[i].flat, p[i].zcr, p[i].lo, p[i].mid, p[i].hi, p[i].db });
        };
        fill(Class::Kick, kick);
        fill(Class::Snare, snare);
        fill(Class::HatClosed, hatC);
        fill(Class::HatOpen, hatO);
    }

    std::array<ClassStore, numClasses> store {};
    std::array<std::array<FeatureVector, 3>, numClasses> defaults {};
};
} // namespace murmur::dsp
