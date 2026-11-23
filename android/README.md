# Android

The ports also build as an Android app, with on-screen touch controls, tilt steering and gamepads.
It is the same recompiled game and runtime as on the desktop, in the enhanced mode, with the same
touch controls as the [iOS app](../ios/README.md). The game data comes from your own dumps, as for
the desktop build, and goes inside the app. The app is for your own devices only: do not share it
(see [Legal](../README.md#legal)).

## What you need

- A Mac or a Linux computer that builds the desktop port ([Build](../README.md#build)): the app is
  built from the same recompiled game, and the desktop executable calibrates its controls.
- The Android SDK with the NDK: install Android Studio, then in its SDK Manager add "NDK (Side by
  side)" and "CMake". `ANDROID_HOME` finds the SDK, or Android Studio's default place.
- JDK 17 (`JAVA_HOME`; on macOS it is found by itself). Python 3.
- An Android phone or tablet with Android 7 or later, 64-bit (arm64), with OpenGL ES 3.0.
- Your files in `roms/`, as for the desktop build ([Required files](../README.md#required-files)).

## Build and install

```sh
python3 android/prepare.py thrild2 --install      # or gticlub2
```

With the phone connected over USB and USB debugging on (Settings > About phone: tap Build number
seven times, then Settings > System > Developer options > USB debugging), `--install` installs it.
Without `--install`, the APK is `build/android/<id>/<binary>.apk`: copy it to the phone and open it
there (Android asks to allow installing apps from that source).

`android/prepare.py` does the work:

1. it extracts and recompiles the game if that is not done yet (`make extract`, `make recomp`);
2. it builds the desktop executable and runs the first-run calibration with it (`--calibrate`):
   the app takes this calibrated NVRAM on its first launch;
3. it stages what the game reads (the kernel, the CF image, the menu font, the NVRAM, the DS2430
   and the BIOS, about 32 MB) in `build/android/<id>/data/` (steps 1-3: `tools/mobile.py`, shared
   with the iOS app);
4. it fetches SDL2's release sources once, into `build/android/`;
5. it makes the Gradle project `build/android/<id>/project/` from SDL's `android-project` and
   this folder's files; the game's files go into its assets;
6. Gradle builds the APK, signed with this computer's debug key.

The first build downloads Gradle and the Android build plugin, and compiles the whole game: some
minutes. Later builds are quicker.

As for iOS, there is no ready-made APK: the game is recompiled into the app's code, built from
your dumps.

## Playing

The controls, the menus and the options are the iOS app's ([Playing](../ios/README.md#playing)).
Only what differs:

- **Saves:** the NVRAM and the port's settings are in the app's internal storage. Uninstalling
  the app removes them.
- **The first launch** copies the game's files out of the APK (a few seconds), and so does the
  first launch after an update.
- **Haptics:** the touch buttons tap as the system keyboard does, with its setting (Settings >
  Sound & vibration).
- **Settings that restart the game** close the app, as on iOS: open it again.

## Multiplayer

The app links as the desktop ports do, over the Internet with a session code
([Multiplayer](../README.md#multiplayer)): with the desktop ports, other Android phones and the
iOS app. The host's router opens the port by itself (UPnP or NAT-PMP), as for the desktop. To type
or paste a code, the JOIN screen brings up the keyboard.

## Development notes

- **The desktop builds are unchanged:** what the app adds to the shared sources is behind
  `VIPER_ANDROID`, `VIPER_MOBILE` (the phones' common code: the touch controls, the paths, the
  app's life, the menus) and `VIPER_GLES`, which only `android/CMakeLists.txt` defines.
  `runtime/touch.c` and `runtime/platform_android.c` are not in the desktop sources.
- **`runtime/platform_android.c`:** the game's files copied out of the assets (with a stamp per
  build, `viper_files.txt`), the internal storage, the display cutout and the haptic tap (two
  methods of `java/org/viperrecomp/ViperActivity.java`, through JNI), tilt from the accelerometer
  (SDL's sensors), and stderr to logcat with the tag `viper`: `adb logcat -s viper`.
- **Frame pacing:** the swap waits for the vsync, as on the desktop (no display link as on iOS).
- **The sources** come from `tools/buildspec.py`, as for the desktop and iOS builds; the link's
  UPnP and NAT-PMP (`third_party/`) are in.
