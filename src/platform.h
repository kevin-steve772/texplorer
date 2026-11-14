#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace plat {

void initConsole();

void enableMouseMotion();
void disableMouseMotion();

int charWidth(wchar_t wc);

std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(const std::string& s);
std::filesystem::path joinPath(const std::filesystem::path& dir,
                               const std::string& utf8Name);

bool localTime(std::time_t t, std::tm& out);
int caseCmp(const char* a, const char* b);

struct FileMeta {
    bool ok = false;
    std::uintmax_t size = 0;
    std::time_t mtime = 0;
    bool regular = false;
};

FileMeta statFile(const std::filesystem::path& p);
bool isExecutable(const std::filesystem::path& p);
bool isHidden(const std::filesystem::path& p);

struct Location {
    std::string icon;
    std::string label;
    std::filesystem::path path;
};

std::vector<Location> userLocations();
std::vector<Location> rootLocations();
std::filesystem::path homeDir();
std::filesystem::path trashBaseDir();
std::string rootLabel(const std::filesystem::path& p);

bool validFileName(const std::string& name);

std::vector<std::string> commandLineArgs(int argc, char** argv);

bool openNeedsSuspend();

struct OpenOutcome {
    bool ok = false;
    std::string message;
};

OpenOutcome openExternal(const std::filesystem::path& p,
                         const std::filesystem::path& cwd, bool executable);

} // namespace plat
