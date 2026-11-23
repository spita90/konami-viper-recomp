# iPhone and iPad

The ports also build as an iOS app, with on-screen touch controls, tilt steering and gamepads. It is
the same recompiled game and runtime as on the desktop, in the enhanced mode. The game data comes
from your own dumps, as for the desktop build, and goes inside the app. The app is for your own
devices only: do not share it (see [Legal](../README.md#legal)).

## What you need

- A Mac with Xcode 15 or later (and its command-line tools), Python 3, CMake 3.21 or later and
  SDL2 for the desktop build: `brew install cmake sdl2`.
- An iPhone or iPad with iOS 15 or later, and an Apple ID added in Xcode (Settings > Accounts). A
  free Apple ID works: its apps must be installed again from Xcode every 7 days.
- Your files in `roms/`, as for the desktop build ([Required files](../README.md#required-files)).

## Build and install

```sh
make ios GAME=thrild2 TEAM=ABCDE12345     # or: python3 ios/prepare.py thrild2 --team ABCDE12345
open build/ios/thrild2/xcode/viper_ios.xcodeproj
```

`TEAM` is your development team's ID (Xcode > Settings > Accounts, or the Apple Developer site).
Without it, choose the team in Xcode: the `app` target, Signing & Capabilities. Then connect the
iPhone, choose it as the destination next to the `app` scheme, and Run. The first time, iOS asks you
to trust the developer: Settings > General > VPN & Device Management. On iOS 16 and later, also
turn on Developer Mode (Settings > Privacy & Security).

`ios/prepare.py` does the work:

1. it extracts and recompiles the game if that is not done yet (`make extract`, `make recomp`);
2. it builds the desktop executable and runs the first-run calibration with it (`--calibrate`).
   An iOS app cannot start another program, so the app takes this calibrated NVRAM on its first
   launch;
3. it copies what the game reads (the kernel, the CF image, the menu font, the NVRAM, the DS2430
   and the BIOS, about 32 MB) to `build/ios/<id>/data/`;
4. it makes the app's icon from a frame of the attract mode;
5. it generates the Xcode project with CMake (`ios/CMakeLists.txt`), which downloads the SDL2
   sources once.

`--build sim` also builds the app for the iOS Simulator, and `--build device` for a device (with
`--team`). `--bundle-id` sets the app's identifier. By default it is `local.viper.<binary>.<random>`,
kept in `build/ios/<id>/bundle_id`.

### An unsigned IPA

```sh
python3 ios/prepare.py thrild2 --build ipa      # -> build/ios/thrild2/td2-unsigned.ipa
```

This builds the app for a device without signing it and packs it as an IPA, for a sideloading tool
that signs it with your Apple ID and installs it (AltStore, SideStore, Sideloadly, ...). Some of
them also renew the free Apple ID's 7-day signature by themselves. The IPA holds the game, built
from your dumps: keep it for your own devices, and do not share it.

There is no ready-made IPA, and there cannot be one. The game is recompiled into the app's code,
and iOS runs only code that was signed into the app before it was installed, so an app cannot
build the game from ROM files on the phone. Everyone builds the app from their own dumps.

## Playing

The app runs in landscape, in the enhanced mode, with 16:9 as the default aspect ratio.

| Screen | Touch controls |
| ------ | -------------- |
| Race | The **steering stick** on the left half: it appears where your thumb lands and follows it. **BRAKE** and **GAS** at the bottom right: a thumb can slide from one to the other. **▼ ▲** above them shift down and up. **VIEW** (the START button) changes the view. **II** pauses. |
| Menus | **Tap an item** to choose it. On an option, a tap changes its value. **BACK** is at the top left. |
| Rankings name | **◀ ▶** step through the letters and **OK** takes the one shown. **DEL** and **END** delete and finish. The keyboard icon opens the on-screen keyboard to type the name. |
| Course select | The steering stick to the left or right moves through the courses. |

- **Tilt steering:** with a motion sensor, OPTIONS > CONTROLS (or CONTROLS in the pause menu) has
  STEERING: TOUCH or TILT. With TILT, turn the device like a steering wheel. The stick still works
  when touched. STICK RESPONSE also sets how soft the stick and the tilt are.
- **Gamepads:** a controller connected over Bluetooth (Xbox, PlayStation, MFi) plays as on the
  desktop ([Controls](../README.md#controls)). The touch controls hide while it is used and come
  back at the next touch.
- **The background:** the game pauses when you leave the app, and its NVRAM (rankings, settings) is
  saved.
- **Settings that restart the game** (GAME and SOUND options, RENDERER): the desktop ports restart
  by themselves. An iOS app cannot, so the app saves them, says so and closes. Open it again to
  play with them.
- **Saves:** the NVRAM and the port's settings are in the app's Documents folder. You can see them
  in the Files app, under On My iPhone > Thrill Drive 2.
- **Renderer:** the GPU renderer runs on OpenGL ES 3.0, at AUTO resolution up to 2X on a phone
  (DISPLAY > RESOLUTION goes higher). If the app ever closes while the GPU renderer starts, the next
  launch uses the software renderer, and DISPLAY > RENDERER switches back.

## Multiplayer, face to face

Up to four iPhones and iPads race together when they are next to each other, with no router, access
point or SIM card. They link over Wi-Fi: over the network they share if they are on the same one
(a home network, or a Personal Hotspot), otherwise directly, phone to phone, as AirDrop does.

- **Host:** MULTIPLAYER > HOST A GAME. The screen shows the game's tag, for example GAME K7Q, and
  each player as they link. START GAME starts the race for everyone linked.
- **Join:** MULTIPLAYER > JOIN A GAME lists the games nearby. Tap the host's tag.
- **Wi-Fi switched on** on every phone (in Control Center or Settings), joined to a network or not.
  Bluetooth is not needed.
- **Local Network access:** the first time, iOS asks to let the app find devices on the local
  network. Allow it. If it was refused, MULTIPLAYER says so: turn it back on in Settings > Privacy &
  Security > Local Network.
- **The same app** on every phone: the same game version and the same release of the port.
- **Leaving a race:** a linked race never pauses, as on linked cabinets. The button at the top
  left shows ✕ instead of pause. It asks LEAVE THE SESSION (the host: CLOSE THE SESSION) in a
  small panel at the bottom while you keep driving. STAY is the default, and the panel goes away
  by itself after 5 seconds, so a tap by mistake costs nothing. Confirming takes you back to the
  main menu. When the host closes the session, everyone goes back there, with THE HOST CLOSED THE
  SESSION. With a gamepad or keyboard: Back or Esc asks, Y or Enter leaves, and Back or Esc again
  stays.
- **A player who drops out** (left the app, out of range): the others see PLAYER N DISCONNECTED.
  That car stands still for about 2 seconds and then leaves the race, which goes on. If the host
  drops out, the others go back to the main menu after about 7 seconds, with NO ANSWER FROM THE
  HOST. A player who leaves the app stops sending, and after 30 seconds is out of the session.
- **NETWORK ERROR:** if the game itself stops the link, the desktop ports restart it. The app
  closes instead: open it again.
- The desktop ports link over the Internet with session codes ([Multiplayer](../README.md#multiplayer)).
  The app links only with the phones nearby, and not with the desktop ports.

## Development notes

- **The desktop builds are unchanged:** what the app adds to the shared sources is behind
  `VIPER_IOS`, `VIPER_GLES` and `VIPER_P2P`, which only `ios/CMakeLists.txt` (and `make P2P=1`)
  define. `runtime/touch.c`, `runtime/platform_ios.m` and `runtime/net_p2p.m` are not in the
  desktop sources (`tools/buildspec.py`). The desktop executable only gains `--calibrate`, which
  `ios/prepare.py` uses.
- **Differences from the desktop build:** `VIPER_IOS` is on (the app's paths, lifecycle and
  menus), and so is `VIPER_GLES` (OpenGL ES 3.0 in `runtime/gpu_gl.cpp`). `runtime/touch.c` is
  the touch controls, and `runtime/platform_ios.m` the iOS services: the Documents folder, the safe
  area, a display link pacing the frame loop, tilt and haptics.
- **Link play (`VIPER_P2P`):** `runtime/net.c` sends its datagrams through
  `runtime/net_p2p.m` (Network.framework, peer to peer, Bonjour service `_viperlink._udp`) instead
  of a socket. Each connection stands for an address, so the rest of the netcode is the same as the
  desktop's. The app needs `NSLocalNetworkUsageDescription` and `NSBonjourServices`
  (`ios/Info.plist.in`), and no entitlement.
- **Trying the link on a Mac:** `make P2P=1 GAME=thrild2` builds `./td2_p2p` with the same
  transport. Two instances link on one Mac, for example headless, with the menu scripted:
  `RT_ENH_MENU="16:down,16.3:down,16.6:ok,17.2:ok" ./td2_p2p --headless --realtime` hosts, and
  `RT_ENH_MENU="16:down,16.3:down,16.6:ok,17.2:down,17.5:ok,22:ok" ./td2_p2p --headless --realtime`
  joins the first game it finds. The Simulator's app links with them too.
- **The iOS Simulator:** its OpenGL ES runs on the CPU, so the app uses the software renderer
  there. On a device it is the GPU.
- **Scripted touches** for tests: `RT_TOUCH_SCRIPT="seconds:d|m|u:finger:x:y,..."`, with x and y
  in 0..1 of the window. In the Simulator, prefix the environment variables with `SIMCTL_CHILD_`
  for `xcrun simctl launch`.
