// Settings from a file, for what launches programs without a way to set
// their environment (the WineMetalGL Settings app, src/settings-app).
#pragma once

#include <string>

namespace wmgl {

// The settings file: WINEMETALGL_SETTINGS when set ("0": none), else
// ~/Library/Application Support/WineMetalGL/settings.conf.
std::string settingsPath();

// Sets the WINEMETALGL_* variables of the settings file ("NAME=value" lines,
// # comments) that the environment does not set: the environment wins. Runs
// when the core loads, before anything reads its settings. The names it set
// go in WINEMETALGL_SETTINGS_APPLIED: a child process (a game Steam starts)
// inherits them, and drops them to read the file as it is now.
void applySettingsFile();

// Sets the WINEMETALGL_* variables of a game's own settings (winemetalgl.conf
// beside its .exe, read by the Wine side), the same lines as the settings
// file's: over the settings file's values, under the environment's. Before
// anything reads its settings (the Wine side's first call).
void applyGameSettings(const std::string& text);

}  // namespace wmgl
