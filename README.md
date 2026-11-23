# RatBox26

RatBox26 is a vocal channel strip for 64-bit Windows 11 and macOS DAWs. It is available as VST3 on both platforms and as an Audio Unit on macOS. The editor groups the controls in signal-flow order and displays input, output, noise reduction, and compressor gain reduction. It supports mono, stereo, and mono-to-stereo layouts where the DAW offers them. Controls can be automated, and DAW sessions save the plugin settings.

## Signal path

| Stage | What it does |
| --- | --- |
| Input Trim | Adjusts the signal before Low Cut and the rest of the chain from −18 to +18 dB, starting at 0 dB. The input meter follows the trimmed level. In global Bypass, it shows the original signal level. |
| Low Cut | A 12 dB/octave high-pass filter from 70 to 350 Hz. Click the Low Cut checkbox to switch the filter off or on. It starts on so older sessions retain their sound. |
| Background Noise | Adaptive downward expansion that estimates the noise floor and attenuates steady background during gaps and quiet passages. Adjust it while listening to the quiet ends of phrases. Background that overlaps a loud vocal can remain audible. |
| De-ess | Dynamic high-frequency reduction for sharp consonants. |
| Tonal Focus | A bell EQ with a 150–500 Hz center and −6 to +6 dB of gain. It starts flat at 280 Hz. Apply a small temporary boost and sweep the center to locate the tone, then set the final cut or boost. |
| Character | Adds presence, air, and light saturation inspired by the Sony C‑800G sound. Results depend on the voice, recording, and microphone. |
| Compression | Light peak control with a linked detector that follows the EQ and character stages. |
| Reverb | Filtered stereo reverb with adjustable level, decay, and vocal-driven ducking. The detector uses the compressed vocal. The wet signal recedes during phrases and recovers in the gaps. The adaptation runs locally using DSP. |
| Output | Final gain. The **CLIP** indicator stays lit after a sample reaches 0 dBFS. Click **CLIP** to clear it. Global Bypass passes the original dry audio. |

## Install on Windows 11 x64

1. Sign in to GitHub and open a successful Windows VST3 workflow run. Under **Artifacts**, download `RatBox26-<version>-Windows-x64-VST3`. The ZIP contains the complete `RatBox26.vst3` bundle and this guide. [Windows VST3 workflow](https://github.com/brian513black-creator/RatBox26/actions/workflows/windows.yml)
2. On a machine without the current x64 Microsoft Visual C++ Redistributable, install it before scanning the plugin. A build from source also requires the tools listed below. [Microsoft Visual C++ Redistributable downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
3. Close your DAW, extract the ZIP, and copy the entire extracted `RatBox26.vst3` folder to `C:\Program Files\Common Files\VST3`. If you are updating, replace the previous bundle in that location.
4. Open the DAW, rescan VST3 plugins, and insert **RatBox26** on a vocal track.

## Install on macOS

The universal Mac build targets macOS 11 or later on Intel and Apple silicon Macs.

1. Sign in to GitHub and download `RatBox26-<version>-macOS-Universal-VST3-AU` from a successful macOS workflow run. Extract the download, then open the ZIP inside it to get the two plugin bundles. [macOS workflow](https://github.com/brian513black-creator/RatBox26/actions/workflows/macos.yml)
2. Close your DAW. In Finder, use **Go → Go to Folder** and copy the complete bundle for the format your DAW uses. Create the destination folder if needed.

   | Format | Bundle | Per-user destination |
   | --- | --- | --- |
   | VST3 | `RatBox26.vst3` | `~/Library/Audio/Plug-Ins/VST3` |
   | Audio Unit (Logic Pro and other AU hosts) | `RatBox26.component` | `~/Library/Audio/Plug-Ins/Components` |

3. Reopen the DAW, rescan or validate its plugins, and insert **RatBox26** on a vocal track.

The CI bundles are ad hoc signed. Developer ID signing and Apple notarization require release credentials.

## Check the plugin on a recorded vocal

1. Loop a phrase with quiet words, sibilants, and a pause. Check the input meter and adjust Input Trim if the chain is receiving too much or too little level. Then adjust Low Cut, Background Noise, and De-ess while listening for clear words and natural breaths.
2. Switch **Bypass** on and off. Adjust **Output** with bypass off until the processed and dry phrase sound equally loud. Then compare intelligibility, tone, and compression at that matched level.
3. Increase **Reverb Level** enough to hear its tail and listen to the pause. Adjust **Vocal Ducking** to hear the wet signal recede during the phrase and recover afterward.
4. Save the session, reopen it, and confirm the controls and bypass setting return to the saved values. Bounce the phrase and check the tail and output level against playback.

## Build from source on Windows 11

Install Visual Studio 2022 with **Desktop development with C++**, CMake 3.22 or later, and Git. From the repository root in PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

CMake fetches JUCE 8.0.6. The Release bundle is at `build/RatBox26_artefacts/Release/VST3/RatBox26.vst3`. Copy it to the same installation path as the downloaded bundle.

Windows CI builds the x64 VST3, runs the DSP and host smoke tests, and uploads the versioned package. It then downloads the artifact, checks the bundle layout and version, and runs the smoke test against the packaged plugin. The host test covers the three channel layouts, inactive passthrough, clearing audio after restart, automation, state restore, and opening and resizing the editor.

## Build from source on macOS

Install Xcode or its Command Line Tools, CMake 3.22 or later, and Git. From the repository root:

```sh
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
cmake --build build-macos --parallel 3
ctest --test-dir build-macos --output-on-failure
```

The bundles are in `build-macos/RatBox26_artefacts/Release/VST3` and `build-macos/RatBox26_artefacts/Release/AU`. Mac CI validates the universal archive on native Intel and Apple silicon runners, including the VST3 smoke tests and Apple's Audio Unit validator.

## DSP checks without JUCE

```sh
cmake -S . -B build-dsp -DRATBOX_BUILD_PLUGIN=OFF
cmake --build build-dsp
ctest --test-dir build-dsp --output-on-failure
```

The checks cover EQ, noise reduction, de-essing, character, compression, stereo reverb tails, vocal ducking, bypass, and nonfinite input at 44.1, 48, and 96 kHz.
