#include "platform.h"

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sys/stat.h>

#include <curses.h>

extern "C" int PDC_wcwidth(const std::int32_t ucs);

#else

#include <fcntl.h>
#include <ncurses.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

std::string wideToUtf8(const wchar_t* w) {
    if (!w)
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return "";
    std::string out((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) {
        result = fs::path(raw);
        CoTaskMemFree(raw);
    }
    return result;
}

fs::path envPath(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    if (v && *v)
        return fs::path(v);
    return fs::path();
}

#else

fs::path envPath(const char* name) {
    const char* v = std::getenv(name);
    if (v && *v)
        return fs::path(v);
    return fs::path();
}

#endif

} // namespace

namespace plat {

void initConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    static const char* locales[] = {".UTF-8", ".utf8", "C.UTF-8", ""};
    for (const char* loc : locales) {
        if (std::setlocale(LC_ALL, loc))
            break;
    }
#else
    std::setlocale(LC_ALL, "");
#endif
}

void enableMouseMotion() {
#ifndef _WIN32
    std::fputs("\033[?1002h\033[?1003h", stdout);
    std::fflush(stdout);
#endif
}

void disableMouseMotion() {
#ifndef _WIN32
    std::fputs("\033[?1003l\033[?1002l", stdout);
    std::fflush(stdout);
#endif
}

int charWidth(wchar_t wc) {
#if defined(PDCURSES)
    return PDC_wcwidth((std::int32_t)wc);
#else
    return wcwidth(wc);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
#else
    return p.u8string();
#endif
}

fs::path pathFromUtf8(const std::string& s) {
    return fs::u8path(s);
}

fs::path joinPath(const fs::path& dir, const std::string& utf8Name) {
    return dir / pathFromUtf8(utf8Name);
}

bool localTime(std::time_t t, std::tm& out) {
    std::tm* tmp = std::localtime(&t);
    if (!tmp)
        return false;
    out = *tmp;
    return true;
}

int caseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

FileMeta statFile(const fs::path& p) {
    FileMeta m;
#ifdef _WIN32
    struct _stat64 st {};
    if (_wstat64(p.c_str(), &st) != 0)
        return m;
    m.ok = true;
    m.size = (std::uintmax_t)st.st_size;
    m.mtime = (std::time_t)st.st_mtime;
    m.regular = ((st.st_mode & _S_IFMT) == _S_IFREG);
#else
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0)
        return m;
    m.ok = true;
    m.size = (std::uintmax_t)st.st_size;
    m.mtime = st.st_mtime;
    m.regular = S_ISREG(st.st_mode);
#endif
    return m;
}

bool isExecutable(const fs::path& p) {
#ifdef _WIN32
    std::string ext = pathToUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return ext == ".exe" || ext == ".com" || ext == ".bat" || ext == ".cmd" ||
           ext == ".ps1";
#else
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

bool isHidden(const fs::path& p) {
    std::string name = pathToUtf8(p.filename());
    if (!name.empty() && name[0] == '.')
        return true;
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    return false;
#endif
}

fs::path homeDir() {
#ifdef _WIN32
    fs::path p = knownFolder(FOLDERID_Profile);
    if (!p.empty())
        return p;
    p = envPath(L"USERPROFILE");
    if (!p.empty())
        return p;
    fs::path drive = envPath(L"HOMEDRIVE");
    fs::path hp = envPath(L"HOMEPATH");
    if (!drive.empty() && !hp.empty())
        return drive / hp;
    p = envPath(L"PUBLIC");
    if (!p.empty())
        return p;
    return fs::path(L"C:\\");
#else
    fs::path p = envPath("HOME");
    return p.empty() ? fs::path("/") : p;
#endif
}

std::vector<Location> userLocations() {
    std::vector<Location> out;
    fs::path home = homeDir();

#ifdef _WIN32
    struct Def {
        REFKNOWNFOLDERID id;
        const char* icon;
        const char* label;
        const wchar_t* sub;
    };
    static const Def defs[] = {
        {FOLDERID_Profile, "~", "主页", nullptr},
        {FOLDERID_Desktop, "▣", "桌面", L"Desktop"},
        {FOLDERID_Documents, "▤", "文档", L"Documents"},
        {FOLDERID_Downloads, "↓", "下载", L"Downloads"},
        {FOLDERID_Pictures, "▦", "图片", L"Pictures"},
        {FOLDERID_Music, "♪", "音乐", L"Music"},
        {FOLDERID_Videos, "▷", "视频", L"Videos"},
    };
    for (const auto& d : defs) {
        fs::path p = knownFolder(d.id);
        std::error_code ec;
        if (p.empty() || !fs::is_directory(p, ec)) {
            ec.clear();
            p = d.sub ? (home / d.sub) : home;
        }
        if (!p.empty() && fs::is_directory(p, ec))
            out.push_back({d.icon, d.label, p});
    }
#else
    struct Def {
        const char* icon;
        const char* label;
        const char* sub;
    };
    static const Def defs[] = {
        {"~", "主页", nullptr},       {"▣", "桌面", "Desktop"},
        {"▤", "文档", "Documents"},   {"↓", "下载", "Downloads"},
        {"▦", "图片", "Pictures"},    {"♪", "音乐", "Music"},
        {"▷", "视频", "Videos"},
    };
    for (const auto& d : defs) {
        fs::path p = d.sub ? (home / d.sub) : home;
        std::error_code ec;
        if (fs::is_directory(p, ec))
            out.push_back({d.icon, d.label, p});
    }
#endif
    return out;
}

std::vector<Location> rootLocations() {
    std::vector<Location> out;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        char letter = (char)('A' + i);
        std::string label;
        label += letter;
        label += ":\\";
        fs::path root = fs::path(label);
        std::error_code ec;
        if (fs::is_directory(root, ec))
            out.push_back({"▣", label, root});
    }
    if (out.empty())
        out.push_back({"▣", "C:\\", fs::path(L"C:\\")});
#else
    out.push_back({"/", "根目录", fs::path("/")});
#endif
    return out;
}

fs::path trashBaseDir() {
#ifdef _WIN32
    fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        local = envPath(L"LOCALAPPDATA");
    if (local.empty())
        local = homeDir() / L"AppData" / L"Local";
    return local / L"TExplorer" / L"Trash";
#else
    fs::path xdg = envPath("XDG_DATA_HOME");
    if (!xdg.empty())
        return xdg / "Trash";
    fs::path home = envPath("HOME");
    if (!home.empty())
        return home / ".local" / "share" / "Trash";
    return fs::path(".local") / "share" / "Trash";
#endif
}

std::string rootLabel(const fs::path& p) {
#ifdef _WIN32
    fs::path r = p.root_path();
    return pathToUtf8(r.empty() ? p : r);
#else
    (void)p;
    return "/";
#endif
}

bool validFileName(const std::string& name) {
    if (name.empty() || name == "." || name == "..")
        return false;
#ifdef _WIN32
    static const std::string invalid = "<>:\"/\\|?*";
    for (unsigned char c : name) {
        if (c < 32)
            return false;
        if (invalid.find((char)c) != std::string::npos)
            return false;
    }
    char last = name.back();
    if (last == ' ' || last == '.')
        return false;

    std::string base = name.substr(0, name.find('.'));
    std::string lower;
    lower.reserve(base.size());
    for (char c : base)
        lower.push_back((char)std::tolower((unsigned char)c));
    static const char* reserved[] = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
        "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
        "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : reserved) {
        if (lower == r)
            return false;
    }
    return true;
#else
    return name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
#endif
}

std::vector<std::string> commandLineArgs(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 0; i < wargc; ++i)
            out.push_back(wideToUtf8(wargv[i]));
        LocalFree(wargv);
    }
    if (out.empty()) {
        for (int i = 0; i < argc; ++i)
            out.push_back(argv[i] ? argv[i] : "");
    }
#else
    for (int i = 0; i < argc; ++i)
        out.push_back(argv[i] ? argv[i] : "");
#endif
    return out;
}

bool openNeedsSuspend() {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

OpenOutcome openExternal(const fs::path& p, const fs::path& cwd,
                         bool executable) {
    OpenOutcome o;
#ifdef _WIN32
    std::wstring file = p.wstring();
    std::wstring dir = cwd.wstring();
    HINSTANCE r = ShellExecuteW(nullptr, L"open", file.c_str(), nullptr,
                                dir.c_str(), SW_SHOWNORMAL);
    std::intptr_t code = (std::intptr_t)r;
    if (code > 32) {
        o.ok = true;
        o.message = (executable ? "已运行 " : "已打开 ") + pathToUtf8(p.filename());
    } else {
        o.ok = false;
        o.message = "无法打开: " + pathToUtf8(p.filename());
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        (void)chdir(cwd.c_str());
        if (executable)
            execl(p.c_str(), p.filename().c_str(), (char*)nullptr);
        else
            execlp("nano", "nano", p.c_str(), (char*)nullptr);
        _exit(127);
    }

    int status = 0;
    if (pid > 0)
        waitpid(pid, &status, 0);

    if (pid < 0) {
        o.ok = false;
        o.message = "无法启动进程";
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        o.ok = false;
        o.message = executable ? ("无法运行: " + pathToUtf8(p.filename()))
                               : "无法启动 nano";
    } else {
        o.ok = true;
        o.message = executable ? ("已运行 " + pathToUtf8(p.filename()))
                               : ("已用 nano 编辑 " + pathToUtf8(p.filename()));
    }
#endif
    return o;
}

} // namespace plat
