// snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// Features:
// - Blue playfield, green snake ("●"), yellow food ("●")
// - Pac-Man-ish gulp overlay when eating, plus score increment
// - Poop trail: 3 brown dots after each eat, fading over time
// - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// - Splash: inline PNG if kitty/iTerm2 supports it; otherwise colored ASCII splash
//   (No external Preview/Quick Look window; stays inside terminal)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/stat.h>

using namespace std;

// ---------- Sound config ----------
static constexpr bool ENABLE_SOUNDS = true;
static constexpr bool ENABLE_BEEP_FALLBACK = false;

static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
static constexpr const char* FART_SOUND   = "Submarine";
static constexpr const char* SPLASH_SOUND = "Purr"; // soft hiss/rumble

static inline void play_system_sound(const char* name) {
    if (!ENABLE_SOUNDS || name == nullptr) return;
    std::string cmd = "afplay '/System/Library/Sounds/" + std::string(name) + ".aiff' >/dev/null 2>&1 &";
    (void)std::system(cmd.c_str());
    if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
}
static inline void play_random_bite_sound(std::mt19937& rng) {
    std::uniform_int_distribution<int> dist(0, (int)(sizeof(BITE_SOUNDS)/sizeof(BITE_SOUNDS[0])) - 1);
    play_system_sound(BITE_SOUNDS[dist(rng)]);
}

// ---------- ANSI colors ----------
static constexpr const char* RESET = "\x1b[0m";
static constexpr const char* BG_BLUE = "\x1b[44m";
static constexpr const char* FG_WHITE = "\x1b[37m";
static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
static constexpr const char* FG_GRAY          = "\x1b[90m";
static constexpr const char* FG_GREEN         = "\x1b[32m";

// ---------- Config ----------
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
static constexpr chrono::milliseconds TICK{100}; // 10 FPS
static constexpr int POOP_TTL = 12;              // poop fades

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
    if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
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

// ---------- Small helpers ----------
static bool have_cmd(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
static bool file_exists(const char* p) {
    struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static void center_line(const std::string& s) {
    int w = COLS; int pad = std::max(0, (int)(w - (int)s.size()) / 2);
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << s << "\n";
}
static bool env_set(const char* name) { return std::getenv(name) != nullptr; }

// ---------- Splash (inline PNG if kitty/iTerm2; else ASCII) ----------
static constexpr const char* SPLASH_PATH = "assets/splash.png";

static void ascii_splash_art() {
    const char* G  = "\x1b[92m";   // green
    const char* Y  = "\x1b[93m";   // yellow
    const char* R  = "\x1b[91m";   // red
    const char* Wt = "\x1b[97m";   // white
    const char* Br = "\x1b[38;5;130m"; // brown
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
    center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
    std::cout << "\n";
    for (auto& line : art) center_line(line);
    std::cout << "\n";
}

static void cinematic_splash_and_wait() {
    using namespace std::chrono;

    // Clear and hide cursor
    std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;
    play_system_sound(SPLASH_SOUND); // soft hiss

    bool drew_inline_png = false;

    // True inline image paths
    if (env_set("KITTY_WINDOW_ID") && have_cmd("kitty") && file_exists(SPLASH_PATH)) {
        drew_inline_png = true;
        const char* sizes[] = { "40%", "70%", "100%" };
        for (const char* s : sizes) {
            std::cout << "\x1b[2J\x1b[H";
            std::string cmd = std::string("kitty +kitten icat --align center --place ")
                              + s + "x" + s + "@0x0 '" + SPLASH_PATH + "'";
            (void)std::system(cmd.c_str());
            center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
            std::this_thread::sleep_for(200ms);
        }
    } else if (env_set("ITERM_SESSION_ID") && have_cmd("imgcat") && file_exists(SPLASH_PATH)) {
        // Use imgcat only when actually inside iTerm2
        drew_inline_png = true;
        int widths[] = { 45, 70, 95 };
        for (int w : widths) {
            std::cout << "\x1b[2J\x1b[H";
            std::string cmd = "imgcat --width=" + std::to_string(w) + " '" + SPLASH_PATH + "'";
            int rc = std::system(cmd.c_str());
            if (rc != 0) { drew_inline_png = false; break; } // fallback to ASCII
            center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    if (!drew_inline_png) {
        // Always-works fallback that stays inside the terminal
        std::cout << "\x1b[2J\x1b[H";
        ascii_splash_art();
    }

    // Pulsing prompt
    bool bright = true;
    auto last = std::chrono::steady_clock::now();
    while (true) {
        if (auto k = read_key_now()) break;
        auto now = std::chrono::steady_clock::now();
        if (now - last >= 400ms) {
            bright = !bright; last = now;
            std::cout << "\r";
            std::string msg = bright
                ? std::string("\x1b[92m[ Press any key to continue ]\x1b[0m")
                : std::string("\x1b[32m[ Press any key to continue ]\x1b[0m");
            int w = COLS; int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
            for (int i = 0; i < pad; ++i) std::cout << ' ';
            std::cout << msg << std::flush;
        }
        std::this_thread::sleep_for(50ms);
    }

    // Restore cursor + clear before game
    std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
}

// ---------- Game model ----------
struct Point { int r, c; };
enum class Dir { Up, Down, Left, Right };
struct Poop { Point p; int ttl; };

struct Game {
    deque<Point> snake; // front=head
    Dir dir = Dir::Right;
    Point food{0, 0};
    bool game_over = false;
    int score = 0;

    // Bite animation
    bool consuming = false;
    int chomp_frames = 0;
    static constexpr int CHOMP_TOTAL = 8;

    // Poop
    int poop_to_drop = 0;
    vector<Poop> poops;

    mt19937 rng{random_device{}()};

    Game() {
        int r = ROWS / 2, c = COLS / 2;
        snake.push_back({r, c});
        snake.push_back({r, c - 1});
        snake.push_back({r, c - 2});
        place_food();
    }

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

    void decay_poops() {
        for (auto &pp : poops) pp.ttl--;
        poops.erase(remove_if(poops.begin(), poops.end(),
                              [](const Poop& p){ return p.ttl <= 0; }),
                    poops.end());
    }

    void update() {
        if (game_over) return;

        decay_poops();

        if (consuming) {
            if (--chomp_frames <= 0) {
                Point nh = next_head(snake.front());
                if (any_of(snake.begin(), snake.end(),
                           [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
                    game_over = true; return;
                }
                snake.push_front(nh); // grow
                score += 10;

                play_random_bite_sound(rng);
                poop_to_drop = 3;

                place_food();
                consuming = false;
            }
            return;
        }

        Point head = snake.front();
        Point nh = next_head(head);

        if (nh.r == food.r && nh.c == food.c) {
            consuming = true;
            chomp_frames = CHOMP_TOTAL;
            return;
        }

        if (any_of(snake.begin(), snake.end(),
                   [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
            game_over = true; return;
        }

        Point tail_before = snake.back();
        snake.push_front(nh);
        snake.pop_back();

        if (poop_to_drop > 0) {
            poops.push_back(Poop{tail_before, POOP_TTL});
            poop_to_drop--;
            play_system_sound(FART_SOUND);
        }
    }

    // Pac-Man style circular overlay (5x5) for gulp
    bool pac_overlay(int r, int c, const char*& outGlyph) const {
        if (!consuming || snake.empty()) return false;

        int phase = CHOMP_TOTAL - chomp_frames;
        Point h = snake.front();

        auto wrap_delta = [](int d, int maxv){
            if (d >  maxv/2) d -= maxv;
            if (d < -maxv/2) d += maxv;
            return d;
        };
        int dy = wrap_delta(r - h.r, ROWS);
        int dx = wrap_delta(c - h.c, COLS);

        if (abs(dx) > 2 || abs(dy) > 2) return false;

        double radius = (phase <= 1) ? 2.4 : (phase <= 3) ? 2.2 : 2.0;
        double r2 = dx*dx + dy*dy;
        if (r2 > radius*radius) return false;

        int vx = 0, vy = 0;
        switch (dir) {
            case Dir::Right: vx = 1; vy = 0; break;
            case Dir::Left:  vx = -1; vy = 0; break;
            case Dir::Up:    vx = 0; vy = -1; break;
            case Dir::Down:  vx = 0; vy = 1; break;
        }

        int mouth_band     = (phase <= 1) ? 2 : (phase <= 3) ? 1 : 0;
        int forward_thresh = (phase <= 1) ? 0 : (phase <= 3) ? 1 : 99;

        int forward = vx*dx + vy*dy;
        int perp    = (-vy)*dx + (vx)*dy;

        bool in_mouth_open = (forward >= forward_thresh) && (abs(perp) <= mouth_band);
        if (in_mouth_open) return false;

        outGlyph = "█";
        return true;
    }

    bool cell_has_poop(int rr, int cc) const {
        for (const auto& p : poops) {
            if (p.p.r == rr && p.p.c == cc) return true;
        }
        return false;
    }

    void render() const {
        cout << "\x1b[2J\x1b[H";
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+  Score: " << score
             << (consuming ? "   (CHOMP!)" : "")
             << (poop_to_drop > 0 ? "   (Dropping...)" : "")
             << "\n";

        for (int r = 0; r < ROWS; ++r) {
            cout << '|';
            cout << BG_BLUE << FG_WHITE;

            for (int c = 0; c < COLS; ++c) {
                const char* overlayGlyph = nullptr;
                if (pac_overlay(r, c, overlayGlyph)) {
                    cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
                    continue;
                }
                if (food.r == r && food.c == c) {
                    cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
                    continue;
                }
                bool on_snake = false;
                for (const auto& seg : snake) {
                    if (seg.r == r && seg.c == c) { on_snake = true; break; }
                }
                if (on_snake) {
                    cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
                    continue;
                }
                if (cell_has_poop(r, c)) {
                    cout << FG_BROWN_256 << "●" << FG_WHITE;
                } else {
                    cout << ' ';
                }
            }

            cout << RESET << "|\n";
        }

        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+\n";
        cout << "W/A/S/D to move, Q to quit.\n";
        if (game_over) cout << "Game Over. Press Q to exit.\n";
        cout.flush();
    }
};

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    RawTerm raw;

    // Inline PNG if supported (kitty/iTerm2), else colored ASCII — all inside terminal
    cinematic_splash_and_wait();

    Game game;
    auto next_tick = chrono::steady_clock::now();

    while (running.load()) {
        // pump raw keys
        while (true) {
            auto k = read_key_now();
            if (!k) break;
            if (*k == 3) { running.store(false); break; } // Ctrl-C
            enqueue(*k);
        }

        if (auto key = poll_key()) {
            if (*key == 'Q') running.store(false);
            else game.change_dir(*key);
        }

        auto now = chrono::steady_clock::now();
        if (now >= next_tick) {
            while (now >= next_tick) {
                game.update();
                next_tick += TICK;
            }
            game.render();
        } else {
            this_thread::sleep_until(next_tick);
        }
    }

    cout << RESET << "\x1b[2J\x1b[H";
    cout << "Thanks for playing.\n";
    return 0;
}


// // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // Features kept:
// // - Blue playfield, green snake, yellow food
// // - Pac-Man gulp animation (round 5x5 head opens/closes)
// // - Poop trail: 3 brown dots after each eat, fading
// // - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// // - PNG splash screen: now cinematic (hiss + zoom + pulsing prompt)

// #include <algorithm>
// #include <atomic>
// #include <chrono>
// #include <cctype>
// #include <cmath>
// #include <deque>
// #include <iostream>
// #include <mutex>
// #include <optional>
// #include <queue>
// #include <random>
// #include <string>
// #include <thread>
// #include <vector>
// #include <termios.h>
// #include <unistd.h>
// #include <cstdlib>
// #include <sys/stat.h>

// using namespace std;

// // --- Sound config ---
// static constexpr bool ENABLE_SOUNDS = true;
// static constexpr bool ENABLE_BEEP_FALLBACK = false;

// static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
// static constexpr const char* FART_SOUND   = "Submarine";
// static constexpr const char* SPLASH_SOUND = "Purr";     // soft hiss/rumble on splash

// static inline void play_system_sound(const char* name) {
//     if (!ENABLE_SOUNDS || name == nullptr) return;
//     std::string cmd = "afplay '/System/Library/Sounds/" + std::string(name) + ".aiff' >/dev/null 2>&1 &";
//     (void)std::system(cmd.c_str());
//     if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
// }
// static inline void play_random_bite_sound(std::mt19937& rng) {
//     std::uniform_int_distribution<int> dist(0, (int)(sizeof(BITE_SOUNDS)/sizeof(BITE_SOUNDS[0])) - 1);
//     play_system_sound(BITE_SOUNDS[dist(rng)]);
// }

// // --- ANSI colors ---
// static constexpr const char* RESET = "\x1b[0m";
// static constexpr const char* BG_BLUE = "\x1b[44m";
// static constexpr const char* FG_WHITE = "\x1b[37m";
// static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
// static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
// static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
// static constexpr const char* FG_GRAY          = "\x1b[90m";
// static constexpr const char* FG_GREEN         = "\x1b[32m";

// // --- Config ---
// static constexpr int ROWS = 20;
// static constexpr int COLS = 80;
// static constexpr chrono::milliseconds TICK{100}; // 10 FPS
// static constexpr int POOP_TTL = 12;              // poop fades

// // --- Raw terminal guard ---
// struct RawTerm {
//     termios orig{};
//     bool ok{false};
//     RawTerm() {
//         if (!isatty(STDIN_FILENO)) return;
//         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
//         termios raw = orig;
//         raw.c_lflag &= ~(ICANON | ECHO);
//         raw.c_cc[VMIN]  = 0;
//         raw.c_cc[VTIME] = 0;
//         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
//         ok = true;
//     }
//     ~RawTerm() {
//         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
//     }
// };

// // --- Input queue ---
// mutex in_mtx;
// queue<char> in_q;
// atomic<bool> running{true};

// optional<char> read_key_now() {
//     unsigned char ch;
//     ssize_t n = read(STDIN_FILENO, &ch, 1);
//     if (n == 1) return static_cast<char>(ch);
//     return nullopt;
// }
// void enqueue(char ch) {
//     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
//     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
//         lock_guard<mutex> lk(in_mtx);
//         in_q.push(ch);
//     }
// }
// optional<char> poll_key() {
//     lock_guard<mutex> lk(in_mtx);
//     if (in_q.empty()) return nullopt;
//     char c = in_q.front(); in_q.pop();
//     return c;
// }

// // ================= PNG splash helpers (robust path + file logging + Preview activate) =================
// #include <limits.h>
// #include <unistd.h>
// #include <ctime>
// #include <cstdio>

// static constexpr const char* SPLASH_REL = "assets/splash.png";
// static constexpr bool SPLASH_DEBUG = true;   // << keep true to write /tmp/snake_splash.log

// static void dbg(const std::string& msg) {
//     if (!SPLASH_DEBUG) return;
//     FILE* f = std::fopen("/tmp/snake_splash.log", "a");
//     if (!f) return;
//     std::time_t t = std::time(nullptr);
//     std::fprintf(f, "[%ld] %s\n", (long)t, msg.c_str());
//     std::fclose(f);
// }

// static std::string cwd_abs() {
//     char buf[PATH_MAX];
//     if (::getcwd(buf, sizeof(buf))) return std::string(buf);
//     return ".";
// }
// static std::string join_path(const std::string& a, const std::string& b) {
//     if (a.empty()) return b;
//     if (a.back() == '/') return a + b;
//     return a + "/" + b;
// }
// static std::string realpath_or(const std::string& p) {
//     char buf[PATH_MAX];
//     if (::realpath(p.c_str(), buf)) return std::string(buf);
//     return p;
// }
// static bool file_exists_abs(const std::string& p) {
//     struct stat st{}; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
// }
// static bool have_cmd(const char* name) {
//     std::string cmd = "command -v ";
//     cmd += name;
//     cmd += " >/dev/null 2>&1";
//     int rc = std::system(cmd.c_str());
//     dbg(std::string("have_cmd('") + name + "') rc=" + std::to_string(rc));
//     return rc == 0;
// }
// static void close_external_viewers() {
//     std::system("killall qlmanage >/dev/null 2>&1");
//     std::system("osascript -e 'tell application \"Preview\" to quit' >/dev/null 2>&1");
// }
// static void center_line(const std::string& s) {
//     int w = COLS;
//     int pad = std::max(0, (int)(w - (int)s.size()) / 2); // <-- use s.size(), not msg.size()
//     for (int i = 0; i < pad; ++i) std::cout << ' ';
//     std::cout << s << "\n";
// }


// static void cinematic_splash_and_wait() {
//     using namespace std::chrono;

//     const std::string joined = join_path(cwd_abs(), SPLASH_REL);
//     const std::string abs_splash = realpath_or(joined);
//     dbg(std::string("CWD=") + cwd_abs());
//     dbg(std::string("SPLASH_REL=") + SPLASH_REL);
//     dbg(std::string("joined=") + joined);
//     dbg(std::string("abs_splash=") + abs_splash);
//     dbg(std::string("exists=") + (file_exists_abs(abs_splash) ? "yes" : "no"));

//     std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;

//     bool used_external = false;
//     bool inline_img = false;

//     play_system_sound("Purr");

//     if (file_exists_abs(abs_splash)) {
//         if (std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty")) {
//             inline_img = true;
//             const char* sizes[] = { "40%", "70%", "100%" };
//             for (const char* s : sizes) {
//                 std::cout << "\x1b[2J\x1b[H";
//                 std::string cmd = std::string("kitty +kitten icat --align center --place ")
//                                   + s + "x" + s + "@0x0 '" + abs_splash + "'";
//                 dbg(std::string("RUN: ") + cmd);
//                 int rc = std::system(cmd.c_str());
//                 dbg(std::string("RET=") + std::to_string(rc));
//                 center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//                 std::this_thread::sleep_for(200ms);
//             }
//         } else if (std::getenv("ITERM_SESSION_ID") && have_cmd("imgcat")) {
//             inline_img = true;
// int widths[] = { 45, 70, 95 };
// for (int w : widths) {
//     std::cout << "\x1b[2J\x1b[H";
//     std::string cmd = "imgcat --width=" + std::to_string(w) + " '" + abs_splash + "'";
//     int rc = std::system(cmd.c_str());
//     if (rc != 0) { // imgcat failed — bail to Preview
//         inline_img = false;
//         break;
//     }
//     center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//     std::this_thread::sleep_for(std::chrono::milliseconds(200));
// }

//         }

//         if (!inline_img) {
//             // Reliable fallback: Preview (bring to front)
//             std::string cmd = "open -g -a Preview '" + abs_splash + "'";
//             dbg(std::string("RUN: ") + cmd);
//             int rc = std::system(cmd.c_str());
//             dbg(std::string("RET=") + std::to_string(rc));
//             // Activate Preview so it doesn’t hide on another Space
//             int rc2 = std::system("osascript -e 'tell application \"Preview\" to activate' >/dev/null 2>&1");
//             dbg(std::string("activate Preview rc=") + std::to_string(rc2));

//             used_external = true;
//             std::this_thread::sleep_for(150ms);
//             center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//         }
//     } else {
//         center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//         center_line("\x1b[90m(splash image missing)\x1b[0m");
//     }

//     bool bright = true;
//     auto last = steady_clock::now();
//     while (true) {
//         if (auto k = read_key_now()) break;

//         auto now = steady_clock::now();
//         if (now - last >= 400ms) {
//             bright = !bright;
//             last = now;
//             std::cout << "\r";
//             std::string msg = bright
//                 ? std::string("\x1b[92m[ Press any key to continue ]\x1b[0m")
//                 : std::string("\x1b[32m[ Press any key to continue ]\x1b[0m");
//             int w = COLS; int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
//             for (int i = 0; i < pad; ++i) std::cout << ' ';
//             std::cout << msg << std::flush;
//         }
//         std::this_thread::sleep_for(50ms);
//     }

//     if (used_external) close_external_viewers();

//     std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
// }
// // =====================================================================


// // --- Game model ---
// struct Point { int r, c; };
// enum class Dir { Up, Down, Left, Right };
// struct Poop { Point p; int ttl; };

// struct Game {
//     deque<Point> snake; // front=head
//     Dir dir = Dir::Right;
//     Point food{0, 0};
//     bool game_over = false;
//     int score = 0;

//     // Bite animation
//     bool consuming = false;
//     int chomp_frames = 0;
//     static constexpr int CHOMP_TOTAL = 8;

//     // Poop
//     int poop_to_drop = 0;
//     vector<Poop> poops;

//     mt19937 rng{random_device{}()};

//     Game() {
//         int r = ROWS / 2, c = COLS / 2;
//         snake.push_back({r, c});
//         snake.push_back({r, c - 1});
//         snake.push_back({r, c - 2});
//         place_food();
//     }

//     Point wrap(Point p) const {
//         if (p.r < 0) p.r = ROWS - 1;
//         if (p.r >= ROWS) p.r = 0;
//         if (p.c < 0) p.c = COLS - 1;
//         if (p.c >= COLS) p.c = 0;
//         return p;
//     }
//     Point next_head(Point head) const {
//         switch (dir) {
//             case Dir::Up:    head.r--; break;
//             case Dir::Down:  head.r++; break;
//             case Dir::Left:  head.c--; break;
//             case Dir::Right: head.c++; break;
//         }
//         return wrap(head);
//     }

//     void place_food() {
//         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
//         while (true) {
//             Point p{R(rng), C(rng)};
//             bool on_snake = any_of(snake.begin(), snake.end(),
//                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
//             if (!on_snake) { food = p; return; }
//         }
//     }

//     void change_dir(char key) {
//         auto opp = [&](Dir a, Dir b) {
//             return (a == Dir::Up && b == Dir::Down) ||
//                    (a == Dir::Down && b == Dir::Up) ||
//                    (a == Dir::Left && b == Dir::Right) ||
//                    (a == Dir::Right && b == Dir::Left);
//         };
//         Dir ndir = dir;
//         if (key == 'W') ndir = Dir::Up;
//         else if (key == 'S') ndir = Dir::Down;
//         else if (key == 'A') ndir = Dir::Left;
//         else if (key == 'D') ndir = Dir::Right;
//         if (!opp(dir, ndir)) dir = ndir;
//     }

//     void decay_poops() {
//         for (auto &pp : poops) pp.ttl--;
//         poops.erase(remove_if(poops.begin(), poops.end(),
//                               [](const Poop& p){ return p.ttl <= 0; }),
//                     poops.end());
//     }

//     void update() {
//         if (game_over) return;

//         decay_poops();

//         if (consuming) {
//             if (--chomp_frames <= 0) {
//                 Point nh = next_head(snake.front());
//                 if (any_of(snake.begin(), snake.end(),
//                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
//                     game_over = true; return;
//                 }
//                 snake.push_front(nh); // grow
//                 score += 10;

//                 play_random_bite_sound(rng);
//                 poop_to_drop = 3;

//                 place_food();
//                 consuming = false;
//             }
//             return;
//         }

//         Point head = snake.front();
//         Point nh = next_head(head);

//         if (nh.r == food.r && nh.c == food.c) {
//             consuming = true;
//             chomp_frames = CHOMP_TOTAL;
//             return;
//         }

//         if (any_of(snake.begin(), snake.end(),
//                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
//             game_over = true; return;
//         }

//         Point tail_before = snake.back();
//         snake.push_front(nh);
//         snake.pop_back();

//         if (poop_to_drop > 0) {
//             poops.push_back(Poop{tail_before, POOP_TTL});
//             poop_to_drop--;
//             play_system_sound(FART_SOUND);
//         }
//     }

//     // Pac-Man style circular overlay (5x5) for gulp
//     bool pac_overlay(int r, int c, const char*& outGlyph) const {
//         if (!consuming || snake.empty()) return false;

//         int phase = CHOMP_TOTAL - chomp_frames;
//         Point h = snake.front();

//         auto wrap_delta = [](int d, int maxv){
//             if (d >  maxv/2) d -= maxv;
//             if (d < -maxv/2) d += maxv;
//             return d;
//         };
//         int dy = wrap_delta(r - h.r, ROWS);
//         int dx = wrap_delta(c - h.c, COLS);

//         if (abs(dx) > 2 || abs(dy) > 2) return false;

//         double radius = (phase <= 1) ? 2.4 : (phase <= 3) ? 2.2 : 2.0;
//         double r2 = dx*dx + dy*dy;
//         if (r2 > radius*radius) return false;

//         int vx = 0, vy = 0;
//         switch (dir) {
//             case Dir::Right: vx = 1; vy = 0; break;
//             case Dir::Left:  vx = -1; vy = 0; break;
//             case Dir::Up:    vx = 0; vy = -1; break;
//             case Dir::Down:  vx = 0; vy = 1; break;
//         }

//         int mouth_band     = (phase <= 1) ? 2 : (phase <= 3) ? 1 : 0;
//         int forward_thresh = (phase <= 1) ? 0 : (phase <= 3) ? 1 : 99;

//         int forward = vx*dx + vy*dy;
//         int perp    = (-vy)*dx + (vx)*dy;

//         bool in_mouth_open = (forward >= forward_thresh) && (abs(perp) <= mouth_band);
//         if (in_mouth_open) return false;

//         outGlyph = "█";
//         return true;
//     }

//     bool cell_has_poop(int rr, int cc) const {
//         for (const auto& p : poops) {
//             if (p.p.r == rr && p.p.c == cc) return true;
//         }
//         return false;
//     }

//     void render() const {
//         cout << "\x1b[2J\x1b[H";
//         cout << '+';
//         for (int c = 0; c < COLS; ++c) cout << '-';
//         cout << "+  Score: " << score
//              << (consuming ? "   (CHOMP!)" : "")
//              << (poop_to_drop > 0 ? "   (Dropping...)" : "")
//              << "\n";

//         for (int r = 0; r < ROWS; ++r) {
//             cout << '|';
//             cout << BG_BLUE << FG_WHITE;

//             for (int c = 0; c < COLS; ++c) {
//                 const char* overlayGlyph = nullptr;
//                 if (pac_overlay(r, c, overlayGlyph)) {
//                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
//                     continue;
//                 }
//                 if (food.r == r && food.c == c) {
//                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
//                     continue;
//                 }
//                 bool on_snake = false;
//                 for (const auto& seg : snake) {
//                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
//                 }
//                 if (on_snake) {
//                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
//                     continue;
//                 }
//                 if (cell_has_poop(r, c)) {
//                     cout << FG_BROWN_256 << "●" << FG_WHITE;
//                 } else {
//                     cout << ' ';
//                 }
//             }

//             cout << RESET << "|\n";
//         }

//         cout << '+';
//         for (int c = 0; c < COLS; ++c) cout << '-';
//         cout << "+\n";
//         cout << "W/A/S/D to move, Q to quit.\n";
//         if (game_over) cout << "Game Over. Press Q to exit.\n";
//         cout.flush();
//     }
// };

// int main() {
//     ios::sync_with_stdio(false);
//     cin.tie(nullptr);

//     RawTerm raw;

//     // Cinematic PNG splash and wait for key
//     cinematic_splash_and_wait();

//     Game game;
//     auto next_tick = chrono::steady_clock::now();

//     while (running.load()) {
//         while (true) {
//             auto k = read_key_now();
//             if (!k) break;
//             if (*k == 3) { running.store(false); break; } // Ctrl-C
//             enqueue(*k);
//         }

//         if (auto key = poll_key()) {
//             if (*key == 'Q') running.store(false);
//             else game.change_dir(*key);
//         }

//         auto now = chrono::steady_clock::now();
//         if (now >= next_tick) {
//             while (now >= next_tick) {
//                 game.update();
//                 next_tick += TICK;
//             }
//             game.render();
//         } else {
//             this_thread::sleep_until(next_tick);
//         }
//     }

//     cout << RESET << "\x1b[2J\x1b[H";
//     cout << "Thanks for playing.\n";
//     return 0;
// }
