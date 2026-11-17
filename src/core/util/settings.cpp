#include "core/util/settings.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace wmgl {

std::string settingsPath() {
    if (const char* path = std::getenv("WINEMETALGL_SETTINGS")) return std::strcmp(path, "0") ? path : "";
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/Library/Application Support/WineMetalGL/settings.conf" : "";
}

namespace {

const char* const kApplied = "WINEMETALGL_SETTINGS_APPLIED";

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r");
    const size_t last = s.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// Calls `apply(name, value)` for each WINEMETALGL_* "NAME=value" line of
// `text` (# comments), never the file's own path or the applied list.
template <typename Apply>
void forEachSetting(std::istream& text, Apply apply) {
    std::string line;
    while (std::getline(text, line)) {
        line = trim(line);
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string name = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        if (name.rfind("WINEMETALGL_", 0) != 0 || name == "WINEMETALGL_SETTINGS" || name == kApplied) continue;
        apply(name, value);
    }
}

bool listed(const std::string& list, const std::string& name) {
    return ("," + list + ",").find("," + name + ",") != std::string::npos;
}

}  // namespace

void applySettingsFile() {
    // Variables an earlier process took from the file (a launcher, Steam)
    // reach its children as environment: they are the file's, not the
    // user's, so the file is read again rather than its old values winning.
    if (const char* applied = std::getenv(kApplied)) {
        std::string names = applied;
        for (size_t start = 0; start < names.size();) {
            const size_t end = names.find(',', start) == std::string::npos ? names.size() : names.find(',', start);
            const std::string name = names.substr(start, end - start);
            if (name.rfind("WINEMETALGL_", 0) == 0) unsetenv(name.c_str());
            start = end + 1;
        }
        unsetenv(kApplied);
    }
    const std::string path = settingsPath();
    if (path.empty()) return;
    std::ifstream file(path);
    std::string applied;
    forEachSetting(file, [&](const std::string& name, const std::string& value) {
        if (std::getenv(name.c_str())) return;  // the environment wins
        setenv(name.c_str(), value.c_str(), 1);
        applied += (applied.empty() ? "" : ",") + name;
    });
    if (!applied.empty()) setenv(kApplied, applied.c_str(), 1);
}

void applyGameSettings(const std::string& text) {
    std::istringstream lines(text);
    const char* current = std::getenv(kApplied);
    std::string applied = current ? current : "";
    forEachSetting(lines, [&](const std::string& name, const std::string& value) {
        // Over the settings file's value, never over the environment's.
        if (std::getenv(name.c_str()) && !listed(applied, name)) return;
        setenv(name.c_str(), value.c_str(), 1);
        if (!listed(applied, name)) applied += (applied.empty() ? "" : ",") + name;
    });
    if (!applied.empty()) setenv(kApplied, applied.c_str(), 1);
}

namespace {
__attribute__((constructor)) void applyAtLoad() { applySettingsFile(); }
}  // namespace

}  // namespace wmgl
