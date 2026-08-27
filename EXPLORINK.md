# The ExplorInk fork of the CrossPoint simulator

This repo is a fork of
[crosspoint-reader/crosspoint-simulator](https://github.com/crosspoint-reader/crosspoint-simulator),
kept for [ExplorInk](https://github.com/rfordinal/explorink) — a fork of
CrossPoint that turns an e-ink reader into a motorcycle/trail map device.

The fork exists for the reason `FORKING.md` gives: the simulator **replaces**
the firmware's `lib/hal/`, so it is pinned to one firmware's HAL. ExplorInk's
HAL has diverged, so its simulator has to diverge with it.

## Layout

- `main` tracks upstream. Merge from `upstream/main`, never commit here.
- `explorink` is the development branch. All fork work lands here.

```bash
git remote -v
# origin    git@github.com:rfordinal/explorink-simulator.git
# upstream  https://github.com/crosspoint-reader/crosspoint-simulator.git

git fetch upstream && git merge upstream/main    # on explorink
```

## How ExplorInk builds it

ExplorInk's `platformio.ini` carries an `[env:simulator]` and a `[simdep]`
section that names this repo:

```ini
[simdep]
source = https://github.com/rfordinal/explorink-simulator#explorink
```

`platformio.local.ini` (gitignored) points `[simdep]` at a local checkout while
working on the simulator itself:

```ini
[simdep]
source = symlink://../../firmware/explorink-simulator
```

Then, from the firmware repo:

```bash
pio run -e simulator                    # build
pio run -e simulator -t run_simulator   # build and launch
```

Verified 2026-08-23 on Ubuntu (SDL2 2.30, OpenSSL 3.0): boots, draws Home, and
draws the ExplorInk map screen from real `.tib` tiles on the simulated SD card.

## The fork's diff against upstream

Two kinds, per `FORKING.md`. Keep them separated — the first kind is a PR
upstream will take, the second one never leaves this fork.

### Platform emulation gaps — belongs upstream

| File | Change |
| --- | --- |
| `src/Arduino.h` | `pinMode` / `digitalWrite` / `analogRead` plus `LOW`, `HIGH`, `INPUT`, `OUTPUT`, `INPUT_PULLUP`, `INPUT_PULLDOWN`. FreeInk's `BoardConfig.h` powers the SD rail up and down with these, so any firmware that includes the SDK's own `BoardConfig` instead of the simulator's reduced one fails to compile without them. |
| `src/Print.h` | The rest of Arduino's `Print` surface: `print`/`println` for `char` and every integer and float width, and a real `printf`. Upstream had `print(const char*)`, `println(const char*)`, and a `println(int)` that printed the literal `"1"`. |
| `src/HardwareSerial.h` | `HWCDC::availableForWrite()`, returning 1024. Firmware that chunks a long reply against it (ExplorInk's `writeAllChunked`, for the 48,000-byte `CMD:SCREENSHOT` dump) otherwise stalls until its own timeout. |
| `src/freertos/{FreeRTOS,semphr,task}.h` | A real binary semaphore (`xSemaphoreCreateBinary`), an `xSemaphoreTake` that honours `ticksToWait` and returns `pdFALSE` on timeout, a `vTaskDelay` that sleeps, and `pdMS_TO_TICKS`. The mutex path is untouched: `SemaphoreHandle_t` is now a tagged handle with two arms, because the renderer depends on the recursive mutex and on `xSemaphoreGetMutexHolder` reading its internals. Any firmware waiting on a semaphore with a timeout otherwise never times out, and a retry loop of 40 x 25 ms runs in zero time. `docs/freertos-shim.md`. |
| `src/NimBLE*.h/.cpp`, `src/SimBle*.{h,cpp}`, `src/host/ble_gap.h`, `docs/ble-shim.md`, `tests/` | A NimBLE peripheral shim: the API surface a firmware BLE server uses, a GATT model dispatching on its own host thread, and a socket transport speaking newline-delimited JSON, so a client on a socket plays the central. No NimBLE source is compiled. Callbacks never run inline, an indication confirm is out of band and withholdable, a second `indicate()` clobbers the first, and the client sets the MTU -- the four things a polite fake gets wrong. Off unless `CROSSPOINT_SIM_BLE_PORT` is set. |

| `src/HalDisplay.cpp` | `CROSSPOINT_SIM_SCALE`: window scale as three modes -- `1:1`, `zoom:N`, `real[:dpi]` -- with the logical size pinned to the panel so nothing in the draw path changes, plus a screenshot capture that composes into its own panel-sized target so a BMP is device pixels whatever the window is. Also fixes a real bug on the way: `extern GfxRenderer renderer` inside the anonymous namespace named an anonymous-namespace symbol, so the new code linking against it failed until the declaration moved to file scope. The mode plumbing is upstream material. |

### HAL surface — stays in the fork

| File | Change |
| --- | --- |
| `src/HalDisplay.h/.cpp` | `displayWindow(uint16_t, uint16_t, uint16_t, uint16_t, bool turnOffScreen)`. ExplorInk's partial-window refresh takes `uint16_t` geometry and the same `turnOffScreen` flag as the full-panel calls. |
| `src/HalPowerManager.h` | `getBatteryMillivolts(uint8_t samples = 8)`, returning a fixed 3900 mV. ExplorInk's power log and map header read millivolts, not just percent. |
| `src/HalDisplay.cpp` | Nearest sampling for `1:1` and `zoom`. Upstream sets `SDL_HINT_RENDER_SCALE_QUALITY` to linear unconditionally and says why: dithered pixels average to the right grey when scaled. For an e-reader that is a reasonable call. For ExplorInk it is the opposite of what is wanted -- the map is 1-bit and a dither is judged as dots, so a filtered hairline is exactly the "resampled hairline reads as a smudge" failure the parent repo's 1:1 rule exists to prevent. Linear survives in `real` only. This one does not go upstream. |

## What ExplorInk needs from the firmware side

`[env:simulator]` in ExplorInk's `platformio.ini` deliberately omits
`BoardConfig` and `XteinkDetect` from `lib_deps`. The simulator ships its own
reduced `src/BoardConfig.h` and `src/XteinkDetect.h`; letting the FreeInk SDK
versions win instead pulls in `driver/gpio.h` and `Wire.h`, which do not exist
on a native build.

`lib_ignore` also carries `SecureNet` (wolfSSL) and `BLE`. `BlePositionServer`
is **not** ignored: its header is NimBLE-free by design and every method links a
stub when `FREEINK_CAP_BLE_PERIPHERAL` is unset, so the map screen builds and
simply reports "Bluetooth failed to start".

## Known gaps

- **No serial input.** `HWCDC::available()` returns 0 and `read()` returns -1,
  so ExplorInk's `CMD:` grammar and its map console are unreachable in the
  simulator. That is where a synthetic GPS fix would come from. Until it is
  wired to stdin, seed `fs_/.crosspoint/settings.json` with `mapHasLastFix`,
  `mapLastLatE7` and `mapLastLonE7` instead.

