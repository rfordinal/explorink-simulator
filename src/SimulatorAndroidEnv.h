#pragma once

// Android has no way for the Java side to hand environment variables to the
// native code: SDLActivity loads the native libraries inside onCreate, so
// nothing Java does before that can call setenv, and by the time it could the
// SDL thread may already be running.
//
// Every simulator knob is an environment variable -- CROSSPOINT_SIM_SD,
// CROSSPOINT_SIM_BLE_PORT, CROSSPOINT_SIM_INPUT_SCRIPT,
// CROSSPOINT_SIM_SCREENSHOTS, CROSSPOINT_SIM_HTTP_PORT -- so on Android all of
// them were unreachable. This reads them from a file instead, once, before
// anything looks at them.
namespace SimulatorAndroidEnv {

// Reads KEY=VALUE lines from <app files dir>/sim-env and applies each one with
// setenv(..., 0): a variable already set in the real environment wins, so this
// only ever fills gaps. Blank lines and lines starting with '#' are skipped.
// Does nothing off Android. Returns how many variables it set.
int load();

} // namespace SimulatorAndroidEnv
