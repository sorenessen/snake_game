// snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// Poop → Bomb (harmless if eaten) system, with BIG emoji head while chomping
// + Floating text: when a poop activates, a random taunt rises up (only one at a time).
// + Ornate Egyptian gold frame (ankhs, pyramids, Nile-wave bevel) around the playfield.
// + Per-group poop triplet WAV logic (a sound on every completed 3-poop grouping)
// + Title-screen: right-side smooth credit roll (soft fade, 30 FPS, no flicker)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <ctime>
#include <signal.h>
#include <sys/wait.h>

using namespace std;

// ---------- Sound config ----------
static constexpr bool ENABLE_SOUNDS = true;
static constexpr bool ENABLE_BEEP_FALLBACK = false;

static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
static constexpr const char* FART_SOUND    = "Submarine";
static constexpr const char* SPLASH_SOUND  = "Purr";
static constexpr const char* REWARD_SOUND  = "Glass";
static constexpr const char* LEVEL_SOUND   = "Hero";
static constexpr const char* BOMB_SOUND    = "Basso";
static constexpr const char* DISARM_SOUND  = "Ping";

// 🟤 Custom local WAVs inside assets/
static constexpr const char* POOP_WAV   = "assets/snake_shit.wav"; // on poop activation (tail vacates)
static constexpr const char* NOM_WAV    = "assets/nom_nom_nom.wav"; // poop-eating (final pellet of group)
static constexpr const char* NASTY_WAV  = "assets/nasty.wav";       // poop-eating (final pellet of group)
static constexpr const char* GROSS_WAV  = "assets/gross.wav";       // poop-eating (final pellet of group)
static constexpr const char* TITLE_MUSIC_WAV = "assets/groove.wav"; // splash/title theme
static constexpr const char* BG_MUSIC_WAV    = "assets/banzai.wav"; // quiet gameplay loop
static constexpr const char* BG_MUSIC_VOL    = "0.19";              // 0.0..1.0 volume for afplay

// Play macOS built-in system sound by name
static inline void play_system_sound(const char* name) {
    if (!ENABLE_SOUNDS || name == nullptr) return;
    std::string cmd = "afplay '/System/Library/Sounds/" + std::string(name) + ".aiff' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
    if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
}

// Play any local wav/aif file via afplay
static inline void play_wav(const char* path) {
    if (!ENABLE_SOUNDS || path == nullptr) return;
    std::string cmd = "afplay '" + std::string(path) + "' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
}

// ----- One-sound-per-frame queue (prevents echo/overlap) -----
enum class SndType { None, Wav, Sys };

struct PendingSound {
    SndType type{SndType::None};
    const char* wav{nullptr};
    const char* sys{nullptr};
};
static PendingSound g_pending;

static inline void queue_wav(const char* path) {
    if (!ENABLE_SOUNDS || !path) return;
    g_pending.type = SndType::Wav;
    g_pending.wav  = path;
    g_pending.sys  = nullptr;
}
static inline void queue_sys(const char* name) {
    if (!ENABLE_SOUNDS || !name) return;
    g_pending.type = SndType::Sys;
    g_pending.sys  = name;
    g_pending.wav  = nullptr;
}
static inline void flush_sound() {
    if (!ENABLE_SOUNDS) { g_pending = {}; return; }
    if (g_pending.type == SndType::Wav && g_pending.wav) {
        play_wav(g_pending.wav);
    } else if (g_pending.type == SndType::Sys && g_pending.sys) {
        play_system_sound(g_pending.sys);
    }
    g_pending = {};
}

// ----- Title & Background music lifecycle (own processes we can stop cleanly) -----
static pid_t g_title_music_pid = -1;

// For background music we track both the shell PID and its process group ID.
static pid_t g_bg_music_pid  = -1;
static pid_t g_bg_music_pgid = -1;

static bool file_exists(const char* p) {
    struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void stop_pid(pid_t& pid_ref) {
    if (pid_ref > 0) {
        kill(pid_ref, SIGTERM);
        for (int i = 0; i < 50; ++i) { // up to ~500ms
            if (waitpid(pid_ref, nullptr, WNOHANG) > 0) { pid_ref = -1; return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid_ref, SIGKILL);
        waitpid(pid_ref, nullptr, 0);
        pid_ref = -1;
    }
}

static void stop_pgroup(pid_t& pgid_ref, pid_t& wait_pid_ref) {
    if (pgid_ref > 0) {
        kill(-pgid_ref, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            if (waitpid(wait_pid_ref, nullptr, WNOHANG) > 0) {
                wait_pid_ref = -1; pgid_ref = -1; return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(-pgid_ref, SIGKILL);
        (void)waitpid(wait_pid_ref, nullptr, 0);
        wait_pid_ref = -1; pgid_ref = -1;
    }
}

static void start_title_music() {
    if (!ENABLE_SOUNDS) return;
    if (!TITLE_MUSIC_WAV || !std::strlen(TITLE_MUSIC_WAV)) return;
    if (!file_exists(TITLE_MUSIC_WAV)) return;
    if (g_title_music_pid > 0) return; // already running

    pid_t pid = fork();
    if (pid == 0) {
        execlp("afplay", "afplay", TITLE_MUSIC_WAV, (char*)nullptr);
        _exit(127);
    } else if (pid > 0) {
        g_title_music_pid = pid;
    }
}

static void start_bg_music() {
    if (!ENABLE_SOUNDS) return;
    if (!BG_MUSIC_WAV || !std::strlen(BG_MUSIC_WAV)) return;
    if (!file_exists(BG_MUSIC_WAV)) return;
    if (g_bg_music_pid > 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        std::string script = std::string("while :; do afplay -q 1 -v ") + BG_MUSIC_VOL + " '" + BG_MUSIC_WAV + "'; done";
        execlp("sh", "sh", "-c", script.c_str(), (char*)nullptr);
        _exit(127);
    } else if (pid > 0) {
        setpgid(pid, pid);
        g_bg_music_pid  = pid;
        g_bg_music_pgid = pid;
    }
}

static void stop_title_music() { stop_pid(g_title_music_pid); }
static void stop_bg_music()    { stop_pgroup(g_bg_music_pgid, g_bg_music_pid); }

// ---------- ANSI colors ----------
static constexpr const char* RESET = "\x1b[0m";
static constexpr const char* BG_BLUE = "\x1b[44m";
static constexpr const char* FG_WHITE = "\x1b[37m";
static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
static constexpr const char* FG_RED           = "\x1b[91m";
static constexpr const char* FG_ORANGE_208    = "\x1b[38;5;208m";
static constexpr const char* FG_YELLOW        = "\x1b[33m";

// ✨ Gold frame shades
static constexpr const char* FG_GOLD_DARK   = "\x1b[38;5;178m"; // outer
static constexpr const char* FG_GOLD_MAIN   = "\x1b[38;5;220m"; // inner pillars
static constexpr const char* FG_GOLD_ACCENT = "\x1b[38;5;223m"; // bevel line

// Background fill toggle for playfield rows
static constexpr bool USE_BLUE_BG = false; // set true for solid blue playfield

// ---------- Box-drawing (UTF-8) ----------
static constexpr const char* BOX_TL = "╔";
static constexpr const char* BOX_TR = "╗";
static constexpr const char* BOX_BL = "╚";
static constexpr const char* BOX_BR = "╝";
static constexpr const char* BOX_H  = "═";
static constexpr const char* BOX_V  = "║";
static constexpr const char* BOX_AL = "╟";
static constexpr const char* BOX_AR = "╢";
static constexpr const char* BOX_AH = "─";

// ---------- Config ----------
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
static constexpr int BASE_TICK_MS = 100;
static constexpr int MIN_TICK_MS  = 40;
static constexpr int TICK_DECR_MS = 10;
static constexpr int GROW_DECR_MS = 3;

// Poop/Bomb timings & penalty
static constexpr auto GOOD_WINDOW = std::chrono::seconds(15);
static constexpr auto BOMB_WINDOW = std::chrono::seconds(15);
static constexpr int  BOMB_GROW_UNITS = 2;

// Wide-head glyph during chomp (double-width in most terminals)
static const std::string WIDE_HEAD = "🟢";

// ---------- Taunts for floating text ----------
static const char* TAUNTS[] = {
    "PBBBBBT",
    "PLOP PLOP PLOP",
    "CLEAN UP YOUR MESS!",
    "SQUIRT SQUIRT PBBBBT",
    "YOU'RE WHY DAD LEFT",
    "DISGUSTING!",
    "SHARTING IS A SKILL!"
};
static constexpr int TAUNTS_COUNT = (int)(sizeof(TAUNTS) / sizeof(TAUNTS[0]));

// ============================================================
//              Egyptian Frame Customization
// ============================================================
static constexpr bool EGYPTIAN_FRAME = true;

static constexpr const char* GLYPH_PYRAMID = "▲";  // U+25B2
static constexpr const char* GLYPH_ANKH    = "☥";  // U+2625
static constexpr const char* GLYPH_NILE    = "≋";  // U+224B
static constexpr const char* GLYPH_ROSETTE = "◈";  // U+25C8

static inline void print_egyptian_bar(int pad, bool top) {
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << FG_GOLD_DARK << (top ? BOX_TL : BOX_BL) << RESET;

    const char* PAT[6] = { GLYPH_PYRAMID, GLYPH_ANKH, GLYPH_PYRAMID, GLYPH_ANKH, GLYPH_NILE, GLYPH_ANKH };
    for (int c = 0; c < COLS; ++c) {
        bool accent = ((c % 2) == 0);
        std::cout << (accent ? FG_GOLD_ACCENT : FG_GOLD_MAIN) << PAT[c % 6] << RESET;
    }

    std::cout << FG_GOLD_DARK << (top ? BOX_TR : BOX_BR) << RESET << "\n";
}

static inline void print_egyptian_bevel(int pad) {
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << FG_GOLD_ACCENT << GLYPH_ROSETTE << RESET;
    for (int c = 0; c < COLS; ++c) std::cout << FG_GOLD_ACCENT << GLYPH_NILE << RESET;
    std::cout << FG_GOLD_ACCENT << GLYPH_ROSETTE << RESET << "\n";
}

// ---------- Credits data ----------
static const std::vector<std::string> CREDITS = {
    "Lead Developer: Cinnamon Essen",
    "UX Designer: Gregory Birdmouth",
    "Master Logician: Skip Sinclair",
    "Ham Curer: Bellingham Frisk",
    "Q-Tip Procurer: Soren Essen",
    "Soap Dispenser Repair: Bob Garry",
    "Sweat Dobber: Sarah from Marketing",
    "Snake Costume Designer: Wulf Kraut",
    "Poop Shoveler: Dixon Osbeck",
    "Graphic Artist: Kentucky Graham",
    "",
    "And last but not least...",
    "",
    "Officer McGreggor from D.A.R.E,",
    "Olympia School District, 1992-2001"
};

// ---------- Terminal helpers ----------
static inline void cursor_xy(int row1, int col1) { std::cout << "\x1b[" << row1 << ";" << col1 << "H"; }
static inline void clear_screen() { std::cout << "\x1b[2J\x1b[H"; }

// ---------- ASCII splash fallback ----------
static constexpr const char* SPLASH_PATH = "assets/splash.png";
static constexpr int SPLASH_SCALE_PCT = 40;

static void center_line(const std::string& s) {
    int w = ([](){ winsize ws{}; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==0 && ws.ws_col>0) return (int)ws.ws_col; return COLS; })();
    int pad = std::max(0, (int)(w - (int)s.size()) / 2);
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << s << "\n";
}

static bool read_file(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> buf(4096);
    out.clear();
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) out.append((const char*)buf.data(), n);
    std::fclose(f);
    return true;
}
static std::string b64_encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve((in.size()*4+2)/3);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned int v = ((unsigned char)in[i] << 16) |
                         ((unsigned char)in[i+1] << 8) |
                         ((unsigned char)in[i+2]);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6)  & 63]);
        out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned int v = ((unsigned char)in[i]) << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        unsigned int v = (((unsigned char)in[i]) << 16) |
                         (((unsigned char)in[i+1]) << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('=');
    }
    return out;
}
static bool is_iterm() { return std::getenv("ITERM_SESSION_ID") != nullptr; }

static void ascii_splash_art() {
    const char* G  = "\x1b[92m";
    const char* Y  = "\x1b[93m";
    const char* R  = "\x1b[91m";
    const char* Wt = "\x1b[97m";
    const char* Br = "\x1b[38;5;130m";
    const char* Rt = "\x1b[0m";
    std::vector<std::string> art = {
        std::string(G) + "           ________                          " + Rt,
        std::string(G) + "        .-`  ____  `-.                       " + Rt,
        std::string(G) + "      .'   .`    `.   `.                     " + Rt,
        std::string(G) + "     /   .'   " + R + "◥◤" + G + "   `.   \\                    " + Rt,
        std::string(G) + "    ;   /    " + Wt + " __ __ " + G + "   \\   ;                   " + Rt,
        std::string(G) + "    |  |   " + Wt + " /__V__\\ " + G + "  |  |   " + Y + "   ●" + Rt,
        std::string(G) + "    |  |  " + R + "  \\____/ " + G + "  " + R + "\\/" + G + " |  |   " + Br + "  ● ● ●" + Rt,
        std::string(G) + "    ;   \\      " + R + "┏━┓" + G + "      /   ;   " + Br + "  ●●● ●●●" + Rt,
        std::string(G) + "     \\    `._ " + Wt + "V  V" + G + "  _.'   /                    " + Rt,
        std::string(G) + "      `.     `-.__.-'     .'                 " + Rt,
        std::string(G) + "        `-._            _.-'                  " + Rt
    };
    center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE WHO EATS ITS OWN SHIT FOR BREAKFAST\x1b[0m");
    std::cout << "\n";
    for (auto& line : art) center_line(line);
    std::cout << "\n";
}

// ---------- Raw terminal guard ----------
struct RawTerm {
    termios orig{};
    bool ok{false};
    RawTerm() {
        if (!isatty(STDIN_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
        termios raw = orig;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
        ok = true;
    }
    ~RawTerm() {
        if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
};

// ---------- Input queue ----------
mutex in_mtx;
queue<char> in_q;
atomic<bool> running{true};

optional<char> read_key_now() {
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n == 1) return static_cast<char>(ch);
    return nullopt;
}
void enqueue(char ch) {
    ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    // Allow any key on title screen; gameplay listens to WASD/Q
    if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q' || ch != 0) {
        lock_guard<mutex> lk(in_mtx);
        in_q.push(ch);
    }
}
optional<char> poll_key() {
    lock_guard<mutex> lk(in_mtx);
    if (in_q.empty()) return nullopt;
    char c = in_q.front(); in_q.pop();
    return c;
}

// ---------- Helpers ----------
static bool have_cmd(const char* name) {
    std::string cmd = "command -v "; cmd += name; cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
static int term_cols() {
    winsize ws{}; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col; return COLS;
}
static int term_rows() {
    winsize ws{}; if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row; return ROWS + 6;
}
static bool is_small_term() { return term_cols() < (COLS + 30) || term_rows() < (ROWS + 8); }

// ============================================================
// Credits Panel (double-buffered, soft-fade, no borders)
// ============================================================
static inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Render a credits panel at (left, top) of given size. scroll is a float (rows advanced).
// Double-buffered: only rewrite rows that actually changed; no side borders.
static void render_credits_panel(int left, int top, int width, int height, double scroll) {
    if (width < 6 || height < 3) return;

    const int content_left  = left + 1;              // 1-col margin to avoid edge artifacts
    const int content_right = left + width - 2;
    const int content_width = std::max(0, content_right - content_left + 1);

    struct Buf { int left, top, w, h; std::vector<std::string> rows; };
    static Buf back{ -1, -1, -1, -1, {} };

    auto ensure_buffer = [&](){
        if (back.left != left || back.top != top || back.w != width || back.h != height) {
            back.left = left; back.top = top; back.w = width; back.h = height;
            back.rows.assign(height, std::string(width, ' '));
            for (int r = 0; r < height; ++r) { // one-time clear of the panel
                cursor_xy(top + r, left);
                for (int c = 0; c < width; ++c) std::cout << ' ';
            }
        }
    };
    ensure_buffer();

    // Build front buffer for this frame
    std::vector<std::string> front(height, std::string(width, ' '));

    const int spacing = 2;
    const double bottom_y = height - 1;
    const double PI = 3.14159265358979323846;

    for (size_t i = 0; i < CREDITS.size(); ++i) {
        double virt = bottom_y - (scroll - i * spacing);
        if (virt < -2.0 || virt > height + 2.0) continue; // offscreen slack

        // Use floor for stable monotonic positioning (no +/-1 jitter)
        int row_idx = clampi((int)std::floor(virt + 1e-6), 0, height - 1);

        std::string s = CREDITS[i];
        if ((int)s.size() > content_width) s.resize(content_width);

        // small inner padding to avoid hugging the right edge
        int inner_pad = std::max(0, (content_width - (int)s.size()) / 10);
        int col_start = content_left + inner_pad;
        int max_col   = content_left + content_width - 1;
        if (col_start + (int)s.size() - 1 > max_col) {
            s.resize(std::max(0, max_col - col_start + 1));
        }

        int local_col = col_start - left; // convert to panel-local column
        for (int k = 0; k < (int)s.size() && local_col + k < width; ++k) {
            front[row_idx][local_col + k] = s[k];
        }
    }

    // Diff & draw row-by-row (soft center fade per row)
    for (int r = 0; r < height; ++r) {
        if (front[r] == back.rows[r]) continue;

        double t = height > 1 ? (double)r / (double)(height - 1) : 0.0; // 0..1 top→bottom
        double fade = std::sin(PI * t);               // 0..1..0
        if (fade < 0.0) fade = 0.0;
        int gray = 232 + (int)std::round(23.0 * fade);  // 232..255..232
        if (gray < 232) gray = 232; if (gray > 255) gray = 255;

        cursor_xy(top + r, left);
        std::cout << "\x1b[38;5;" << gray << "m" << front[r] << "\x1b[0m";
    }

    back.rows.swap(front);
}

// ============================================================
// Title sequence with smooth credits roll
// ============================================================
static void cinematic_splash_and_wait() {
    using namespace std::chrono;

    clear_screen();
    std::cout << "\x1b[?25l" << std::flush; // hide cursor

    // Title theme: if present, start it; otherwise do a quick built-in ping
    if (file_exists(TITLE_MUSIC_WAV)) start_title_music();
    else                              play_system_sound(SPLASH_SOUND);

    bool showed_image = false;

    int cols = term_cols();
    int rows = term_rows();
    int img_cols = std::max(10, (cols * SPLASH_SCALE_PCT) / 100);
    int pad = std::max(0, (cols - img_cols) / 2);

    // Left: splash art or ASCII; Right: credits panel
    if (!showed_image && is_iterm() && file_exists(SPLASH_PATH)) {
        std::string data;
        if (read_file(SPLASH_PATH, data)) {
            center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE WHO EATS PIECES OF SHIT LIKE YOU FOR BREAKFAST!\x1b[0m");
            std::cout << "\n";
            for (int i = 0; i < pad; ++i) std::cout << ' ';
            std::string b64 = b64_encode(data);
            std::string tok = std::to_string(std::time(nullptr));
            std::cout << "\x1b]1337;File=name=splash.png?" << tok
                      << ";inline=1;cache=0;width=" << img_cols
                      << ";preserveAspectRatio=1:" << b64 << "\x07\n";
            showed_image = true;
        }
    }
    if (!showed_image) {
        ascii_splash_art();
    }

    // --- Credits panel geometry (right side) ---
    int panel_width  = std::max(28, cols / 3);
    int panel_height = std::max(8, rows - 6);
    int panel_left   = std::max(3, cols - panel_width - 6); // outer margin for safety
    int panel_top    = 4;

    // Softly pulsing prompt (shade only)
    auto draw_prompt = [&](double phase){
        int gray = 244 + (int)std::round(8.0 * std::sin(phase * 2.0 * 3.14159265358979323846)); // 244±8
        if (gray < 232) gray = 232; if (gray > 255) gray = 255;
        std::string msg = "[ Press any key to start ]";
        int w = term_cols();
        int pad2 = std::max(0, (int)(w - (int)msg.size()) / 2);
        cursor_xy(term_rows() - 2, 1);
        for (int i = 0; i < pad2; ++i) std::cout << ' ';
        std::cout << "\x1b[38;5;" << gray << "m" << msg << "\x1b[0m";
    };

    // ---- Timing setup (single source of truth) ----
    using steady_clock = std::chrono::steady_clock;
    const double rows_per_second = 0.55;                 // scroll speed
    const std::chrono::milliseconds frame_dt(33);        // ~30 FPS

    auto start = steady_clock::now();
    auto last  = start;
    double scroll = 0.0;
    double prompt_phase = 0.0;

    // Main title wait loop with credits roll (diffed renderer; no full clears)
    while (true) {
        if (auto k = read_key_now()) break;

        auto now = steady_clock::now();
        if (now - last >= frame_dt) {
            double elapsed = std::chrono::duration<double>(now - start).count();
            scroll = elapsed * rows_per_second;

            render_credits_panel(panel_left, panel_top, panel_width, panel_height, scroll);

            // Gentle prompt pulse (~0.7 Hz)
            prompt_phase = std::fmod(elapsed * 0.7, 1.0);
            draw_prompt(prompt_phase);

            last = now;
            std::cout << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Leaving splash → stop the title theme now
    stop_title_music();
    std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush; // show cursor, clear
}

// ---------- Game model ----------
struct Point { int r, c; };
enum class Dir { Up, Down, Left, Right };
enum class PoopState { Good, Bomb };

// Poop knows its 3-pack group id
struct Poop {
    Point p;
    std::chrono::steady_clock::time_point activated_at;
    PoopState state{PoopState::Good};
    bool expired_punished{false};
    int group_id{-1};
};

// Seed carries future group id
struct PoopSeed {
    Point p;
    int group_id{-1};
};

struct Explosion {
    Point center;
    int frames_left{5};
    std::vector<Point> ring;
};

struct FloatText {
    std::string msg;
    int row;
    int col_start;
    int age{0};
    int life{20};
    int step{3};
};

struct Game {
    deque<Point> snake; // front=head
    Dir dir = Dir::Right;
    Point food{0, 0};
    bool game_over = false;
    int score = 0;

    bool consuming = false;
    int chomp_frames = 0;
    static constexpr int CHOMP_TOTAL = 8;

    int poop_to_drop = 0;
    vector<Poop>  poops;
    vector<PoopSeed> poop_seeds;
    vector<Explosion> booms;

    vector<FloatText> floats;

    int growth_pending = 0;
    int level = 1;
    int level_flash = 0;
    bool level_up_trigger = false;

    int  reward_flash = 0;
    bool slow_down_trigger = false;
    int  shrink_amount = 0;

    int  idle_ticks = 0;
    int  idle_bloat_threshold = 120;

    bool speed_bump_trigger = false;
    int  speed_bump_amount  = 0;

    // Per-group (triplet) WAV logic
    std::unordered_map<int,int> group_remaining;  // gid -> remaining Good poops (starts at 3)
    int current_drop_gid{-1};                     // gid for the currently-dropping triplet
    int next_gid{1};

    // Round-robin index for poop-eating sound rotation
    int eat_poop_sound_idx = 0;
    vector<const char*> eat_sfx;

    mt19937 rng{random_device{}()};

    Game() {
        int r = ROWS / 2, c = COLS / 2;
        snake.push_back({r, c});
        snake.push_back({r, c - 1});
        snake.push_back({r, c - 2});
        place_food();

        const char* candidates[] = { NOM_WAV, NASTY_WAV, GROSS_WAV };
        for (const char* p : candidates) if (file_exists(p)) eat_sfx.push_back(p);

        if (eat_sfx.empty()) {
            std::cerr << "[eat-poop sfx] none found in ./assets (fallback to system sound)\n";
        } else {
            std::cerr << "[eat-poop sfx] using:"; for (auto* p : eat_sfx) std::cerr << " " << p; std::cerr << "\n";
        }

        if (!eat_sfx.empty()) {
            std::uniform_int_distribution<int> dist(0, (int)eat_sfx.size() - 1);
            eat_poop_sound_idx = dist(rng);
        }
    }

    void on_player_input() { idle_ticks = 0; }
    void refresh_idle_threshold() { idle_bloat_threshold = std::max(80, 120 - (level - 1) * 5); }

    Point wrap(Point p) const {
        if (p.r < 0) p.r = ROWS - 1;
        if (p.r >= ROWS) p.r = 0;
        if (p.c < 0) p.c = COLS - 1;
        if (p.c >= COLS) p.c = 0;
        return p;
    }
    Point next_head(Point head) const {
        switch (dir) {
            case Dir::Up:    head.r--; break;
            case Dir::Down:  head.r++; break;
            case Dir::Left:  head.c--; break;
            case Dir::Right: head.c++; break;
        }
        return wrap(head);
    }

    void place_food() {
        uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
        while (true) {
            Point p{R(rng), C(rng)};
            bool on_snake = any_of(snake.begin(), snake.end(),
                                   [&](const Point& s){ return s.r == p.r && s.c == p.c; });
            if (!on_snake) { food = p; return; }
        }
    }

    void change_dir(char key) {
        auto opp = [&](Dir a, Dir b) {
            return (a == Dir::Up && b == Dir::Down) ||
                   (a == Dir::Down && b == Dir::Up) ||
                   (a == Dir::Left && b == Dir::Right) ||
                   (a == Dir::Right && b == Dir::Left);
        };
        Dir ndir = dir;
        if (key == 'W') ndir = Dir::Up;
        else if (key == 'S') ndir = Dir::Down;
        else if (key == 'A') ndir = Dir::Left;
        else if (key == 'D') ndir = Dir::Right;
        if (!opp(dir, ndir)) dir = ndir;
    }

    static vector<Point> explosion_ring(Point c) {
        vector<Point> v = {
            {c.r-1,c.c-1},{c.r-1,c.c},{c.r-1,c.c+1},
            {c.r  ,c.c-1},           {c.r  ,c.c+1},
            {c.r+1,c.c-1},{c.r+1,c.c},{c.r+1,c.c+1}
        };
        for (auto &p : v) {
            if (p.r < 0) p.r += ROWS;
            if (p.r >= ROWS) p.r -= ROWS;
            if (p.c < 0) p.c += COLS;
            if (p.c >= COLS) p.c -= COLS;
        }
        return v;
    }

    void trigger_bomb_expire(Point at) {
        growth_pending += BOMB_GROW_UNITS;
        speed_bump_trigger = true;
        speed_bump_amount  += BOMB_GROW_UNITS;

        Explosion e; e.center = at; e.frames_left = 5; e.ring = explosion_ring(at);
        booms.push_back(e);

        queue_sys(BOMB_SOUND);
    }

    void tick_poop_lifecycle() {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();

        for (auto &pp : poops) {
            auto age = now - pp.activated_at;
            if (age >= GOOD_WINDOW && age < GOOD_WINDOW + BOMB_WINDOW) {
                pp.state = PoopState::Bomb; // arm
            }
        }

        vector<Poop> remaining;
        remaining.reserve(poops.size());
        for (auto &pp : poops) {
            auto age = now - pp.activated_at;
            if (age >= GOOD_WINDOW + BOMB_WINDOW) {
                if (!pp.expired_punished) {
                    pp.expired_punished = true;
                    trigger_bomb_expire(pp.p);
                }
            } else {
                remaining.push_back(pp);
            }
        }
        poops.swap(remaining);
    }

    void decay_booms() {
        for (auto &b : booms) b.frames_left--;
        booms.erase(remove_if(booms.begin(), booms.end(),
                              [](const Explosion& e){ return e.frames_left <= 0; }),
                    booms.end());
    }

    void tick_float_texts() {
        for (auto &ft : floats) {
            ft.age++;
            if (ft.age % ft.step == 0 && ft.row > 0) ft.row--;
        }
        floats.erase(remove_if(floats.begin(), floats.end(),
                               [](const FloatText& f){ return f.age >= f.life || f.row < 0; }),
                     floats.end());
    }

    bool cell_on_snake(int rr, int cc) const {
        for (const auto& seg : snake) if (seg.r == rr && seg.c == cc) return true; return false;
    }

    bool find_poop_at(Point p, size_t* idx_out=nullptr) const {
        for (size_t i = 0; i < poops.size(); ++i) {
            if (poops[i].p.r == p.r && poops[i].p.c == p.c) { if (idx_out) *idx_out = i; return true; }
        }
        return false;
    }

    void maybe_activate_poops() {
        if (poop_seeds.empty()) return;
        vector<PoopSeed> remaining;
        remaining.reserve(poop_seeds.size());
        auto now = std::chrono::steady_clock::now();

        bool spawned_float_this_frame = false;

        for (const auto& s : poop_seeds) {
            if (!cell_on_snake(s.p.r, s.p.c)) {
                Poop pp; pp.p = s.p; pp.activated_at = now; pp.state = PoopState::Good; pp.expired_punished = false; pp.group_id = s.group_id;
                poops.push_back(pp);

                if (file_exists(POOP_WAV)) queue_wav(POOP_WAV);
                else                       queue_sys(FART_SOUND);

                if (floats.empty() && !spawned_float_this_frame) {
                    std::uniform_int_distribution<int> pick(0, TAUNTS_COUNT - 1);
                    std::string msg = TAUNTS[pick(rng)];
                    int len = (int)msg.size();
                    int c0 = std::max(0, std::min(COLS - len, s.p.c - len/2));
                    floats.push_back(FloatText{msg, s.p.r, c0, 0, 20, 3});
                    spawned_float_this_frame = true;
                }
            } else {
                remaining.push_back(s);
            }
        }
        poop_seeds.swap(remaining);
    }

    void queue_next_eat_poop_wav() {
        if (!eat_sfx.empty()) {
            const char* path = eat_sfx[eat_poop_sound_idx];
            queue_wav(path);
            eat_poop_sound_idx = (eat_poop_sound_idx + 1) % (int)eat_sfx.size();
        } else {
            queue_sys(REWARD_SOUND);
        }
    }

    void update() {
        if (game_over) return;

        tick_poop_lifecycle();
        maybe_activate_poops();
        decay_booms();
        tick_float_texts();

        if (level_flash  > 0) level_flash--;
        if (reward_flash > 0) reward_flash--;

        idle_ticks++;

        if (consuming) {
            if (--chomp_frames <= 0) {
                Point nh = next_head(snake.front());
                if (any_of(snake.begin(), snake.end(), [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
                    game_over = true; return;
                }
                snake.push_front(nh);
                score += 10;

                speed_bump_trigger = true;
                speed_bump_amount  += 1;

                if (score % 100 == 0) {
                    level++;
                    level_flash = 12;
                    level_up_trigger = true;
                    queue_sys(LEVEL_SOUND);
                    refresh_idle_threshold();
                }

                std::uniform_int_distribution<int> dist(0, (int)(sizeof(BITE_SOUNDS)/sizeof(BITE_SOUNDS[0])) - 1);
                queue_sys(BITE_SOUNDS[dist(rng)]);

                // Start a new 3-poop grouping for this food
                poop_to_drop = 3;
                current_drop_gid = next_gid++;
                group_remaining[current_drop_gid] = 3;

                place_food();
                consuming = false;
            }
            return;
        }

        Point nh = next_head(snake.front());

        if (nh.r == food.r && nh.c == food.c) {
            consuming = true;
            chomp_frames = CHOMP_TOTAL;
            return;
        }

        size_t poop_idx = 0;
        bool on_poop = find_poop_at(nh, &poop_idx);

        if (any_of(snake.begin(), snake.end(), [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
            game_over = true; return;
        }

        Point tail_before = snake.back();
        snake.push_front(nh);

        bool grew_this_tick = false;

        if (on_poop) {
            PoopState st = poops[poop_idx].state;
            int gid = poops[poop_idx].group_id;
            poops.erase(poops.begin() + (long)poop_idx);

            if (st == PoopState::Good) {
                grew_this_tick = true;
                slow_down_trigger = true;

                int safe_min = 3;
                int desired = 2;
                int can_remove = std::max(0, (int)snake.size() - safe_min);
                int to_remove = std::min(desired, can_remove);
                shrink_amount = to_remove;
                while (to_remove-- > 0 && !snake.empty()) snake.pop_back();

                reward_flash = 10;

                auto it = group_remaining.find(gid);
                if (it != group_remaining.end()) {
                    if (--(it->second) <= 0) {
                        queue_next_eat_poop_wav();
                        group_remaining.erase(it);
                    }
                }
            } else {
                queue_sys(DISARM_SOUND);
            }
        } else if (growth_pending > 0) {
            grew_this_tick = true;
            growth_pending--;
            speed_bump_trigger = true;
            speed_bump_amount  += 1;
        } else if (idle_ticks >= idle_bloat_threshold) {
            grew_this_tick = true;
            idle_ticks = 0;
            speed_bump_trigger = true;
            speed_bump_amount  += 1;
        }

        if (!grew_this_tick) snake.pop_back();

        if (poop_to_drop > 0) {
            poop_seeds.push_back(PoopSeed{tail_before, current_drop_gid});
            poop_to_drop--;
            if (poop_to_drop == 0) current_drop_gid = -1;
        }
    }

    bool cell_has_good_or_bomb(int rr, int cc, PoopState* st_out=nullptr) const {
        for (const auto& p : poops) {
            if (p.p.r == rr && p.p.c == cc) {
                if (st_out) *st_out = p.state;
                return true;
            }
        }
        return false;
    }

    bool draw_float_at(int rr, int cc) const {
        for (const auto& ft : floats) {
            if (rr != ft.row) continue;
            int len = (int)ft.msg.size();
            if (cc >= ft.col_start && cc < ft.col_start + len) {
                char ch = ft.msg[cc - ft.col_start];
                std::cout << FG_BRIGHT_YELLOW << ch << FG_WHITE;
                return true;
            }
        }
        return false;
    }

    void render() const {
        int box_width = COLS + 2;
        int pad = std::max(0, (term_cols() - box_width) / 2);

        std::cout << "\x1b[2J\x1b[H";

        // centered status
        {
            std::string status = "Score: " + std::to_string(score) + "   Level: " + std::to_string(level);
            if (consuming)           status += "   (CHOMP!)";
            if (!poop_seeds.empty()) status += "   (Dropping...)";
            if (growth_pending > 0)  status += "   (Penalty growth +" + std::to_string(growth_pending) + ")";
            if (reward_flash > 0) {
                status += "   \x1b[93m(Time slowed!";
                if (shrink_amount > 0) status += "  Length -" + std::to_string(shrink_amount);
                status += ")\x1b[0m";
            }
            center_line(status);
        }

        // Top cap
        if (EGYPTIAN_FRAME) {
            print_egyptian_bar(pad, /*top=*/true);
            print_egyptian_bevel(pad);
        } else {
            for (int i = 0; i < pad; ++i) cout << ' ';
            cout << FG_GOLD_DARK << BOX_TL << RESET;
            for (int c = 0; c < COLS; ++c) cout << FG_GOLD_MAIN << BOX_H << RESET;
            cout << FG_GOLD_DARK << BOX_TR << RESET << "\n";
            for (int i = 0; i < pad; ++i) cout << ' ';
            cout << FG_GOLD_ACCENT << BOX_AL << RESET;
            for (int c = 0; c < COLS; ++c) cout << FG_GOLD_ACCENT << BOX_AH << RESET;
            cout << FG_GOLD_ACCENT << BOX_AR << RESET << "\n";
        }

        Point head = snake.front();
        bool show_wide_head = consuming && head.c < COLS - 1;

        for (int r = 0; r < ROWS; ++r) {
            for (int i = 0; i < pad; ++i) cout << ' ';

            if (EGYPTIAN_FRAME) {
                if ((r % 3) == 1) cout << FG_GOLD_MAIN << GLYPH_ANKH << RESET;
                else              cout << FG_GOLD_MAIN << BOX_V     << RESET;
            } else {
                cout << FG_GOLD_MAIN << BOX_V << RESET;
            }

            bool invert = (level_flash > 0 && ((level_flash / 2) % 2) == 0);
            if (invert) cout << "\x1b[7m";
            if (USE_BLUE_BG) cout << BG_BLUE;
            cout << FG_WHITE;

            for (int c = 0; c < COLS; ++c) {
                // 1) Floating text overlays everything
                if (draw_float_at(r, c)) continue;

                // 2) Explosions
                bool drew_boom = false;
                for (const auto &b : booms) {
                    if ((r == b.center.r && c == b.center.c)) {
                        cout << FG_ORANGE_208 << "✹" << FG_WHITE;
                        drew_boom = true; break;
                    }
                    for (const auto &p : b.ring) {
                        if (p.r == r && p.c == c) {
                            cout << ((b.frames_left % 2) ? FG_RED : FG_YELLOW)
                                 << ((b.frames_left % 2) ? "+" : "×") << FG_WHITE;
                            drew_boom = true; break;
                        }
                    }
                    if (drew_boom) break;
                }
                if (drew_boom) continue;

                // 3) Big head
                if (show_wide_head && r == head.r && c == head.c) {
                    std::cout << WIDE_HEAD; ++c; continue;
                }

                // 4) Food
                if (food.r == r && food.c == c) { cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE; continue; }

                // 5) Head (normal)
                if ((!show_wide_head || r != head.r || c != head.c) && r == head.r && c == head.c) {
                    cout << FG_BRIGHT_GREEN << "●" << FG_WHITE; continue;
                }

                // 6) Body
                bool on_body = false;
                for (size_t i = 1; i < snake.size(); ++i) {
                    if (snake[i].r == r && snake[i].c == c) { on_body = true; break; }
                }
                if (on_body) { cout << FG_BRIGHT_GREEN << "●" << FG_WHITE; continue; }

                // 7) Poop / Bomb
                PoopState st;
                if (cell_has_good_or_bomb(r, c, &st)) {
                    if (st == PoopState::Good) {
                        cout << FG_BROWN_256 << "●" << FG_WHITE;
                    } else {
                        bool flash = ((std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count()/240) % 2) == 0;
                        cout << (flash ? FG_RED : FG_ORANGE_208) << "✹" << FG_WHITE;
                    }
                } else {
                    cout << ' ';
                }
            }

            if (EGYPTIAN_FRAME) {
                if ((r % 3) == 1) cout << RESET << FG_GOLD_MAIN << GLYPH_ANKH << RESET << "\n";
                else              cout << RESET << FG_GOLD_MAIN << BOX_V     << RESET << "\n";
            } else {
                cout << RESET << FG_GOLD_MAIN << BOX_V << RESET << "\n";
            }
        }

        // Bottom cap
        if (EGYPTIAN_FRAME) {
            print_egyptian_bevel(pad);
            print_egyptian_bar(pad, /*top=*/false);
        } else {
            for (int i = 0; i < pad; ++i) cout << ' ';
            cout << FG_GOLD_ACCENT << BOX_AL << RESET;
            for (int c = 0; c < COLS; ++c) cout << FG_GOLD_ACCENT << BOX_AH << RESET;
            cout << FG_GOLD_ACCENT << BOX_AR << RESET << "\n";

            for (int i = 0; i < pad; ++i) cout << ' ';
            cout << FG_GOLD_DARK << BOX_BL << RESET;
            for (int c = 0; c < COLS; ++c) cout << FG_GOLD_MAIN << BOX_H << RESET;
            cout << FG_GOLD_DARK << BOX_BR << RESET << "\n";
        }

        center_line("W/A/S/D to move, Q to quit.");
        if (game_over) center_line("Game Over. Press Q to exit.");
        if (level_flash > 0) center_line("\x1b[1m\x1b[93mLEVEL UP!  Speed increased\x1b[0m");

        cout.flush();
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    RawTerm raw;

    // Splash (title theme starts/stops internally) + credit roll
    cinematic_splash_and_wait();

    // Start quiet background loop for gameplay
    start_bg_music();

    Game game;
    game.refresh_idle_threshold();

    int tick_ms = BASE_TICK_MS;
    auto current_tick = chrono::milliseconds(tick_ms);
    auto next_tick = chrono::steady_clock::now();

    while (running.load()) {
        bool steered_this_frame = false;
        while (true) {
            auto k = read_key_now();
            if (!k) break;
            if (*k == 3) { running.store(false); break; } // Ctrl-C
            enqueue(*k);
        }

        if (auto key = poll_key()) {
            if (*key == 'Q') { running.store(false); break; }
            else {
                game.change_dir(*key);
                steered_this_frame = true;
            }
        }
        if (steered_this_frame) game.on_player_input();

        auto now = chrono::steady_clock::now();
        if (now >= next_tick) {
            while (now >= next_tick) {
                game.speed_bump_trigger = false;
                game.speed_bump_amount  = 0;

                game.update();

                if (game.slow_down_trigger) {
                    game.slow_down_trigger = false;
                    tick_ms = BASE_TICK_MS;
                    current_tick = chrono::milliseconds(tick_ms);
                    game.speed_bump_trigger = false;
                    game.speed_bump_amount  = 0;
                } else {
                    if (game.level_up_trigger) {
                        game.level_up_trigger = false;
                        tick_ms = std::max(MIN_TICK_MS, tick_ms - TICK_DECR_MS);
                        current_tick = chrono::milliseconds(tick_ms);
                    }
                    if (game.speed_bump_trigger && game.speed_bump_amount > 0) {
                        int total = GROW_DECR_MS * game.speed_bump_amount;
                        tick_ms = std::max(MIN_TICK_MS, tick_ms - total);
                        current_tick = chrono::milliseconds(tick_ms);
                    }
                }

                next_tick += current_tick;
            }

            flush_sound();
            game.render();
        } else {
            this_thread::sleep_until(next_tick);
        }
    }

    stop_bg_music();
    stop_title_music();

    cout << RESET << "\x1b[2J\x1b[H";
    cout << "Thanks for playing.\n";
    return 0;
}
