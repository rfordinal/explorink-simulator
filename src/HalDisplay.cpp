#include "HalDisplay.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <SDL.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static SDL_Window *window = nullptr;
static SDL_Renderer *sdl_renderer = nullptr;
static SDL_Texture *texture = nullptr;

// How big the window is against the panel. Three modes, because they answer
// different questions and ExplorInk's rule is that a frame always says which one
// it is (parent repo CLAUDE.md, "Every render is pixel perfect, 1:1, always", and
// docs/device-preview.md, "1:1 and real size"):
//
//   PixelPerfect  one device pixel, one screen pixel. The default, and the only
//                 mode a hairline decision may be taken in.
//   Zoom          an integer factor with NEAREST sampling, so one device pixel is
//                 an NxN block of identical pixels. Nothing is invented: a 1 px
//                 hairline stays a hard-edged line N pixels wide. For reading a
//                 12 px height number or counting dither dots.
//   Real          one device pixel at the size it has in the hand, so a 2 px road
//                 looks like what a rider sees rather than a comfortable 2 px line
//                 on a ~100 ppi monitor. This one IS a resample and is allowed
//                 only because it is deliberate and labelled.
//
// `CROSSPOINT_SIM_SCALE` picks it: unset or `1:1`, an integer (`3` / `zoom:3`),
// or `real` / `real:<monitor-dpi>`.
enum class WindowScaleMode { PixelPerfect, Zoom, Real };

// The panel's own density, for Real. 218 ppi on the X4 and X4 Pro (same
// GDEQ0426T82), 257 on the X3 -- parent repo docs/device-preview.md, "1:1 and
// real size", which sources both. Compile-time per device profile rather than a
// runtime guess, because the binary already knows which panel it is emulating.
#if defined(SIMULATOR_DEVICE_X3)
static constexpr double DEVICE_PPI = 257.0;
#else
static constexpr double DEVICE_PPI = 218.0;
#endif

// What a monitor pixel measures, when nothing better is known. SDL can be asked
// (SDL_GetDisplayDPI) and on X11/Wayland it frequently answers with the panel's
// physical size divided by its resolution, which is right, or with 96 flat, which
// is a placeholder. So: ask, sanity-check, and say in the title which it was.
static constexpr double NOMINAL_MONITOR_DPI = 96.0;

static WindowScaleMode scaleMode = WindowScaleMode::PixelPerfect;
static int zoomFactor = 1;
static double monitorDpi = 0.0;      // 0 = not stated, ask SDL
static bool monitorDpiFromSdl = false;
static double realFactor = 1.0;

// Parsed once, before the window exists, because the filtering hint has to be set
// before the texture is created.
static void resolveWindowScale() {
  const char *spec = std::getenv("CROSSPOINT_SIM_SCALE");
  if (!spec || !*spec)
    return;
  std::string value(spec);
  if (value == "1:1" || value == "1")
    return;

  const std::string realPrefix = "real";
  if (value.compare(0, realPrefix.size(), realPrefix) == 0) {
    scaleMode = WindowScaleMode::Real;
    const size_t colon = value.find(':');
    if (colon != std::string::npos) {
      const double stated = std::atof(value.c_str() + colon + 1);
      if (stated > 20.0 && stated < 1000.0)
        monitorDpi = stated;
      else
        std::cerr << "[SIM] scale: ignoring monitor dpi '"
                  << value.substr(colon + 1) << "' -- expected 20..1000"
                  << std::endl;
    }
    return;
  }

  const std::string zoomPrefix = "zoom:";
  const char *number = value.c_str();
  if (value.compare(0, zoomPrefix.size(), zoomPrefix) == 0)
    number = value.c_str() + zoomPrefix.size();
  const int factor = std::atoi(number);
  if (factor >= 2 && factor <= 8) {
    scaleMode = WindowScaleMode::Zoom;
    zoomFactor = factor;
    return;
  }
  // Refuse rather than guess: a typo that silently gave 1:1 would be read as
  // "the flag does nothing".
  std::cerr << "[SIM] scale: cannot read '" << value
            << "' -- use 1:1, an integer 2..8 (or zoom:N), real, or real:<dpi>. "
               "Staying at 1:1."
            << std::endl;
}

// Real's factor: how many screen pixels one device pixel should occupy.
// monitor_dpi / device_ppi, the same arithmetic the style watcher uses.
static void resolveRealFactor() {
  if (scaleMode != WindowScaleMode::Real) {
    realFactor = scaleMode == WindowScaleMode::Zoom ? zoomFactor : 1.0;
    return;
  }
  if (monitorDpi <= 0.0) {
    float ddpi = 0.0f, hdpi = 0.0f, vdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && hdpi > 20.0f &&
        hdpi < 1000.0f) {
      monitorDpi = hdpi;
      monitorDpiFromSdl = true;
    } else {
      monitorDpi = NOMINAL_MONITOR_DPI;
    }
  }
  realFactor = monitorDpi / DEVICE_PPI;
  // A window under about a third of the panel is unusable and almost certainly a
  // bad dpi rather than a real request. Clamp and say so.
  if (realFactor < 0.30) {
    std::cerr << "[SIM] scale: real factor " << realFactor
              << " is implausibly small, clamping to 0.30" << std::endl;
    realFactor = 0.30;
  }
}

static const char *scaleModeName() {
  switch (scaleMode) {
  case WindowScaleMode::Zoom:
    return "zoom";
  case WindowScaleMode::Real:
    return "real";
  default:
    return "1:1";
  }
}

// Pixel buffer written by the render task, read by the main thread for
// SDL_RenderPresent. On macOS, SDL calls must happen on the main thread.
static uint32_t
    pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::mutex pixelBufMutex;
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by
// shouldQuit().
std::atomic<bool> quitRequested{false};

static int currentWindowWidth = 0;
static int currentWindowHeight = 0;

// The one renderer instance, declared here rather than with a local `extern`
// inside the anonymous namespace below -- an `extern` in there names an
// anonymous-namespace symbol, which links to nothing at all.
extern GfxRenderer renderer;

// Forward declarations at file scope, deliberately outside the anonymous namespace
// below: declared inside it they are different functions, and the calls further
// down resolve to two candidates rather than one.
static void composePanel(GfxRenderer::Orientation orientation);
static void getLogicalWindowSize(GfxRenderer::Orientation orientation, int *width,
                                 int *height);

namespace {

struct GrayscalePreviewState {
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> bwBase{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> lsbPlane{};
  std::array<uint8_t, HalDisplay::BUFFER_SIZE> msbPlane{};
  bool bwBaseValid = false;
  bool lsbValid = false;
  bool msbValid = false;
};

constexpr uint8_t kGrayWhite = 255;
constexpr uint8_t kGrayLight = 200;
constexpr uint8_t kGrayDark = 96;
constexpr uint8_t kGrayBlack = 0;

GrayscalePreviewState grayscalePreviewState;
std::array<uint8_t, HalDisplay::BUFFER_SIZE> frameBufferStorage{};
bool frameBufferLent = false;

struct ScreenshotEvent {
  unsigned long atMs;
  std::string path;
  bool handled = false;
};

std::vector<ScreenshotEvent> screenshotEvents;
bool screenshotEventsInitialized = false;
const std::thread::id simulatorMainThread = std::this_thread::get_id();

void initializeScreenshotEvents() {
  if (screenshotEventsInitialized)
    return;
  screenshotEventsInitialized = true;

  const char *schedule = std::getenv("CROSSPOINT_SIM_SCREENSHOTS");
  if (!schedule || schedule[0] == '\0')
    return;

  const std::string spec(schedule);
  size_t start = 0;
  while (start < spec.size()) {
    const size_t end = spec.find(';', start);
    const std::string item = spec.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos && colon + 1 < item.size()) {
      screenshotEvents.push_back(
          {std::strtoul(item.substr(0, colon).c_str(), nullptr, 10),
           item.substr(colon + 1)});
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
}

bool hasDueScreenshot() {
  initializeScreenshotEvents();
  const unsigned long now = millis();
  for (const auto &event : screenshotEvents) {
    if (!event.handled && event.atMs <= now)
      return true;
  }
  return false;
}


// **A screenshot is always device pixels, whatever the window is doing.** It used
// to read the renderer's output size, which is the window's drawable -- so a zoom
// or real-size window produced a scaled BMP, and every artifact built from one
// would have been a resampled frame presented as device output. That is precisely
// what the parent repo's 1:1 rule forbids (CLAUDE.md, "Every render is pixel
// perfect"), and it would have been invisible: the file still looks like a
// screenshot.
//
// So the capture composes into its own panel-sized target instead of reading the
// window. One texture, kept for the life of the process, because a scripted run
// takes several shots.
static SDL_Texture *screenshotTarget = nullptr;
static int screenshotTargetW = 0;
static int screenshotTargetH = 0;

bool saveRendererBmp(const std::string &path) {
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  int width = 0;
  int height = 0;
  getLogicalWindowSize(orientation, &width, &height);
  if (width <= 0 || height <= 0) {
    std::cerr << "[SIM] Cannot determine screenshot size" << std::endl;
    return false;
  }

  if (screenshotTarget &&
      (screenshotTargetW != width || screenshotTargetH != height)) {
    SDL_DestroyTexture(screenshotTarget);
    screenshotTarget = nullptr;
  }
  if (!screenshotTarget) {
    screenshotTarget =
        SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_TARGET, width, height);
    if (!screenshotTarget) {
      std::cerr << "[SIM] Cannot create screenshot target: " << SDL_GetError()
                << std::endl;
      return false;
    }
    screenshotTargetW = width;
    screenshotTargetH = height;
  }

  // Logical size is already the panel (getLogicalWindowSize), so composing into a
  // panel-sized target is 1:1 by construction and no filter runs at all.
  if (SDL_SetRenderTarget(sdl_renderer, screenshotTarget) != 0) {
    std::cerr << "[SIM] Cannot target the screenshot texture: " << SDL_GetError()
              << std::endl;
    return false;
  }
  composePanel(orientation);

  std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
  const int readResult =
      SDL_RenderReadPixels(sdl_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                           pixels.data(), width * sizeof(uint32_t));
  SDL_SetRenderTarget(sdl_renderer, nullptr);
  if (readResult != 0) {
    std::cerr << "[SIM] Cannot read screenshot pixels: " << SDL_GetError()
              << std::endl;
    return false;
  }

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      pixels.data(), width, height, 32, width * sizeof(uint32_t),
      SDL_PIXELFORMAT_ARGB8888);
  if (!surface) {
    std::cerr << "[SIM] Cannot create screenshot surface: " << SDL_GetError()
              << std::endl;
    return false;
  }

  const bool saved = SDL_SaveBMP(surface, path.c_str()) == 0;
  if (!saved) {
    std::cerr << "[SIM] Cannot save screenshot " << path << ": "
              << SDL_GetError() << std::endl;
  } else {
    std::cerr << "[SIM] Saved screenshot: " << path << std::endl;
  }
  SDL_FreeSurface(surface);
  return saved;
}

void captureDueScreenshots() {
  const unsigned long now = millis();
  for (auto &event : screenshotEvents) {
    if (event.handled || event.atMs > now)
      continue;
    event.handled = true;
    saveRendererBmp(event.path);
  }
}

uint32_t argbGray(uint8_t level) {
  return 0xFF000000u | (static_cast<uint32_t>(level) << 16) |
         (static_cast<uint32_t>(level) << 8) | level;
}

bool getBit(const uint8_t *buffer, int x, int y) {
  const int byteIdx = (y * HalDisplay::DISPLAY_WIDTH + x) / 8;
  const int bitIdx = 7 - (x % 8);
  return (buffer[byteIdx] & (1 << bitIdx)) != 0;
}

void renderBwPixels(const uint8_t *fb) {
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  const bool invert = display.isInverted();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool white = getBit(fb, x, y);
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] =
          (white != invert) ? 0xFFFFFFFFu : 0xFF000000u;
    }
  }
  pendingPresent.store(true);
}

void clearGrayscalePlanes() {
  grayscalePreviewState.lsbPlane.fill(0);
  grayscalePreviewState.msbPlane.fill(0);
  grayscalePreviewState.lsbValid = false;
  grayscalePreviewState.msbValid = false;
}

void snapshotBwBase(const uint8_t *fb) {
  memcpy(grayscalePreviewState.bwBase.data(), fb, HalDisplay::BUFFER_SIZE);
  grayscalePreviewState.bwBaseValid = true;
  clearGrayscalePlanes();
}

void copyPlane(std::array<uint8_t, HalDisplay::BUFFER_SIZE> &dst,
               const uint8_t *src, bool &valid) {
  if (!src) {
    valid = false;
    dst.fill(0);
    return;
  }
  memcpy(dst.data(), src, HalDisplay::BUFFER_SIZE);
  valid = true;
}

void composeGrayscalePreview() {
  const std::lock_guard<std::mutex> lock(pixelBufMutex);
  const uint8_t *bwBase = grayscalePreviewState.bwBaseValid
                              ? grayscalePreviewState.bwBase.data()
                              : display.getFrameBuffer();
  for (int y = 0; y < HalDisplay::DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < HalDisplay::DISPLAY_WIDTH; x++) {
      const bool baseWhite = getBit(bwBase, x, y);
      const bool lsbActive =
          grayscalePreviewState.lsbValid &&
          getBit(grayscalePreviewState.lsbPlane.data(), x, y);
      const bool msbActive =
          grayscalePreviewState.msbValid &&
          getBit(grayscalePreviewState.msbPlane.data(), x, y);

      uint8_t level = kGrayWhite;
      if (!baseWhite) {
        if (msbActive) {
          level = lsbActive ? kGrayDark : kGrayLight;
        } else if (lsbActive) {
          level = kGrayDark;
        } else {
          level = kGrayBlack;
        }
      }

      if (display.isInverted())
        level = static_cast<uint8_t>(255 - level);
      pixelBuf[y * HalDisplay::DISPLAY_WIDTH + x] = argbGray(level);
    }
  }
  pendingPresent.store(true);
}

} // namespace

static bool isPortraitOrientation(GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Portrait ||
         orientation == GfxRenderer::PortraitInverted;
}

// The panel's own size for this orientation. **Always the logical size**, whatever
// the window scale: every draw below is in these coordinates and SDL maps them to
// the drawable, so a scale change touches the window and nothing else. It is also
// what keeps a screenshot 480x800 -- see saveRendererBmp.
static void getLogicalWindowSize(GfxRenderer::Orientation orientation,
                                 int *width, int *height) {
  const bool isPortrait = isPortraitOrientation(orientation);
  *width = isPortrait ? HalDisplay::DISPLAY_HEIGHT : HalDisplay::DISPLAY_WIDTH;
  *height = isPortrait ? HalDisplay::DISPLAY_WIDTH : HalDisplay::DISPLAY_HEIGHT;
}

// The window, which is the logical size times the scale. Rounded, and never below
// one pixel per axis.
static void getWindowPixelSize(GfxRenderer::Orientation orientation, int *width,
                               int *height) {
  int logicalW = 0, logicalH = 0;
  getLogicalWindowSize(orientation, &logicalW, &logicalH);
  const double factor = realFactor > 0.0 ? realFactor : 1.0;
  *width = static_cast<int>(logicalW * factor + 0.5);
  *height = static_cast<int>(logicalH * factor + 0.5);
  if (*width < 1)
    *width = 1;
  if (*height < 1)
    *height = 1;
}

static void applyWindowGeometryIfNeeded(GfxRenderer::Orientation orientation) {
  if (!window || !sdl_renderer)
    return;

  int logicalW = 0, logicalH = 0;
  getLogicalWindowSize(orientation, &logicalW, &logicalH);
  if (logicalW == currentWindowWidth && logicalH == currentWindowHeight)
    return;

  int pixelW = 0, pixelH = 0;
  getWindowPixelSize(orientation, &pixelW, &pixelH);
  SDL_SetWindowSize(window, pixelW, pixelH);
  // Logical stays the panel; the window carries the scale.
  SDL_RenderSetLogicalSize(sdl_renderer, logicalW, logicalH);
  currentWindowWidth = logicalW;
  currentWindowHeight = logicalH;
}

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

#if defined(SIMULATOR_DISPLAY_UC8179)
#define SIMULATOR_CONTROLLER_TITLE "UC8179"
#elif defined(SIMULATOR_DISPLAY_UC8279)
#define SIMULATOR_CONTROLLER_TITLE "UC8279"
#else
#define SIMULATOR_CONTROLLER_TITLE "SSD1677"
#endif

#if defined(SIMULATOR_DEVICE_PAPERMONO)
static constexpr const char *WINDOW_TITLE =
    "Simulator - M5Stack PaperMono (SSD1677)";
#elif defined(SIMULATOR_DEVICE_STICKY)
static constexpr const char *WINDOW_TITLE =
    "Simulator - Seeed Sticky (SSD1677)";
#elif defined(SIMULATOR_DEVICE_X4_PRO)
static constexpr const char *WINDOW_TITLE =
    "Simulator - XTEINK X4 Pro (" SIMULATOR_CONTROLLER_TITLE ")";
#elif defined(SIMULATOR_DEVICE_X3)
#if defined(SIMULATOR_DISPLAY_UC8279)
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3 (UC8279d)";
#else
static constexpr const char *WINDOW_TITLE = "Simulator - XTEINK X3 (UC8253)";
#endif
#else
static constexpr const char *WINDOW_TITLE =
    "Simulator - XTEINK X4 (" SIMULATOR_CONTROLLER_TITLE ")";
#endif

#undef SIMULATOR_CONTROLLER_TITLE

void HalDisplay::begin() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError()
              << std::endl;
    return;
  }

  resolveWindowScale();
  resolveRealFactor();

  int logicalW = 0, logicalH = 0;
  extern GfxRenderer renderer;
  getLogicalWindowSize(renderer.getOrientation(), &logicalW, &logicalH);
  int pixelW = 0, pixelH = 0;
  getWindowPixelSize(renderer.getOrientation(), &pixelW, &pixelH);

  // The title carries the scale, because a screenshot or a photo of this window
  // is worthless without it -- the same rule the style watcher's captions follow.
  std::string title(WINDOW_TITLE);
  if (scaleMode == WindowScaleMode::Zoom) {
    title += " [zoom x" + std::to_string(zoomFactor) + ", nearest]";
  } else if (scaleMode == WindowScaleMode::Real) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), " [real x%.3f, %.0f ppi panel, %.0f dpi %s]",
                  realFactor, DEVICE_PPI, monitorDpi,
                  monitorDpiFromSdl ? "from SDL" : "ASSUMED");
    title += buf;
  } else {
    title += " [1:1]";
  }

  // SDL_WINDOW_ALLOW_HIGHDPI lets the renderer use full Retina/HiDPI pixels on
  // macOS so we get crisp 1:1 rendering instead of a blurry upscale.
  window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, pixelW, pixelH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);

  // **Before the renderer and the texture, not after.** The hint is read when a
  // texture is created, so setting it later is a coin flip on driver caching.
  //
  // NEAREST for 1:1 and zoom, and this is the load-bearing part rather than a
  // preference. Upstream set linear unconditionally, reasoning that "Bayer-
  // dithered pixels average to correct gray at scaled sizes" -- true, and exactly
  // wrong for a 1-bit map: a dither has to be judged as dots, because dots is what
  // the panel has. Linear at an integer zoom turns a 1 px hairline into a grey
  // ramp, which is the "resampled hairline reads as a smudge" failure the parent
  // repo's 1:1 rule was written against.
  //
  // Real is the one mode that keeps linear: its factor is fractional by
  // definition, nearest at x0.44 drops every other row outright, and averaging is
  // closer to what an eye does at that size. It is also why nothing measured is
  // allowed to come out of Real.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
              scaleMode == WindowScaleMode::Real ? "1" : "0");

  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  // All drawing stays in panel coordinates; SDL maps them onto the window.
  SDL_RenderSetLogicalSize(sdl_renderer, logicalW, logicalH);
  currentWindowWidth = logicalW;
  currentWindowHeight = logicalH;

  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);

  std::cerr << "[SIM] window " << pixelW << "x" << pixelH << " for a "
            << logicalW << "x" << logicalH << " panel, scale "
            << scaleModeName();
  if (scaleMode == WindowScaleMode::Zoom)
    std::cerr << " x" << zoomFactor;
  else if (scaleMode == WindowScaleMode::Real)
    std::cerr << " x" << realFactor << " (" << monitorDpi << " dpi "
              << (monitorDpiFromSdl ? "from SDL" : "assumed") << ", panel "
              << DEVICE_PPI << " ppi)";
  std::cerr << std::endl;
  if (scaleMode == WindowScaleMode::Real)
    std::cerr << "[SIM] real size is a resample -- nothing judged here counts as "
                 "evidence, and screenshots stay 1:1 regardless"
              << std::endl;
}

void HalDisplay::begin(bool /*seamless*/) { begin(); }

void HalDisplay::clearScreen(uint8_t color) const {
  memset(getFrameBuffer(), color, BUFFER_SIZE);
}

void HalDisplay::drawImage(const uint8_t *imageData, uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h, bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t *imageData, uint16_t x,
                                      uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t *fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT)
      break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES)
        break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::setInverted(bool value) { inverted = value; }

bool HalDisplay::toggleInverted() {
  inverted = !inverted;
  return inverted;
}

bool HalDisplay::isInverted() const { return inverted; }

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  refreshDisplay(mode, turnOffScreen);
  if (std::this_thread::get_id() == simulatorMainThread) {
    presentIfNeeded();
  }
}

void HalDisplay::displayBufferAsync(RefreshMode mode) {
  // SDL presentation is already handed off to the main thread. The framebuffer
  // conversion itself remains synchronous, so advertise no genuine overlap.
  refreshDisplay(mode, false);
}

void HalDisplay::waitRefreshComplete() {}

bool HalDisplay::supportsAsyncRefresh() const { return false; }

void HalDisplay::displayWindow(uint16_t, uint16_t, uint16_t, uint16_t, bool turnOffScreen) {
  // No windowed update on the host: repaint the whole texture. The window
  // rectangle only matters for panel timing, which is not modelled.
  refreshDisplay(RefreshMode::FAST_REFRESH, turnOffScreen);
}

// Called from the render task (background thread): convert framebuffer to
// pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
  const uint8_t *fb = getFrameBuffer();
  snapshotBwBase(fb);
  renderBwPixels(fb);
}

// Clear the current render target and draw the panel texture into it, rotated for
// the orientation. Its own function so a screenshot can run it against an
// offscreen, panel-sized target instead of the window -- see saveRendererBmp.
static void composePanel(GfxRenderer::Orientation orientation) {
  SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
  SDL_RenderClear(sdl_renderer);

  // For portrait modes the landscape panel texture must be rotated to fill the
  // portrait window. SDL_RenderCopyEx rotates around the centre of dst, so dst
  // must stay landscape-oriented and be offset so its centre coincides with the
  // window centre. After rotation the result fills the portrait window.
  //
  // Portrait rotateCoordinates stores content rotated 90° CCW in the physical
  // buffer, so we rotate +90° CW here to undo it. PortraitInverted stores
  // content rotated 90° CW → undo with -90°.
  switch (orientation) {
  case GfxRenderer::Portrait: {
    // dst centre = window centre, landscape-sized panel texture.
    SDL_Rect dst = {(HalDisplay::DISPLAY_HEIGHT - HalDisplay::DISPLAY_WIDTH) / 2,
                    HalDisplay::DISPLAY_WIDTH / 2 - HalDisplay::DISPLAY_HEIGHT / 2, HalDisplay::DISPLAY_WIDTH,
                    HalDisplay::DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::PortraitInverted: {
    SDL_Rect dst = {(HalDisplay::DISPLAY_HEIGHT - HalDisplay::DISPLAY_WIDTH) / 2,
                    HalDisplay::DISPLAY_WIDTH / 2 - HalDisplay::DISPLAY_HEIGHT / 2, HalDisplay::DISPLAY_WIDTH,
                    HalDisplay::DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, -90.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  case GfxRenderer::LandscapeClockwise: {
    SDL_Rect dst = {0, 0, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT};
    SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 180.0, nullptr,
                     SDL_FLIP_NONE);
    break;
  }
  default: {
    SDL_Rect dst = {0, 0, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT};
    SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
    break;
  }
  }

}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
  const bool screenshotDue = hasDueScreenshot();
  if (!pendingPresent.exchange(false) && !screenshotDue)
    return;

  if (!texture || !sdl_renderer)
    return;

  extern GfxRenderer renderer;
  const GfxRenderer::Orientation orientation = renderer.getOrientation();
  applyWindowGeometryIfNeeded(orientation);

  {
    const std::lock_guard<std::mutex> lock(pixelBufMutex);
    SDL_UpdateTexture(texture, nullptr, pixelBuf,
                      DISPLAY_WIDTH * sizeof(uint32_t));
  }
  // The clear colour was never set anywhere in this file, so this cleared with
  // whatever the renderer's draw colour happened to be. Black is what an e-ink
  // frame's surround should be, and an unset colour is a bug waiting for a
  // platform to pick something else.
  //
  // It also removed a coloured fringe around the panel on Android: the border
  // pixels went from (136,169,52) to (0,0,0). Linear filtering blends the
  // scaled texture's edge against the surround, so an undefined surround shows
  // up as a five-pixel gradient there. See ANDROID.md.
  // Screenshot first, then the window. The capture switches the render target and
  // switches back, and some drivers do not promise the backbuffer survives that --
  // composing the window afterwards costs one extra pass on a screenshot frame and
  // removes the question.
  if (screenshotDue) {
    captureDueScreenshots();
  }
  composePanel(orientation);
  SDL_RenderPresent(sdl_renderer);
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() { presentIfNeeded(); }

uint8_t *HalDisplay::getFrameBuffer() const {
  if (frameBufferLent) {
    return nullptr;
  }
  return frameBufferStorage.data();
}

uint8_t *HalDisplay::lendFrameBufferStorage(uint32_t *sizeOut) {
  if (sizeOut) {
    *sizeOut = frameBufferLent ? 0 : BUFFER_SIZE;
  }
  if (frameBufferLent) {
    return nullptr;
  }
  frameBufferLent = true;
  return frameBufferStorage.data();
}

void HalDisplay::returnFrameBufferStorage() {
  if (!frameBufferLent) {
    return;
  }
  frameBufferStorage.fill(0xFF);
  frameBufferLent = false;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t *lsbBuffer,
                                      const uint8_t *msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}
void HalDisplay::displayGrayscaleBase(RefreshMode fallback,
                                      bool turnOffScreen) {
  if (combinesGrayscaleBase()) {
    snapshotBwBase(getFrameBuffer());
    return;
  }
  displayBuffer(fallback, turnOffScreen);
}
void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t *lsbBuffer) {
  copyPlane(grayscalePreviewState.lsbPlane, lsbBuffer,
            grayscalePreviewState.lsbValid);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t *msbBuffer) {
  copyPlane(grayscalePreviewState.msbPlane, msbBuffer,
            grayscalePreviewState.msbValid);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t *bwBuffer) {
  if (bwBuffer) {
    snapshotBwBase(bwBuffer);
  } else {
    grayscalePreviewState.bwBaseValid = false;
    grayscalePreviewState.bwBase.fill(0);
    clearGrayscalePlanes();
  }
}
void HalDisplay::displayGrayBuffer(bool, const unsigned char *, bool) {
  composeGrayscalePreview();
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t *rows,
                                          uint16_t yStart, uint16_t numRows) {
  if (!rows || numRows == 0 || yStart >= DISPLAY_HEIGHT) {
    return;
  }

  const uint16_t rowsToCopy =
      (yStart + numRows > DISPLAY_HEIGHT) ? (DISPLAY_HEIGHT - yStart) : numRows;
  const size_t offset = static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES;
  const size_t byteCount =
      static_cast<size_t>(rowsToCopy) * DISPLAY_WIDTH_BYTES;
  auto &plane = lsbPlane ? grayscalePreviewState.lsbPlane
                         : grayscalePreviewState.msbPlane;
  memcpy(plane.data() + offset, rows, byteCount);
  if (lsbPlane) {
    grayscalePreviewState.lsbValid = true;
  } else {
    grayscalePreviewState.msbValid = true;
  }
}
bool HalDisplay::supportsStripGrayscale() const { return true; }
bool HalDisplay::combinesGrayscaleBase() const {
  return BoardConfig::isPaperMono();
}

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const {
  return DISPLAY_WIDTH_BYTES;
}
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

HalDisplay display;
