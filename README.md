# RatBox26

RatBox26 is a vocal channel-strip VST3 for 64-bit Windows 11 DAWs. The editor groups the controls in signal-flow order and displays input, output, noise reduction, and compressor gain reduction. It supports mono, stereo, and mono-to-stereo layouts where the DAW offers them. Controls can be automated, and DAW sessions save the plugin settings.

## Signal path

| Stage | What it does |
| --- | --- |
| Low Cut | A 12 dB/octave high-pass filter. Start at 70 Hz and raise it toward 350 Hz as needed to reduce rumble or proximity buildup. |
| Background Noise | Adaptive downward expansion that estimates the noise floor and attenuates steady background during gaps and quiet passages. Adjust it while listening to the quiet ends of phrases. Background that overlaps a loud vocal can remain audible. |
| De-ess | Dynamic high-frequency reduction for sharp consonants. |
| Tonal Focus | A bell EQ with a 150–500 Hz center and −6 to +6 dB of gain. It starts flat at 280 Hz. Apply a small temporary boost and sweep the center to locate the tone, then set the final cut or boost. |
| Character | Adds presence, air, and light saturation inspired by the Sony C‑800G sound. Results depend on the voice, recording, and microphone. |
| Compression | Light peak control with a linked detector that follows the EQ and character stages. |
| Reverb | Filtered stereo reverb with adjustable level, decay, and vocal-driven ducking. The detector uses the compressed vocal. The wet signal recedes during phrases and recovers in the gaps. The adaptation runs locally using DSP. |
| Output | Final gain, with a red indication on peaks at or above 0 dBFS. Use Bypass for a dry comparison. |

## Build and install on Windows 11

Install Visual Studio 2022 with **Desktop development with C++**, CMake 3.22 or later, and Git. From the repository root in PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

CMake fetches JUCE 8.0.6. Copy the entire `build/RatBox26_artefacts/Release/VST3/RatBox26.vst3` bundle to `C:\Program Files\Common Files\VST3`, then rescan plugins in the DAW. The Windows GitHub Actions workflow builds the x64 VST3, runs DSP tests, and loads the built plugin in a smoke-test host before uploading the bundle. The host checks mono, stereo, mono-to-stereo, dry passthrough while the plugin is inactive, and clearing audio after a processing restart.

## DSP checks without JUCE

```sh
cmake -S . -B build-dsp -DRATBOX_BUILD_PLUGIN=OFF
cmake --build build-dsp
ctest --test-dir build-dsp --output-on-failure
```

The checks cover EQ, noise reduction, de-essing, character, compression, stereo reverb tails, vocal ducking, bypass, and nonfinite input at 44.1, 48, and 96 kHz.
