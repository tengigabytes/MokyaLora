// SPDX-License-Identifier: MIT
// hal_pc_stdin.cpp — PC stdin key-event source (raw-mode terminal reader).
//
// Platform support:
//   POSIX (Linux, macOS) — termios + select()
//   Windows              — SetConsoleMode + _kbhit() / _getch()

#include "hal_pc_stdin.h"
#include "key_map.h"

#ifdef _WIN32
#  include <conio.h>
#  include <windows.h>
#else
#  include <sys/select.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace mie {
namespace pc {

// ── Platform raw-mode helpers ───────────────────────────────────────────────

#ifdef _WIN32

static DWORD s_orig_console_mode = 0;

void HalPcStdin::set_raw_mode(bool enable) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (enable) {
        GetConsoleMode(h, &s_orig_console_mode);
        SetConsoleMode(h, s_orig_console_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    } else {
        SetConsoleMode(h, s_orig_console_mode);
    }
}

int HalPcStdin::read_pc_key() {
    if (!_kbhit()) return -1;
    int ch = _getch();
    // Windows arrow / F-keys come as two-byte sequences (0x00 or 0xE0 prefix).
    if (ch == 0x00 || ch == 0xE0) {
        int ext = _getch();
        switch (ext) {
            case 72: return KEY_UP;
            case 80: return KEY_DOWN;
            case 75: return KEY_LEFT;
            case 77: return KEY_RIGHT;
            case 59: return KEY_F1;
            case 60: return KEY_F2;
            case 83: return KEY_DELETE;
        }
        return -1;
    }
    return ch;
}

#else // POSIX

static struct termios s_orig_termios;

void HalPcStdin::set_raw_mode(bool enable) {
    if (enable) {
        tcgetattr(STDIN_FILENO, &s_orig_termios);
        struct termios raw = s_orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    } else {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
    }
}

int HalPcStdin::read_pc_key() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return -1;

    unsigned char buf[4] = {};
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return -1;

    // xterm / VT100 escape sequences for arrow keys.
    if (n >= 3 && buf[0] == 0x1B && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'D': return KEY_LEFT;
            case 'C': return KEY_RIGHT;
            case '3': return KEY_DELETE;
        }
    }
    if (n >= 3 && buf[0] == 0x1B && buf[1] == 'O') {
        switch (buf[2]) {
            case 'P': return KEY_F1;
            case 'Q': return KEY_F2;
        }
    }
    return static_cast<int>(buf[0]);
}

#endif // _WIN32

// ── HalPcStdin ──────────────────────────────────────────────────────────────

HalPcStdin::HalPcStdin() : raw_mode_active_(false) {
    set_raw_mode(true);
    raw_mode_active_ = true;
}

HalPcStdin::~HalPcStdin() {
    if (raw_mode_active_) set_raw_mode(false);
}

bool HalPcStdin::poll(KeyEvent& out, uint32_t now_ms) {
    int pc_key = read_pc_key();
    if (pc_key < 0) return false;

    // ESC → caller-signal to quit.
    if (pc_key == KEY_ESCAPE) {
        out = KeyEvent{MOKYA_KEY_NONE, false, now_ms};
        return true;
    }

    for (const KeyMapEntry* e = kPcKeyMap; e->pc_key != -1; ++e) {
        if (e->pc_key == pc_key) {
            out = KeyEvent{e->keycode, true, now_ms};
            return true;
        }
    }
    return false;   // unmapped
}

} // namespace pc
} // namespace mie
