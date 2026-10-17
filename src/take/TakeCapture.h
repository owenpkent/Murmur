#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// TakeCapture: the lock-free recording buffer behind Record and Stop (docs/TAKE_EDITOR_DESIGN.md).
//
// Thread contract, the same handoff as okstudio::TakeRecorder: arm()/stop() on the message
// thread only; write() on the audio thread, no lock and no allocation. arm() allocates the
// whole buffer up front and publishes it through an atomic pointer; stop() unpublishes it,
// waits out any write in flight (the in-use flag), then takes the buffer back. At the cap the
// buffer simply stops accepting and `capReached` goes true, so the data is correct whether or
// not anything is watching; the message thread decides when to call stop().
namespace murmur::take
{
class TakeCapture
{
public:
    struct Recording
    {
        std::vector<float> audio; // trimmed to what was written
        double sampleRate = 0.0;
        std::optional<double> anchorPpq; // set only if the host was playing at the first sample
        double anchorBpm = 120.0;
        bool hitCap = false;
    };

    TakeCapture() = default;
    ~TakeCapture() { stop(); }

    // Message thread. Allocates (about 35 MB for 3 minutes at 48 kHz). False if already armed
    // or the arguments are unusable.
    bool arm(double sampleRate, double maxSeconds = 180.0)
    {
        if (isRecording() || sampleRate <= 0.0 || maxSeconds <= 0.0)
            return false;
        const auto capacity = (size_t) (sampleRate * maxSeconds);
        if (capacity == 0)
            return false;
        buffer = std::make_unique<Buffer>();
        buffer->data.assign(capacity, 0.0f);
        rate = sampleRate;
        written.store(0);
        anchorSet.store(false);
        anchorPpqValue.store(0.0);
        anchorBpmValue.store(120.0);
        cap.store(false);
        firstWrite = true;
        active.store(buffer.get()); // release: the audio thread sees the buffer fully built
        return true;
    }

    // Message thread. Returns what was recorded (empty audio if never armed).
    Recording stop()
    {
        Recording r;
        if (buffer == nullptr)
            return r;
        active.store(nullptr);
        while (inWrite.load())
            std::this_thread::yield();
        const auto n = (size_t) written.load();
        buffer->data.resize(std::min(n, buffer->data.size()));
        r.audio = std::move(buffer->data);
        r.sampleRate = rate;
        if (anchorSet.load())
            r.anchorPpq = anchorPpqValue.load();
        r.anchorBpm = anchorBpmValue.load();
        r.hitCap = cap.load();
        buffer.reset();
        return r;
    }

    // Audio thread. Appends one mono chunk if armed. ppqAtFirstSample is the host position of
    // `mono[0]`; it is only read on the first write of a take, and only if hostPlaying.
    void write(const float* mono, int n, bool hostPlaying, double ppqAtFirstSample, double bpm) noexcept
    {
        inWrite.store(true);
        if (auto* b = active.load())
        {
            if (firstWrite)
            {
                firstWrite = false;
                if (hostPlaying)
                {
                    anchorPpqValue.store(ppqAtFirstSample);
                    anchorBpmValue.store(bpm);
                    anchorSet.store(true);
                }
            }
            const int pos = written.load();
            const int room = (int) b->data.size() - pos;
            const int count = std::min(n, std::max(room, 0));
            if (count > 0)
            {
                std::copy_n(mono, count, b->data.data() + pos);
                written.store(pos + count);
            }
            if (count < n)
                cap.store(true);
        }
        inWrite.store(false);
    }

    bool isRecording() const noexcept { return active.load() != nullptr; }
    double secondsRecorded() const noexcept { return rate > 0.0 ? (double) written.load() / rate : 0.0; }
    bool capReached() const noexcept { return cap.load(); }

private:
    struct Buffer
    {
        std::vector<float> data;
    };

    std::unique_ptr<Buffer> buffer; // owned by the message thread; the audio thread sees `active`
    std::atomic<Buffer*> active { nullptr };
    std::atomic<bool> inWrite { false };
    std::atomic<int> written { 0 };
    std::atomic<bool> cap { false };
    std::atomic<bool> anchorSet { false };
    std::atomic<double> anchorPpqValue { 0.0 };
    std::atomic<double> anchorBpmValue { 120.0 };
    bool firstWrite = true; // audio thread only, reset by arm() before publishing
    double rate = 0.0;      // written by arm() before publishing; read by the UI getter
};
} // namespace murmur::take
