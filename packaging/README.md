# Release packaging

Two artifacts are produced from one staged folder.

## Stage

```bash
# build first (see README.AkvariumOBS.md), then:
rm -rf release && mkdir -p release/stage/AkvariumOBS
cp -r build_x64/rundir/RelWithDebInfo/{bin,data,obs-plugins} release/stage/AkvariumOBS/
find release/stage -name "*.pdb" -delete
rm -f release/stage/AkvariumOBS/bin/64bit/akvarium-*-test.exe

# speech model shipped with the app (data/voice is discovered at runtime)
mkdir -p release/stage/AkvariumOBS/data/voice
cp /path/to/ggml-base.bin release/stage/AkvariumOBS/data/voice/

# VC++ runtime + OpenMP + Vulkan loader, so no redistributable is needed
CRT="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/<ver>/x64"
cp "$CRT/Microsoft.VC143.CRT/"{msvcp140,msvcp140_1,msvcp140_2,vcruntime140,vcruntime140_1}.dll \
   "$CRT/Microsoft.VC143.OpenMP/vcomp140.dll" C:/Windows/System32/vulkan-1.dll \
   release/stage/AkvariumOBS/bin/64bit/

cp COPYING release/stage/AkvariumOBS/
cp packaging/README.txt release/stage/AkvariumOBS/
```

## Portable

```powershell
Compress-Archive -Path 'release\stage\AkvariumOBS' `
                 -DestinationPath 'release\AkvariumOBS-32.2.2-Windows-x64-Portable.zip' -Force
```

## Installer (NSIS 3)

```bash
"C:/Program Files (x86)/NSIS/makensis.exe" /V2 \
  /DSTAGE="G:/Repository/release/stage/AkvariumOBS" /DVERSION=32.2.2 packaging/installer.nsi
```

The installer writes `HKLM\Software\AkvariumOBS` and the Add/Remove Programs entry,
creates Start Menu and optional desktop shortcuts, and keeps user settings in
`%APPDATA%\obs-studio` (shared with OBS Studio, so profiles transfer directly).

Silent install for testing: `installer.exe /S /D=D:\path`. Uninstall: `uninstall.exe /S`.
