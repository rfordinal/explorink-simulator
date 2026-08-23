#include "SimulatorAndroidEnv.h"

#if defined(__ANDROID__)

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace SimulatorAndroidEnv {

int load() {
  const char *base = SDL_AndroidGetInternalStoragePath();
  if (!base || !*base)
    return 0;
  const std::string path = std::string(base) + "/sim-env";
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f)
    return 0;

  int applied = 0;
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
    // Trim the newline and any trailing whitespace; a file written by a shell
    // heredoc or by adb usually has one.
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
      line[--len] = '\0';
    }
    char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t')
      ++cursor;
    if (*cursor == '\0' || *cursor == '#')
      continue;

    char *eq = std::strchr(cursor, '=');
    if (!eq)
      continue;
    *eq = '\0';
    const char *key = cursor;
    const char *value = eq + 1;
    if (*key == '\0')
      continue;
    // Overwrite 0: a real environment variable, if one can ever be set, stays
    // authoritative. The file is the fallback, not the override.
    if (setenv(key, value, 0) == 0)
      ++applied;
  }
  std::fclose(f);
  if (applied > 0)
    SDL_Log("[SIM] sim-env: applied %d variable(s) from %s", applied,
            path.c_str());
  return applied;
}

} // namespace SimulatorAndroidEnv

#else

namespace SimulatorAndroidEnv {
int load() { return 0; }
} // namespace SimulatorAndroidEnv

#endif
