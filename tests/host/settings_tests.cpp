#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "core/util/settings.h"
#include "test.h"

using namespace wmgl;

// The settings file fills what the environment leaves unset, and only
// WineMetalGL's variables.
TEST(settings_file_under_environment) {
    char path[] = "/tmp/wmgl-settings-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    const std::string text = "# written by WineMetalGL Settings\n"
                             "WINEMETALGL_UPSCALE = performance\n"
                             "  WINEMETALGL_TEST_SETTING_A=from-file\r\n"
                             "WINEMETALGL_TEST_SETTING_B=from-file\n"
                             "HOME=/nowhere\n"
                             "WINEMETALGL_SETTINGS=/elsewhere\n"
                             "not a setting\n";
    CHECK(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    close(fd);

    const char* home = std::getenv("HOME");
    const std::string savedHome = home ? home : "";
    setenv("WINEMETALGL_SETTINGS", path, 1);
    unsetenv("WINEMETALGL_UPSCALE");
    unsetenv("WINEMETALGL_TEST_SETTING_A");
    setenv("WINEMETALGL_TEST_SETTING_B", "from-environment", 1);
    CHECK(settingsPath() == path);
    applySettingsFile();
    CHECK(std::getenv("WINEMETALGL_UPSCALE") && !std::strcmp(std::getenv("WINEMETALGL_UPSCALE"), "performance"));
    CHECK(std::getenv("WINEMETALGL_TEST_SETTING_A") && !std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_A"), "from-file"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_B"), "from-environment"));
    CHECK(savedHome == (std::getenv("HOME") ? std::getenv("HOME") : ""));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_SETTINGS"), path));

    // A child of a process that applied the file inherits its values, marked:
    // the file as it is now wins over them, the environment still over it.
    CHECK(std::getenv("WINEMETALGL_SETTINGS_APPLIED") != nullptr);
    setenv("WINEMETALGL_TEST_SETTING_A", "stale", 1);
    setenv("WINEMETALGL_UPSCALE", "stale", 1);
    setenv("WINEMETALGL_SETTINGS_APPLIED", "WINEMETALGL_TEST_SETTING_A,WINEMETALGL_UPSCALE", 1);
    applySettingsFile();
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_A"), "from-file"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_UPSCALE"), "performance"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_B"), "from-environment"));

    setenv("WINEMETALGL_SETTINGS", "0", 1);
    CHECK(settingsPath().empty());
    unsetenv("WINEMETALGL_SETTINGS");
    unsetenv("WINEMETALGL_UPSCALE");
    unsetenv("WINEMETALGL_TEST_SETTING_A");
    unsetenv("WINEMETALGL_TEST_SETTING_B");
    unsetenv("WINEMETALGL_SETTINGS_APPLIED");
    std::remove(path);
}

// A game's winemetalgl.conf (issue #76) wins over the settings file, the
// environment over both; its names join the applied list, so a child process
// reads the files again.
TEST(settings_game_file_between_file_and_environment) {
    char path[] = "/tmp/wmgl-settings-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    const std::string text = "WINEMETALGL_MAX_FRAME_RATE=60\nWINEMETALGL_TEST_SETTING_A=from-file\n";
    CHECK(write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    close(fd);
    setenv("WINEMETALGL_SETTINGS", path, 1);
    unsetenv("WINEMETALGL_SETTINGS_APPLIED");
    unsetenv("WINEMETALGL_MAX_FRAME_RATE");
    unsetenv("WINEMETALGL_TEST_SETTING_A");
    setenv("WINEMETALGL_TEST_SETTING_B", "from-environment", 1);
    applySettingsFile();
    applyGameSettings("# Alien: Isolation\nWINEMETALGL_MAX_FRAME_RATE = 100\r\n"
                      "WINEMETALGL_TEST_SETTING_B=from-game\nWINEMETALGL_TEST_SETTING_C=from-game\n"
                      "HOME=/nowhere\nWINEMETALGL_SETTINGS=/elsewhere\n");
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_MAX_FRAME_RATE"), "100"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_A"), "from-file"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_B"), "from-environment"));
    CHECK(std::getenv("WINEMETALGL_TEST_SETTING_C") && !std::strcmp(std::getenv("WINEMETALGL_TEST_SETTING_C"), "from-game"));
    CHECK(!std::strcmp(std::getenv("WINEMETALGL_SETTINGS"), path));
    const std::string applied = std::getenv("WINEMETALGL_SETTINGS_APPLIED");
    CHECK(applied.find("WINEMETALGL_TEST_SETTING_C") != std::string::npos);
    CHECK(applied.find("WINEMETALGL_TEST_SETTING_B") == std::string::npos);
    CHECK(applied.find("WINEMETALGL_MAX_FRAME_RATE") == applied.rfind("WINEMETALGL_MAX_FRAME_RATE"));
    for (const char* name : {"WINEMETALGL_SETTINGS", "WINEMETALGL_SETTINGS_APPLIED", "WINEMETALGL_MAX_FRAME_RATE",
                             "WINEMETALGL_TEST_SETTING_A", "WINEMETALGL_TEST_SETTING_B", "WINEMETALGL_TEST_SETTING_C"})
        unsetenv(name);
    std::remove(path);
}
