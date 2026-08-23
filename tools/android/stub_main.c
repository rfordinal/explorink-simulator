/* Smoke test for the Android shell: proves gradle, jniLibs, SDLActivity and
 * adb install all work before 208 firmware objects are added to the picture.
 * Draws nothing but a moving stripe, so a screenshot tells you it is alive.
 *
 * Replaced by the firmware's own libmain.so once linking works.
 */
#include <SDL.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("ExplorInk Simulator", 0, 0, 0, 0,
                                        SDL_WINDOW_FULLSCREEN);
  if (!window) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return 1;
  }
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
  if (!renderer) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return 1;
  }

  int width = 0, height = 0;
  SDL_GetRendererOutputSize(renderer, &width, &height);
  SDL_Log("ExplorInk stub alive: renderer %dx%d", width, height);

  int frame = 0;
  int running = 1;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = 0;
    }
    /* E-ink white background, one dark stripe that moves, so consecutive
     * screenshots are distinguishable. */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_Rect stripe = {0, (frame * 4) % (height > 0 ? height : 1), width, 40};
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderFillRect(renderer, &stripe);
    SDL_RenderPresent(renderer);
    ++frame;
    SDL_Delay(16);
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
