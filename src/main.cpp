#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static const char* ICON_DIR = "📁";
static const char* ICON_FILE = "📄";
static const char* ICON_LINK = "🔗";

static std::string sanitize(const std::string& s) {
    std::string out;
    mbstate_t st{};
    const char* p = s.data();
    size_t n = s.size();
    while (n > 0) {
        wchar_t wc = 0;
        size_t r = mbrtowc(&wc, p, n, &st);
        if (r == (size_t)-1) {
            out += '?';
            std::memset(&st, 0, sizeof(st));
            ++p;
            --n;
            continue;
        }
        if (r == (size_t)-2) {
            out += '?';
            break;
        }
        if (r == 0) {
            out += ' ';
            ++p;
            --n;
            continue;
        }
        if (wc < 32 || wc == 127)
            out += ' ';
        else
            out.append(p, r);
        p += r;
        n -= r;
    }
    return out;
}

static int utf8Cols(const std::string& s) {
    mbstate_t st{};
    int w = 0;
    const char* p = s.data();
    size_t n = s.size();
    while (n > 0) {
        wchar_t wc = 0;
        size_t r = mbrtowc(&wc, p, n, &st);
        if (r == (size_t)-1) {
            std::memset(&st, 0, sizeof(st));
            ++p;
            --n;
            w += 1;
            continue;
        }
        if (r == (size_t)-2) {
            w += 1;
            break;
        }
        if (r == 0) {
            ++p;
            --n;
            continue;
        }
        int cw = wcwidth(wc);
        if (cw < 0)
            cw = 0;
        w += cw;
        p += r;
        n -= r;
    }
    return w;
}

static std::string utf8Clip(const std::string& s, int maxCols) {
    if (maxCols <= 0)
        return "";
    if (utf8Cols(s) <= maxCols)
        return s;
    int target = maxCols - 1;
    std::string out;
    int w = 0;
    mbstate_t st{};
    const char* p = s.data();
    size_t n = s.size();
    while (n > 0) {
        wchar_t wc = 0;
        size_t r = mbrtowc(&wc, p, n, &st);
        if (r == (size_t)-1 || r == (size_t)-2 || r == 0)
            break;
        int cw = wcwidth(wc);
        if (cw < 0)
            cw = 0;
        if (w + cw > target)
            break;
        out.append(p, r);
        w += cw;
        p += r;
        n -= r;
    }
    out += "…";
    return out;
}

static std::string fitCols(const std::string& s, int w) {
    if (w <= 0)
        return "";
    std::string t = utf8Clip(s, w);
    int cw = utf8Cols(t);
    if (cw < w)
        t.append((size_t)(w - cw), ' ');
    return t;
}

static std::string fitRight(const std::string& s, int w) {
    if (w <= 0)
        return "";
    std::string t = utf8Clip(s, w);
    int cw = utf8Cols(t);
    if (cw < w)
        return std::string((size_t)(w - cw), ' ') + t;
    return t;
}

static std::string centerCols(const std::string& s, int w) {
    int cw = utf8Cols(s);
    if (cw >= w)
        return utf8Clip(s, w);
    int left = (w - cw) / 2;
    return std::string((size_t)left, ' ') + s;
}

static void wput(WINDOW* win, int y, int x, int w, const std::string& text, int attr = 0) {
    if (!win || w <= 0)
        return;
    int maxw = getmaxx(win) - x;
    if (maxw <= 0)
        return;
    if (w > maxw)
        w = maxw;
    std::string t = fitCols(text, w);
    wattron(win, attr);
    mvwaddnstr(win, y, x, t.c_str(), (int)t.size());
    wattroff(win, attr);
}

static std::string toUtf8(wchar_t wc) {
    char buf[MB_LEN_MAX];
    mbstate_t st{};
    size_t n = wcrtomb(buf, wc, &st);
    if (n == (size_t)-1)
        return "";
    return std::string(buf, n);
}

static size_t prevUtf8Pos(const std::string& s, size_t pos) {
    if (pos == 0 || s.empty())
        return 0;
    if (pos > s.size())
        pos = s.size();
    size_t i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        --i;
    return i;
}

static size_t nextUtf8Pos(const std::string& s, size_t pos) {
    if (pos >= s.size())
        return s.size();
    ++pos;
    while (pos < s.size() && ((unsigned char)s[pos] & 0xC0) == 0x80)
        ++pos;
    return pos;
}

static std::string asciiLower(std::string s) {
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

static std::string humanSize(uintmax_t n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    char buf[64];
    if (i == 0)
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    else
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[i]);
    return std::string(buf);
}

static std::string formatTime(std::time_t t) {
    std::tm tm{};
    if (t <= 0 || !localtime_r(&t, &tm))
        return "";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tm);
    return std::string(buf);
}

static std::string typeFor(const fs::path& p, bool isDir, bool isLink) {
    if (isLink)
        return isDir ? "文件夹快捷方式" : "快捷方式";
    if (isDir)
        return "文件夹";
    std::string ext = p.extension().string();
    if (!ext.empty() && ext[0] == '.')
        ext.erase(ext.begin());
    ext = asciiLower(ext);
    static const std::unordered_map<std::string, std::string> types = {
        {"txt", "文本文档"},       {"md", "Markdown 文档"},  {"pdf", "PDF 文档"},
        {"doc", "Word 文档"},      {"docx", "Word 文档"},    {"xls", "Excel 工作表"},
        {"xlsx", "Excel 工作表"},  {"ppt", "PowerPoint 演示文稿"},
        {"pptx", "PowerPoint 演示文稿"},                     {"png", "图像"},
        {"jpg", "图像"},           {"jpeg", "图像"},         {"gif", "图像"},
        {"bmp", "图像"},           {"webp", "图像"},         {"svg", "图像"},
        {"mp4", "视频"},           {"mkv", "视频"},          {"avi", "视频"},
        {"mov", "视频"},           {"mp3", "音频"},          {"flac", "音频"},
        {"wav", "音频"},           {"ogg", "音频"},          {"zip", "压缩文件"},
        {"7z", "压缩文件"},        {"rar", "压缩文件"},      {"tar", "压缩文件"},
        {"gz", "压缩文件"},        {"xz", "压缩文件"},       {"c", "C 源文件"},
        {"h", "C 头文件"},         {"cpp", "C++ 源文件"},    {"cc", "C++ 源文件"},
        {"cxx", "C++ 源文件"},     {"hpp", "C++ 头文件"},    {"py", "Python 源文件"},
        {"js", "JavaScript 文件"}, {"ts", "TypeScript 文件"},{"sh", "Shell 脚本"},
        {"json", "JSON 文件"},     {"html", "HTML 文档"},    {"htm", "HTML 文档"},
        {"css", "样式表"},         {"exe", "应用程序"},      {"iso", "光盘映像"},
    };
    auto it = types.find(ext);
    if (it != types.end())
        return it->second;
    if (ext.empty())
        return "文件";
    for (char& c : ext)
        c = (char)toupper((unsigned char)c);
    return ext + " 文件";
}

static std::vector<std::string> wrapText(const std::string& s, int width) {
    std::vector<std::string> lines;
    if (width < 1)
        width = 1;
    std::string cur;
    int curW = 0;
    mbstate_t st{};
    const char* p = s.data();
    size_t n = s.size();
    while (n > 0) {
        wchar_t wc = 0;
        size_t r = mbrtowc(&wc, p, n, &st);
        if (r == (size_t)-1) {
            std::memset(&st, 0, sizeof(st));
            if (curW + 1 > width && curW > 0) {
                lines.push_back(cur);
                cur.clear();
                curW = 0;
            }
            cur += '?';
            curW += 1;
            ++p;
            --n;
            continue;
        }
        if (r == (size_t)-2)
            break;
        if (r == 0) {
            ++p;
            --n;
            continue;
        }
        if (wc == L'\n') {
            lines.push_back(cur);
            cur.clear();
            curW = 0;
            p += r;
            n -= r;
            continue;
        }
        if (wc < 32 || wc == 127) {
            p += r;
            n -= r;
            continue;
        }
        int cw = wcwidth(wc);
        if (cw < 0)
            cw = 0;
        if (curW + cw > width && curW > 0) {
            lines.push_back(cur);
            cur.clear();
            curW = 0;
        }
        cur.append(p, r);
        curW += cw;
        p += r;
        n -= r;
    }
    lines.push_back(cur);
    return lines;
}

static fs::path normPath(const fs::path& p) {
    std::error_code ec;
    fs::path q = fs::weakly_canonical(p, ec);
    if (!ec)
        return q;
    fs::path a = fs::absolute(p, ec);
    return ec ? p : a;
}

static bool samePath(const fs::path& a, const fs::path& b) {
    return normPath(a) == normPath(b);
}

static bool isSubPath(const fs::path& child, const fs::path& parent) {
    fs::path c = normPath(child);
    fs::path p = normPath(parent);
    auto ci = c.begin();
    auto pi = p.begin();
    for (; pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi)
            return false;
    }
    return true;
}

static int dlgButtonW(const std::string& label) {
    return utf8Cols(label) + 4;
}

static void drawDlgButton(WINDOW* win, int y, int x, const std::string& label, bool primary) {
    std::string s = "[ " + label + " ]";
    if (primary)
        wattron(win, A_REVERSE);
    mvwaddstr(win, y, x, s.c_str());
    if (primary)
        wattroff(win, A_REVERSE);
}

class App {
public:
    explicit App(fs::path start) : startPath(std::move(start)) {}

    void run();

private:
    enum class SortKey { Name, Date, Type, Size };
    enum Cmd {
        CMD_NONE,
        CMD_OPEN,
        CMD_NEWFOLDER,
        CMD_RENAME,
        CMD_DELETE,
        CMD_COPY,
        CMD_CUT,
        CMD_PASTE,
        CMD_REFRESH,
        CMD_PROPERTIES,
        CMD_BACK,
        CMD_FORWARD,
        CMD_UP,
        CMD_SELECTALL,
        CMD_TOGGLEHIDDEN,
        CMD_TOGGLESIDEBAR,
        CMD_SORT_NAME,
        CMD_SORT_DATE,
        CMD_SORT_TYPE,
        CMD_SORT_SIZE,
        CMD_HELP,
        CMD_QUIT
    };

    struct Entry {
        std::string name;
        fs::path path;
        bool isDir = false;
        bool isLink = false;
        uintmax_t size = 0;
        std::time_t mtime = 0;
        std::string type;
    };

    struct Rect {
        int y = 0, x = 0, h = 0, w = 0;
        bool contains(int yy, int xx) const {
            return yy >= y && yy < y + h && xx >= x && xx < x + w;
        }
    };

    struct Btn {
        Rect r;
        int id = 0;
    };

    struct TabState {
        fs::path current;
        std::vector<fs::path> history;
        int histPos = 0;
        std::set<std::string> selected;
        int cursor = 0;
        int scroll = 0;
        int anchor = 0;
        SortKey sortKey = SortKey::Name;
        bool sortAsc = true;
    };

    struct MenuItem {
        Cmd cmd = CMD_NONE;
        std::string label;
        bool enabled = true;
        bool sep = false;
    };

    struct Quick {
        std::string icon;
        std::string label;
        fs::path path;
    };

    fs::path startPath;
    fs::path current;
    std::vector<Entry> entries;
    std::vector<fs::path> history;
    int histPos = 0;
    std::set<std::string> selected;
    int cursor = 0;
    int scroll = 0;
    int anchor = 0;
    SortKey sortKey = SortKey::Name;
    bool sortAsc = true;
    bool showHidden = false;
    bool showSidebar = true;
    bool shouldQuit = false;
    std::vector<fs::path> clipboard;
    bool clipboardCut = false;
    std::string statusMessage;
    uintmax_t freeBytes = 0;
    bool freeOk = false;
    int attrSel = A_REVERSE;

    std::vector<Quick> quickItems;

    std::string searchBuf;
    std::chrono::steady_clock::time_point searchTime;
    bool searchActive = false;
    std::chrono::steady_clock::time_point lastClickTime;
    int lastClickX = -1, lastClickY = -1;
    std::chrono::steady_clock::time_point lastRightTime;

    int toolbarY = 0, addrY = 1, contentTop = 2, contentBottom = 0;
    int statusY = 0, hintsY = 0;
    int sidebarW = 0;
    int listX = 0, listW = 0;
    int headerY = 0, rowsTop = 0, rowsBottom = 0, visibleRows = 0;
    int colNameX = 0, colNameW = 0;
    int colDateX = 0, colDateW = 0;
    int colTypeX = 0, colTypeW = 0;
    int colSizeX = 0, colSizeW = 0;
    Rect headerNameR, headerDateR, headerTypeR, headerSizeR;
    std::vector<Btn> toolbarButtons;
    std::vector<TabState> tabs;
    int activeTab = 0;
    std::vector<Btn> tabButtons;
    Rect newTabRect, closeAppRect;
    std::vector<Rect> crumbRects;
    std::vector<fs::path> crumbPaths;
    std::vector<Rect> sidebarRects;
    std::vector<int> sidebarIndex;

    void initState(const fs::path& start);
    TabState snapshotTab() const;
    void saveActiveTab();
    void loadTab(int idx);
    void switchTab(int idx);
    void newTab();
    void closeTab(int idx);
    void computeLayout();
    void draw();
    void drawTabs();
    void drawAddress();
    void drawSidebar();
    void drawList();
    void drawStatus();
    void drawHints();

    void refresh();
    void sortEntries();
    void applySort(SortKey k);
    void ensureVisible();
    void setStatus(const std::string& msg) { statusMessage = msg; }

    void navigate(const fs::path& p, bool addHistory = true);
    void goBack();
    void goForward();
    void goUp();

    void selectOnly(int idx);
    void expandTo(int idx);
    void toggleIdx(int idx);
    void selectAll();
    void moveCursor(int delta);
    std::string currentName() const;

    void openCurrent();
    void openExternal(const fs::path& p);

    void copySel(bool cut);
    void paste();
    void deleteSelection();
    void newFolder();
    void renameCurrent();
    void showProperties();
    void showHelp();

    std::vector<fs::path> selectionPaths() const;
    std::string uniqueName(const fs::path& dir, const std::string& base) const;
    std::string uniqueCopyName(const fs::path& dir, const std::string& filename) const;

    void handleMouse();
    void doToolbar(int id);
    void executeCmd(Cmd cmd);
    void showActionMenu(int px = -1, int py = -1);
    bool processKey(int k);
    bool processChar(wint_t c);
    void typeSearch(wint_t c);

    int showDialog(const std::string& title, const std::vector<std::string>& lines,
                   const std::vector<std::string>& buttons, int defaultIdx);
    bool confirmDialog(const std::string& title, const std::string& message);
    void infoDialog(const std::string& title, const std::vector<std::string>& lines);
    bool inputDialog(const std::string& title, const std::string& label, std::string& value);
    Cmd popupMenu(const std::vector<MenuItem>& items, int px, int py);
};

void App::run() {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_BLACK, COLOR_CYAN);
        attrSel = COLOR_PAIR(1);
    } else {
        attrSel = A_REVERSE;
    }
    mousemask(ALL_MOUSE_EVENTS, nullptr);
    mouseinterval(0);

    initState(startPath);
    tabs.push_back(snapshotTab());
    activeTab = 0;
    draw();

    bool running = true;
    while (running) {
        wint_t wc = 0;
        int r = get_wch(&wc);
        if (r == ERR)
            continue;
        bool quit = false;
        if (r == KEY_CODE_YES)
            quit = processKey((int)wc);
        else
            quit = processChar(wc);
        if (quit || shouldQuit)
            running = false;
        else
            draw();
    }
    endwin();
}

void App::initState(const fs::path& start) {
    std::error_code ec;
    fs::path p = fs::absolute(start, ec);
    if (ec) {
        ec.clear();
        p = start;
    }
    fs::path c = fs::weakly_canonical(p, ec);
    if (!ec)
        p = c;
    if (!fs::is_directory(p, ec)) {
        ec.clear();
        p = fs::current_path(ec);
        if (ec)
            p = fs::path("/");
    }
    current = p;
    history.clear();
    history.push_back(current);
    histPos = 0;
    cursor = 0;
    scroll = 0;
    anchor = 0;
    selected.clear();

    quickItems.clear();
    const char* home = std::getenv("HOME");
    if (home && *home) {
        fs::path h(home);
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
            fs::path q = d.sub ? h / d.sub : h;
            std::error_code e2;
            if (fs::is_directory(q, e2))
                quickItems.push_back({d.icon, d.label, normPath(q)});
            e2.clear();
        }
    }
    quickItems.push_back({"/", "根目录", fs::path("/")});

    refresh();
}

void App::computeLayout() {
    int W = COLS;
    int H = LINES;
    toolbarY = 0;
    addrY = 1;
    contentTop = 2;
    statusY = H - 2;
    hintsY = H - 1;
    contentBottom = H - 3;

    sidebarW = (showSidebar && W >= 72) ? 26 : 0;
    listX = sidebarW ? sidebarW + 1 : 0;
    listW = W - listX;

    headerY = contentTop;
    rowsTop = contentTop + 1;
    rowsBottom = contentBottom;
    visibleRows = std::max(0, rowsBottom - rowsTop + 1);

    int wSize = listW >= 34 ? 10 : 0;
    int wDate = listW >= 50 ? 17 : 0;
    int wType = listW >= 64 ? 22 : 0;
    while (wSize + wDate + wType > listW - 12) {
        if (wType)
            wType = 0;
        else if (wDate)
            wDate = 0;
        else if (wSize)
            wSize = 0;
        else
            break;
    }
    int nameW = listW - wSize - wDate - wType;
    if (nameW < 0)
        nameW = 0;
    colNameX = listX;
    colNameW = nameW;
    colDateX = colNameX + nameW;
    colDateW = wDate;
    colTypeX = colDateX + wDate;
    colTypeW = wType;
    colSizeX = colTypeX + wType;
    colSizeW = wSize;

    headerNameR = Rect{headerY, colNameX, 1, colNameW};
    headerDateR = Rect{headerY, colDateX, 1, colDateW};
    headerTypeR = Rect{headerY, colTypeX, 1, colTypeW};
    headerSizeR = Rect{headerY, colSizeX, 1, colSizeW};
}

void App::draw() {
    computeLayout();
    erase();

    if (COLS < 40 || LINES < 10) {
        std::string m = "终端窗口太小，请调整大小";
        wput(stdscr, LINES / 2, 0, COLS, centerCols(m, COLS), A_BOLD);
        std::string h = "F10 退出";
        wput(stdscr, LINES - 1, 0, COLS, centerCols(h, COLS), A_DIM);
        wrefresh(stdscr);
        return;
    }

    drawTabs();
    drawAddress();
    drawSidebar();
    drawList();
    drawStatus();
    drawHints();

    wrefresh(stdscr);
}

App::TabState App::snapshotTab() const {
    TabState t;
    t.current = current;
    t.history = history;
    t.histPos = histPos;
    t.selected = selected;
    t.cursor = cursor;
    t.scroll = scroll;
    t.anchor = anchor;
    t.sortKey = sortKey;
    t.sortAsc = sortAsc;
    return t;
}

void App::saveActiveTab() {
    if (activeTab >= 0 && activeTab < (int)tabs.size())
        tabs[activeTab] = snapshotTab();
}

void App::loadTab(int idx) {
    if (idx < 0 || idx >= (int)tabs.size())
        return;
    activeTab = idx;
    const TabState& t = tabs[idx];
    current = t.current;
    history = t.history;
    histPos = t.histPos;
    selected = t.selected;
    cursor = t.cursor;
    scroll = t.scroll;
    anchor = t.anchor;
    sortKey = t.sortKey;
    sortAsc = t.sortAsc;
    entries.clear();
    searchActive = false;
    statusMessage.clear();
    refresh();
}

void App::switchTab(int idx) {
    if (idx < 0 || idx >= (int)tabs.size() || idx == activeTab)
        return;
    saveActiveTab();
    loadTab(idx);
}

void App::newTab() {
    saveActiveTab();
    TabState t;
    t.current = current;
    t.history.push_back(current);
    t.sortKey = sortKey;
    t.sortAsc = sortAsc;
    tabs.push_back(t);
    loadTab((int)tabs.size() - 1);
    setStatus("新建标签页");
}

void App::closeTab(int idx) {
    if (idx < 0 || idx >= (int)tabs.size())
        return;
    saveActiveTab();
    bool wasActive = (idx == activeTab);
    tabs.erase(tabs.begin() + idx);
    if (tabs.empty()) {
        shouldQuit = true;
        return;
    }
    if (idx < activeTab)
        --activeTab;
    else if (idx == activeTab)
        activeTab = std::min(idx, (int)tabs.size() - 1);
    if (wasActive || activeTab >= (int)tabs.size())
        loadTab(activeTab);
    setStatus("已关闭标签页");
}

void App::drawTabs() {
    tabButtons.clear();
    newTabRect = Rect{};
    closeAppRect = Rect{toolbarY, COLS - 3, 1, 3};
    wput(stdscr, toolbarY, COLS - 3, 3, " × ", A_REVERSE | A_BOLD);

    int limit = std::max(0, COLS - 4);
    std::vector<std::string> labels;
    std::vector<int> widths;
    for (size_t i = 0; i < tabs.size(); ++i) {
        fs::path p = (i == (size_t)activeTab) ? current : tabs[i].current;
        std::string name = p.filename().string();
        if (name.empty())
            name = "/";
        name = utf8Clip(sanitize(name), 20);
        std::string lab = " " + name + " ";
        labels.push_back(lab);
        widths.push_back(utf8Cols(lab));
    }

    int first = activeTab;
    if (first < 0)
        first = 0;
    if (first >= (int)tabs.size())
        first = (int)tabs.size() - 1;
    int used = widths[first];
    while (first > 0) {
        int add = widths[first - 1] + 1;
        if (used + add > limit)
            break;
        used += add;
        --first;
    }

    int x = 0;
    for (int i = first; i < (int)tabs.size(); ++i) {
        if (x + widths[i] > limit)
            break;
        int attr = (i == activeTab) ? (attrSel | A_BOLD) : A_DIM;
        wput(stdscr, toolbarY, x, widths[i], labels[i], attr);
        tabButtons.push_back(Btn{Rect{toolbarY, x, 1, widths[i]}, i});
        x += widths[i];
        if (i + 1 < (int)tabs.size() && x < limit) {
            wput(stdscr, toolbarY, x, 1, "│", A_DIM);
            ++x;
        }
    }
    if (x + 3 <= limit) {
        wput(stdscr, toolbarY, x, 3, " + ", A_BOLD);
        newTabRect = Rect{toolbarY, x, 1, 3};
    }
}

void App::drawAddress() {
    wput(stdscr, addrY, 0, COLS, "", A_REVERSE);

    toolbarButtons.clear();
    struct Def {
        int id;
        const char* label;
        bool enabled;
    };
    Def defs[] = {
        {1, "‹", histPos > 0},
        {2, "›", histPos + 1 < (int)history.size()},
        {3, "↑", current != current.root_path()},
    };
    int x = 1;
    for (const auto& d : defs) {
        std::string label = std::string(" ") + d.label + " ";
        int w = utf8Cols(label);
        if (x + w >= COLS - 1)
            break;
        int attr = d.enabled ? (attrSel | A_BOLD) : (A_REVERSE | A_DIM);
        wput(stdscr, addrY, x, w, label, attr);
        toolbarButtons.push_back(Btn{Rect{addrY, x, 1, w}, d.id});
        x += w + 1;
    }
    int startX = x;

    std::vector<fs::path> chain;
    fs::path p = current;
    while (true) {
        chain.push_back(p);
        fs::path par = p.parent_path();
        if (par == p || par.empty())
            break;
        p = par;
    }
    std::reverse(chain.begin(), chain.end());

    std::vector<std::string> labels;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i == 0) {
            labels.push_back("/");
        } else {
            std::string nm = chain[i].filename().string();
            labels.push_back(nm.empty() ? sanitize(chain[i].string()) : sanitize(nm));
        }
    }

    const int sepW = utf8Cols(" › ");
    const int ellW = utf8Cols("… › ");
    int avail = COLS - startX - 1;

    int totalW = 0;
    for (size_t i = 0; i < labels.size(); ++i)
        totalW += utf8Cols(labels[i]) + (i ? sepW : 0);

    std::vector<int> idx;
    bool ell = false;
    if (totalW <= avail) {
        for (size_t i = 0; i < labels.size(); ++i)
            idx.push_back((int)i);
    } else {
        int budget = avail - ellW;
        int used = 0;
        for (int i = (int)labels.size() - 1; i >= 0; --i) {
            int lw = utf8Cols(labels[i]);
            int add = lw + (idx.empty() ? 0 : sepW);
            if (used + add > budget)
                break;
            used += add;
            idx.push_back(i);
        }
        std::reverse(idx.begin(), idx.end());
        ell = idx.size() < labels.size();
    }

    crumbRects.clear();
    crumbPaths.clear();
    x = startX;
    if (ell) {
        wput(stdscr, addrY, x, ellW, "… › ", A_REVERSE | A_DIM);
        x += ellW;
    }
    bool first = true;
    for (int i : idx) {
        if (!first) {
            wput(stdscr, addrY, x, sepW, " › ", A_REVERSE | A_DIM);
            x += sepW;
        }
        std::string lab = labels[i];
        int lw = utf8Cols(lab);
        if (x + lw > COLS - 1)
            break;
        wput(stdscr, addrY, x, lw, lab, A_REVERSE | A_BOLD);
        crumbRects.push_back(Rect{addrY, x, 1, lw});
        crumbPaths.push_back(chain[i]);
        x += lw;
        first = false;
    }
}

void App::drawSidebar() {
    sidebarRects.clear();
    sidebarIndex.clear();
    if (sidebarW <= 0)
        return;

    int y = contentTop;
    wput(stdscr, y, 0, sidebarW, "快速访问", A_BOLD);
    y++;

    for (size_t i = 0; i < quickItems.size(); ++i) {
        if (y > contentBottom)
            break;
        const Quick& q = quickItems[i];
        bool active = samePath(current, q.path);
        int attr = active ? attrSel : 0;
        std::string text = "  " + q.icon + "  " + q.label;
        wput(stdscr, y, 0, sidebarW, text, attr);
        sidebarRects.push_back(Rect{y, 0, 1, sidebarW});
        sidebarIndex.push_back((int)i);
        y++;
    }

    for (int yy = contentTop; yy <= contentBottom; ++yy)
        mvaddch(yy, sidebarW, ACS_VLINE | A_DIM);
}

void App::drawList() {
    wput(stdscr, headerY, listX, listW, "", A_REVERSE);
    auto mark = [&](SortKey k) -> std::string {
        if (sortKey != k)
            return "";
        return sortAsc ? " ↑" : " ↓";
    };
    if (colNameW > 0)
        wput(stdscr, headerY, colNameX, colNameW, "名称" + mark(SortKey::Name),
             A_REVERSE | A_BOLD);
    if (colDateW > 0)
        wput(stdscr, headerY, colDateX, colDateW, "修改日期" + mark(SortKey::Date),
             A_REVERSE | A_BOLD);
    if (colTypeW > 0)
        wput(stdscr, headerY, colTypeX, colTypeW, "类型" + mark(SortKey::Type),
             A_REVERSE | A_BOLD);
    if (colSizeW > 0)
        wput(stdscr, headerY, colSizeX, colSizeW, fitRight("大小" + mark(SortKey::Size), colSizeW - 1),
             A_REVERSE | A_BOLD);

    if (visibleRows <= 0)
        return;

    if (entries.empty()) {
        std::string msg = "此文件夹为空";
        wput(stdscr, rowsTop, listX, listW, centerCols(msg, listW), A_DIM);
        return;
    }

    for (int r = 0; r < visibleRows; ++r) {
        int i = scroll + r;
        int y = rowsTop + r;
        if (i >= (int)entries.size())
            break;
        const Entry& e = entries[i];
        bool sel = selected.count(e.name) > 0;
        bool cur = (i == cursor);
        int base = sel ? attrSel : 0;
        int nameAttr = base;
        if (!sel) {
            if (cur)
                nameAttr |= A_UNDERLINE;
            if (e.isDir)
                nameAttr |= A_BOLD;
        }
        int metaAttr = sel ? base : A_DIM;

        std::string icon = e.isLink ? ICON_LINK : (e.isDir ? ICON_DIR : ICON_FILE);
        std::string nameText = icon + " " + sanitize(e.name);
        wput(stdscr, y, colNameX, colNameW, nameText, nameAttr);
        if (colDateW > 0)
            wput(stdscr, y, colDateX, colDateW, formatTime(e.mtime), metaAttr);
        if (colTypeW > 0)
            wput(stdscr, y, colTypeX, colTypeW, e.type, metaAttr);
        if (colSizeW > 0) {
            std::string sizeText = e.isDir ? "<DIR>" : humanSize(e.size);
            wput(stdscr, y, colSizeX, colSizeW, fitRight(sizeText, colSizeW - 1), metaAttr);
        }
    }
}

void App::drawStatus() {
    wput(stdscr, statusY, 0, COLS, "", A_REVERSE);

    std::string left;
    if (!statusMessage.empty()) {
        left = sanitize(statusMessage);
    } else {
        left = "共 " + std::to_string(entries.size()) + " 个项目";
        int sc = 0;
        uintmax_t sb = 0;
        for (const auto& e : entries) {
            if (selected.count(e.name)) {
                sc++;
                if (!e.isDir)
                    sb += e.size;
            }
        }
        if (sc > 0) {
            left += "，已选择 " + std::to_string(sc) + " 个项目";
            if (sb > 0)
                left += "（" + humanSize(sb) + "）";
        }
        if (!clipboard.empty())
            left += "    剪贴板: " + std::to_string(clipboard.size()) + " 个项目" +
                    (clipboardCut ? "（剪切）" : "（复制）");
    }

    std::string right = "可用空间 " + (freeOk ? humanSize(freeBytes) : std::string("未知"));
    int rw = utf8Cols(right);
    int rx = COLS - rw - 1;
    if (rx > 1)
        wput(stdscr, statusY, rx, rw, right, A_REVERSE | A_DIM);

    int lw = (rx > 1 ? rx - 2 : COLS - 2);
    if (lw > 0)
        wput(stdscr, statusY, 1, lw, left, A_REVERSE);
}

void App::drawHints() {
    std::string h =
        "a 动作  Ctrl+T 新标签  Ctrl+W 关闭标签  Ctrl+Tab 切换  F1 帮助  "
        "F2 重命名  F5 刷新  F7 新建  F9 隐藏  Del 删除  F10 退出";
    wput(stdscr, hintsY, 1, COLS - 1, h, A_DIM);
}

void App::sortEntries() {
    std::sort(entries.begin(), entries.end(), [this](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        int cmp = 0;
        switch (sortKey) {
        case SortKey::Name:
            cmp = strcasecmp(a.name.c_str(), b.name.c_str());
            break;
        case SortKey::Date:
            cmp = (a.mtime < b.mtime) ? -1 : (a.mtime > b.mtime ? 1 : 0);
            break;
        case SortKey::Type:
            cmp = strcasecmp(a.type.c_str(), b.type.c_str());
            break;
        case SortKey::Size:
            cmp = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
            break;
        }
        if (!sortAsc)
            cmp = -cmp;
        if (cmp == 0)
            cmp = strcasecmp(a.name.c_str(), b.name.c_str());
        return cmp < 0;
    });
}

void App::applySort(SortKey k) {
    if (sortKey == k)
        sortAsc = !sortAsc;
    else {
        sortKey = k;
        sortAsc = true;
    }
    std::string keep = currentName();
    std::string keepAnchor;
    if (anchor >= 0 && anchor < (int)entries.size())
        keepAnchor = entries[anchor].name;
    sortEntries();
    if (!keep.empty()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == keep) {
                cursor = (int)i;
                break;
            }
        }
    }
    if (!keepAnchor.empty()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name == keepAnchor) {
                anchor = (int)i;
                break;
            }
        }
    }
    ensureVisible();
}

void App::refresh() {
    std::string keep;
    if (cursor >= 0 && cursor < (int)entries.size())
        keep = entries[cursor].name;
    std::string keepAnchor;
    if (anchor >= 0 && anchor < (int)entries.size())
        keepAnchor = entries[anchor].name;
    int oldCursor = cursor;

    entries.clear();
    std::error_code ec;
    fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
        fs::path p = it->path();
        std::string nm = p.filename().string();
        if (!nm.empty() && (showHidden || nm[0] != '.')) {
            Entry e;
            e.name = nm;
            e.path = p;
            std::error_code e2;
            e.isDir = it->is_directory(e2);
            e2.clear();
            e.isLink = it->is_symlink(e2);
            struct stat st;
            if (::stat(p.c_str(), &st) == 0) {
                e.size = (uintmax_t)st.st_size;
                e.mtime = st.st_mtime;
            }
            e.type = typeFor(p, e.isDir, e.isLink);
            entries.push_back(std::move(e));
        }
        it.increment(ec);
    }
    if (ec)
        statusMessage = "读取目录失败: " + sanitize(ec.message());

    sortEntries();

    std::set<std::string> names;
    for (const auto& e : entries)
        names.insert(e.name);
    for (auto sit = selected.begin(); sit != selected.end();) {
        if (!names.count(*sit))
            sit = selected.erase(sit);
        else
            ++sit;
    }

    if (entries.empty()) {
        cursor = 0;
        scroll = 0;
        anchor = 0;
        selected.clear();
    } else {
        int idx = -1;
        if (!keep.empty()) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].name == keep) {
                    idx = (int)i;
                    break;
                }
            }
        }
        if (idx >= 0)
            cursor = idx;
        else
            cursor = std::clamp(oldCursor, 0, (int)entries.size() - 1);
        bool anchorFound = false;
        if (!keepAnchor.empty()) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].name == keepAnchor) {
                    anchor = (int)i;
                    anchorFound = true;
                    break;
                }
            }
        }
        if (!anchorFound && (anchor < 0 || anchor >= (int)entries.size()))
            anchor = cursor;
        if (selected.empty())
            selected.insert(entries[cursor].name);
    }

    std::error_code se;
    fs::space_info sp = fs::space(current, se);
    freeOk = !se;
    freeBytes = se ? 0 : sp.available;

    ensureVisible();
}

void App::ensureVisible() {
    if (entries.empty() || visibleRows <= 0) {
        if (entries.empty())
            scroll = 0;
        return;
    }
    if (cursor < scroll)
        scroll = cursor;
    if (cursor >= scroll + visibleRows)
        scroll = cursor - visibleRows + 1;
    int maxScroll = std::max(0, (int)entries.size() - visibleRows);
    scroll = std::clamp(scroll, 0, maxScroll);
}

void App::navigate(const fs::path& p, bool addHistory) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        statusMessage = "无法打开: " + sanitize(p.string());
        return;
    }
    fs::path abs = fs::absolute(p, ec);
    if (ec) {
        ec.clear();
        abs = p;
    }
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (!ec)
        abs = canon;

    if (addHistory && !history.empty() && abs == history[histPos])
        return;

    current = abs;
    if (addHistory) {
        history.resize((size_t)histPos + 1);
        history.push_back(current);
        histPos = (int)history.size() - 1;
    }
    selected.clear();
    cursor = 0;
    scroll = 0;
    anchor = 0;
    searchActive = false;
    statusMessage.clear();
    refresh();
}

void App::goBack() {
    if (histPos > 0) {
        histPos--;
        current = history[histPos];
        selected.clear();
        cursor = 0;
        scroll = 0;
        anchor = 0;
        searchActive = false;
        statusMessage.clear();
        refresh();
    }
}

void App::goForward() {
    if (histPos + 1 < (int)history.size()) {
        histPos++;
        current = history[histPos];
        selected.clear();
        cursor = 0;
        scroll = 0;
        anchor = 0;
        searchActive = false;
        statusMessage.clear();
        refresh();
    }
}

void App::goUp() {
    fs::path par = current.parent_path();
    if (par.empty() || par == current)
        return;
    navigate(par);
}

void App::selectOnly(int idx) {
    if (entries.empty())
        return;
    idx = std::clamp(idx, 0, (int)entries.size() - 1);
    cursor = idx;
    anchor = idx;
    selected.clear();
    selected.insert(entries[idx].name);
    ensureVisible();
}

void App::expandTo(int idx) {
    if (entries.empty())
        return;
    idx = std::clamp(idx, 0, (int)entries.size() - 1);
    if (anchor < 0 || anchor >= (int)entries.size())
        anchor = cursor;
    int a = std::min(anchor, idx);
    int b = std::max(anchor, idx);
    selected.clear();
    for (int i = a; i <= b; ++i)
        selected.insert(entries[i].name);
    cursor = idx;
    ensureVisible();
}

void App::toggleIdx(int idx) {
    if (entries.empty())
        return;
    idx = std::clamp(idx, 0, (int)entries.size() - 1);
    cursor = idx;
    anchor = idx;
    const std::string& nm = entries[idx].name;
    if (selected.count(nm))
        selected.erase(nm);
    else
        selected.insert(nm);
    ensureVisible();
}

void App::selectAll() {
    if (entries.empty())
        return;
    selected.clear();
    for (const auto& e : entries)
        selected.insert(e.name);
    setStatus("已全选 " + std::to_string(entries.size()) + " 个项目");
}

void App::moveCursor(int delta) {
    if (entries.empty())
        return;
    int ni = std::clamp(cursor + delta, 0, (int)entries.size() - 1);
    selectOnly(ni);
}

std::string App::currentName() const {
    if (cursor >= 0 && cursor < (int)entries.size())
        return entries[cursor].name;
    return "";
}

std::vector<fs::path> App::selectionPaths() const {
    std::vector<fs::path> out;
    for (const auto& e : entries) {
        if (selected.count(e.name))
            out.push_back(e.path);
    }
    if (out.empty() && cursor >= 0 && cursor < (int)entries.size())
        out.push_back(entries[cursor].path);
    return out;
}

void App::openCurrent() {
    if (entries.empty() || cursor < 0 || cursor >= (int)entries.size())
        return;
    Entry e = entries[cursor];
    if (e.isDir)
        navigate(e.path);
    else
        openExternal(e.path);
}

void App::openExternal(const fs::path& p) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t inner = fork();
        if (inner == 0) {
            setsid();
            int fd = ::open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2)
                    close(fd);
            }
            execlp("xdg-open", "xdg-open", p.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
    }
}

void App::copySel(bool cut) {
    std::vector<fs::path> items = selectionPaths();
    if (items.empty()) {
        setStatus("没有选中的项目");
        return;
    }
    clipboard = items;
    clipboardCut = cut;
    setStatus(std::string(cut ? "已剪切 " : "已复制 ") + std::to_string(items.size()) + " 个项目");
}

std::string App::uniqueName(const fs::path& dir, const std::string& base) const {
    std::error_code ec;
    if (!fs::exists(dir / base, ec))
        return base;
    for (int i = 2; i < 10000; ++i) {
        std::string n = base + " (" + std::to_string(i) + ")";
        if (!fs::exists(dir / n, ec))
            return n;
    }
    return base;
}

std::string App::uniqueCopyName(const fs::path& dir, const std::string& filename) const {
    fs::path fp(filename);
    std::string stem = fp.stem().string();
    std::string ext = fp.extension().string();
    std::string base = stem + " - 副本" + ext;
    std::error_code ec;
    if (!fs::exists(dir / base, ec))
        return base;
    for (int i = 2; i < 10000; ++i) {
        std::string n = stem + " - 副本 (" + std::to_string(i) + ")" + ext;
        if (!fs::exists(dir / n, ec))
            return n;
    }
    return base;
}

void App::paste() {
    if (clipboard.empty()) {
        setStatus("剪贴板为空");
        return;
    }
    fs::path target = current;
    int done = 0, skipped = 0, failed = 0;

    for (const fs::path& src : clipboard) {
        std::error_code ec;
        if (!fs::exists(src, ec)) {
            failed++;
            continue;
        }
        fs::path dst = target / src.filename();

        if (clipboardCut && samePath(src.parent_path(), target)) {
            skipped++;
            continue;
        }
        if (fs::is_directory(src, ec) && isSubPath(target, src)) {
            failed++;
            continue;
        }
        if (samePath(src, dst)) {
            dst = target / uniqueCopyName(target, src.filename().string());
        } else if (fs::exists(dst, ec)) {
            bool ow = confirmDialog("确认替换", "目标位置已存在同名项目：\n" +
                                                      sanitize(src.filename().string()) +
                                                      "\n\n是否替换？");
            if (!ow) {
                skipped++;
                continue;
            }
            fs::remove_all(dst, ec);
            if (ec) {
                failed++;
                continue;
            }
        }

        ec.clear();
        if (clipboardCut) {
            fs::rename(src, dst, ec);
            if (ec) {
                ec.clear();
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
                if (!ec) {
                    std::error_code e2;
                    fs::remove_all(src, e2);
                    if (e2)
                        ec = e2;
                }
            }
        } else {
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        }
        if (ec)
            failed++;
        else
            done++;
    }

    if (clipboardCut)
        clipboard.clear();

    refresh();
    if (failed > 0) {
        infoDialog("粘贴",
                   {"操作已完成。",
                    "成功: " + std::to_string(done) + " 个",
                    "跳过: " + std::to_string(skipped) + " 个",
                    "失败: " + std::to_string(failed) + " 个"});
    } else {
        std::string msg = "粘贴完成：" + std::to_string(done) + " 个项目";
        if (skipped > 0)
            msg += "，跳过 " + std::to_string(skipped) + " 个";
        setStatus(msg);
    }
}

void App::deleteSelection() {
    std::vector<fs::path> items = selectionPaths();
    if (items.empty()) {
        setStatus("没有可删除的项目");
        return;
    }
    std::string msg = "确定要永久删除选中的 " + std::to_string(items.size()) +
                      " 个项目吗？\n此操作无法撤销。";
    if (!confirmDialog("删除", msg))
        return;

    int ok = 0, fail = 0;
    for (const fs::path& p : items) {
        std::error_code ec;
        uintmax_t n = fs::remove_all(p, ec);
        if (ec || n == static_cast<uintmax_t>(-1))
            fail++;
        else
            ok++;
    }
    refresh();
    if (fail > 0)
        infoDialog("删除", {"已删除 " + std::to_string(ok) + " 个项目，", std::to_string(fail) + " 个删除失败。"});
    else
        setStatus("已删除 " + std::to_string(ok) + " 个项目");
}

void App::newFolder() {
    std::string name = uniqueName(current, "新建文件夹");
    if (!inputDialog("新建文件夹", "文件夹名称:", name))
        return;
    if (name.empty() || name.find('/') != std::string::npos) {
        infoDialog("新建文件夹", {"名称无效。"});
        return;
    }
    fs::path p = current / name;
    std::error_code ec;
    if (fs::exists(p, ec)) {
        infoDialog("新建文件夹", {"该名称已存在。"});
        return;
    }
    if (!fs::create_directory(p, ec)) {
        infoDialog("新建文件夹", {"创建失败: " + sanitize(ec.message())});
        return;
    }
    refresh();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            selectOnly((int)i);
            break;
        }
    }
    setStatus("已创建文件夹: " + sanitize(name));
}

void App::renameCurrent() {
    if (entries.empty() || cursor < 0 || cursor >= (int)entries.size())
        return;
    Entry e = entries[cursor];
    std::string name = e.name;
    if (!inputDialog("重命名", "新名称:", name))
        return;
    if (name.empty() || name.find('/') != std::string::npos) {
        infoDialog("重命名", {"名称无效。"});
        return;
    }
    if (name == e.name)
        return;
    fs::path dst = e.path.parent_path() / name;
    std::error_code ec;
    if (fs::exists(dst, ec)) {
        infoDialog("重命名", {"该名称已存在。"});
        return;
    }
    fs::rename(e.path, dst, ec);
    if (ec) {
        infoDialog("重命名", {"重命名失败: " + sanitize(ec.message())});
        return;
    }
    refresh();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            selectOnly((int)i);
            break;
        }
    }
    setStatus("已重命名: " + sanitize(name));
}

void App::showProperties() {
    if (entries.empty() || cursor < 0 || cursor >= (int)entries.size())
        return;
    const Entry& e = entries[cursor];
    std::vector<std::string> lines;
    lines.push_back("名称: " + sanitize(e.name));
    lines.push_back("位置: " + sanitize(current.string()));
    lines.push_back("类型: " + e.type);
    lines.push_back("大小: " + (e.isDir ? std::string("—") : humanSize(e.size)));
    lines.push_back("修改时间: " + formatTime(e.mtime));
    lines.push_back("完整路径: " + sanitize(e.path.string()));
    infoDialog("属性", lines);
}

void App::showHelp() {
    infoDialog("帮助", {
                           "动作菜单：选中文件后按 a 打开（右键也可），包含全部操作。",
                           "标签页：Ctrl+T 新建，Ctrl+W 关闭，Ctrl+Tab 切换；点击标签切换，中键关闭。",
                           "地址栏左侧为后退/前进/上级；右上角 × 退出程序。",
                           "鼠标：单击选择，双击打开，滚轮滚动。",
                           "键盘：↑↓ 移动，Enter 打开，Backspace 后退。",
                           "F2 重命名，F4 属性，F5 刷新，F7 新建文件夹。",
                           "F9 显示隐藏项目，F10 退出。",
                           "Ctrl+C 复制，Ctrl+X 剪切，Ctrl+V 粘贴。",
                           "Ctrl+A 全选，Ctrl+U 上一级，Ctrl+B 显示/隐藏导航窗格。",
                           "Shift+单击 连续选择，Ctrl+单击 多选。",
                       });
}

void App::handleMouse() {
    computeLayout();
    if (COLS < 40 || LINES < 10)
        return;
    MEVENT ev;
    if (getmouse(&ev) != OK)
        return;
    int x = ev.x;
    int y = ev.y;

    if (ev.bstate & (BUTTON4_PRESSED | BUTTON4_CLICKED)) {
        if (y >= contentTop && y <= contentBottom) {
            scroll -= 3;
            int maxScroll = std::max(0, (int)entries.size() - visibleRows);
            scroll = std::clamp(scroll, 0, maxScroll);
        }
        return;
    }
    if (ev.bstate & (BUTTON5_PRESSED | BUTTON5_CLICKED)) {
        if (y >= contentTop && y <= contentBottom) {
            scroll += 3;
            int maxScroll = std::max(0, (int)entries.size() - visibleRows);
            scroll = std::clamp(scroll, 0, maxScroll);
        }
        return;
    }

    bool middle = (ev.bstate & (BUTTON2_PRESSED | BUTTON2_CLICKED)) != 0;
    if (middle) {
        if (y == toolbarY) {
            for (const auto& b : tabButtons) {
                if (b.r.contains(y, x)) {
                    closeTab(b.id);
                    return;
                }
            }
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    bool leftPress = (ev.bstate & BUTTON1_PRESSED) != 0;
    bool leftClick = (ev.bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) != 0;
    bool rightPress = (ev.bstate & BUTTON3_PRESSED) != 0;
    bool rightClick = (ev.bstate & BUTTON3_CLICKED) != 0;

    bool left = leftPress;
    if (!left && leftClick && now - lastClickTime > std::chrono::milliseconds(500))
        left = true;
    bool right = rightPress;
    if (!right && rightClick && now - lastRightTime > std::chrono::milliseconds(500))
        right = true;

    bool dbl = false;
    if (left) {
        dbl = (now - lastClickTime <= std::chrono::milliseconds(400)) &&
              x == lastClickX && y == lastClickY;
        lastClickTime = now;
        lastClickX = x;
        lastClickY = y;
    }
    if (right)
        lastRightTime = now;
    if (!left && !right)
        return;

    if ((left || right) && closeAppRect.contains(y, x)) {
        shouldQuit = true;
        return;
    }
    if (left && y == toolbarY) {
        for (const auto& b : tabButtons) {
            if (b.r.contains(y, x)) {
                switchTab(b.id);
                return;
            }
        }
        if (newTabRect.contains(y, x)) {
            newTab();
            return;
        }
        return;
    }

    if (right) {
        if (y >= rowsTop && y <= rowsBottom) {
            int idx = scroll + (y - rowsTop);
            if (idx >= 0 && idx < (int)entries.size()) {
                cursor = idx;
                if (!selected.count(entries[idx].name)) {
                    selected.clear();
                    selected.insert(entries[idx].name);
                    anchor = idx;
                }
                ensureVisible();
            }
        }
        showActionMenu(x, y);
        return;
    }

    for (const auto& b : toolbarButtons) {
        if (b.r.contains(y, x)) {
            doToolbar(b.id);
            return;
        }
    }
    for (size_t i = 0; i < crumbRects.size(); ++i) {
        if (crumbRects[i].contains(y, x) && i < crumbPaths.size()) {
            navigate(crumbPaths[i]);
            return;
        }
    }
    for (size_t i = 0; i < sidebarRects.size(); ++i) {
        if (sidebarRects[i].contains(y, x) && i < sidebarIndex.size()) {
            navigate(quickItems[sidebarIndex[i]].path);
            return;
        }
    }
    if (headerNameR.contains(y, x)) {
        applySort(SortKey::Name);
        return;
    }
    if (headerDateR.contains(y, x)) {
        applySort(SortKey::Date);
        return;
    }
    if (headerTypeR.contains(y, x)) {
        applySort(SortKey::Type);
        return;
    }
    if (headerSizeR.contains(y, x)) {
        applySort(SortKey::Size);
        return;
    }

    if (y >= rowsTop && y <= rowsBottom) {
        int idx = scroll + (y - rowsTop);
        bool shift = (ev.bstate & BUTTON_SHIFT) != 0;
        bool ctrl = (ev.bstate & BUTTON_CTRL) != 0;
        if (idx >= 0 && idx < (int)entries.size()) {
            if (shift)
                expandTo(idx);
            else if (ctrl)
                toggleIdx(idx);
            else
                selectOnly(idx);
            if (dbl)
                openCurrent();
        } else {
            selected.clear();
        }
    }
}

void App::doToolbar(int id) {
    if (id == 1)
        goBack();
    else if (id == 2)
        goForward();
    else if (id == 3)
        goUp();
}

void App::executeCmd(Cmd cmd) {
    switch (cmd) {
    case CMD_OPEN:
        openCurrent();
        break;
    case CMD_NEWFOLDER:
        newFolder();
        break;
    case CMD_RENAME:
        renameCurrent();
        break;
    case CMD_DELETE:
        deleteSelection();
        break;
    case CMD_COPY:
        copySel(false);
        break;
    case CMD_CUT:
        copySel(true);
        break;
    case CMD_PASTE:
        paste();
        break;
    case CMD_REFRESH:
        statusMessage.clear();
        refresh();
        break;
    case CMD_PROPERTIES:
        showProperties();
        break;
    case CMD_BACK:
        goBack();
        break;
    case CMD_FORWARD:
        goForward();
        break;
    case CMD_UP:
        goUp();
        break;
    case CMD_SELECTALL:
        selectAll();
        break;
    case CMD_TOGGLEHIDDEN:
        showHidden = !showHidden;
        refresh();
        setStatus(showHidden ? "显示隐藏项目" : "隐藏隐藏项目");
        break;
    case CMD_TOGGLESIDEBAR:
        showSidebar = !showSidebar;
        statusMessage.clear();
        break;
    case CMD_SORT_NAME:
        applySort(SortKey::Name);
        break;
    case CMD_SORT_DATE:
        applySort(SortKey::Date);
        break;
    case CMD_SORT_TYPE:
        applySort(SortKey::Type);
        break;
    case CMD_SORT_SIZE:
        applySort(SortKey::Size);
        break;
    case CMD_HELP:
        showHelp();
        break;
    case CMD_QUIT:
        shouldQuit = true;
        break;
    default:
        break;
    }
}

void App::showActionMenu(int px, int py) {
    computeLayout();
    bool has = !entries.empty();
    auto sortLabel = [&](const char* what, SortKey key) {
        std::string s = std::string("按") + what + "排序";
        if (sortKey == key)
            s += sortAsc ? " ↑" : " ↓";
        return s;
    };
    std::vector<MenuItem> items = {
        {CMD_OPEN, "打开", has, false},
        {CMD_NONE, "", false, true},
        {CMD_COPY, "复制", has, false},
        {CMD_CUT, "剪切", has, false},
        {CMD_PASTE, "粘贴", !clipboard.empty(), false},
        {CMD_RENAME, "重命名", has, false},
        {CMD_DELETE, "删除", has, false},
        {CMD_PROPERTIES, "属性", has, false},
        {CMD_NONE, "", false, true},
        {CMD_NEWFOLDER, "新建文件夹", true, false},
        {CMD_REFRESH, "刷新", true, false},
        {CMD_SELECTALL, "全选", has, false},
        {CMD_TOGGLEHIDDEN, showHidden ? "隐藏隐藏项目" : "显示隐藏项目", true, false},
        {CMD_TOGGLESIDEBAR, showSidebar ? "隐藏导航窗格" : "显示导航窗格", true, false},
        {CMD_NONE, "", false, true},
        {CMD_BACK, "后退", histPos > 0, false},
        {CMD_FORWARD, "前进", histPos + 1 < (int)history.size(), false},
        {CMD_UP, "上一级", current != current.root_path(), false},
        {CMD_NONE, "", false, true},
        {CMD_SORT_NAME, sortLabel("名称", SortKey::Name), true, false},
        {CMD_SORT_DATE, sortLabel("日期", SortKey::Date), true, false},
        {CMD_SORT_TYPE, sortLabel("类型", SortKey::Type), true, false},
        {CMD_SORT_SIZE, sortLabel("大小", SortKey::Size), true, false},
        {CMD_NONE, "", false, true},
        {CMD_HELP, "帮助", true, false},
        {CMD_QUIT, "退出", true, false},
    };
    if (px < 0)
        px = listX + 2;
    if (py < 0)
        py = contentTop;
    Cmd c = popupMenu(items, px, py);
    if (c != CMD_NONE)
        executeCmd(c);
}

bool App::processKey(int k) {
    switch (k) {
    case KEY_UP:
        moveCursor(-1);
        break;
    case KEY_DOWN:
        moveCursor(1);
        break;
    case KEY_PPAGE:
        moveCursor(-std::max(1, visibleRows - 1));
        break;
    case KEY_NPAGE:
        moveCursor(std::max(1, visibleRows - 1));
        break;
    case KEY_HOME:
        selectOnly(0);
        break;
    case KEY_END:
        selectOnly((int)entries.size() - 1);
        break;
    case KEY_ENTER:
        openCurrent();
        break;
    case KEY_BACKSPACE:
        goBack();
        break;
    case KEY_DC:
        deleteSelection();
        break;
    case KEY_F(1):
        showHelp();
        break;
    case KEY_F(2):
        renameCurrent();
        break;
    case KEY_F(4):
        showProperties();
        break;
    case KEY_F(5):
        statusMessage.clear();
        refresh();
        break;
    case KEY_F(7):
        newFolder();
        break;
    case KEY_F(9):
        showHidden = !showHidden;
        refresh();
        setStatus(showHidden ? "显示隐藏项目" : "隐藏隐藏项目");
        break;
    case KEY_F(10):
        return true;
    case KEY_BTAB:
        if (tabs.size() > 1)
            switchTab((activeTab + (int)tabs.size() - 1) % (int)tabs.size());
        break;
    case KEY_MOUSE:
        handleMouse();
        break;
    default:
        break;
    }
    return false;
}

bool App::processChar(wint_t c) {
    if (c == 3) {
        copySel(false);
    } else if (c == 24) {
        copySel(true);
    } else if (c == 22) {
        paste();
    } else if (c == 1) {
        selectAll();
    } else if (c == 21) {
        goUp();
    } else if (c == 2) {
        showSidebar = !showSidebar;
        statusMessage.clear();
    } else if (c == 12) {
        statusMessage.clear();
        refresh();
    } else if (c == 17) {
        return true;
    } else if (c == '\n' || c == '\r') {
        openCurrent();
    } else if (c == 127 || c == 8) {
        goBack();
    } else if (c == 27) {
        statusMessage.clear();
    } else if (c == 20) {
        newTab();
    } else if (c == 23) {
        closeTab(activeTab);
    } else if (c == '\t') {
        if (tabs.size() > 1)
            switchTab((activeTab + 1) % (int)tabs.size());
    } else if (c == 'a' || c == 'A') {
        auto now = std::chrono::steady_clock::now();
        bool typing = searchActive && now - searchTime <= std::chrono::milliseconds(1000);
        if (typing)
            typeSearch(c);
        else
            showActionMenu();
    } else if (c >= 32) {
        typeSearch(c);
    }
    return false;
}

void App::typeSearch(wint_t c) {
    if (entries.empty())
        return;
    std::string u = toUtf8((wchar_t)c);
    if (u.empty())
        return;
    auto now = std::chrono::steady_clock::now();
    if (!searchActive || now - searchTime > std::chrono::milliseconds(1000))
        searchBuf.clear();
    searchActive = true;
    searchTime = now;
    searchBuf += u;
    std::string key = asciiLower(searchBuf);
    for (size_t i = 0; i < entries.size(); ++i) {
        std::string nm = asciiLower(sanitize(entries[i].name));
        if (nm.rfind(key, 0) == 0) {
            selectOnly((int)i);
            return;
        }
    }
}

int App::showDialog(const std::string& title, const std::vector<std::string>& rawLines,
                    const std::vector<std::string>& buttons, int defaultIdx) {
    if (COLS < 34 || LINES < 8 || buttons.empty())
        return -1;

    int maxCols = 30;
    for (const auto& l : rawLines)
        maxCols = std::max(maxCols, utf8Cols(l));
    for (const auto& b : buttons)
        maxCols = std::max(maxCols, dlgButtonW(b) + 2);
    maxCols = std::max(maxCols, utf8Cols(title) + 4);

    int w = std::min(COLS - 4, std::max(42, maxCols + 6));
    std::vector<std::string> lines;
    for (const auto& l : rawLines) {
        std::vector<std::string> parts = wrapText(l, w - 4);
        for (const auto& q : parts)
            lines.push_back(q);
    }
    if (lines.empty())
        lines.push_back("");

    int maxH = LINES - 2;
    if ((int)lines.size() + 4 > maxH) {
        int keep = std::max(1, maxH - 4);
        lines.resize((size_t)keep);
    }
    int h = (int)lines.size() + 4;
    if (h < 6)
        h = 6;
    int y = (LINES - h) / 2;
    int x = (COLS - w) / 2;

    WINDOW* win = newwin(h, w, y, x);
    if (!win)
        return -1;
    keypad(win, TRUE);
    int focus = std::clamp(defaultIdx, 0, (int)buttons.size() - 1);
    int ret = -1;

    while (ret < 0) {
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD);
        mvwaddstr(win, 0, 2, (" " + title + " ").c_str());
        wattroff(win, A_BOLD);
        for (size_t i = 0; i < lines.size(); ++i)
            wput(win, 2 + (int)i, 2, w - 4, lines[i], 0);

        int total = 0;
        for (const auto& b : buttons)
            total += dlgButtonW(b) + 2;
        total -= 2;
        int bx = std::max(2, (w - total) / 2);
        int by = h - 2;
        std::vector<int> bxs, bws;
        for (size_t i = 0; i < buttons.size(); ++i) {
            int bw = dlgButtonW(buttons[i]);
            bxs.push_back(bx);
            bws.push_back(bw);
            drawDlgButton(win, by, bx, buttons[i], (int)i == focus);
            bx += bw + 2;
        }
        wrefresh(win);

        wint_t wc = 0;
        int r = wget_wch(win, &wc);
        if (r == ERR)
            break;
        if (r == KEY_CODE_YES) {
            if (wc == KEY_MOUSE) {
                MEVENT ev;
                if (getmouse(&ev) == OK &&
                    (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
                    int ly = ev.y - y;
                    int lx = ev.x - x;
                    for (size_t i = 0; i < buttons.size(); ++i) {
                        if (ly == by && lx >= bxs[i] && lx < bxs[i] + bws[i]) {
                            ret = (int)i;
                            break;
                        }
                    }
                }
            } else if (wc == KEY_LEFT) {
                focus = (focus + (int)buttons.size() - 1) % (int)buttons.size();
            } else if (wc == KEY_RIGHT) {
                focus = (focus + 1) % (int)buttons.size();
            } else if (wc == KEY_ENTER) {
                ret = focus;
            } else if (wc == KEY_RESIZE) {
                break;
            }
        } else {
            if (wc == 27) {
                break;
            } else if (wc == '\n' || wc == '\r') {
                ret = focus;
            } else if (wc == '\t') {
                focus = (focus + 1) % (int)buttons.size();
            } else if ((wc == 'y' || wc == 'Y') && buttons.size() == 2) {
                ret = 0;
            } else if ((wc == 'n' || wc == 'N') && buttons.size() == 2) {
                ret = 1;
            }
        }
    }

    delwin(win);
    return ret;
}

bool App::confirmDialog(const std::string& title, const std::string& message) {
    return showDialog(title, {message}, {"是", "否"}, 0) == 0;
}

void App::infoDialog(const std::string& title, const std::vector<std::string>& lines) {
    showDialog(title, lines, {"确定"}, 0);
}

bool App::inputDialog(const std::string& title, const std::string& label, std::string& value) {
    if (COLS < 30 || LINES < 10)
        return false;

    int w = std::min(COLS - 4, std::max(34, std::max(50, utf8Cols(label) + 10)));
    int h = 8;
    int y = (LINES - h) / 2;
    int x = (COLS - w) / 2;

    WINDOW* win = newwin(h, w, y, x);
    if (!win)
        return false;
    keypad(win, TRUE);

    std::string val = value;
    size_t pos = val.size();
    bool selAll = !val.empty();
    int fieldY = 3;
    int fieldX = 2;
    int fieldW = w - 4;
    int bw1 = dlgButtonW("确定");
    int bw2 = dlgButtonW("取消");
    int total = bw1 + bw2 + 2;
    int bx = std::max(2, (w - total) / 2);
    int okX = bx;
    int noX = bx + bw1 + 2;
    int btnY = 6;
    bool ok = false;
    bool cancel = false;

    while (!ok && !cancel) {
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD);
        mvwaddstr(win, 0, 2, (" " + title + " ").c_str());
        wattroff(win, A_BOLD);
        wput(win, 2, 2, w - 4, label, 0);
        wput(win, fieldY, fieldX, fieldW, "", 0);
        std::string shown = utf8Clip(val, fieldW);
        if (selAll && !val.empty()) {
            wattron(win, A_REVERSE);
            mvwaddstr(win, fieldY, fieldX, shown.c_str());
            wattroff(win, A_REVERSE);
        } else {
            mvwaddstr(win, fieldY, fieldX, shown.c_str());
            int cw = utf8Cols(val.substr(0, pos));
            if (cw >= fieldW)
                cw = fieldW - 1;
            if (cw >= 0)
                mvwaddch(win, fieldY, fieldX + cw, ' ' | A_REVERSE);
        }
        drawDlgButton(win, btnY, okX, "确定", true);
        drawDlgButton(win, btnY, noX, "取消", false);
        wrefresh(win);

        wint_t wc = 0;
        int r = wget_wch(win, &wc);
        if (r == ERR)
            break;
        if (r == KEY_CODE_YES) {
            if (wc == KEY_MOUSE) {
                MEVENT ev;
                if (getmouse(&ev) == OK &&
                    (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
                    int ly = ev.y - y;
                    int lx = ev.x - x;
                    if (ly == fieldY && lx >= fieldX && lx < fieldX + fieldW) {
                        selAll = false;
                        int target = lx - fieldX;
                        size_t p2 = 0;
                        int cw2 = 0;
                        while (p2 < val.size()) {
                            size_t np = nextUtf8Pos(val, p2);
                            int c = utf8Cols(val.substr(p2, np - p2));
                            if (cw2 + c > target)
                                break;
                            cw2 += c;
                            p2 = np;
                        }
                        pos = p2;
                    } else if (ly == btnY && lx >= okX && lx < okX + bw1) {
                        ok = true;
                    } else if (ly == btnY && lx >= noX && lx < noX + bw2) {
                        cancel = true;
                    }
                }
            } else if (wc == KEY_BACKSPACE) {
                if (selAll) {
                    val.clear();
                    pos = 0;
                    selAll = false;
                } else if (pos > 0) {
                    size_t p2 = prevUtf8Pos(val, pos);
                    val.erase(p2, pos - p2);
                    pos = p2;
                }
            } else if (wc == KEY_DC) {
                if (selAll) {
                    val.clear();
                    pos = 0;
                    selAll = false;
                } else if (pos < val.size()) {
                    size_t p2 = nextUtf8Pos(val, pos);
                    val.erase(pos, p2 - pos);
                }
            } else if (wc == KEY_LEFT) {
                if (selAll) {
                    pos = 0;
                    selAll = false;
                } else {
                    pos = prevUtf8Pos(val, pos);
                }
            } else if (wc == KEY_RIGHT) {
                if (selAll) {
                    pos = val.size();
                    selAll = false;
                } else {
                    pos = nextUtf8Pos(val, pos);
                }
            } else if (wc == KEY_HOME) {
                pos = 0;
                selAll = false;
            } else if (wc == KEY_END) {
                pos = val.size();
                selAll = false;
            } else if (wc == KEY_ENTER) {
                ok = true;
            } else if (wc == KEY_RESIZE) {
                cancel = true;
            }
        } else {
            if (wc == 27) {
                cancel = true;
            } else if (wc == '\n' || wc == '\r') {
                ok = true;
            } else if (wc == 8 || wc == 127) {
                if (selAll) {
                    val.clear();
                    pos = 0;
                    selAll = false;
                } else if (pos > 0) {
                    size_t p2 = prevUtf8Pos(val, pos);
                    val.erase(p2, pos - p2);
                    pos = p2;
                }
            } else if (wc >= 32) {
                std::string u = toUtf8((wchar_t)wc);
                if (!u.empty()) {
                    if (selAll) {
                        val.clear();
                        pos = 0;
                        selAll = false;
                    }
                    val.insert(pos, u);
                    pos += u.size();
                }
            }
        }
    }

    delwin(win);
    if (ok)
        value = val;
    return ok;
}

App::Cmd App::popupMenu(const std::vector<MenuItem>& items, int px, int py) {
    if (COLS < 20 || LINES < 8 || items.empty())
        return CMD_NONE;

    int maxCols = 10;
    for (const auto& it : items) {
        if (!it.sep)
            maxCols = std::max(maxCols, utf8Cols(it.label));
    }
    int w = std::min(COLS - 2, maxCols + 6);
    int total = (int)items.size();
    int h = std::min(total + 2, LINES - 2);
    int n = h - 2;
    int x = std::clamp(px, 0, std::max(0, COLS - w));
    int y = std::clamp(py, 0, std::max(0, LINES - h));

    WINDOW* win = newwin(h, w, y, x);
    if (!win)
        return CMD_NONE;
    keypad(win, TRUE);

    auto firstEnabled = [&]() {
        for (int i = 0; i < total; ++i)
            if (!items[i].sep && items[i].enabled)
                return i;
        return -1;
    };
    auto lastEnabled = [&]() {
        for (int i = total - 1; i >= 0; --i)
            if (!items[i].sep && items[i].enabled)
                return i;
        return -1;
    };

    int focus = firstEnabled();
    int top = 0;
    auto clampView = [&]() {
        if (focus < 0)
            return;
        if (focus < top)
            top = focus;
        if (focus >= top + n)
            top = focus - n + 1;
        top = std::clamp(top, 0, std::max(0, total - n));
    };
    clampView();

    Cmd ret = CMD_NONE;
    bool close = false;

    while (ret == CMD_NONE && !close) {
        werase(win);
        box(win, 0, 0);
        for (int i = 0; i < n; ++i) {
            int ai = top + i;
            if (ai >= total)
                break;
            if (items[ai].sep) {
                mvwhline(win, 1 + i, 1, ACS_HLINE, w - 2);
                continue;
            }
            int attr = 0;
            if (!items[ai].enabled)
                attr |= A_DIM;
            if (ai == focus)
                attr |= A_REVERSE;
            wput(win, 1 + i, 1, w - 2, "  " + items[ai].label, attr);
        }
        if (top > 0)
            mvwaddch(win, 1, w - 2, ACS_UARROW);
        if (top + n < total)
            mvwaddch(win, h - 2, w - 2, ACS_DARROW);
        wrefresh(win);

        wint_t wc = 0;
        int r = wget_wch(win, &wc);
        if (r == ERR)
            break;
        if (r == KEY_CODE_YES) {
            if (wc == KEY_MOUSE) {
                MEVENT ev;
                if (getmouse(&ev) == OK &&
                    (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
                    int ly = ev.y - y;
                    int lx = ev.x - x;
                    if (ly >= 1 && ly <= n && lx >= 1 && lx < w - 1) {
                        int idx = top + (ly - 1);
                        if (idx >= 0 && idx < total && !items[idx].sep && items[idx].enabled)
                            ret = items[idx].cmd;
                        else if (idx >= total)
                            close = true;
                    } else {
                        close = true;
                    }
                }
            } else if (wc == KEY_UP || wc == KEY_DOWN) {
                int dir = (wc == KEY_UP) ? -1 : 1;
                if (focus < 0) {
                    focus = firstEnabled();
                } else {
                    int i = focus;
                    for (int steps = 0; steps < total; ++steps) {
                        i = (i + dir + total) % total;
                        if (!items[i].sep && items[i].enabled) {
                            focus = i;
                            break;
                        }
                    }
                }
                clampView();
            } else if (wc == KEY_HOME) {
                focus = firstEnabled();
                top = 0;
            } else if (wc == KEY_END) {
                focus = lastEnabled();
                top = std::max(0, total - n);
            } else if (wc == KEY_ENTER) {
                if (focus >= 0 && focus < total && items[focus].enabled && !items[focus].sep)
                    ret = items[focus].cmd;
            } else if (wc == KEY_RESIZE) {
                close = true;
            }
        } else {
            if (wc == 27) {
                close = true;
            } else if (wc == '\n' || wc == '\r') {
                if (focus >= 0 && focus < total && items[focus].enabled && !items[focus].sep)
                    ret = items[focus].cmd;
            }
        }
    }

    delwin(win);
    return ret;
}

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            std::printf("用法: %s [目录]\n", argv[0]);
            std::printf("TExplorer — 简易 TUI 文件资源管理器（支持鼠标）。\n");
            return 0;
        }
    }

    fs::path start = (argc > 1) ? fs::path(argv[1]) : fs::current_path();
    try {
        App app(start);
        app.run();
    } catch (const std::exception& e) {
        endwin();
        std::fprintf(stderr, "错误: %s\n", e.what());
        return 1;
    }
    return 0;
}
