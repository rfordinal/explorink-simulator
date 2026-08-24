# The simulator as an Android app

The firmware compiled for arm64 and hosted in an `SDLActivity`, so the UI runs
on a phone with no reader hardware in reach.

**Not the companion app.** `org.explorink.gpsbridge` (repo
`rfordinal/explorink-android`) talks to a reader over BLE. This is
`org.explorink.simulator`: it *is* the reader. Two package ids, both install
side by side, neither replaces the other.

Not an emulator either. No ESP32 `.bin` is loaded -- the APK carries the
firmware compiled as native arm64 code, so new firmware means a new APK.

## Layout

```
android/                     gradle project, AGP 8.13.2 / gradle 8.14.3 / JDK 17
  app/src/main/java/org/explorink/simulator/SimulatorActivity.java
  app/src/main/java-sdl/     SDL's own Java classes    (gitignored, copied in)
  app/src/main/jniLibs/      libSDL2.so + libmain.so   (gitignored, prebuilt)
tools/android/fetch_sdl2.sh  fetch SDL2, build libSDL2.so, install both above
tools/android/build_stub.sh  smoke-test libmain.so (tools/android/stub_main.c)
tools/android/provision_sd.sh  push map tiles and a position onto a phone
tools/android/set_mode.sh      set the panel size mode on a phone
```

**Gradle compiles no C++.** The firmware is cross-compiled by PlatformIO and
`libSDL2.so` by `ndk-build`; both are prebuilt `.so` files that gradle only
packages. That keeps one source of truth for the file list -- the firmware's
`build_src_filter` and `lib_deps` -- instead of a CMake copy of it.

SDL is not vendored. `fetch_sdl2.sh` clones a pinned tag into
`~/.cache/explorink/sdl2/`, outside every checkout, so worktrees stay small and
share one copy.

## Build and run

```bash
tools/android/fetch_sdl2.sh          # once: SDL2 source + libSDL2.so

# libmain.so is the firmware. Built from the firmware repo, not here:
#   pio run -e simulator -t compiledb
#   python3 scripts/android_build.py
# It writes straight into this repo's jniLibs. tools/android/build_stub.sh
# puts a 20-line SDL smoke test there instead, for when the shell itself is
# what is being debugged.

cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
cd .. && tools/android/provision_sd.sh <parent-repo>/mapbuilder/cdn   # tiles + a fix
adb shell am start -n org.explorink.simulator/.SimulatorActivity
```

`provision_sd.sh` needs the app installed first: it writes through `run-as`,
which only works on a debuggable build.

**What its check does not catch.** It compares the file count and the total byte
size, which catches a missing file and a truncated one -- and a truncated file is
what a half-finished transfer produces. It does not catch a corrupted one: same
length, wrong content passes. A checksum per file over `adb` for 1318 files was
judged too slow to be worth it.

Input works with no on-screen buttons yet -- `adb shell input keyevent`
reaches SDL through `SDLActivity`, so `66` (ENTER) is the confirm button.
Scancode map: ESCAPE back, RETURN confirm, arrows, P power, S sleep, H home
(`src/HalGPIO.cpp:35-50`).

`android/local.properties` needs `sdk.dir=<Android SDK>`; it is gitignored.

## Verified 2026-08-23

NDK r27.3.13750724, SDL2 2.32.10, one APK, both phones. The smoke test
installs, launches and draws: white screen, one dark stripe scrolling top to
bottom. Confirmed by eye and by `adb exec-out screencap`.

| phone | Android | API |
|---|---|---|
| Samsung SM-S928B (S24 Ultra) | 16 | 36 |
| Samsung SM-G973F (S10) | 12 | 31 |
| Samsung SM-G950F (S8) | 9 | 28 |

The BLE bridge is verified on the S10 and the S8 as peripherals, with the S24 as
the central.

All arm64-v8a. Three API levels from 28 to 36 off one APK. The older phones are
more than spares: they are the real test of `minSdk 24` and of SDL's GL path on
2017 hardware, and one can hold a running simulator without occupying the main
phone. With several devices attached, every `adb` call needs `-s <serial>` or
`ANDROID_SERIAL`.

**The firmware itself runs, not just the shell.** Same three phones, same day:

- 208 of 208 translation units compile for `aarch64-linux-android24` and link
  into an 18 MB `libmain.so`, with `-Wl,--no-undefined` on, so nothing is
  deferred to a `dlopen` failure. There were no missing symbols at all -- the
  expectation that a pile of host assumptions would surface was wrong.
- Boot, the Home screen (logo, menu, battery) and the map screen all draw, with
  the firmware's own `GfxRenderer` and fonts.
- The map draws real `.tib` tiles with place labels, the header's place name,
  compass, scale bar and the position marker, from the seeded fix.
- The firmware does its normal side work too: it creates `trailink/power.csv`
  from `PowerTelemetry` while running.

Screenshots are identical across all three phones.

### The first thing it found

**The map's right edge is clipped: the zoom `+`/`-` buttons are cut off**, and
the bottom soft-key row is cut as well. The same clipping was already open from
the Linux run (`README.md`, "Verified 2026-08-23" in the firmware repo's
`docs/simulator.md`), where it was unclear whether the simulator's orientation
transform or the style's anchoring was at fault.

This narrows it. The clipping is **pixel-identical on three phones with three
different window sizes** (720x1560, 1080x2280, 720x1480). So it is not the
window, the aspect ratio or the scaling -- it happens inside the panel's own
480x800 coordinate space, before anything host-specific touches it.

**And the rotation is exact, which closes the remaining doubt about the
simulator being at fault.** Portrait logical size is 480x800; the destination
rect is `{-160, 160, 800, 480}`, whose centre is (240, 400) -- exactly the
centre of the logical space -- so rotating 90 degrees maps 800x480 onto 480x800
with nothing left over (`src/HalDisplay.cpp`, `presentIfNeeded`). The
screenshots agree: content occupies 720x1200 inside a 720x1560 window, which is
480x800 scaled by exactly 1.5, with black bars top and bottom.

So SDL is letterboxing perfectly and the buttons are drawn past x=480 by the
firmware itself. The device renders into the same 480-wide framebuffer, so it
clips them the same way. A device screenshot would still be worth having, but
the simulator is no longer a plausible culprit.

What that pass establishes:

- gradle, `jniLibs`, the manifest, `SDLActivity`, `adb install` and launch all
  work as a chain.
- **The entry point needs no source change.** `src/simulator_main.cpp` includes
  `<SDL.h>` before defining `main`, and SDL's macro renames it to `SDL_main` on
  Android too. logcat: `Running main function SDL_main from library
  .../libmain.so`.
- Renderer size is the phone's full screen, not the panel's 480x800. Scaling the
  panel into it is still open.

## What the app looks like

Three parts stacked: a bar with the size menu, the panel, and the device's
buttons.

**The buttons are the device's, not Android's.** Back, Left, Right, Select on
the first row; Up, Down, Power, Sleep, Home on the second. Each one calls
`SDLActivity.onNativeKeyDown/Up` with the Android keycode SDL turns into the
scancode `HalGPIO` already maps (`src/HalGPIO.cpp:35-50`). So a tap is
indistinguishable from a keyboard press on the desktop simulator, and neither
`HalGPIO` nor anything else in the HAL changed. `adb shell input keyevent`
still works and goes through the same path.

The buttons follow the device rather than the phone: the front row underneath
the panel (Back, Select, Left, Right) and the page keys on the right edge, Up
above Down, their text turned to read along that edge with the baseline against
it. Power, sleep and the X4 Pro home key are in the top bar's menu -- real
buttons, rarely pressed, and not worth the screen.

**The panel is never covered and never runs off.** A fixed-size panel is shifted
left so the page keys sit in the strip beside it, not on top of it. If a size is
wider than the room available the keys overlap instead, because the alternative
is the panel disappearing off the left edge, and the bar says `keys overlap, no
room`. A panel the layout had to clip says `clipped to WxH` rather than claiming
a size it does not have.

**Three panel sizes**, from the bar's menu or `tools/android/set_mode.sh`:

| mode | what it is |
|---|---|
| `FIT` | the panel fills the space available. SDL letterboxes, so the aspect is right and the pixels are scaled. |
| `ONE_TO_ONE` | one panel pixel to one screen pixel, 480x800. On a dense phone this is physically **smaller** than the real device. |
| `REAL` | the panel's physical size: 480x800 at 220 PPI is 55x92 mm (parent repo `README.md:137` -- vendor nominal, unmeasured, see below). Scaled by the screen's own `xdpi`/`ydpi`. |

`ONE_TO_ONE` being smaller than `REAL` is the point of having both: a phone is
denser than the reader, so pixel-perfect and life-size are different pictures.

**`REAL` is self-consistent, which is less than honest.** All three phones in
`REAL` at once, held together, show the panel at the same physical size, and
that is worth having: three different screen densities agreeing means each
reports `xdpi`/`ydpi` truthfully and the arithmetic is right. Confirmed by the
maintainer by eye, 2026-08-23. A phone that rounded its density badly would show
up immediately as a panel that does not match the others.

**It cannot check the panel figure, and that figure is not measured.** 220 PPI is
the vendor's nominal number and nothing in this project has measured the X4's
active area (parent repo `docs/IDEAS.md:294`; `docs/wallet-plan.md:103` marks it
`OPEN`). If it is wrong, all three phones are wrong by the same factor and agree
perfectly -- so this check structurally cannot catch it. The repo's own numbers
already disagree by 1.4%: 480x800 at 220 PPI is a 4.24 inch diagonal, while
`README.md:137` says 4.3 inch, which would be 217 PPI and 56.2 x 93.6 mm instead
of 55.4 x 92.4.

**Open -- needs a caliper on an X4's active area.** No device exists to measure
(parent `docs/PROGRESS.md`, 2026-08-22).

All three are the size of the SDL surface in the Android layout, nothing more.
The renderer keeps its 480x800 logical size and SDL maps it, so no firmware or
HAL code is involved. The mode is remembered in `SharedPreferences`, read in
`onCreate`, which is why `set_mode.sh` restarts the activity.

Panel geometry and PPI are constants in `SimulatorActivity` and have to track
the simulator's compiled device profile. Only the X4 env is wired, so only X4's
numbers are there.

## The sleep timer killed the app, and that is fixed

The firmware sleeps on an idle timer, and waking from its sleep screen is a
**fresh process**: a native build has no ESP deep-sleep resume path, so the
desktop simulator relaunches itself with `execvp(gArgv[0], gArgv)`
(`src/SimulatorLifecycle.cpp`).

On Android that killed the app. SDL sets `argv[0]` to `"app_process"`, so
`execvp` replaced the app with a bare system binary that died immediately. It
looked exactly like a crash, and because the trigger is an idle timer it
happened unattended -- two phones left alone were simply gone.

`rebootAsPowerWake()` now asks Java for a fresh activity
(`SimulatorActivity.relaunchForWake()`) and exits, so Android starts a new
process. Two details:

- **The wake reason cannot travel in the environment**, because a relaunched
  process does not inherit it. It goes through a marker file in app-private
  storage, written before the exit and consumed on the next start.
- **The Java side blocks until the intent is submitted**, because the caller
  `_exit(0)`s the moment it returns.

Verified on a phone: pressing Sleep then Select changes the process id and comes
back to Home, instead of the app vanishing.

## Three Android facts that cost time

### `HWCDC`'s stderr never reaches `adb logcat`, and nothing says so

The desktop simulator's `Serial`/`logSerial` is `HWCDC`, backed by
`std::cerr` (`src/HardwareSerial.h`). That is a real terminal on a laptop.
On Android it is a Zygote-forked app process with no terminal at all, and
nothing redirects its fd 2 into the pipe `logcat` reads -- so every
`LOG_ERR`/`LOG_INF`/`LOG_DBG` line the firmware ever prints simply
disappears. No error, no truncation warning, nothing: the code path runs,
the write call returns, and the byte are gone.

Confirmed 2026-08-24 chasing a real firmware bug (pin save failing with
"Card refused the write" on an S8): the `PINS`/`PINLOG` `LOG_ERR` lines
that had to be firing were never once seen in `logcat`, across several
reproductions, while an unrelated Java-side `SDL_Log` call showed up fine
under the `SDL/APP` tag in the same window. That contrast is what gave it
away -- some things from this process do reach logcat, just not stderr.

Fixed by also routing `HWCDC::write()`/`printf()` through
`__android_log_print` (tag `explorink`) when `__ANDROID__` is defined,
buffered to whole lines since firmware writes are not always
newline-terminated per call. `liblog` is already linked (this file's
Milestone 1 `NEEDED` list). `adb logcat -s explorink:*` now shows every
firmware log line live. Desktop `std::cerr` output is unchanged.

### 16 kB page alignment is mandatory, and the warning lies about being current

Android 15 and newer refuse a `.so` whose ELF `LOAD` segments are aligned to
4 kB. The app still starts, but the system puts a dialog over it: *"not
compatible with 16 kB mode ... segment LOAD is not aligned"*.

**The phone doing the complaining need not have 16 kB pages itself.** The S24
here reports `getconf PAGE_SIZE` = 4096 and still warns: the check is about
whether the app *could* run on a 16 kB device, not about the current one. So
there is no "this hardware is fine" exemption to lean on.

Older Android does not check at all -- the Galaxy S8 on Android 9 runs a
4 kB-aligned build with no dialog.

Two different alignments have to be right, and they are fixed in two places:

- **ELF segments.** `APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true` in
  `Application.mk` for SDL (`tools/android/fetch_sdl2.sh`), and
  `-Wl,-z,max-page-size=16384` on our own link (`tools/android/build_stub.sh`).
- **Zip entries.** AGP 8.13.2 already page-aligns them; nothing to do, but
  check it rather than assume.

**The dialog is not a live indicator.** It persists on screen from the install
that caused it, so it is still there after a fixed build is installed and
launched. Do not judge the fix by whether it appears. Check the APK:

```bash
unzip -p app-debug.apk lib/arm64-v8a/libmain.so > /tmp/x.so
llvm-readelf -l /tmp/x.so | grep LOAD          # align must be 0x4000
zipalign -c -P 16 -v 4 app-debug.apk | tail -1 # "Verification successful"
```

### SDL decides fullscreen, not the manifest

`SDL_WINDOW_FULLSCREEN` in the `SDL_CreateWindow` call is what hides the status
bar on Android. Changing the activity's theme away from
`Theme.NoTitleBar.Fullscreen` does nothing while that flag is set. Verified
both ways on the phone.

It matters beyond looks: a white screen with a moving dark stripe and no system
bars reads as a dead phone, not as a running app. The stub now asks for a plain
window (`tools/android/stub_main.c`), and the panel will be letterboxed inside
it rather than stretched.

The requested window size is ignored on Android -- the window is whatever the
activity got, 720x1560 here -- so the 480x800 in that call is documentation, not
a request.

### Edge-to-edge is not optional

`targetSdk` 36 means Android 15 and up draw the app under the status and
navigation bars. The first layout looked right in a screenshot and was not: the
top bar sat behind the status bar and the second row of buttons under the
navigation bar, clipped. Fixed with an `OnApplyWindowInsetsListener` on the root
view that turns the system-bar insets into padding, with the pre-API-30 branch
for the older phones.

### `set -o pipefail` and a grep that matches nothing

`provision_sd.sh` filters one expected error out of tar's stderr. Written as a
pipeline, `... | grep -v expected`, it killed the script **exactly when there
was nothing to report**: pipefail reports a grep that matched nothing as a
failure. The filtering happens on a captured string instead.

Worth remembering for any script here that greps away an expected warning.

### Java cannot set an environment variable before the native code loads

The obvious way to point the firmware at its simulated SD card is
`setenv("CROSSPOINT_SIM_SD", ...)` from `onCreate`. It cannot work:
`SDLActivity.onCreate` is where the native libraries are loaded, so any native
call ahead of `super.onCreate()` throws `UnsatisfiedLinkError`, and after it the
SDL thread may already be running.

So the storage root has to be resolved natively, from
`SDL_AndroidGetInternalStoragePath()` inside `HalStorage`
(`src/HalStorage.cpp:20-25` picks up `CROSSPOINT_SIM_SD` today). Not written
yet.

`SimulatorActivity` is deliberately almost empty for the same reason: anything
it could do, the native side can do without an ordering problem.

## Real Bluetooth, phone to phone

**Verified 2026-08-23** on two peripherals, an S10 (Android 12) and an S8
(Android 9). The companion app on a Galaxy S24 Ultra connected to each over
actual Bluetooth, and the firmware took the phone's real GPS fix and redrew the
map. No laptop in the path.

Android 9 needed no code change and no runtime prompt: `BLUETOOTH` and
`BLUETOOTH_ADMIN` are install-time permissions there, and the runtime request is
guarded to API 31 and up. Full negotiation either way -- CCCD writes on both
indicate characteristics and MTU 517 on the 2017 phone as much as on the 2024
one.

Worth seeing side by side: the S8 reports 281 dpi and the S10 416, because the
S8 is running a downscaled display mode. Real size draws the panel at 55x92 mm
on both, which is the whole point of the mode.

`BleBridge.java` is the bridge: a socket client on the shim's loopback port on
one side, an Android `BluetoothGattServer` and `BluetoothLeAdvertiser` on the
other. It is a translation and nothing more -- every decision it makes for
itself is a decision the test stops testing. `tools/blebridge.py` in the parent
repo is the same translation against BlueZ and was the reference.

It starts by itself when `CROSSPOINT_SIM_BLE_PORT` is set, and retries until the
shim is listening -- which is when a screen that needs BLE opens, the map or
Sync, not at boot. A status
token in the top bar says what it is: a filled dot for a central on the link, a
hollow one for advertising and waiting, a dash for attached to the simulator with
no radio up yet.

**That token is driven by state, not by the last log message**, and the
distinction was a bug first. Matching on messages meant the firmware's "asked
for new conn params" a few seconds after every connect -- which is not a state
at all -- knocked the indicator back to unknown while a central was plainly
connected. The bridge now reports transitions and logs separately.

From the first run's log: four characteristics built from the shim's own `gatt`
event (props 8, 56, 8, 32), the service UUID advertised, a central connected,
both indicate-capable characteristics subscribed, MTU 517.

**Three things Android does better than BlueZ did**, and the bridge uses all
three:

- **The CCCD write arrives with its value**, so `subscribe` carries the bit the
  central actually wrote (`value 2`, indicate). The BlueZ bridge could only
  report what the characteristic was capable of, so the firmware never learned
  which the peer chose (parent `docs/ble-bridge.md`).
- **The negotiated MTU has its own callback**, instead of arriving only as a
  side effect of a write.
- **Notify versus indicate is the peripheral's choice here**, made from that
  CCCD value, which is what a real device does.

Two things it cannot forward, both because the real stack owns the decision, and
both the same on BlueZ: `connparams_request` (Android gives a peripheral no way
to answer) and a read, which is answered locally from the last pushed value
because the wire protocol has no read op and the firmware exposes nothing
readable.

Android also does not tell `onNotificationSent` which characteristic it belongs
to. The bridge records the one it indicated rather than guessing from the
subscription set, which would confirm a characteristic that was never sent.

### Killing the app does not drop the link, and the peer does not notice

Tested 2026-08-23, deliberately: the simulator was `force-stop`ped mid-link, so
no teardown of ours ran.

**Both Bluetooth stacks kept the ACL connection.** The next GATT server this side
opened was handed the existing central immediately -- one millisecond *before*
`onStartSuccess`, same peer address, and with no CCCD write and no MTU exchange.
That left the firmware believing a central was connected while that central
believed it was subscribed to a GATT table that no longer existed. Indications
would have gone nowhere.

Fixed here: a central that arrives before this server has started advertising is
not answering our advertisement, so `dropStaleLink()` drops it. Verified --
`dropping a link left over from a previous run` in the log, then a clean
advertisement.

**The peer is still wedged, and that is not fixable from this side.** The
companion app's own on-screen log, four minutes after the kill and after our
drop: still `connected to Galaxy S10`, no disconnect line, `last sent 20:46:22`.
`BluetoothGattServer.cancelConnection` releases this server's reference and does
not tear the ACL down while the other side holds it.

The parent repo's `docs/ble-bridge.md` left exactly this open -- "whether a clean
GATT disconnect from the BlueZ side is enough, or the app also needs a fix for a
peer that vanishes". It is not enough. The app needs to notice a peer that stops
answering, which is app-side work.

Incidentally, this is also what finally proved which device was connecting. The
peer's address is a rotating random one and says nothing; the app's own log says
`found: Galaxy S10` and `connected: Galaxy S10`, matching the scan response's
device name.

### Two rules for running it

- **Different phones.** One runs the simulator as the peripheral, another the
  companion app as the central. Not one phone doing both.
- **A run carries the rider's real position.** The companion app sends its
  actual GPS, so the firmware's screen, any screenshot of it and any log
  identify where the maintainer is. Those stay on the local machine -- same rule
  as device screenshots in the parent `CLAUDE.md`.

`auto_confirm` is sent `false` the moment the bridge attaches. With the shim
confirming its own indications the phone's real confirm timing -- the entire
reason for a real radio -- is never measured.

Permissions: `BLUETOOTH_ADVERTISE` and `BLUETOOTH_CONNECT`, requested at runtime
on Android 12 and up, with the pre-31 pair declared for the older phone.
`neverForLocation` is on `BLUETOOTH_CONNECT` because nothing here scans.

## BLE works on a phone, driven from a laptop

The simulator's NimBLE shim (`docs/ble-shim.md`) compiles and links for Android
and runs there. Verified 2026-08-23 on a Galaxy S10 (Android 12):

- 213 of 213 translation units compile and link, including the shim's four files
  and the firmware's own `BlePositionServer.cpp`. `libmain.so` grows from 18.2 to
  18.5 MB.
- The shim's listener comes up on `127.0.0.1:8765` **when the map or the Sync
  screen opens**, not at boot: those are the two places the firmware starts BLE
  (`MapActivity.cpp:1829`, `TileSyncActivity.cpp:106`). Checked in
  `/proc/net/tcp` (`0100007F:223D`, state 0A). Only the map was found at first,
  by a grep whose pattern could not match the actual call
  (`BlePositionServer::getInstance().begin()`); an empty grep is not a list of
  one.
- `adb forward tcp:8765 tcp:8765` makes that socket reachable from the laptop, so
  **the existing laptop tools work against the phone unchanged**:
  `python3 tools/blepos.py 48.3810 17.5930 --heading 4 --speed 42 --sim
  127.0.0.1:8765` moved the map, turned the compass and put the packet's clock in
  the header.

The header's Bluetooth indicator tracks it live: the `X` over the bars means
`connIntervalMs() == 0`, no central connected
(`firmware/explorink/docs/map-header-status.md`). A one-shot tool disconnects
when it exits, so a screenshot taken afterwards correctly shows the `X` again --
worth knowing before reading it as a failure. With a client holding the link
open, the bars replace it.

**One client at a time, and a killed client costs the next one.** The shim
refuses a second client (`docs/ble-shim.md`, "The second client is refused").
A tool killed mid-run left its socket behind for long enough that the next
attempt died on `ConnectionResetError: Connection lost` at its first write --
which reads like a firmware or transport fault and is neither. Check
`/proc/net/tcp` for a live connection to the port before blaming anything else.
That matters for the Android bridge, which will be that single client.

So the firmware's real BLE code runs on the phone. What is missing for
[plan A](../../docs/ble-bridge.md) -- a phone advertising for real, with the
companion app on a second phone as the central -- is only the bridge between the
shim's socket and Android's own `BluetoothGattServer`. `tools/blebridge.py` in
the parent repo is the same translation against BlueZ and is the reference.

Two things carry over from that bridge and are not optional. `auto_confirm` must
be set false immediately, or the shim confirms its own indications and the real
peer's timing is never measured. And the test devices have to be **different
phones**: one runs the simulator as the peripheral, another runs the companion
app as the central.

### Every `CROSSPOINT_SIM_*` knob now works on Android

The shim is off unless `CROSSPOINT_SIM_BLE_PORT` is set, and on Android nothing
could set it -- the same reason the SD card path had to be resolved natively.
That blocked every simulator knob, not just this one.

`src/SimulatorAndroidEnv.cpp` reads `KEY=VALUE` lines from `<app files>/sim-env`
at the top of `main()` and applies each with `setenv(..., 0)`, so a real
environment variable still wins where one can be set. Off Android it compiles to
nothing. `tools/android/set_env.sh` writes the file and restarts the activity:

```bash
tools/android/set_env.sh CROSSPOINT_SIM_BLE_PORT=8765 --serial <phone>
tools/android/set_env.sh --clear --serial <phone>
```

`CROSSPOINT_SIM_INPUT_SCRIPT`, `CROSSPOINT_SIM_SCREENSHOTS` and
`CROSSPOINT_SIM_HTTP_PORT` go in the same way, so the scripted-run harness is
reachable on a phone too. Untested for those three.

## A coloured fringe around the panel, and the wrong theory about it

In a fixed-size mode (1:1 or real size) the panel has a one-to-two pixel
olive-green edge. Measured, not eyeballed: at the left border the pixels run
`(14,14,14)` surround, then `(136,169,52)`, `(185,215,107)`, `(204,225,148)`,
`(232,242,207)`, then white. A five-pixel gradient, so it is a blend, not a line
anything drew.

**It was the renderer's clear colour, and it is fixed.** `SDL_RenderClear` was
called in `HalDisplay.cpp` without `SDL_SetRenderDrawColor` appearing anywhere in
that file, so the surround was cleared with whatever draw colour happened to be
current. Setting it to black changed the border pixels from `(136,169,52)` to
`(0,0,0)`, measured, in five consecutive captures.

Two honest caveats, because this was got wrong twice on the way:

- One capture showed the fringe from an APK that already carried the fixed
  library. Whether that process had actually loaded it was not established, so
  the possibility that the fringe is also intermittent is not ruled out.
- It is not a developer option; `show_surface_updates`, `debug.layout` and
  `debug.hwui.*` are all unset on the phone. That was checked before guessing.

The five-pixel gradient is what a scaled edge looks like with
`SDL_HINT_RENDER_SCALE_QUALITY=1` blending the texture edge against the
surround, which is why the surround's colour is what showed. That hint is
deliberate: nearest filtering turns Bayer dithering into hard stripes (the
fork's `CLAUDE.md`).

## Open

- **Linking the firmware** into `libmain.so`. Compiling is proven (208 of 208
  translation units for `aarch64-linux-android24`, see the firmware repo's
  `docs/simulator-android.md`); linking is not. Expect undefined symbols for
  whatever the HAL assumes a host provides. One is known: OpenSSL, needed only
  by `src/MD5Builder_linux.h:12`, which a bundled MD5 would remove.
- **The recorded main thread is the wrong one.**
  `src/HalDisplay.cpp:66` captures it in a global initialiser, which on Android
  runs when `System.loadLibrary` maps `libmain.so` -- the activity's UI thread.
  `SDL_main` runs on another. Confirmed in this run: `onCreate` logged from tid
  23021, `SDL_main` from tid 23127. So the `displayBuffer` check at
  `src/HalDisplay.cpp:389` never matches and it never presents. Frames still
  appear, one loop iteration late, via `simulator_main.cpp`'s own
  `presentIfNeeded()`. Fix: capture the id at the top of `main()`. That is a
  latent bug anywhere the two threads differ, so it belongs upstream.
- **Panel geometry.** 480x800 panel into a 720x1560 window.
- **Buttons.** On-screen, in Java, injecting key events -- so `HalGPIO` stays
  untouched and the fork's diff does not grow.
- **The simulated SD card**: tiles and settings pushed into app-private storage.
