#pragma once

#include "GranulatorEngine.h"

#include <array>
#include <atomic>
#include <cmath>

namespace tide
{
class GrainSnapshotExchange
{
public:
    using Frame = GranulatorEngine::VisualFrame;

    void publish(const Frame& frame) noexcept
    {
        frames[static_cast<size_t>(back)] = frame;
        back = middle.exchange(back, std::memory_order_acq_rel);
        dirty.store(true, std::memory_order_release);
    }

    bool readLatest(Frame& destination) noexcept
    {
        const auto changed = dirty.exchange(false, std::memory_order_acquire);
        if (changed)
            front = middle.exchange(front, std::memory_order_acq_rel);

        destination = frames[static_cast<size_t>(front)];
        return changed;
    }

private:
    std::array<Frame, 3> frames {};
    std::atomic<int> middle { 1 };
    std::atomic<bool> dirty { false };
    int front = 0;
    int back = 2;
};

inline float sourceProgressInWindow(
    const GranulatorEngine::VisualFrame& frame,
    const GranulatorEngine::GrainVisualState& grain,
    const double waveformNewestSample,
    const double visibleSampleCount) noexcept
{
    if (visibleSampleCount <= 0.0 || ! std::isfinite(grain.sourceDelaySamples))
        return -1.0f;

    const auto sourceSample = static_cast<double>(frame.totalSamplesWritten)
        - grain.sourceDelaySamples;
    const auto samplesBehindRightEdge = waveformNewestSample - sourceSample;
    return static_cast<float>(1.0 - samplesBehindRightEdge / visibleSampleCount);
}
} // namespace tide
