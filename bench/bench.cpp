#include "GranulatorEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace tide;

namespace
{
struct Result
{
    double nsPerSample = 0.0;
    double realtimeFraction = 0.0; // processing time / audio time (one core)
};

Result runScenario(const float density,
                   const int sampleRate,
                   const int blockSize,
                   const int channels,
                   const long timedBlocks,
                   const bool sync = false)
{
    GranulatorEngine engine;
    engine.prepare(sampleRate, blockSize, channels, 0.5f, 0.3f, 0.6f, 0.3f);
    engine.setRandomSeed(0x1234ABCDULL);

    GranulatorEngine::Controls controls;
    controls.density = density;
    controls.mix = 0.5f;
    controls.timeSeconds = 1.0f;
    controls.grainSizeMilliseconds = 40.0f;
    controls.shape = 0.5f;
    controls.spread = 0.5f;
    controls.feedback = 0.3f;
    controls.tide = 0.6f;
    controls.drift = 0.3f;
    controls.sync = sync;
    controls.syncDivision = 4;
    controls.gridEnd = true;
    GranulatorEngine::Timing timing;
    timing.clockValid = sync;
    timing.playing = sync;
    timing.bpm = 120.0;

    // A fixed reference input block copied in fresh each iteration, mimicking a
    // host handing us a new buffer.
    juce::AudioBuffer<float> reference(channels, blockSize);
    for (int ch = 0; ch < channels; ++ch)
        for (int n = 0; n < blockSize; ++n)
            reference.setSample(ch, n,
                                0.25f * std::sin(2.0f * 3.14159265f * 220.0f
                                                 * static_cast<float>(n)
                                                 / static_cast<float>(sampleRate)));

    juce::AudioBuffer<float> buffer(channels, blockSize);

    // Warm-up: fill history and settle to the steady grain population.
    for (int b = 0; b < 400; ++b)
    {
        for (int ch = 0; ch < channels; ++ch)
            buffer.copyFrom(ch, 0, reference, ch, 0, blockSize);
        engine.process(buffer, controls, timing);
        timing.ppqPosition += static_cast<double>(blockSize) * timing.bpm
            / (60.0 * static_cast<double>(sampleRate));
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (long b = 0; b < timedBlocks; ++b)
    {
        for (int ch = 0; ch < channels; ++ch)
            buffer.copyFrom(ch, 0, reference, ch, 0, blockSize);
        engine.process(buffer, controls, timing);
        timing.ppqPosition += static_cast<double>(blockSize) * timing.bpm
            / (60.0 * static_cast<double>(sampleRate));
    }
    auto end = std::chrono::high_resolution_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const long samples = timedBlocks * blockSize;
    const double audioSeconds = static_cast<double>(samples)
        / static_cast<double>(sampleRate);

    Result r;
    r.nsPerSample = seconds / static_cast<double>(samples) * 1.0e9;
    r.realtimeFraction = seconds / audioSeconds;
    return r;
}
} // namespace

int main()
{
    const int sampleRate = 48000;
    const int blockSize = 512;
    const int channels = 2;
    const long timedBlocks = 40000; // ~7 min of audio per scenario

    std::printf("%-10s %14s %18s\n", "density", "ns/sample", "CPU (1 core)");
    std::printf("------------------------------------------------\n");
    for (float density : { 10.0f, 24.0f, 48.0f, 64.0f })
    {
        // Best-of-3 to reduce scheduler noise.
        Result best;
        best.nsPerSample = 1e18;
        for (int trial = 0; trial < 3; ++trial)
        {
            auto r = runScenario(density, sampleRate, blockSize, channels, timedBlocks);
            if (r.nsPerSample < best.nsPerSample)
                best = r;
        }
        std::printf("%-10.0f %12.3f ns %15.2f %%\n",
                    density, best.nsPerSample, best.realtimeFraction * 100.0);
    }
    const auto sync = runScenario(64.0f, sampleRate, blockSize, channels,
                                  timedBlocks, true);
    std::printf("sync-64    %12.3f ns %15.2f %%\n",
                sync.nsPerSample, sync.realtimeFraction * 100.0);
    return 0;
}
