# AkvariumOBS

Personal fork of **OBS Studio 32.2.2** (GPLv2) with three additions:

1. **Voice commands for the Replay Buffer** — say a phrase into the microphone that the
   current scene uses and the replay is saved. Recognition runs fully offline
   (whisper.cpp, CPU) and only on detected speech.
2. **Per-scene audio mixer state** — every scene remembers which sources were muted
   and at what volume, so a mic can be muted in one scene and live in another.
3. **Scene binding to applications** — switch scenes when an application is in the
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

- One phrase per line, Russian or English.
- Pick a model (`tiny`, `base`, `small`) and press **Download / verify model** once.
  Models are stored in `<config>/AkvariumOBS/voice/`.
- Microphone: *Auto* follows the active audio-capture source of the current scene,
  or select a specific OBS audio source.
- With *Listen only while the Replay Buffer is running* (default) the engine is idle
  unless the buffer is active.

How it stays cheap: a WASAPI capture thread does energy VAD at 16 kHz mono; whisper
runs only on 0.3–6 s speech segments, on a low-priority worker, with `n_threads`
(default 2) and no context carry-over.

Command matching tolerates recognition errors (`сделай реплеи` matches `сделай реплей`,
`safe replay` matches `save replay`) and rejects long unrelated speech.

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
```

## License

GPLv2, same as OBS Studio. Original copyright notices and the `COPYING` file are kept.
`About` states the fork and its upstream base. Because the project is GPLv2, if you
distribute binaries you must also make the corresponding source available.
