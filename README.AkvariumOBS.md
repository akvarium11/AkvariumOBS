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

- One phrase per line, Russian or English. Nothing else has to be tuned.
- Pick a model preset (`Fast`/`Balanced`/`Accurate`) and press **Download / verify model**
  once. Models are stored in `<config>/AkvariumOBS/voice/`.
- Microphone: *Auto* follows the active audio-capture source of the current scene,
  a specific OBS source, or any Windows input/output device (output = loopback).
- With *Listen only while the Replay Buffer is running* (default) the engine is idle
  unless the buffer is active.

Latency is preset, not user-tuned: WASAPI capture at 16 kHz mono with an adaptive
noise gate, a 350 ms speech tail, and whisper running with half of the logical CPU
cores (`audio_ctx 768`, greedy, no context). Measured 0.7 s recognition for a 1.4 s
command on a Ryzen 5 5500; the model stays loaded between replay buffer runs.

Command matching tolerates recognition errors (`сделай реплеи` matches `сделай реплей`,
`safe replay` matches `save replay`) and rejects long unrelated speech.

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

# recognition on a WAV file
cmake --build build_x64 --config RelWithDebInfo --target akvarium-voice-selftest
akvarium-voice-selftest <model.bin> <audio.wav> "save replay|сделай реплей"

# clip sound: folder discovery, config round-trip, MCI playback
cmake --build build_x64 --config RelWithDebInfo --target akvarium-replay-sound-test
akvarium-replay-sound-test <empty scratch dir> [reference.mp3]
```

## License

GPLv2, same as OBS Studio. Original copyright notices and the `COPYING` file are kept.
`About` states the fork and its upstream base. Because the project is GPLv2, if you
distribute binaries you must also make the corresponding source available.
