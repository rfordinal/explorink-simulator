# Running the simulator in a browser (WebAssembly / Emscripten)

Evaluated 2026-08-29. Nothing built, nothing compiled. This is a read of the
existing code plus one checked platform fact, not a build log -- see
[`ANDROID.md`](ANDROID.md) for what an actual verified port looks like.

The question: could this repo compile to WebAssembly and run in a page, as a
zero-install alternative to the Android build?

## The one blocker that is not an engineering problem

**Real BLE -- advertising as a peripheral, a phone connecting to the browser
tab -- cannot be built at all, on any browser.** The Web Bluetooth
specification defines only the central/client role:

> "The first version of this specification allows web pages, running on a UA
> in the Central role, to connect to GATT Servers over either a BR/EDR or LE
> connection."

(<https://webbluetoothcg.github.io/web-bluetooth/>, introduction, fetched
2026-08-29). There is no peripheral/GATT-server API anywhere in the spec, so
this is not a missing feature to wait for -- it is out of scope by design.
`ANDROID.md`, "Real Bluetooth, phone to phone" has no browser equivalent, ever,
regardless of engineering effort.

Even the central role that does exist ships in Chromium only (desktop and
Android); Firefox and every Safari (macOS, iOS, iPadOS) carry none of it. So a
browser demo would reach *fewer* BLE-capable visitors than the Android APK
already does. Its only advantage over the APK is "no install", never "wider
BLE reach".

## What would need to change to compile at all

- **Threading.** `src/freertos/FreeRTOS.h:48` and `src/freertos/task.h:31` --
  every FreeRTOS task is a `std::thread`. Emscripten threads need `pthreads` +
  `SharedArrayBuffer`, which needs `Cross-Origin-Opener-Policy` /
  `Cross-Origin-Embedder-Policy` response headers on whatever serves the page.
  `explorink.com` is a plain `git pull` static deploy today (parent
  `CLAUDE.md`, "The public site"), no such headers.
- **Main loop.** `src/simulator_main.cpp:16-38` blocks in a `while` loop with
  `SDL_Delay(1)`. Emscripten wants `emscripten_set_main_loop` (a callback the
  browser drives), not a blocking loop -- a rewrite, not a recompile.
- **Sockets.** `src/WebServer.cpp:424-445`, `src/SimBleLink.cpp:345-359`,
  `src/NetworkClient.cpp:42` open real BSD sockets, including on `127.0.0.1`.
  None of this runs in a browser sandbox without a websocket proxy Emscripten
  does not provide by default. Compiling these out for a wasm target also
  removes the local web server and the loopback BLE shim -- another way the
  BLE story does not survive the port.
- **MD5.** `src/MD5Builder.h` dispatches on `__ANDROID__` / `__APPLE__` /
  `__linux__`. A wasm build needs its own branch, or -- better -- the bundled
  MD5 implementation already flagged as an open cleanup for the OpenSSL
  dependency on Linux would remove this branch too.

## What would probably just work

- **Storage.** `src/HalStorage.cpp` uses plain POSIX `open`/`lseek`/`read`/
  `write`. Emscripten ships a POSIX-compatible virtual filesystem (MEMFS) by
  default, so this code likely needs no change -- only a way to get tiles into
  that virtual filesystem before `setup()` runs (`--preload-file` at build
  time, or `fetch()` plus a write at page load). Not verified: no build was
  attempted.

## Read, not verified

Everything above is read off this repo's code plus general Emscripten
platform knowledge, except the Web Bluetooth central-only claim, which is
checked against the spec directly, not memory. Nothing here was compiled or
run. A real answer needs an actual `emcc` build attempt against a stripped
target (map screen only, no web server, no BLE, single-threaded) -- open.

## What this settles

A browser build is possible only as a **reduced** demo: map and UI, no BLE, no
web server, one thread. Anything claiming BLE, a running web server, or full
feature parity with the Android build, in a browser, is wrong by construction.
The distribution-decision framing (worth building at all, against what
audience) is parent repo `docs/IDEAS.md`, "Product, site and distribution",
and Outline, "Simulátor ako demo" -- this file is the technical half only.
