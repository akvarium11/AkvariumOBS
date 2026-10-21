AkvariumOBS 1.1a (Windows x64)
============================

Fork of OBS Studio 32.2.2 (GPLv2) with:
  - voice commands for the Replay Buffer (offline recognition, CPU or GPU);
  - replay clip sounds (pick one audio file played on every replay save);
  - per-scene audio mixer state (mute/volume remembered per scene);
  - scene binding to applications (foreground / running / closed rules).

Requirements
------------
  - Windows 10 or 11, 64-bit
  - x64 CPU with AVX2 for speech recognition (falls back gracefully)
  - Vulkan-capable GPU is optional: recognition is faster on the GPU, otherwise CPU
  - No Visual C++ redistributable or Vulkan runtime install is required: both are
    bundled in bin\64bit

Installed version
-----------------
Run AkvariumOBS from the Start Menu. Settings and scenes live in
%APPDATA%\obs-studio, exactly like OBS Studio, so an existing OBS setup can be
imported on first start (the app asks once).

Portable version
----------------
Unpack anywhere and run bin\64bit\AkvariumOBS.exe. Add --portable to keep all
settings next to the application (in .\config) instead of %APPDATA%.

Voice commands
--------------
Settings -> Output -> Replay Buffer -> Voice Commands (Replay Buffer):
  1. enable voice commands;
  2. click "Train new phrase (5 samples)" to enroll your trigger phrases;
  3. pick the microphone (Auto follows the active mic of the current scene);
  4. start the Replay Buffer and say the phrase.
The recognition model (ggml-base-q8_0.bin) ships with the package in data\voice.

Replay clip sound
-----------------
Settings -> Output -> Replay Buffer -> Replay Sound (Replay Buffer):
  1. press "Open sounds folder" and drop your .wav / .mp3 / .wma files there
     (subfolders are scanned too);
  2. press "Refresh list", pick a sound, set the volume slider, then tick
     "Play a sound when a replay is saved" and press Apply.
The chosen file is played on the default output device every time a replay is
saved, whether that comes from the Save Replay hotkey, the button, the tray or a
voice command, so desktop-audio capture picks it up for the stream. The folder is
%APPDATA%\obs-studio\AkvariumOBS\sounds (or <app>\config\AkvariumOBS\sounds in
portable mode).

Per-scene audio state and application rules
-------------------------------------------
Both are enabled by default. Right-click a scene to toggle "Remember volume and
mute for this scene". Application rules live in the dock "Scene Binding to
Applications" on the right side of the main window.

License
-------
GPLv2, see COPYING. This is an unofficial fork, not affiliated with the OBS
Project. Original OBS Studio copyright notices are retained.
