// See server_console.h.
//
// Interactive when both input and output are a terminal (a person at a
// console or an SSH session): the line being typed stays at the bottom while
// output scrolls above it, with cursor keys, history and Tab. Otherwise
// (Docker logs, systemd, a pipe) plain lines in and out, since escape codes
// there would only be noise.
#include "server_console.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace thistle::detail {

namespace {

struct TypedLines {
    std::mutex mutex;
    std::deque<std::string> lines;
};

bool is_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

size_t code_points(const std::string& s, size_t from = 0, size_t to = std::string::npos) {
    to = std::min(to, s.size());
    size_t n = 0;
    for (size_t i = from; i < to; ++i) n += is_continuation(s[i]) ? 0 : 1;
    return n;
}

// The byte offset `count` code points after `from`.
size_t advance(const std::string& s, size_t from, size_t count) {
    size_t i = from;
    while (count > 0 && i < s.size()) {
        ++i;
        while (i < s.size() && is_continuation(s[i])) ++i;
        --count;
    }
    return i;
}

// What the console changed about the terminal, kept outside the console
// object so restore_console() can undo it from a signal handler.
std::atomic<bool> g_changed{false};
#if defined(_WIN32)
HANDLE g_in = nullptr, g_out = nullptr;
DWORD g_saved_in_mode = 0, g_saved_out_mode = 0;
UINT g_saved_cp = 0;
#else
termios g_saved_termios;
#endif

} // namespace

void restore_console() {
    if (!g_changed.exchange(false)) return;
#if defined(_WIN32)
    SetConsoleMode(g_in, g_saved_in_mode);
    SetConsoleMode(g_out, g_saved_out_mode);
    if (g_saved_cp) SetConsoleOutputCP(g_saved_cp);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
#endif
}

struct ServerConsole::Impl {
    std::shared_ptr<TypedLines> typed = std::make_shared<TypedLines>();
    std::mutex mutex; // output, and the line being typed
    bool open = false;
    bool interactive = false;
    std::atomic<bool> stopping{false};
    std::thread reader;
    std::string partial; // plain mode: a line not finished yet

    // Plain mode: lines from stdin. Raw reads, not std::getline: a thread
    // blocked in getline holds the C library's lock on stdin, and exit()
    // waits for that lock to flush the streams, so a server whose stdin
    // stayed open without input (docker run -i, no -t) hung instead of
    // exiting after its save.
    void take(const char* data, size_t n) {
        partial.append(data, n);
        size_t nl;
        while ((nl = partial.find('\n')) != std::string::npos) {
            std::string line = partial.substr(0, nl);
            partial.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::lock_guard<std::mutex> lock(typed->mutex);
            typed->lines.push_back(std::move(line));
        }
    }
#if defined(_WIN32)
    void read_lines() {
        const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        const DWORD type = GetFileType(h);
        char buf[512];
        while (!stopping) {
            DWORD n = 0;
            if (type == FILE_TYPE_PIPE) { // pipes can't be waited on: ask how much is there
                DWORD avail = 0;
                if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return; // closed
                if (avail == 0) { Sleep(50); continue; }
            } else if (type == FILE_TYPE_CHAR) {
                if (WaitForSingleObject(h, 100) != WAIT_OBJECT_0) continue;
            }
            if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) return;
            take(buf, n);
        }
    }
#else
    void read_lines() {
        char buf[512];
        while (!stopping) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) return; // end of input
            take(buf, static_cast<size_t>(n));
        }
    }
#endif

    std::vector<std::string> words; // for Tab
    std::string line;               // being typed (UTF-8)
    size_t cursor = 0;              // byte offset, on a code point boundary
    std::vector<std::string> history;
    size_t history_pos = 0;         // history.size() = the line being typed
    std::string draft;              // what was typed before browsing history

    int width() const {
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(g_out, &info)) return info.srWindow.Right - info.srWindow.Left + 1;
#else
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
        return 80;
    }

    void write_out(const std::string& s) {
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }

    // "> " and the part of the line that fits, scrolled to keep the cursor
    // in view; then the cursor where it belongs.
    std::string prompt_text() const {
        const size_t room = static_cast<size_t>(std::max(10, width() - 3));
        const size_t total = code_points(line);
        const size_t at = code_points(line, 0, cursor);
        size_t first = 0;
        if (total > room) first = at > room - 1 ? at - (room - 1) : 0;
        const size_t a = advance(line, 0, first);
        const size_t b = advance(line, a, room);
        std::string out = "\r\x1b[2K> " + line.substr(a, b - a);
        const size_t right = code_points(line, a, b) - (at - first);
        if (right > 0) out += "\x1b[" + std::to_string(right) + "D";
        return out;
    }
    void redraw() { write_out(prompt_text()); }

    void set_line(const std::string& s) {
        line = s;
        cursor = line.size();
    }

    void enter() {
        const std::string typed_line = line;
        write_out("\r\x1b[2K> " + typed_line + "\n"); // it stays in the scrollback
        if (!typed_line.empty() && (history.empty() || history.back() != typed_line)) history.push_back(typed_line);
        if (history.size() > 200) history.erase(history.begin());
        history_pos = history.size();
        draft.clear();
        set_line("");
        {
            std::lock_guard<std::mutex> lock(typed->mutex);
            typed->lines.push_back(typed_line);
        }
        redraw();
    }

    void complete() {
        if (line.find(' ') != std::string::npos) return; // only the command name
        const std::string start = line.size() > 0 && line[0] == '/' ? line.substr(1) : line;
        const std::string slash = line.size() > 0 && line[0] == '/' ? "/" : "";
        std::vector<std::string> matches;
        for (const std::string& w : words) if (w.rfind(start, 0) == 0) matches.push_back(w);
        if (matches.empty()) return;
        if (matches.size() == 1) {
            set_line(slash + matches[0] + " ");
            redraw();
            return;
        }
        std::string common = matches[0];
        for (const std::string& m : matches) {
            size_t n = 0;
            while (n < common.size() && n < m.size() && common[n] == m[n]) ++n;
            common.resize(n);
        }
        std::string list;
        for (const std::string& m : matches) list += (list.empty() ? "" : "  ") + m;
        write_out("\r\x1b[2K" + list + "\n");
        set_line(slash + common);
        redraw();
    }

    void history_step(int dir) {
        if (history.empty()) return;
        if (history_pos == history.size()) draft = line;
        if (dir < 0 && history_pos > 0) --history_pos;
        else if (dir > 0 && history_pos < history.size()) ++history_pos;
        else return;
        set_line(history_pos == history.size() ? draft : history[history_pos]);
        redraw();
    }

    enum class Key { Left, Right, Home, End, Up, Down, Delete, Backspace, Tab, Enter, ClearLine, DeleteWord };

    void key(Key k) {
        std::lock_guard<std::mutex> lock(mutex);
        switch (k) {
            case Key::Left: if (cursor > 0) { do --cursor; while (cursor > 0 && is_continuation(line[cursor])); } break;
            case Key::Right: cursor = advance(line, cursor, 1); break;
            case Key::Home: cursor = 0; break;
            case Key::End: cursor = line.size(); break;
            case Key::Up: history_step(-1); return;
            case Key::Down: history_step(1); return;
            case Key::Delete: line.erase(cursor, advance(line, cursor, 1) - cursor); break;
            case Key::Backspace:
                if (cursor > 0) {
                    size_t start = cursor;
                    do --start; while (start > 0 && is_continuation(line[start]));
                    line.erase(start, cursor - start);
                    cursor = start;
                }
                break;
            case Key::Tab: complete(); return;
            case Key::Enter: enter(); return;
            case Key::ClearLine: line.erase(0, cursor); cursor = 0; break;
            case Key::DeleteWord: {
                size_t start = cursor;
                while (start > 0 && line[start - 1] == ' ') --start;
                while (start > 0 && line[start - 1] != ' ') --start;
                line.erase(start, cursor - start);
                cursor = start;
                break;
            }
        }
        redraw();
    }

    void insert(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        line.insert(cursor, text);
        cursor += text.size();
        redraw();
    }

#if defined(_WIN32)
    bool start_interactive() {
        g_in = GetStdHandle(STD_INPUT_HANDLE);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!GetConsoleMode(g_in, &g_saved_in_mode) || !GetConsoleMode(g_out, &g_saved_out_mode)) return false;
        // Escape codes need Windows 10's virtual terminal mode; without it,
        // plain lines.
        if (!SetConsoleMode(g_out, g_saved_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) return false;
        // Keys one at a time, no echo. Processed input stays on: Ctrl+C is
        // the stop signal, not a key.
        SetConsoleMode(g_in, (g_saved_in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE)) |
                                 ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS);
        g_saved_cp = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        g_changed = true;
        reader = std::thread([this] { read_keys(); });
        return true;
    }
    void stop_interactive() { restore_console(); }
    void read_keys() {
        while (!stopping) {
            if (WaitForSingleObject(g_in, 100) != WAIT_OBJECT_0) continue;
            INPUT_RECORD rec[16];
            DWORD n = 0;
            if (!ReadConsoleInputW(g_in, rec, 16, &n)) return;
            for (DWORD i = 0; i < n; ++i) {
                if (rec[i].EventType != KEY_EVENT || !rec[i].Event.KeyEvent.bKeyDown) continue;
                const KEY_EVENT_RECORD& k = rec[i].Event.KeyEvent;
                for (WORD r = 0; r < std::max<WORD>(1, k.wRepeatCount); ++r) {
                    switch (k.wVirtualKeyCode) {
                        case VK_LEFT: key(Key::Left); continue;
                        case VK_RIGHT: key(Key::Right); continue;
                        case VK_HOME: key(Key::Home); continue;
                        case VK_END: key(Key::End); continue;
                        case VK_UP: key(Key::Up); continue;
                        case VK_DOWN: key(Key::Down); continue;
                        case VK_DELETE: key(Key::Delete); continue;
                        case VK_BACK: key(Key::Backspace); continue;
                        case VK_TAB: key(Key::Tab); continue;
                        case VK_RETURN: key(Key::Enter); continue;
                        default: break;
                    }
                    const wchar_t ch = k.uChar.UnicodeChar;
                    if (ch == 0x15) { key(Key::ClearLine); continue; }  // Ctrl+U
                    if (ch == 0x17) { key(Key::DeleteWord); continue; } // Ctrl+W
                    if (ch < 0x20) continue;
                    char utf8[8];
                    const int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8), nullptr, nullptr);
                    if (len > 0) insert(std::string(utf8, static_cast<size_t>(len)));
                }
            }
        }
    }
#else
    bool start_interactive() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
        const char* term = std::getenv("TERM");
        if (term && std::strcmp(term, "dumb") == 0) return false;
        if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) return false;
        termios raw = g_saved_termios;
        // Keys one at a time, no echo. ISIG stays: Ctrl+C is still the
        // stop signal.
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
        g_changed = true;
        reader = std::thread([this] { read_keys(); });
        return true;
    }
    void stop_interactive() { restore_console(); }
    void read_keys() {
        std::string pending;
        while (!stopping) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            char buf[64];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) return;
            pending.append(buf, static_cast<size_t>(n));
            size_t i = 0;
            while (i < pending.size()) {
                const unsigned char c = static_cast<unsigned char>(pending[i]);
                if (c == 0x1B) { // an escape sequence: cursor keys and friends
                    if (i + 1 >= pending.size()) break; // the rest is still coming
                    const char kind = pending[i + 1];
                    if (kind != '[' && kind != 'O') { i += 1; continue; }
                    size_t j = i + 2;
                    while (j < pending.size() && !(pending[j] >= 0x40 && pending[j] <= 0x7E)) ++j;
                    if (j >= pending.size()) break;
                    const std::string params = pending.substr(i + 2, j - (i + 2));
                    switch (pending[j]) {
                        case 'A': key(Key::Up); break;
                        case 'B': key(Key::Down); break;
                        case 'C': key(Key::Right); break;
                        case 'D': key(Key::Left); break;
                        case 'H': key(Key::Home); break;
                        case 'F': key(Key::End); break;
                        case '~':
                            if (params == "3") key(Key::Delete);
                            else if (params == "1" || params == "7") key(Key::Home);
                            else if (params == "4" || params == "8") key(Key::End);
                            break;
                        default: break;
                    }
                    i = j + 1;
                    continue;
                }
                if (c == '\r' || c == '\n') key(Key::Enter);
                else if (c == 0x7F || c == 0x08) key(Key::Backspace);
                else if (c == '\t') key(Key::Tab);
                else if (c == 0x01) key(Key::Home);       // Ctrl+A
                else if (c == 0x05) key(Key::End);        // Ctrl+E
                else if (c == 0x15) key(Key::ClearLine);  // Ctrl+U
                else if (c == 0x17) key(Key::DeleteWord); // Ctrl+W
                else if (c >= 0x20) {
                    // A whole UTF-8 character at once.
                    size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
                    if (i + len > pending.size()) break;
                    insert(pending.substr(i, len));
                    i += len;
                    continue;
                }
                ++i;
            }
            pending.erase(0, i);
        }
    }
#endif
};

ServerConsole::ServerConsole() : impl_(std::make_unique<Impl>()) {}
ServerConsole::~ServerConsole() { close(); }

void ServerConsole::open(std::vector<std::string> words) {
    Impl& im = *impl_;
    if (im.open) return;
    im.open = true;
    std::sort(words.begin(), words.end());
    im.words = std::move(words);
    im.interactive = im.start_interactive();
    if (im.interactive) {
        std::lock_guard<std::mutex> lock(im.mutex);
        im.redraw();
        return;
    }
    im.stopping = false;
    im.reader = std::thread([&im] { im.read_lines(); });
}

void ServerConsole::close() {
    Impl& im = *impl_;
    if (!im.open) return;
    im.open = false;
    im.stopping = true;
    if (im.reader.joinable()) im.reader.join();
    if (!im.interactive) return;
    {
        std::lock_guard<std::mutex> lock(im.mutex);
        im.write_out("\r\x1b[2K");
    }
    im.stop_interactive();
    im.interactive = false;
}

void ServerConsole::print(const std::string& line) {
    Impl& im = *impl_;
    std::lock_guard<std::mutex> lock(im.mutex);
    if (im.interactive) {
        im.write_out("\r\x1b[2K" + line + "\n" + im.prompt_text());
        return;
    }
    im.write_out(line + "\n");
}

bool ServerConsole::poll(std::string& line) {
    TypedLines& typed = *impl_->typed;
    std::lock_guard<std::mutex> lock(typed.mutex);
    if (typed.lines.empty()) return false;
    line = std::move(typed.lines.front());
    typed.lines.pop_front();
    return true;
}

} // namespace thistle::detail
