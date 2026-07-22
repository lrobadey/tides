# TIDES

TIDES is a mono/stereo granular audio effect built around the motion of the ocean.
It transforms a rolling window of incoming audio into waves of particulate
sound, moving continuously between immediate granulation and a dispersed grain
delay.

## Current state

The plug-in now contains the integrated asynchronous granular-wave DSP:

- AU, VST3, and standalone targets through JUCE and CMake
- stable host-automation parameter IDs
- parameter state save/restore
- a custom resizable JUCE editor with a parameter-reactive tidal field
- a heavily branded deep-water visual system with live internal-history waves and exact grain read heads
- a preallocated five-second stereo rolling buffer
- a preallocated pool supporting a target population of 1–64 simultaneous one-shot grains
- independently staggered grain births with deterministic timing jitter and no shared amplitude cycle
- Size-defined nominal lifetimes: each grain plays once, releases, and never restarts
- a slowly moving shared source centroid that keeps the captured input recognizable
- Tide-controlled birth trails: low Tide stays close behind the centroid; high Tide spills farther into its wake
- bounded, deterministic Drift in launch timing, source position, playback speed, and stereo placement
- per-grain birth state that remains fixed until that grain finishes
- stereo-preserving output with smoothed population normalisation for mostly steady aggregate loudness
- rounded-saw-to-sine one-shot grain envelopes
- working Time, Tide, Size, Density, Shape, Spread, Drift, Feedback, and equal-power Mix controls
- a stereo-linked final safety limiter with a -1 dBFS ceiling

The editor displays the exact internal source history that the granulator reads,
including the feedback return. Every active grain is placed at its current
sample-derived read position. Marker brightness follows the real grain envelope,
the progress ring follows its age and lifetime, vertical position and colour
follow pan, and a direction tick exposes playback-speed Drift. The source
centroid is calculated from those live read heads rather than animated separately.

## Initial controls

- **Time**: selected buffer history, up to 5 seconds
- **Tide**: distance that new births may trail the moving source centroid; low is tight, high is broad
- **Size**: nominal lifetime of each one-shot grain
- **Density**: target simultaneous population, from 1 to 64 sounding grains
- **Shape**: softened descending saw envelope to sine envelope
- **Spread**: stereo placement range
- **Drift**: bounded launch-timing, source, playback-speed, and spatial variation
- **Feedback**: processed grains returned to the effect
- **Mix**: dry/wet balance

The scheduler launches new grains before older grains finish, at a rate derived
from Density and Size, with deterministic jitter to avoid a mechanical clock.
Existing grains retain the state they captured at birth, so control changes wash
through the texture as new generations arrive. Tide changes the source-history
trail behind the centroid, not a shared amplitude phase. Drift stays bounded so
the texture moves while the source remains recognizable.

## Host audio

TIDES processes the mono or stereo audio buffer supplied by the host. It can be
inserted on an audio/sample track or after a software instrument on a MIDI track.
It does not consume raw MIDI events. The standalone target can instead process a
selected live audio input.

Pitch shifting and buffer freeze are intentionally outside the first version.
The editable amplitude curve for the delayed grain trail will be stored as
structured plug-in state rather than flattened into a single knob.

## Build

Prerequisites: CMake 3.22 or newer, Git, and the Apple command-line developer
tools on macOS. JUCE 8.0.13 is fetched during the first CMake configuration.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target TideGrainsTests TideGrains_Standalone TideGrains_AU TideGrains_VST3 --parallel 4
ctest --test-dir build --output-on-failure
open "build/TideGrains_artefacts/Debug/Standalone/TIDES.app"
```

Generated plug-ins remain inside the build directory because automatic copying
into system plug-in folders is disabled.

JUCE has its own licensing terms. Review them before distributing a build.
