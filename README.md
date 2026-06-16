![RTCW Banner](https://github.com/DrBeef/RTCWQuest/blob/master/assets/banner.jpg)

# RTCWQuest

RTCWQuest is a standalone Android VR port of the _Return to Castle Wolfenstein_
single-player campaign. The current development branch has been migrated away
from the original Oculus Mobile SDK / VrApi backend and now uses OpenXR with an
arm64-only Android build.

This repository contains the engine, Android wrapper, VR integration, and the
project's VR assets. It does not contain the full commercial RTCW game data.

## Current Platform Status

The current Android build is:

- OpenXR based, using the Khronos Android OpenXR loader.
- arm64-v8a only.
- Built with Android Gradle Plugin 8.2.1 and NDK 25.1.8937393.
- Targeted at Android API 32 with a minimum API level of 29.
- Intended for standalone Android OpenXR headsets, including Meta Quest and Pico.
- Still using the RTCW/id Tech 3 engine structure, including native game modules
  for `qagame`, `cgame`, and `ui`.

The old VrApi/Oculus Mobile SDK build path is deprecated and should not be used
for this branch.

## Repository Layout

Important directories:

- `Projects/Android` - Android Gradle project.
- `Projects/Android/build.gradle` - main Android build configuration.
- `Projects/Android/AndroidManifest.xml` - app manifest, OpenXR permissions,
  headset launch categories, and runtime metadata.
- `Projects/Android/jni` - native Android/NDK build root.
- `Projects/Android/jni/RTCWVR` - VR layer, OpenXR backend, input, frame setup,
  and Android surface integration.
- `Projects/Android/jni/rtcw` - RTCW engine, game, cgame, ui, renderer, and
  Android native build files.
- `Projects/Android/jni/SupportLibs/gl4es` - gl4es support library.
- `Projects/Android/z_vr_assets` - VR asset source folder packed into
  `assets/z_vr_assets.pk3` during Gradle builds.
- `java/com/drbeef/rtcwquest` - Java activity and native library bootstrap.
- `assets` - packaged APK assets.
- `res` - Android resources.

## Required Tools

Install:

- Android Studio or command-line Android SDK tools.
- Android SDK Platform 32.
- Android NDK `25.1.8937393`.
- Java runtime compatible with Android Gradle Plugin 8.2.1.
- ADB for installing and launching on a headset.

The Gradle wrapper is included in `Projects/Android`, so a separate Gradle
installation is not required.

## Android SDK Configuration

Create or update:

```text
Projects/Android/local.properties
```

Example:

```properties
sdk.dir=C\:\\Users\\yourname\\AppData\\Local\\Android\\Sdk
ndk.dir=C\:\\Users\\yourname\\AppData\\Local\\Android\\Sdk\\ndk\\25.1.8937393
```

Use paths that match your machine. Do not commit personal SDK paths.

## Building

From the repository root:

```powershell
cd Projects\Android
.\gradlew.bat assembleDebug
```

The debug APK is written to:

```text
Projects/Android/build/outputs/apk/debug/rtcwquest-debug.apk
```

For a release build:

```powershell
cd Projects\Android
.\gradlew.bat assembleRelease
```

The release APK is written to:

```text
Projects/Android/build/outputs/apk/release/rtcwquest-release.apk
```

The default debug and release signing configs use
`Projects/Android/android.debug.keystore` unless Gradle properties are supplied:

```properties
key.store=...
key.store.password=...
key.alias=...
key.alias.password=...
```

## Installing On A Headset

Connect the headset with developer mode enabled, then run:

```powershell
adb devices
adb install -r -d Projects\Android\build\outputs\apk\debug\rtcwquest-debug.apk
```

Launch from the headset UI, or from ADB:

```powershell
adb shell monkey -p com.drbeef.rtcwquest -c android.intent.category.LAUNCHER 1
```

For clean testing:

```powershell
adb shell am force-stop com.drbeef.rtcwquest
adb logcat -c
adb shell monkey -p com.drbeef.rtcwquest -c android.intent.category.LAUNCHER 1
```

Useful logcat filters:

```powershell
adb logcat RTCWVR:* OpenXR:* AndroidRuntime:* libc:* *:S
```

## Game Data

This is an engine port. The full commercial game data is not included.

To play the full game, install RTCW on a PC and copy the relevant `.pk3` files
from the installed RTCW game folder to the headset's RTCWQuest data directory.
The app creates its folders after the first launch.

Typical headset path:

```text
/sdcard/RTCWQuest/Main
```

If the folder does not appear immediately over USB/MTP, launch the app once and
restart or reconnect the headset.

## OpenXR Migration Notes

The original RTCWQuest build was Quest-only and used Oculus VrApi. This branch
uses OpenXR instead.

Key points:

- Java loads `openxr_loader` before `rtcw_client`.
- Gradle depends on:

```groovy
org.khronos.openxr:openxr_loader_for_android:1.1.60
```

- The build extracts the loader AAR's arm64 shared library into generated
  `jniLibs`:

```text
Projects/Android/build/generated/openxr-loader/jniLibs/arm64-v8a/libopenxr_loader.so
```

- `Projects/Android/jni/Android.mk` imports that generated
  `libopenxr_loader.so` as a prebuilt shared library.
- The native client links against `openxr_loader` and `gl4es`.
- The manifest declares OpenXR permissions, runtime broker queries, and
  immersive headset launch categories.
- OpenXR runtime selection is handled by the platform runtime broker on the
  headset.

The native OpenXR code lives primarily in:

- `Projects/Android/jni/RTCWVR/TBXR_Common.c`
- `Projects/Android/jni/RTCWVR/TBXR_Common.h`
- `Projects/Android/jni/RTCWVR/OpenXrInput.c`
- `Projects/Android/jni/RTCWVR/RTCWVR_SurfaceView.c`

The OpenXR backend initializes the Android OpenXR loader, creates the instance
and session, uses OpenGL ES swapchains, and drives the frame lifecycle used by
the existing RTCW VR hooks.

## 64-bit Migration Notes

This branch is arm64-only. The build intentionally does not produce
`armeabi-v7a` output.

Relevant build settings:

- `Projects/Android/build.gradle`
  - `abiFilters 'arm64-v8a'`
  - `minSdkVersion 29`
  - `targetSdkVersion 32`
  - `compileSdkVersion 32`
  - `ndkVersion '25.1.8937393'`
- `Projects/Android/jni/Application.mk`
  - `APP_ABI := arm64-v8a`
  - `APP_MODULES := gl4es qagame ui cgame rtcw_client`

The native game modules are built as normal shared libraries:

- `libqagame.so`
- `libcgame.so`
- `libui.so`
- `librtcw_client.so`
- `libgl4es.so`
- `libopenxr_loader.so`

The 64-bit conversion requires care around the VM/native DLL boundary. In this
codebase, pointer-sized values crossing module boundaries must use pointer-sized
types such as `intptr_t`; do not reintroduce assumptions that pointers fit in
`int`. QVM bytecode and interpreted VM storage remain 32-bit where the original
engine format requires it.

When touching VM, syscall, or module-loading code, check for:

- Pointer-to-`int` casts.
- Function pointer casts through integer types.
- Syscall argument arrays using `int *` where native code needs `intptr_t *`.
- `VM_Call`, `vmMain`, and `dllEntry` signatures.
- Savegame compatibility code that reads historical 32-bit structures.

## Runtime Data And Saves

The app uses the RTCWQuest external data folder for user data and game data.
Existing 32-bit-era saves may need compatibility handling because native struct
sizes and pointer-sized fields changed in the arm64 build. The project contains
compatibility work for older save data, but any further savegame changes should
be tested against both newly-created arm64 saves and old user saves.

Recommended save testing:

- Start a new game, save, quit, relaunch, and load.
- Quick save and quick load.
- Load a legacy 32-bit save.
- Transition levels after loading.
- Verify cgame, game, and UI modules agree on struct sizes.

## VR Rendering Notes

The renderer still uses the RTCW/id Tech 3 rendering flow with gl4es and an
OpenGL ES backend. OpenXR owns headset frame timing and swapchain presentation.

Important behavior:

- Immersive gameplay is rendered stereo.
- Menus, loading, console, and non-immersive states use a virtual screen path.
- Gameplay HUD is drawn through the normal cgame 2D draw path rather than as an
  OpenXR compositor overlay.
- Scope and binocular FOV handling must keep the game/culling FOV and submitted
  OpenXR projection FOV in sync.

When changing FOV, culling, cinematic cameras, sky rendering, or HUD projection,
test all of these:

- Normal gameplay.
- Cinematic cutscenes.
- Loading screens.
- Console/menu virtual screen.
- Binocular zoom.
- Scoped weapons.
- Head pitch/yaw/roll.
- Skybox behavior.

## Input Notes

OpenXR actions are translated into the existing RTCWQuest VR input state and
then into engine commands. The goal is to preserve legacy RTCWQuest controls
while using OpenXR-backed controller poses and buttons.

Important gameplay behaviors to preserve:

- Dominant-hand weapon aiming.
- Left-handed mode.
- Snap and smooth turn.
- Weapon stabilization / virtual stock.
- Scope engagement.
- Binoculars.
- Backpack weapon selection.
- Grenade throwing.
- Mounted guns.
- Teleport.
- External haptics service events plus OpenXR controller haptics.

## Packaged APK Contents

A successful APK should include only `arm64-v8a` native libraries. Useful check:

```powershell
cd Projects\Android
.\gradlew.bat assembleDebug
jar tf build\outputs\apk\debug\rtcwquest-debug.apk | findstr /i "lib/.*\\.so"
```

Expected native libraries include:

- `lib/arm64-v8a/librtcw_client.so`
- `lib/arm64-v8a/libqagame.so`
- `lib/arm64-v8a/libcgame.so`
- `lib/arm64-v8a/libui.so`
- `lib/arm64-v8a/libgl4es.so`
- `lib/arm64-v8a/libopenxr_loader.so`

There should be no `armeabi-v7a` libraries in the APK.

## Troubleshooting

### Gradle cannot find the SDK or NDK

Check `Projects/Android/local.properties`. Make sure `sdk.dir` and `ndk.dir`
point to installed SDK/NDK folders.

### OpenXR runtime fails to initialize

Check that the headset has an OpenXR runtime installed and enabled. Pull logs:

```powershell
adb logcat RTCWVR:* OpenXR:* AndroidRuntime:* libc:* *:S
```

Verify `libopenxr_loader.so` is present in the APK and that Java is loading it
before `rtcw_client`.

### APK launches flat or exits immediately

Check:

- Manifest OpenXR permissions and categories.
- Runtime broker availability on the headset.
- `adb logcat` for OpenXR initialization errors.
- That the build contains only arm64 libraries on an arm64 headset.

### Missing game data

Launch once to create `/sdcard/RTCWQuest/Main`, then copy the full RTCW `.pk3`
files into that folder.

### Save load fails

Check whether the save was produced by the older 32-bit build or the current
arm64 build. Legacy saves may require compatibility conversion. Use logcat to
inspect reported struct sizes and savegame version checks.

## Development Guidance

For future work:

- Do not reintroduce Oculus VrApi or Oculus Mobile SDK dependencies.
- Keep the Android build arm64-only unless there is a deliberate reason to add
  another ABI.
- Keep OpenXR loader packaging through the Khronos AAR unless the project makes
  a deliberate loader strategy change.
- Be careful around VM/native boundary types.
- Avoid pointer truncation in savegame, syscall, and VM code.
- Test visual changes in both immersive stereo and virtual screen modes.
- Test on both Meta Quest and Pico when changing OpenXR runtime, manifest, or
  input behavior.

## Credits

RTCWQuest builds on a long chain of work from id Software, the original RTCW
GPL source release, Team Beef VR ports, Beloko Android porting work, gl4es, and
many project contributors.

Special thanks from the original project:

- Emile Belanger for the Android port base.
- Baggyg for VR support, assets, artwork, testing, and model work.
- VR_Bummser for testing and community support.
- The SideQuest team for sideloading support.
- Dark Matter Productions and William Faure for remastered weapon work.
- HellBaron for the Venom Mod basis.
- Johannes Tripolt (Eigenlaut) for weapon fixes and enhancements.
- ptitSeb for gl4es.

## License And Game Data

This repository is an engine/port project. RTCW game assets are owned by their
respective rights holders and are not included as full commercial game content.

The OpenXR migration removes the previous dependency on the deprecated Oculus
VrApi SDK path, making the VR backend friendlier to GPL distribution concerns.
