# AkvariumOBS

Personal fork of **OBS Studio 32.2.2** (GPLv2) with four additions:

1. **Voice commands for the Replay Buffer** — say a phrase into the microphone that the
   current scene uses and the replay is saved. Recognition runs fully offline
   (whisper.cpp, CPU) and only on detected speech.
2. **Replay clip sound** — drop clipsounds into a folder, pick one in the settings and
   it plays every time the Replay Buffer saves a clip.
3. **Per-scene audio mixer state** — every scene remembers which sources were muted
   and at what volume, so a mic can be muted in one scene and live in another.
4. **Scene binding to applications** — switch scenes when an application is in the
   foreground, running in the background, or when it closes.

Product name, window title, executable (`AkvariumOBS.exe`), Windows version resource
and UI strings are rebranded. Internal OBS identifiers, config paths and plugin ABI
stay untouched so the fork remains compatible with upstream plugins.

## Build (Windows x64)

Requirements:

- Visual Studio 2022 Build Tools with the **Desktop development with C++** workload and
  the **C++ ATL** component
- Windows 11 SDK 10.0.26100
- CMake 3.28+ with the Visual Studio 17 2022 generator
- ~6 GB of disk for dependencies, ~4 GB for the build tree

```bash
cmake -S . -B build_x64 -G "Visual Studio 17 2022" -A x64 -DENABLE_BROWSER=ON
cmake --build build_x64 --config RelWithDebInfo --parallel
```

Dependencies (obs-deps, Qt6, CEF) are downloaded into `.deps` on the first configure.
If CMake cannot verify TLS (missing CA store), configure with `CMAKE_TLS_VERIFY=0`;
all downloads are hash-checked against `CMakePresets.json`.

Run the build: `build_x64/rundir/RelWithDebInfo/bin/64bit/AkvariumOBS.exe --portable`

## Voice commands

Settings → Output → Replay Buffer → **Voice Commands (Replay Buffer)**.

- **5-sample enrollment wizard:** click "Train new phrase (5 samples)" — speak your phrase 5 times with guided VAD pacing. The engine stores pronunciation variations and automatically adds the phrase to your active triggers.
- **Pill tag phrase management:** active trigger phrases are displayed as visual pill badges with one-click deletion (`[ ✕ phrase ]`).
- **Initial prompt keyword biasing:** Whisper is prompted with active phrases so recognition accuracy is sharp even in loud game environments.
- **On-the-spot verification:** click "Test recognition" to test your microphone and verify matching in real-time.
- **Morphology and word forms:** supports natural Russian verb/noun forms ("откат" / "откати" / "откатить", "сохрани" / "сохранить", "клип" / "клипни" / "заклипай") and English commands ("save replay", "clip that").
- **False positive prevention:**
  - Conversational speech isolation: mentioning a trigger word inside long speech or Discord banter is filtered out.
  - Subtitle / caption artifact filter.
  - Duplicate trigger cooldown: 2.5 second lockout.
- **First-try reliability:**
  - Adaptive VAD: background noise is tracked continuously, and a 250 ms pre-buffer prevents clipped initial consonants.
  - Natural speech pauses: utterance end gap is set to 450 ms so multi-word commands are never split in half.
  - Full-size Whisper context (`audio_ctx = 0`) and compact `base-q8_0` model (~82 MB) for low latency (~80 ms) and minimal memory footprint (~75 MB RAM).
- **Sensitivity slider:** 1 (loud room) to 5 (whisper / quiet voice), default 3.
- Microphone: *Auto* follows the active audio-capture source of the current scene, or any chosen WASAPI input/output device.
- *Listen only while Replay Buffer is running* ensures zero CPU overhead when the buffer is not running.

## Replay clip sound

Settings → Output → Replay Buffer → **Replay Sound (Replay Buffer)**.

- **Open sounds folder** creates/opens `<config>/AkvariumOBS/sounds`
  (`%APPDATA%\obs-studio\AkvariumOBS\sounds`, or `<app>/config/...` in portable mode).
  Drop `.wav`, `.mp3` or `.wma` files there; subfolders are scanned.
- **Refresh list**, pick a sound, set the **Volume** slider (0–100 %), tick
  *Play a sound when a replay is saved*, **Apply**. **Preview** plays the selection
  immediately with the current volume.
- The sound is played on the default output device the moment a save is requested —
  by the Save Replay hotkey, the control button, the tray or a voice command — so
  desktop-audio capture puts it on stream. Playback is asynchronous and never blocks
  the save; the file handle is released once the clip has played.
  The hotkey path is covered by a `save_requested` signal on the replay buffer output,
  because that hotkey never goes through the frontend save path.
- Stored in `user.ini` as `[AkvariumReplaySound] Enabled` / `Sound` / `Volume`.

## Per-scene audio state

Automatic. On every scene switch the outgoing scene's mixer state is stored inside the
scene JSON, and the incoming scene's state is applied. Sources covered: everything in
the Audio Mixer, including global devices such as `Mic/Aux` and `Desktop Audio`.

Per scene opt-out: right-click a scene → **Remember volume and mute for this scene**.
**Capture current audio state for this scene** overwrites the stored snapshot.

## Scene binding to applications

Dock **Scene Binding to Applications** (right side).

Each rule: application exe, condition, target scene, enabled flag. Conditions:

| Condition | Fires when |
|---|---|
| In foreground | the application owns the foreground window |
| Running (foreground or background) | the process exists |
| Running but not in foreground | the process exists and is not focused |
| Not running (closed) | the process does not exist |

Rules are evaluated top-down every 700 ms; the first matching rule wins and scenes
switch only when the matched rule changes. Rules are stored in `user.ini`
(`[AkvariumAppScenes]`).

## Testing

```bash
# phrase matching (no model needed)
cmake --build build_x64 --config RelWithDebInfo --target akvarium-phrase-match-test
build_x64/frontend/RelWithDebInfo/akvarium-phrase-match-test.exe

# recognition on a WAV file: audio gate, decode confidence, phrase score
cmake --build build_x64 --config RelWithDebInfo --target akvarium-voice-selftest
akvarium-voice-selftest <model.bin> <audio.wav> "save replay|сделай реплей" [ru|en|auto]

# clip sound: folder discovery, config round-trip, MCI playback
cmake --build build_x64 --config RelWithDebInfo --target akvarium-replay-sound-test
akvarium-replay-sound-test <empty scratch dir> [reference.mp3]
```

## License

GPLv2, same as OBS Studio. Original copyright notices and the `COPYING` file are kept.
`About` states the fork and its upstream base. Because the project is GPLv2, if you
distribute binaries you must also make the corresponding source available.
