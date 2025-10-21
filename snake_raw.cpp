// snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// Features:
// - Blue playfield, green snake, yellow food
// - Pac-Man style gulp (round 5x5 head that opens then closes)
// - Poop trail: 3 brown dots after each eat, fading over time
// - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// - PNG splash screen shown via imgcat/kitty icat/Quick Look with "Press any key" prompt

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

// --- Sound config ---
static constexpr bool ENABLE_SOUNDS = true;
static constexpr bool ENABLE_BEEP_FALLBACK = false;

static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
static constexpr const char* FART_SOUND = "Submarine";

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

// --- ANSI colors ---
static constexpr const char* RESET = "\x1b[0m";
static constexpr const char* BG_BLUE = "\x1b[44m";
static constexpr const char* FG_WHITE = "\x1b[37m";
static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
static constexpr const char* FG_GRAY          = "\x1b[90m";
static constexpr const char* FG_CYAN          = "\x1b[96m";

// --- Config ---
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
static constexpr chrono::milliseconds TICK{100}; // 10 FPS
static constexpr int POOP_TTL = 12;              // poop fades

// --- Raw terminal guard ---
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

// --- Input queue ---
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

// ================= PNG splash helpers (AFTER COLS and read_key_now) =================
static constexpr const char* SPLASH_PATH = "assets/splash.png";

static bool file_exists(const char* p) {
    struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool have_cmd(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
static void close_external_viewers() {
    std::system("killall qlmanage >/dev/null 2>&1");
    std::system("osascript -e 'tell application \"Preview\" to quit' >/dev/null 2>&1");
}
static void graphic_splash_and_wait() {
    using namespace std::chrono;

    std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;

    bool showed_external = false;
    if (file_exists(SPLASH_PATH)) {
        if (have_cmd("imgcat")) {
            std::string cmd = "imgcat '" + std::string(SPLASH_PATH) + "'";
            (void)std::system(cmd.c_str());
        } else if (std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty")) {
            std::string cmd = "kitty +kitten icat --align center '" + std::string(SPLASH_PATH) + "'";
            (void)std::system(cmd.c_str());
        } else {
            std::string cmd = "qlmanage -p '" + std::string(SPLASH_PATH) + "' >/dev/null 2>&1 &";
            (void)std::system(cmd.c_str());
            showed_external = true;
        }
    }

    // Title
    auto center = [&](const std::string& s){
        int w = COLS; int pad = std::max(0, (int)(w - (int)s.size()) / 2);
        for (int i = 0; i < pad; ++i) std::cout << ' ';
        std::cout << s << "\n";
    };
    std::cout << "\n\n";
    center(std::string("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m"));
    std::cout << "\n";

    bool show = true;
    auto last = steady_clock::now();
    while (true) {
        if (auto k = read_key_now()) break;

        auto now = steady_clock::now();
        if (now - last >= 500ms) {
            show = !show;
            last = now;
            std::cout << "\r";
            std::string msg = show ? "\x1b[90m[Press any key to continue]\x1b[0m"
                                   : "                              ";
            int w = COLS; int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
            for (int i = 0; i < pad; ++i) std::cout << ' ';
            std::cout << msg << std::flush;
        }
        std::this_thread::sleep_for(50ms);
    }

    if (showed_external) close_external_viewers();
    std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
}
// =====================================================================

// --- Game model ---
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

    // Show PNG splash and wait for key
    graphic_splash_and_wait();

    Game game;
    auto next_tick = chrono::steady_clock::now();

    while (running.load()) {
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
// // Features:
// // - Blue playfield, green snake, yellow food
// // - Pac-Man style gulp (round 5x5 circular head that opens then closes)
// // - Poop trail: 3 brown dots after each eat, fading over time
// // - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// // - NEW: Splash screen with a large, fierce SNAKE (fangs + tongue) and clear poop piles

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

// using namespace std;

// // --- Sound config ---
// static constexpr bool ENABLE_SOUNDS = true;
// static constexpr bool ENABLE_BEEP_FALLBACK = false;

// static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
// static constexpr const char* FART_SOUND = "Submarine";

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
// static constexpr const char* BOLD  = "\x1b[1m";
// static constexpr const char* BG_BLUE = "\x1b[44m";
// static constexpr const char* FG_WHITE = "\x1b[37m";
// static constexpr const char* FG_BRIGHT_WHITE = "\x1b[97m";
// static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";   // food
// static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";   // snake
// static constexpr const char* FG_GREEN         = "\x1b[32m";
// static constexpr const char* FG_RED           = "\x1b[91m";
// static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m"; // poop (256-color)
// static constexpr const char* FG_GRAY          = "\x1b[90m";
// static constexpr const char* FG_CYAN          = "\x1b[96m";

// // --- Splash image helpers (macOS + terminals with inline image support) ---
// #include <sys/stat.h>

// static constexpr const char* SPLASH_PATH = "assets/splash.png";

// static bool file_exists(const char* p) {
//     struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
// }

// static bool have_cmd(const char* name) {
//     std::string cmd = "command -v ";
//     cmd += name;
//     cmd += " >/dev/null 2>&1";
//     int rc = std::system(cmd.c_str());
//     return rc == 0;
// }

// static void close_external_viewers() {
//     // Best-effort cleanup (no errors if nothing running)
//     std::system("killall qlmanage >/dev/null 2>&1");
//     std::system("osascript -e 'tell application \"Preview\" to quit' >/dev/null 2>&1");
// }

// // Shows the splash image (inline if possible; otherwise Quick Look/Preview), draws
// // a blinking "Press any key to continue" prompt, waits for any key, then closes viewers.
// static void graphic_splash_and_wait() {
//     using namespace std::chrono;

//     // Clear terminal and hide cursor for polish
//     std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;

//     bool showed_external = false;

//     if (file_exists(SPLASH_PATH)) {
//         // Prefer inline if terminal supports it
//         if (have_cmd("imgcat")) {
//             std::string cmd = "imgcat '" + std::string(SPLASH_PATH) + "'";
//             (void)std::system(cmd.c_str());
//         } else if (std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty")) {
//             std::string cmd = "kitty +kitten icat --align center '" + std::string(SPLASH_PATH) + "'";
//             (void)std::system(cmd.c_str());
//         } else {
//             // Fallback: Quick Look (separate window)
//             std::string cmd = "qlmanage -p '" + std::string(SPLASH_PATH) + "' >/dev/null 2>&1 &";
//             (void)std::system(cmd.c_str());
//             showed_external = true;
//         }
//     } else {
//         // No image found — just clear screen; gameplay will still continue
//     }

//     // Title + subtitle
//     std::cout << "\n\n";
//     auto center = [&](const std::string& s){
//         int w = COLS; int pad = std::max(0, (int)(w - (int)s.size()) / 2);
//         for (int i = 0; i < pad; ++i) std::cout << ' ';
//         std::cout << s << "\n";
//     };
//     center(std::string("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m"));
//     std::cout << "\n";

//     // Blink “Press any key…”
//     bool show = true;
//     auto last = steady_clock::now();
//     while (true) {
//         // non-blocking key poll using your existing raw input
//         if (auto k = read_key_now()) break;

//         auto now = steady_clock::now();
//         if (now - last >= 500ms) {
//             show = !show;
//             last = now;

//             // rewrite one line in place
//             std::cout << "\r";
//             std::string msg = show ? "\x1b[90m[Press any key to continue]\x1b[0m"
//                                    : "                              ";
//             int w = COLS; int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
//             for (int i = 0; i < pad; ++i) std::cout << ' ';
//             std::cout << msg << std::flush;
//         }
//         std::this_thread::sleep_for(50ms);
//     }

//     if (showed_external) close_external_viewers();

//     // Restore cursor and clear for game
//     std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
// }


// // --- Config ---
// static constexpr int ROWS = 20;
// static constexpr int COLS = 80;
// static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec
// static constexpr int POOP_TTL = 12; // poop fade

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

// // --- Centering helpers ---
// void draw_centered_line(const string& s, int width) {
//     // Strip ANSI when computing padding (rough; good enough for centering)
//     int visible = 0;
//     bool in_esc = false;
//     for (size_t i = 0; i < s.size(); ++i) {
//         unsigned char ch = (unsigned char)s[i];
//         if (!in_esc && ch == 0x1b) { in_esc = true; continue; }
//         if (in_esc) { if (ch == 'm') in_esc = false; continue; }
//         visible++;
//     }
//     int pad = max(0, (width - visible) / 2);
//     for (int i = 0; i < pad; ++i) cout << ' ';
//     cout << s << "\n";
// }

// // --- Splash screen (BIG angry snake + piles of poop) ---
// void draw_splash_frame(bool show_prompt) {
//     cout << "\x1b[2J\x1b[H"; // clear & home

//     draw_centered_line(string(BOLD) + FG_BRIGHT_GREEN + "S  N  A  K  E" + RESET, COLS);
//     cout << "\n";

//     // A large, clear, angry snake head (left-facing) with fangs + tongue
//     // and poop piles behind. Uses simple ASCII/Unicode for crispness.
//     vector<string> art = {
//         //              012345678901234567890123456789012345678901234567890
//         string(FG_BRIGHT_GREEN) + R"(                     ________________                                   )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(                 .-`                `-.                                )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(              .-'    )" + BOLD + "ANGRY SNAKE" + RESET + FG_BRIGHT_GREEN + R"(     `-.                           )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(            .'        ____________       `.                         )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(           /        .'            `.       \                        )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(          /        /   )" + FG_BRIGHT_WHITE + "◣  ◢" + FG_BRIGHT_GREEN + R"(    \       \                       )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         ;        ;   )" + FG_BRIGHT_WHITE + "◥◤" + FG_BRIGHT_GREEN + R"(    )" + FG_BRIGHT_WHITE + "◥◤" + FG_BRIGHT_GREEN + R"(   ;       ;                      )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         |   ____  \      ____      /  ____ |                      )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         |  / __ \  `-.  (____)  .-'  / __\|                      )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         | | (  ) |    `-.____.-'    | (  ) |   )" + FG_RED + "V" + FG_BRIGHT_GREEN + R"(   )" + FG_BRIGHT_YELLOW + " ●" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         |  \__/  |       )" + FG_BRIGHT_WHITE + "VV" + FG_BRIGHT_GREEN + R"(        |  \__/  |                      )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         |        |    .-`      `-.    |       |                      )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(         ;        ;  .'   ____     `.  ;       ;  )" + FG_BROWN_256 + "   ●  ●  ●" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(          \      /  /   .'    `.     \  \     /   )" + FG_BROWN_256 + "  ●●● ●●●" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(           `.__.'  /   /  SSSS  \     \  `-.-'    )" + FG_BROWN_256 + "   ●●●" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(                   \   \  SSSS  /     /          )" + FG_BROWN_256 + "  ● ● ●" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(                    `.  `-.__.-'   .'                                 )" + RESET,
//         string(FG_BRIGHT_GREEN) + R"(                      `-.______.-'                                   )" + RESET
//     };

//     for (auto& line : art) draw_centered_line(line, COLS);

//     cout << "\n";
//     draw_centered_line(string(FG_CYAN) + "Fierce. Hungry. Pooping." + RESET, COLS);
//     cout << "\n";

//     if (show_prompt) {
//         draw_centered_line(string(FG_GRAY) + "[Press any key to continue]" + RESET, COLS);
//     } else {
//         draw_centered_line(string(" "), COLS);
//     }

//     cout.flush();
// }

// void wait_for_any_key_splash() {
//     using namespace std::chrono;
//     auto last_toggle = steady_clock::now();
//     bool show_prompt = true;

//     // Hide cursor during splash for polish
//     cout << "\x1b[?25l" << flush;

//     while (true) {
//         draw_splash_frame(show_prompt);

//         if (auto k = read_key_now()) {
//             cout << "\x1b[?25h" << flush; // show cursor again
//             return;
//         }

//         this_thread::sleep_for(chrono::milliseconds(250));
//         auto now = steady_clock::now();
//         if (now - last_toggle >= chrono::milliseconds(500)) {
//             show_prompt = !show_prompt;
//             last_toggle = now;
//         }
//     }
// }

// // --- Game model ---
// struct Point { int r, c; };
// enum class Dir { Up, Down, Left, Right };
// struct Poop { Point p; int ttl; };

// struct Game {
//     deque<Point> snake; // front = head
//     Dir dir = Dir::Right;
//     Point food{0, 0};
//     bool game_over = false;
//     int score = 0;

//     // Bite animation state
//     bool consuming = false;
//     int chomp_frames = 0;                 // countdown
//     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

//     // Poop state
//     int poop_to_drop = 0;   // after eating, drop 3 poops over next 3 moves
//     vector<Poop> poops;     // fading droppings on the board

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

//         // Poops fade each tick
//         decay_poops();

//         // If we're mid-chomp, play frames without moving
//         if (consuming) {
//             if (--chomp_frames <= 0) {
//                 // Finalize: lunge into next cell and grow
//                 Point nh = next_head(snake.front());
//                 if (any_of(snake.begin(), snake.end(),
//                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
//                     game_over = true; return;
//                 }
//                 snake.push_front(nh); // grow (no pop)
//                 score += 10;

//                 // SOUND: randomized bite
//                 play_random_bite_sound(rng);

//                 // Begin poop drops
//                 poop_to_drop = 3;

//                 place_food();
//                 consuming = false;
//             }
//             return;
//         }

//         // Normal step
//         Point head = snake.front();
//         Point nh = next_head(head);

//         // If next step eats food, start chomp animation (pause movement)
//         if (nh.r == food.r && nh.c == food.c) {
//             consuming = true;
//             chomp_frames = CHOMP_TOTAL;
//             return;
//         }

//         // Self collision
//         if (any_of(snake.begin(), snake.end(),
//                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
//             game_over = true; return;
//         }

//         // Move: remember tail to maybe drop poop there
//         Point tail_before = snake.back();
//         snake.push_front(nh);
//         snake.pop_back();

//         // Drop one poop per move for the next 3 moves
//         if (poop_to_drop > 0) {
//             poops.push_back(Poop{tail_before, POOP_TTL});
//             poop_to_drop--;
//             play_system_sound(FART_SOUND);
//         }
//     }

//     // Pac-Man style circular overlay (5x5) for the gulp
//     bool pac_overlay(int r, int c, const char*& outGlyph) const {
//         if (!consuming || snake.empty()) return false;

//         int phase = CHOMP_TOTAL - chomp_frames; // 0..7
//         Point h = snake.front();

//         auto wrap_delta = [](int d, int maxv){
//             if (d >  maxv/2) d -= maxv;
//             if (d < -maxv/2) d += maxv;
//             return d;
//         };
//         int dy = wrap_delta(r - h.r, ROWS);
//         int dx = wrap_delta(c - h.c, COLS);

//         if (abs(dx) > 2 || abs(dy) > 2) return false;

//         double radius =
//             (phase <= 1) ? 2.4 :
//             (phase <= 3) ? 2.2 :
//                            2.0;
//         double r2 = dx*dx + dy*dy;
//         if (r2 > radius*radius) return false;

//         int vx = 0, vy = 0;
//         switch (dir) {
//             case Dir::Right: vx = 1; vy = 0; break;
//             case Dir::Left:  vx = -1; vy = 0; break;
//             case Dir::Up:    vx = 0; vy = -1; break;
//             case Dir::Down:  vx = 0; vy = 1; break;
//         }

//         int mouth_band =
//             (phase <= 1) ? 2 :
//             (phase <= 3) ? 1 :
//                            0;
//         int forward_thresh =
//             (phase <= 1) ? 0 :
//             (phase <= 3) ? 1 :
//                            99;

//         int forward = vx*dx + vy*dy;
//         int perp    = (-vy)*dx + (vx)*dy;

//         bool in_mouth_open =
//             (forward >= forward_thresh) &&
//             (abs(perp) <= mouth_band);

//         if (in_mouth_open) return false; // let blue bg show wedge

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
//         cout << "\x1b[2J\x1b[H"; // Clear and home

//         // Top border + score
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
//                 // Gulp overlay first
//                 const char* overlayGlyph = nullptr;
//                 if (pac_overlay(r, c, overlayGlyph)) {
//                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
//                     continue;
//                 }

//                 // Food
//                 if (food.r == r && food.c == c) {
//                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
//                     continue;
//                 }

//                 // Snake
//                 bool on_snake = false;
//                 for (const auto& seg : snake) {
//                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
//                 }
//                 if (on_snake) {
//                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
//                     continue;
//                 }

//                 // Poop
//                 if (cell_has_poop(r, c)) {
//                     cout << FG_BROWN_256 << "●" << FG_WHITE;
//                 } else {
//                     cout << ' ';
//                 }
//             }

//             cout << RESET << "|\n";
//         }

//         // Bottom border
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

//     // >>> NEW: show the PNG splash and wait for a key
//     graphic_splash_and_wait();

//     // // Splash screen (any key continues)
//     // wait_for_any_key_splash();

//     Game game;
//     auto next_tick = chrono::steady_clock::now();

//     while (running.load()) {
//         // read all pending keys
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


// // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // Features:
// // // - Blue playfield, green snake, yellow food
// // // - Pac-Man style gulp (round 5x5 circular head that opens then closes)
// // // - Poop trail: 3 brown dots after each eat, fading over time
// // // - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// // // - NEW: Splash screen with fierce pooping snake ASCII art + "Press any key to continue"

// // #include <algorithm>
// // #include <atomic>
// // #include <chrono>
// // #include <cctype>
// // #include <cmath>
// // #include <deque>
// // #include <iostream>
// // #include <mutex>
// // #include <optional>
// // #include <queue>
// // #include <random>
// // #include <string>
// // #include <thread>
// // #include <vector>
// // #include <termios.h>
// // #include <unistd.h>
// // #include <cstdlib>

// // using namespace std;

// // // --- Sound config ---
// // static constexpr bool ENABLE_SOUNDS = true;            // master switch
// // static constexpr bool ENABLE_BEEP_FALLBACK = false;    // also emit terminal bell \a (fallback)

// // // Bite sounds to rotate through (non-blocking via afplay)
// // static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
// // static constexpr const char* FART_SOUND = "Submarine"; // poop sound

// // static inline void play_system_sound(const char* name) {
// //     if (!ENABLE_SOUNDS || name == nullptr) return;
// //     std::string cmd = "afplay '/System/Library/Sounds/" + std::string(name) + ".aiff' >/dev/null 2>&1 &";
// //     (void)std::system(cmd.c_str());
// //     if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
// // }

// // static inline void play_random_bite_sound(std::mt19937& rng) {
// //     std::uniform_int_distribution<int> dist(0, (int)(sizeof(BITE_SOUNDS)/sizeof(BITE_SOUNDS[0])) - 1);
// //     play_system_sound(BITE_SOUNDS[dist(rng)]);
// // }

// // // --- ANSI colors ---
// // static constexpr const char* RESET = "\x1b[0m";
// // static constexpr const char* BG_BLUE = "\x1b[44m";
// // static constexpr const char* FG_WHITE = "\x1b[37m";
// // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";     // food
// // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";     // snake
// // static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m"; // poop (256-color)
// // static constexpr const char* FG_GRAY          = "\x1b[90m";
// // static constexpr const char* FG_CYAN          = "\x1b[96m";

// // // --- Config ---
// // static constexpr int ROWS = 20;
// // static constexpr int COLS = 80;
// // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // Poop behavior
// // static constexpr int POOP_TTL = 12; // moves until a poop fades out

// // // --- Raw terminal guard (RAII) ---
// // struct RawTerm {
// //     termios orig{};
// //     bool ok{false};
// //     RawTerm() {
// //         if (!isatty(STDIN_FILENO)) return;
// //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// //         termios raw = orig;
// //         raw.c_lflag &= ~(ICANON | ECHO);
// //         raw.c_cc[VMIN]  = 0;
// //         raw.c_cc[VTIME] = 0; // non-blocking reads
// //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// //         ok = true;
// //     }
// //     ~RawTerm() {
// //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// //     }
// // };

// // // --- Input queue ---
// // mutex in_mtx;
// // queue<char> in_q;
// // atomic<bool> running{true};

// // optional<char> read_key_now() {
// //     unsigned char ch;
// //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// //     if (n == 1) return static_cast<char>(ch);
// //     return nullopt;
// // }
// // void enqueue(char ch) {
// //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// //         lock_guard<mutex> lk(in_mtx);
// //         in_q.push(ch);
// //     }
// // }
// // optional<char> poll_key() {
// //     lock_guard<mutex> lk(in_mtx);
// //     if (in_q.empty()) return nullopt;
// //     char c = in_q.front(); in_q.pop();
// //     return c;
// // }

// // // --- Splash screen ---
// // void draw_centered_line(const string& s, int width) {
// //     int pad = max(0, (width - (int)s.size()) / 2);
// //     for (int i = 0; i < pad; ++i) cout << ' ';
// //     cout << s << "\n";
// // }

// // void draw_splash_frame(bool show_prompt) {
// //     cout << "\x1b[2J\x1b[H"; // clear & home
// //     // Title
// //     draw_centered_line(string(FG_BRIGHT_GREEN) + "S N A K E" + RESET, COLS);
// //     cout << "\n";

// //     // Fierce pooping snake art (Unicode, colored)
// //     // Head (open mouth), body, and a few brown dots behind.
// //     vector<string> art = {
// //         string(FG_BRIGHT_GREEN) + R"(           ┌─────┐   )" + RESET,
// //         string(FG_BRIGHT_GREEN) + R"(   ◄───    │  ◜◝ │   )" + RESET + FG_BRIGHT_YELLOW + "  ●" + RESET,
// //         string(FG_BRIGHT_GREEN) + R"(  ██████►  │  ◟◞ │   )" + RESET,
// //         string(FG_BRIGHT_GREEN) + R"(           └─────┘   )" + RESET,
// //         string(FG_BRIGHT_GREEN) + R"(            █████    )" + RESET,
// //         string(FG_BRIGHT_GREEN) + R"(              ███    )" + RESET + FG_BROWN_256 + "   ● ● ●" + RESET
// //     };
// //     for (auto& line : art) { draw_centered_line(line, COLS); }

// //     cout << "\n";
// //     draw_centered_line(string(FG_CYAN) + "The Fierce Pooping Snake" + RESET, COLS);
// //     cout << "\n";

// //     if (show_prompt) {
// //         draw_centered_line(string(FG_GRAY) + "[Press any key to continue]" + RESET, COLS);
// //     } else {
// //         // blink off (draw an empty line for same spacing)
// //         draw_centered_line(string(" "), COLS);
// //     }

// //     cout.flush();
// // }

// // void wait_for_any_key_splash() {
// //     using namespace std::chrono;
// //     auto last_toggle = steady_clock::now();
// //     bool show_prompt = true;

// //     // Hide cursor during splash for polish
// //     cout << "\x1b[?25l" << flush;

// //     while (true) {
// //         // Render current frame
// //         draw_splash_frame(show_prompt);

// //         // Poll for any key (raw, non-blocking)
// //         if (auto k = read_key_now()) {
// //             // Show cursor again
// //             cout << "\x1b[?25h" << flush;
// //             return;
// //         }

// //         // Blink prompt ~2 Hz
// //         this_thread::sleep_for(chrono::milliseconds(250));
// //         auto now = steady_clock::now();
// //         if (now - last_toggle >= chrono::milliseconds(500)) {
// //             show_prompt = !show_prompt;
// //             last_toggle = now;
// //         }
// //     }
// // }

// // // --- Game model ---
// // struct Point { int r, c; };
// // enum class Dir { Up, Down, Left, Right };
// // struct Poop { Point p; int ttl; };

// // struct Game {
// //     deque<Point> snake; // front = head
// //     Dir dir = Dir::Right;
// //     Point food{0, 0};
// //     bool game_over = false;
// //     int score = 0;

// //     // Bite animation state
// //     bool consuming = false;
// //     int chomp_frames = 0;                 // countdown
// //     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

// //     // Poop state
// //     int poop_to_drop = 0;   // after eating, drop 3 poops over next 3 moves
// //     vector<Poop> poops;     // fading droppings on the board

// //     mt19937 rng{random_device{}()};

// //     Game() {
// //         int r = ROWS / 2, c = COLS / 2;
// //         snake.push_back({r, c});
// //         snake.push_back({r, c - 1});
// //         snake.push_back({r, c - 2});
// //         place_food();
// //     }

// //     Point wrap(Point p) const {
// //         if (p.r < 0) p.r = ROWS - 1;
// //         if (p.r >= ROWS) p.r = 0;
// //         if (p.c < 0) p.c = COLS - 1;
// //         if (p.c >= COLS) p.c = 0;
// //         return p;
// //     }

// //     Point next_head(Point head) const {
// //         switch (dir) {
// //             case Dir::Up:    head.r--; break;
// //             case Dir::Down:  head.r++; break;
// //             case Dir::Left:  head.c--; break;
// //             case Dir::Right: head.c++; break;
// //         }
// //         return wrap(head);
// //     }

// //     void place_food() {
// //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// //         while (true) {
// //             Point p{R(rng), C(rng)};
// //             bool on_snake = any_of(snake.begin(), snake.end(),
// //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// //             if (!on_snake) { food = p; return; }
// //         }
// //     }

// //     void change_dir(char key) {
// //         auto opp = [&](Dir a, Dir b) {
// //             return (a == Dir::Up && b == Dir::Down) ||
// //                    (a == Dir::Down && b == Dir::Up) ||
// //                    (a == Dir::Left && b == Dir::Right) ||
// //                    (a == Dir::Right && b == Dir::Left);
// //         };
// //         Dir ndir = dir;
// //         if (key == 'W') ndir = Dir::Up;
// //         else if (key == 'S') ndir = Dir::Down;
// //         else if (key == 'A') ndir = Dir::Left;
// //         else if (key == 'D') ndir = Dir::Right;
// //         if (!opp(dir, ndir)) dir = ndir;
// //     }

// //     void decay_poops() {
// //         for (auto &pp : poops) pp.ttl--;
// //         poops.erase(remove_if(poops.begin(), poops.end(),
// //                               [](const Poop& p){ return p.ttl <= 0; }),
// //                     poops.end());
// //     }

// //     void update() {
// //         if (game_over) return;

// //         // Poops fade each tick
// //         decay_poops();

// //         // If we're mid-chomp, play frames without moving
// //         if (consuming) {
// //             if (--chomp_frames <= 0) {
// //                 // Finalize: lunge into next cell and grow
// //                 Point nh = next_head(snake.front());
// //                 if (any_of(snake.begin(), snake.end(),
// //                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// //                     game_over = true; return;
// //                 }
// //                 snake.push_front(nh); // grow (no pop)
// //                 score += 10;

// //                 // SOUND: randomized bite (Pop/Bottle/Funk/Tink/Ping)
// //                 play_random_bite_sound(rng);

// //                 // Start dropping poops on subsequent moves
// //                 poop_to_drop = 3;

// //                 place_food();
// //                 consuming = false;
// //             }
// //             return;
// //         }

// //         // Normal step
// //         Point head = snake.front();
// //         Point nh = next_head(head);

// //         // If next step eats food, start chomp animation (pause movement)
// //         if (nh.r == food.r && nh.c == food.c) {
// //             consuming = true;
// //             chomp_frames = CHOMP_TOTAL;
// //             return;
// //         }

// //         // Self collision
// //         if (any_of(snake.begin(), snake.end(),
// //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// //             game_over = true; return;
// //         }

// //         // Move: remember tail to maybe drop poop there
// //         Point tail_before = snake.back();
// //         snake.push_front(nh);
// //         snake.pop_back();

// //         // Drop one poop per move for the next 3 moves
// //         if (poop_to_drop > 0) {
// //             poops.push_back(Poop{tail_before, POOP_TTL});
// //             poop_to_drop--;

// //             // SOUND: poop
// //             play_system_sound(FART_SOUND);
// //         }
// //     }

// //     // -------- Pac-Man style circular overlay ----------
// //     // Draws a 5x5 circle centered on the current head with a directional mouth wedge
// //     // that narrows over frames (open -> close). Returns true if (r,c) is part of overlay.
// //     bool pac_overlay(int r, int c, const char*& outGlyph) const {
// //         if (!consuming || snake.empty()) return false;

// //         int phase = CHOMP_TOTAL - chomp_frames; // 0..7
// //         Point h = snake.front();

// //         auto wrap_delta = [](int d, int maxv){
// //             if (d >  maxv/2) d -= maxv;
// //             if (d < -maxv/2) d += maxv;
// //             return d;
// //         };
// //         int dy = wrap_delta(r - h.r, ROWS);
// //         int dx = wrap_delta(c - h.c, COLS);

// //         if (abs(dx) > 2 || abs(dy) > 2) return false;

// //         double radius =
// //             (phase <= 1) ? 2.4 :
// //             (phase <= 3) ? 2.2 :
// //                            2.0;
// //         double r2 = dx*dx + dy*dy;
// //         if (r2 > radius*radius) return false;

// //         int vx = 0, vy = 0;
// //         switch (dir) {
// //             case Dir::Right: vx = 1; vy = 0; break;
// //             case Dir::Left:  vx = -1; vy = 0; break;
// //             case Dir::Up:    vx = 0; vy = -1; break;
// //             case Dir::Down:  vx = 0; vy = 1; break;
// //         }

// //         int mouth_band =
// //             (phase <= 1) ? 2 :
// //             (phase <= 3) ? 1 :
// //                            0;
// //         int forward_thresh =
// //             (phase <= 1) ? 0 :
// //             (phase <= 3) ? 1 :
// //                            99;

// //         int forward = vx*dx + vy*dy;
// //         int perp    = (-vy)*dx + (vx)*dy;

// //         bool in_mouth_open =
// //             (forward >= forward_thresh) &&
// //             (abs(perp) <= mouth_band);

// //         if (in_mouth_open) {
// //             return false; // let blue background show through
// //         }

// //         outGlyph = "█";
// //         return true;
// //     }

// //     bool cell_has_poop(int rr, int cc) const {
// //         for (const auto& p : poops) {
// //             if (p.p.r == rr && p.p.c == cc) return true;
// //         }
// //         return false;
// //     }

// //     void render() const {
// //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// //         // Top border + score
// //         cout << '+';
// //         for (int c = 0; c < COLS; ++c) cout << '-';
// //         cout << "+  Score: " << score
// //              << (consuming ? "   (CHOMP!)" : "")
// //              << (poop_to_drop > 0 ? "   (Dropping...)" : "")
// //              << "\n";

// //         for (int r = 0; r < ROWS; ++r) {
// //             cout << '|';
// //             cout << BG_BLUE << FG_WHITE;

// //             for (int c = 0; c < COLS; ++c) {
// //                 // Pac head overlay has highest priority during chomp
// //                 const char* overlayGlyph = nullptr;
// //                 if (pac_overlay(r, c, overlayGlyph)) {
// //                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
// //                     continue;
// //                 }

// //                 // Food
// //                 if (food.r == r && food.c == c) {
// //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// //                     continue;
// //                 }

// //                 // Snake has priority over poop
// //                 bool on_snake = false;
// //                 for (const auto& seg : snake) {
// //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// //                 }
// //                 if (on_snake) {
// //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// //                     continue;
// //                 }

// //                 // Poop (brown, fades)
// //                 if (cell_has_poop(r, c)) {
// //                     cout << FG_BROWN_256 << "●" << FG_WHITE;
// //                 } else {
// //                     cout << ' '; // empty playfield (blue bg shows)
// //                 }
// //             }

// //             cout << RESET << "|\n";
// //         }

// //         // Bottom border
// //         cout << '+';
// //         for (int c = 0; c < COLS; ++c) cout << '-';
// //         cout << "+\n";

// //         cout << "W/A/S/D to move, Q to quit.\n";
// //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// //         cout.flush();
// //     }
// // };

// // int main() {
// //     ios::sync_with_stdio(false);
// //     cin.tie(nullptr);

// //     RawTerm raw;

// //     // Splash screen (any key continues)
// //     wait_for_any_key_splash();

// //     Game game;
// //     auto next_tick = chrono::steady_clock::now();

// //     while (running.load()) {
// //         // read all pending keys (for WASD/quit)
// //         while (true) {
// //             auto k = read_key_now();
// //             if (!k) break;
// //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// //             enqueue(*k);
// //         }

// //         if (auto key = poll_key()) {
// //             if (*key == 'Q') running.store(false);
// //             else game.change_dir(*key);
// //         }

// //         auto now = chrono::steady_clock::now();
// //         if (now >= next_tick) {
// //             while (now >= next_tick) {
// //                 game.update();
// //                 next_tick += TICK;
// //             }
// //             game.render();
// //         } else {
// //             this_thread::sleep_until(next_tick);
// //         }
// //     }

// //     cout << RESET << "\x1b[2J\x1b[H";
// //     cout << "Thanks for playing.\n";
// //     return 0;
// // }


// // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // Playfield interior = blue
// // // // Snake body = bright green dots
// // // // Food = bright yellow dot
// // // // Bite animation: round 5x5 head grows & closes, then lunges
// // // // Poop: after each eat, drop 3 brown dots that fade
// // // // Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping) + Submarine on poop

// // // #include <algorithm>
// // // #include <atomic>
// // // #include <chrono>
// // // #include <cctype>
// // // #include <cmath>
// // // #include <deque>
// // // #include <iostream>
// // // #include <mutex>
// // // #include <optional>
// // // #include <queue>
// // // #include <random>
// // // #include <string>
// // // #include <thread>
// // // #include <vector>
// // // #include <termios.h>
// // // #include <unistd.h>
// // // #include <cstdlib>

// // // using namespace std;

// // // // --- Sound config ---
// // // static constexpr bool ENABLE_SOUNDS = true;            // master switch
// // // static constexpr bool ENABLE_BEEP_FALLBACK = false;    // also emit terminal bell \a (fallback)

// // // // A few nice, punchy macOS system sounds to rotate through on bite:
// // // static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
// // // static constexpr const char* FART_SOUND = "Submarine"; // poop sound

// // // static inline void play_system_sound(const char* name) {
// // //     if (!ENABLE_SOUNDS || name == nullptr) return;
// // //     std::string cmd = "afplay '/System/Library/Sounds/" + std::string(name) + ".aiff' >/dev/null 2>&1 &";
// // //     (void)std::system(cmd.c_str());
// // //     if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
// // // }

// // // static inline void play_random_bite_sound(std::mt19937& rng) {
// // //     std::uniform_int_distribution<int> dist(0, (int)(sizeof(BITE_SOUNDS)/sizeof(BITE_SOUNDS[0])) - 1);
// // //     play_system_sound(BITE_SOUNDS[dist(rng)]);
// // // }

// // // // --- ANSI colors ---
// // // static constexpr const char* RESET = "\x1b[0m";
// // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";   // food
// // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";   // snake
// // // static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m"; // poop (256-color)

// // // // --- Config ---
// // // static constexpr int ROWS = 20;
// // // static constexpr int COLS = 80;
// // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // Poop behavior
// // // static constexpr int POOP_TTL = 12; // moves until a poop fades out

// // // // --- Raw terminal guard (RAII) ---
// // // struct RawTerm {
// // //     termios orig{};
// // //     bool ok{false};
// // //     RawTerm() {
// // //         if (!isatty(STDIN_FILENO)) return;
// // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // //         termios raw = orig;
// // //         raw.c_lflag &= ~(ICANON | ECHO);
// // //         raw.c_cc[VMIN]  = 0;
// // //         raw.c_cc[VTIME] = 0;
// // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // //         ok = true;
// // //     }
// // //     ~RawTerm() {
// // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // //     }
// // // };

// // // // --- Input queue ---
// // // mutex in_mtx;
// // // queue<char> in_q;
// // // atomic<bool> running{true};

// // // optional<char> read_key_now() {
// // //     unsigned char ch;
// // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // //     if (n == 1) return static_cast<char>(ch);
// // //     return nullopt;
// // // }
// // // void enqueue(char ch) {
// // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // //         lock_guard<mutex> lk(in_mtx);
// // //         in_q.push(ch);
// // //     }
// // // }
// // // optional<char> poll_key() {
// // //     lock_guard<mutex> lk(in_mtx);
// // //     if (in_q.empty()) return nullopt;
// // //     char c = in_q.front(); in_q.pop();
// // //     return c;
// // // }

// // // // --- Game model ---
// // // struct Point { int r, c; };
// // // enum class Dir { Up, Down, Left, Right };
// // // struct Poop { Point p; int ttl; };

// // // struct Game {
// // //     deque<Point> snake; // front = head
// // //     Dir dir = Dir::Right;
// // //     Point food{0, 0};
// // //     bool game_over = false;
// // //     int score = 0;

// // //     // Bite animation state
// // //     bool consuming = false;
// // //     int chomp_frames = 0;                 // countdown
// // //     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

// // //     // Poop state
// // //     int poop_to_drop = 0;   // after eating, drop 3 poops over next 3 moves
// // //     vector<Poop> poops;     // fading droppings on the board

// // //     mt19937 rng{random_device{}()};

// // //     Game() {
// // //         int r = ROWS / 2, c = COLS / 2;
// // //         snake.push_back({r, c});
// // //         snake.push_back({r, c - 1});
// // //         snake.push_back({r, c - 2});
// // //         place_food();
// // //     }

// // //     Point wrap(Point p) const {
// // //         if (p.r < 0) p.r = ROWS - 1;
// // //         if (p.r >= ROWS) p.r = 0;
// // //         if (p.c < 0) p.c = COLS - 1;
// // //         if (p.c >= COLS) p.c = 0;
// // //         return p;
// // //     }

// // //     Point next_head(Point head) const {
// // //         switch (dir) {
// // //             case Dir::Up:    head.r--; break;
// // //             case Dir::Down:  head.r++; break;
// // //             case Dir::Left:  head.c--; break;
// // //             case Dir::Right: head.c++; break;
// // //         }
// // //         return wrap(head);
// // //     }

// // //     void place_food() {
// // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // //         while (true) {
// // //             Point p{R(rng), C(rng)};
// // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // //             if (!on_snake) { food = p; return; }
// // //         }
// // //     }

// // //     void change_dir(char key) {
// // //         auto opp = [&](Dir a, Dir b) {
// // //             return (a == Dir::Up && b == Dir::Down) ||
// // //                    (a == Dir::Down && b == Dir::Up) ||
// // //                    (a == Dir::Left && b == Dir::Right) ||
// // //                    (a == Dir::Right && b == Dir::Left);
// // //         };
// // //         Dir ndir = dir;
// // //         if (key == 'W') ndir = Dir::Up;
// // //         else if (key == 'S') ndir = Dir::Down;
// // //         else if (key == 'A') ndir = Dir::Left;
// // //         else if (key == 'D') ndir = Dir::Right;
// // //         if (!opp(dir, ndir)) dir = ndir;
// // //     }

// // //     void decay_poops() {
// // //         for (auto &pp : poops) pp.ttl--;
// // //         poops.erase(remove_if(poops.begin(), poops.end(),
// // //                               [](const Poop& p){ return p.ttl <= 0; }),
// // //                     poops.end());
// // //     }

// // //     void update() {
// // //         if (game_over) return;

// // //         // Poops fade each tick
// // //         decay_poops();

// // //         // If we're mid-chomp, play frames without moving
// // //         if (consuming) {
// // //             if (--chomp_frames <= 0) {
// // //                 // Finalize: lunge into next cell and grow
// // //                 Point nh = next_head(snake.front());
// // //                 if (any_of(snake.begin(), snake.end(),
// // //                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // //                     game_over = true; return;
// // //                 }
// // //                 snake.push_front(nh); // grow (no pop)
// // //                 score += 10;

// // //                 // SOUND: randomized bite (Pop/Bottle/Funk/Tink/Ping)
// // //                 play_random_bite_sound(rng);

// // //                 // Start dropping poops on subsequent moves
// // //                 poop_to_drop = 3;

// // //                 place_food();
// // //                 consuming = false;
// // //             }
// // //             return;
// // //         }

// // //         // Normal step
// // //         Point head = snake.front();
// // //         Point nh = next_head(head);

// // //         // If next step eats food, start chomp animation (pause movement)
// // //         if (nh.r == food.r && nh.c == food.c) {
// // //             consuming = true;
// // //             chomp_frames = CHOMP_TOTAL;
// // //             return;
// // //         }

// // //         // Self collision
// // //         if (any_of(snake.begin(), snake.end(),
// // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // //             game_over = true; return;
// // //         }

// // //         // Move: remember tail to maybe drop poop there
// // //         Point tail_before = snake.back();
// // //         snake.push_front(nh);
// // //         snake.pop_back();

// // //         // Drop one poop per move for the next 3 moves
// // //         if (poop_to_drop > 0) {
// // //             poops.push_back(Poop{tail_before, POOP_TTL});
// // //             poop_to_drop--;

// // //             // SOUND: poop
// // //             play_system_sound(FART_SOUND);
// // //         }
// // //     }

// // //     // -------- Pac-Man style circular overlay ----------
// // //     // Draws a 5x5 circle centered on the current head with a directional mouth wedge
// // //     // that narrows over frames (open -> close). Returns true if (r,c) is part of overlay.
// // //     bool pac_overlay(int r, int c, const char*& outGlyph) const {
// // //         if (!consuming || snake.empty()) return false;

// // //         int phase = CHOMP_TOTAL - chomp_frames; // 0..7
// // //         Point h = snake.front();

// // //         auto wrap_delta = [](int d, int maxv){
// // //             if (d >  maxv/2) d -= maxv;
// // //             if (d < -maxv/2) d += maxv;
// // //             return d;
// // //         };
// // //         int dy = wrap_delta(r - h.r, ROWS);
// // //         int dx = wrap_delta(c - h.c, COLS);

// // //         if (abs(dx) > 2 || abs(dy) > 2) return false;

// // //         double radius =
// // //             (phase <= 1) ? 2.4 :
// // //             (phase <= 3) ? 2.2 :
// // //                            2.0;
// // //         double r2 = dx*dx + dy*dy;
// // //         if (r2 > radius*radius) return false;

// // //         int vx = 0, vy = 0;
// // //         switch (dir) {
// // //             case Dir::Right: vx = 1; vy = 0; break;
// // //             case Dir::Left:  vx = -1; vy = 0; break;
// // //             case Dir::Up:    vx = 0; vy = -1; break;
// // //             case Dir::Down:  vx = 0; vy = 1; break;
// // //         }

// // //         int mouth_band =
// // //             (phase <= 1) ? 2 :
// // //             (phase <= 3) ? 1 :
// // //                            0;
// // //         int forward_thresh =
// // //             (phase <= 1) ? 0 :
// // //             (phase <= 3) ? 1 :
// // //                            99;

// // //         int forward = vx*dx + vy*dy;
// // //         int perp    = (-vy)*dx + (vx)*dy;

// // //         bool in_mouth_open =
// // //             (forward >= forward_thresh) &&
// // //             (abs(perp) <= mouth_band);

// // //         if (in_mouth_open) {
// // //             return false; // let blue background show through
// // //         }

// // //         outGlyph = "█";
// // //         return true;
// // //     }

// // //     bool cell_has_poop(int rr, int cc) const {
// // //         for (const auto& p : poops) {
// // //             if (p.p.r == rr && p.p.c == cc) return true;
// // //         }
// // //         return false;
// // //     }

// // //     void render() const {
// // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // //         // Top border + score
// // //         cout << '+';
// // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // //         cout << "+  Score: " << score
// // //              << (consuming ? "   (CHOMP!)" : "")
// // //              << (poop_to_drop > 0 ? "   (Dropping...)" : "")
// // //              << "\n";

// // //         for (int r = 0; r < ROWS; ++r) {
// // //             cout << '|';
// // //             cout << BG_BLUE << FG_WHITE;

// // //             for (int c = 0; c < COLS; ++c) {
// // //                 // Pac head overlay has highest priority during chomp
// // //                 const char* overlayGlyph = nullptr;
// // //                 if (pac_overlay(r, c, overlayGlyph)) {
// // //                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
// // //                     continue;
// // //                 }

// // //                 // Food (visible until the final lunge overwrites it)
// // //                 if (food.r == r && food.c == c) {
// // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // //                     continue;
// // //                 }

// // //                 // Snake has priority over poop
// // //                 bool on_snake = false;
// // //                 for (const auto& seg : snake) {
// // //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// // //                 }
// // //                 if (on_snake) {
// // //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // //                     continue;
// // //                 }

// // //                 // Poop (brown, fades over time)
// // //                 if (cell_has_poop(r, c)) {
// // //                     cout << FG_BROWN_256 << "●" << FG_WHITE;
// // //                 } else {
// // //                     cout << ' '; // empty playfield (blue bg shows)
// // //                 }
// // //             }

// // //             cout << RESET << "|\n";
// // //         }

// // //         // Bottom border
// // //         cout << '+';
// // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // //         cout << "+\n";

// // //         cout << "W/A/S/D to move, Q to quit.\n";
// // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // //         cout.flush();
// // //     }
// // // };

// // // int main() {
// // //     ios::sync_with_stdio(false);
// // //     cin.tie(nullptr);

// // //     RawTerm raw;
// // //     Game game;
// // //     auto next_tick = chrono::steady_clock::now();

// // //     while (running.load()) {
// // //         // read all pending keys
// // //         while (true) {
// // //             auto k = read_key_now();
// // //             if (!k) break;
// // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // //             enqueue(*k);
// // //         }

// // //         if (auto key = poll_key()) {
// // //             if (*key == 'Q') running.store(false);
// // //             else game.change_dir(*key);
// // //         }

// // //         auto now = chrono::steady_clock::now();
// // //         if (now >= next_tick) {
// // //             while (now >= next_tick) {
// // //                 game.update();
// // //                 next_tick += TICK;
// // //             }
// // //             game.render();
// // //         } else {
// // //             this_thread::sleep_until(next_tick);
// // //         }
// // //     }

// // //     cout << RESET << "\x1b[2J\x1b[H";
// // //     cout << "Thanks for playing.\n";
// // //     return 0;
// // // }


// // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // Playfield interior = blue
// // // // // Snake body = bright green dots
// // // // // Food = bright yellow dot
// // // // // Bite animation: round 5x5 head grows & closes, then lunges
// // // // // Poop: after each eat, drop 3 brown dots that fade
// // // // // NEW: Sound effects (afplay) — "Glass" on eat, "Submarine" on poop

// // // // #include <algorithm>
// // // // #include <atomic>
// // // // #include <chrono>
// // // // #include <cctype>
// // // // #include <cmath>
// // // // #include <deque>
// // // // #include <iostream>
// // // // #include <mutex>
// // // // #include <optional>
// // // // #include <queue>
// // // // #include <random>
// // // // #include <string>
// // // // #include <thread>
// // // // #include <vector>
// // // // #include <termios.h>
// // // // #include <unistd.h>
// // // // #include <cstdlib>

// // // // using namespace std;

// // // // // --- Sound config ---
// // // // static constexpr bool ENABLE_SOUNDS = true;            // master switch
// // // // static constexpr const char* BITE_SOUND = "Glass";     // built-in: Basso, Blow, Bottle, Frog, Funk, Glass, Hero, Morse, Ping, Pop, Purr, Sosumi, Submarine, Tink
// // // // static constexpr const char* FART_SOUND = "Submarine"; // not a real fart, but low & funny enough
// // // // static constexpr bool ENABLE_BEEP_FALLBACK = false;    // also emit terminal bell \a

// // // // static inline void play_system_sound(const char* name) {
// // // //     if (!ENABLE_SOUNDS || name == nullptr) return;
// // // //     // Play in background, suppress output; if afplay or file not found, fail silently.
// // // //     string cmd = "afplay '/System/Library/Sounds/" + string(name) + ".aiff' >/dev/null 2>&1 &";
// // // //     (void)std::system(cmd.c_str());
// // // //     if (ENABLE_BEEP_FALLBACK) { std::cout << '\a' << std::flush; }
// // // // }

// // // // // --- ANSI colors ---
// // // // static constexpr const char* RESET = "\x1b[0m";
// // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";   // food
// // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";   // snake
// // // // static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m"; // poop (256-color)

// // // // // --- Config ---
// // // // static constexpr int ROWS = 20;
// // // // static constexpr int COLS = 80;
// // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // Poop behavior
// // // // static constexpr int POOP_TTL = 12; // moves until a poop fades out

// // // // // --- Raw terminal guard (RAII) ---
// // // // struct RawTerm {
// // // //     termios orig{};
// // // //     bool ok{false};
// // // //     RawTerm() {
// // // //         if (!isatty(STDIN_FILENO)) return;
// // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // //         termios raw = orig;
// // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // //         raw.c_cc[VMIN]  = 0;
// // // //         raw.c_cc[VTIME] = 0;
// // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // //         ok = true;
// // // //     }
// // // //     ~RawTerm() {
// // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // //     }
// // // // };

// // // // // --- Input queue ---
// // // // mutex in_mtx;
// // // // queue<char> in_q;
// // // // atomic<bool> running{true};

// // // // optional<char> read_key_now() {
// // // //     unsigned char ch;
// // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // //     if (n == 1) return static_cast<char>(ch);
// // // //     return nullopt;
// // // // }
// // // // void enqueue(char ch) {
// // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // //         lock_guard<mutex> lk(in_mtx);
// // // //         in_q.push(ch);
// // // //     }
// // // // }
// // // // optional<char> poll_key() {
// // // //     lock_guard<mutex> lk(in_mtx);
// // // //     if (in_q.empty()) return nullopt;
// // // //     char c = in_q.front(); in_q.pop();
// // // //     return c;
// // // // }

// // // // // --- Game model ---
// // // // struct Point { int r, c; };
// // // // enum class Dir { Up, Down, Left, Right };
// // // // struct Poop { Point p; int ttl; };

// // // // struct Game {
// // // //     deque<Point> snake; // front = head
// // // //     Dir dir = Dir::Right;
// // // //     Point food{0, 0};
// // // //     bool game_over = false;
// // // //     int score = 0;

// // // //     // Bite animation state
// // // //     bool consuming = false;
// // // //     int chomp_frames = 0;                 // countdown
// // // //     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

// // // //     // Poop state
// // // //     int poop_to_drop = 0;   // after eating, drop 3 poops over next 3 moves
// // // //     vector<Poop> poops;     // fading droppings on the board

// // // //     mt19937 rng{random_device{}()};

// // // //     Game() {
// // // //         int r = ROWS / 2, c = COLS / 2;
// // // //         snake.push_back({r, c});
// // // //         snake.push_back({r, c - 1});
// // // //         snake.push_back({r, c - 2});
// // // //         place_food();
// // // //     }

// // // //     Point wrap(Point p) const {
// // // //         if (p.r < 0) p.r = ROWS - 1;
// // // //         if (p.r >= ROWS) p.r = 0;
// // // //         if (p.c < 0) p.c = COLS - 1;
// // // //         if (p.c >= COLS) p.c = 0;
// // // //         return p;
// // // //     }

// // // //     Point next_head(Point head) const {
// // // //         switch (dir) {
// // // //             case Dir::Up:    head.r--; break;
// // // //             case Dir::Down:  head.r++; break;
// // // //             case Dir::Left:  head.c--; break;
// // // //             case Dir::Right: head.c++; break;
// // // //         }
// // // //         return wrap(head);
// // // //     }

// // // //     void place_food() {
// // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // //         while (true) {
// // // //             Point p{R(rng), C(rng)};
// // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // //             if (!on_snake) { food = p; return; }
// // // //         }
// // // //     }

// // // //     void change_dir(char key) {
// // // //         auto opp = [&](Dir a, Dir b) {
// // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // //                    (a == Dir::Right && b == Dir::Left);
// // // //         };
// // // //         Dir ndir = dir;
// // // //         if (key == 'W') ndir = Dir::Up;
// // // //         else if (key == 'S') ndir = Dir::Down;
// // // //         else if (key == 'A') ndir = Dir::Left;
// // // //         else if (key == 'D') ndir = Dir::Right;
// // // //         if (!opp(dir, ndir)) dir = ndir;
// // // //     }

// // // //     void decay_poops() {
// // // //         for (auto &pp : poops) pp.ttl--;
// // // //         poops.erase(remove_if(poops.begin(), poops.end(),
// // // //                               [](const Poop& p){ return p.ttl <= 0; }),
// // // //                     poops.end());
// // // //     }

// // // //     void update() {
// // // //         if (game_over) return;

// // // //         // Poops fade each tick
// // // //         decay_poops();

// // // //         // If we're mid-chomp, play frames without moving
// // // //         if (consuming) {
// // // //             if (--chomp_frames <= 0) {
// // // //                 // Finalize: lunge into next cell and grow
// // // //                 Point nh = next_head(snake.front());
// // // //                 if (any_of(snake.begin(), snake.end(),
// // // //                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // //                     game_over = true; return;
// // // //                 }
// // // //                 snake.push_front(nh); // grow (no pop)
// // // //                 score += 10;

// // // //                 // SOUND: bite!
// // // //                 play_system_sound(BITE_SOUND);

// // // //                 // Start dropping poops on subsequent moves
// // // //                 poop_to_drop = 3;

// // // //                 place_food();
// // // //                 consuming = false;
// // // //             }
// // // //             return;
// // // //         }

// // // //         // Normal step
// // // //         Point head = snake.front();
// // // //         Point nh = next_head(head);

// // // //         // If next step eats food, start chomp animation (pause movement)
// // // //         if (nh.r == food.r && nh.c == food.c) {
// // // //             consuming = true;
// // // //             chomp_frames = CHOMP_TOTAL;
// // // //             return;
// // // //         }

// // // //         // Self collision
// // // //         if (any_of(snake.begin(), snake.end(),
// // // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // //             game_over = true; return;
// // // //         }

// // // //         // Move: remember tail to maybe drop poop there
// // // //         Point tail_before = snake.back();
// // // //         snake.push_front(nh);
// // // //         snake.pop_back();

// // // //         // Drop one poop per move for the next 3 moves
// // // //         if (poop_to_drop > 0) {
// // // //             poops.push_back(Poop{tail_before, POOP_TTL});
// // // //             poop_to_drop--;

// // // //             // SOUND: fart-ish
// // // //             play_system_sound(FART_SOUND);
// // // //         }
// // // //     }

// // // //     // -------- Pac-Man style circular overlay ----------
// // // //     // Draws a 5x5 circle centered on the current head with a directional mouth wedge
// // // //     // that narrows over frames (open -> close). Returns true if (r,c) is part of overlay.
// // // //     bool pac_overlay(int r, int c, const char*& outGlyph) const {
// // // //         if (!consuming || snake.empty()) return false;

// // // //         int phase = CHOMP_TOTAL - chomp_frames; // 0..7
// // // //         Point h = snake.front();

// // // //         auto wrap_delta = [](int d, int maxv){
// // // //             if (d >  maxv/2) d -= maxv;
// // // //             if (d < -maxv/2) d += maxv;
// // // //             return d;
// // // //         };
// // // //         int dy = wrap_delta(r - h.r, ROWS);
// // // //         int dx = wrap_delta(c - h.c, COLS);

// // // //         if (abs(dx) > 2 || abs(dy) > 2) return false;

// // // //         double radius =
// // // //             (phase <= 1) ? 2.4 :
// // // //             (phase <= 3) ? 2.2 :
// // // //                            2.0;
// // // //         double r2 = dx*dx + dy*dy;
// // // //         if (r2 > radius*radius) return false;

// // // //         int vx = 0, vy = 0;
// // // //         switch (dir) {
// // // //             case Dir::Right: vx = 1; vy = 0; break;
// // // //             case Dir::Left:  vx = -1; vy = 0; break;
// // // //             case Dir::Up:    vx = 0; vy = -1; break;
// // // //             case Dir::Down:  vx = 0; vy = 1; break;
// // // //         }

// // // //         int mouth_band =
// // // //             (phase <= 1) ? 2 :
// // // //             (phase <= 3) ? 1 :
// // // //                            0;
// // // //         int forward_thresh =
// // // //             (phase <= 1) ? 0 :
// // // //             (phase <= 3) ? 1 :
// // // //                            99;

// // // //         int forward = vx*dx + vy*dy;
// // // //         int perp    = (-vy)*dx + (vx)*dy;

// // // //         bool in_mouth_open =
// // // //             (forward >= forward_thresh) &&
// // // //             (abs(perp) <= mouth_band);

// // // //         if (in_mouth_open) {
// // // //             return false; // let blue background show through
// // // //         }

// // // //         outGlyph = "█";
// // // //         return true;
// // // //     }

// // // //     bool cell_has_poop(int rr, int cc) const {
// // // //         for (const auto& p : poops) {
// // // //             if (p.p.r == rr && p.p.c == cc) return true;
// // // //         }
// // // //         return false;
// // // //     }

// // // //     void render() const {
// // // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // // //         // Top border + score
// // // //         cout << '+';
// // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // //         cout << "+  Score: " << score
// // // //              << (consuming ? "   (CHOMP!)" : "")
// // // //              << (poop_to_drop > 0 ? "   (Dropping...)" : "")
// // // //              << "\n";

// // // //         for (int r = 0; r < ROWS; ++r) {
// // // //             cout << '|';
// // // //             cout << BG_BLUE << FG_WHITE;

// // // //             for (int c = 0; c < COLS; ++c) {
// // // //                 // Pac head overlay has highest priority during chomp
// // // //                 const char* overlayGlyph = nullptr;
// // // //                 if (pac_overlay(r, c, overlayGlyph)) {
// // // //                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
// // // //                     continue;
// // // //                 }

// // // //                 // Food (visible until the final lunge overwrites it)
// // // //                 if (food.r == r && food.c == c) {
// // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // //                     continue;
// // // //                 }

// // // //                 // Snake has priority over poop
// // // //                 bool on_snake = false;
// // // //                 for (const auto& seg : snake) {
// // // //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// // // //                 }
// // // //                 if (on_snake) {
// // // //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // // //                     continue;
// // // //                 }

// // // //                 // Poop (brown, fades over time)
// // // //                 if (cell_has_poop(r, c)) {
// // // //                     cout << FG_BROWN_256 << "●" << FG_WHITE;
// // // //                 } else {
// // // //                     cout << ' '; // empty playfield (blue bg shows)
// // // //                 }
// // // //             }

// // // //             cout << RESET << "|\n";
// // // //         }

// // // //         // Bottom border
// // // //         cout << '+';
// // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // //         cout << "+\n";

// // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // //         cout.flush();
// // // //     }
// // // // };

// // // // int main() {
// // // //     ios::sync_with_stdio(false);
// // // //     cin.tie(nullptr);

// // // //     RawTerm raw;
// // // //     Game game;
// // // //     auto next_tick = chrono::steady_clock::now();

// // // //     while (running.load()) {
// // // //         // read all pending keys
// // // //         while (true) {
// // // //             auto k = read_key_now();
// // // //             if (!k) break;
// // // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // // //             enqueue(*k);
// // // //         }

// // // //         if (auto key = poll_key()) {
// // // //             if (*key == 'Q') running.store(false);
// // // //             else game.change_dir(*key);
// // // //         }

// // // //         auto now = chrono::steady_clock::now();
// // // //         if (now >= next_tick) {
// // // //             while (now >= next_tick) {
// // // //                 game.update();
// // // //                 next_tick += TICK;
// // // //             }
// // // //             game.render();
// // // //         } else {
// // // //             this_thread::sleep_until(next_tick);
// // // //         }
// // // //     }

// // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // //     cout << "Thanks for playing.\n";
// // // //     return 0;
// // // // }


// // // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // // Playfield interior = blue
// // // // // // Snake body = bright green dots
// // // // // // Food = bright yellow dot
// // // // // // Bite animation: round 5x5 head grows & closes, then lunges
// // // // // // NEW: After each eat, snake drops 3 brown "poop" dots that fade away as it moves.

// // // // // #include <algorithm>
// // // // // #include <atomic>
// // // // // #include <chrono>
// // // // // #include <cctype>
// // // // // #include <cmath>
// // // // // #include <deque>
// // // // // #include <iostream>
// // // // // #include <mutex>
// // // // // #include <optional>
// // // // // #include <queue>
// // // // // #include <random>
// // // // // #include <thread>
// // // // // #include <vector>
// // // // // #include <termios.h>
// // // // // #include <unistd.h>

// // // // // using namespace std;

// // // // // // --- ANSI colors ---
// // // // // static constexpr const char* RESET = "\x1b[0m";
// // // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";   // food
// // // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";   // snake
// // // // // static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m"; // poop (256-color)

// // // // // // --- Config ---
// // // // // static constexpr int ROWS = 20;
// // // // // static constexpr int COLS = 80;
// // // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // // Poop behavior
// // // // // static constexpr int POOP_TTL = 12; // moves until a poop fades out

// // // // // // --- Raw terminal guard (RAII) ---
// // // // // struct RawTerm {
// // // // //     termios orig{};
// // // // //     bool ok{false};
// // // // //     RawTerm() {
// // // // //         if (!isatty(STDIN_FILENO)) return;
// // // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // // //         termios raw = orig;
// // // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // // //         raw.c_cc[VMIN]  = 0;
// // // // //         raw.c_cc[VTIME] = 0;
// // // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // // //         ok = true;
// // // // //     }
// // // // //     ~RawTerm() {
// // // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // // //     }
// // // // // };

// // // // // // --- Input queue ---
// // // // // mutex in_mtx;
// // // // // queue<char> in_q;
// // // // // atomic<bool> running{true};

// // // // // optional<char> read_key_now() {
// // // // //     unsigned char ch;
// // // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // // //     if (n == 1) return static_cast<char>(ch);
// // // // //     return nullopt;
// // // // // }
// // // // // void enqueue(char ch) {
// // // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // // //         lock_guard<mutex> lk(in_mtx);
// // // // //         in_q.push(ch);
// // // // //     }
// // // // // }
// // // // // optional<char> poll_key() {
// // // // //     lock_guard<mutex> lk(in_mtx);
// // // // //     if (in_q.empty()) return nullopt;
// // // // //     char c = in_q.front(); in_q.pop();
// // // // //     return c;
// // // // // }

// // // // // // --- Game model ---
// // // // // struct Point { int r, c; };
// // // // // enum class Dir { Up, Down, Left, Right };

// // // // // struct Poop { Point p; int ttl; };

// // // // // struct Game {
// // // // //     deque<Point> snake; // front = head
// // // // //     Dir dir = Dir::Right;
// // // // //     Point food{0, 0};
// // // // //     bool game_over = false;
// // // // //     int score = 0;

// // // // //     // Bite animation state
// // // // //     bool consuming = false;
// // // // //     int chomp_frames = 0;                 // countdown
// // // // //     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

// // // // //     // Poop state
// // // // //     int poop_to_drop = 0;   // after eating, drop 3 poops over next 3 moves
// // // // //     vector<Poop> poops;     // fading droppings on the board

// // // // //     mt19937 rng{random_device{}()};

// // // // //     Game() {
// // // // //         int r = ROWS / 2, c = COLS / 2;
// // // // //         snake.push_back({r, c});
// // // // //         snake.push_back({r, c - 1});
// // // // //         snake.push_back({r, c - 2});
// // // // //         place_food();
// // // // //     }

// // // // //     Point wrap(Point p) const {
// // // // //         if (p.r < 0) p.r = ROWS - 1;
// // // // //         if (p.r >= ROWS) p.r = 0;
// // // // //         if (p.c < 0) p.c = COLS - 1;
// // // // //         if (p.c >= COLS) p.c = 0;
// // // // //         return p;
// // // // //     }

// // // // //     Point next_head(Point head) const {
// // // // //         switch (dir) {
// // // // //             case Dir::Up:    head.r--; break;
// // // // //             case Dir::Down:  head.r++; break;
// // // // //             case Dir::Left:  head.c--; break;
// // // // //             case Dir::Right: head.c++; break;
// // // // //         }
// // // // //         return wrap(head);
// // // // //     }

// // // // //     void place_food() {
// // // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // // //         while (true) {
// // // // //             Point p{R(rng), C(rng)};
// // // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // // //             if (!on_snake) { food = p; return; }
// // // // //         }
// // // // //     }

// // // // //     void change_dir(char key) {
// // // // //         auto opp = [&](Dir a, Dir b) {
// // // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // // //                    (a == Dir::Right && b == Dir::Left);
// // // // //         };
// // // // //         Dir ndir = dir;
// // // // //         if (key == 'W') ndir = Dir::Up;
// // // // //         else if (key == 'S') ndir = Dir::Down;
// // // // //         else if (key == 'A') ndir = Dir::Left;
// // // // //         else if (key == 'D') ndir = Dir::Right;
// // // // //         if (!opp(dir, ndir)) dir = ndir;
// // // // //     }

// // // // //     void decay_poops() {
// // // // //         for (auto &pp : poops) pp.ttl--;
// // // // //         poops.erase(remove_if(poops.begin(), poops.end(),
// // // // //                               [](const Poop& p){ return p.ttl <= 0; }),
// // // // //                     poops.end());
// // // // //     }

// // // // //     void update() {
// // // // //         if (game_over) return;

// // // // //         // Poops fade every tick (keeps things tidy even if paused)
// // // // //         decay_poops();

// // // // //         // If we're mid-chomp, play frames without moving
// // // // //         if (consuming) {
// // // // //             if (--chomp_frames <= 0) {
// // // // //                 // Finalize: lunge into next cell and grow
// // // // //                 Point nh = next_head(snake.front());
// // // // //                 if (any_of(snake.begin(), snake.end(),
// // // // //                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // //                     game_over = true; return;
// // // // //                 }
// // // // //                 snake.push_front(nh); // grow (no pop)
// // // // //                 score += 10;

// // // // //                 // Start dropping 3 poops over the next 3 moves
// // // // //                 poop_to_drop = 3;

// // // // //                 place_food();
// // // // //                 consuming = false;
// // // // //             }
// // // // //             return;
// // // // //         }

// // // // //         // Normal step
// // // // //         Point head = snake.front();
// // // // //         Point nh = next_head(head);

// // // // //         // If next step eats food, start chomp animation (pause movement)
// // // // //         if (nh.r == food.r && nh.c == food.c) {
// // // // //             consuming = true;
// // // // //             chomp_frames = CHOMP_TOTAL;
// // // // //             return;
// // // // //         }

// // // // //         // Self collision
// // // // //         if (any_of(snake.begin(), snake.end(),
// // // // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // //             game_over = true; return;
// // // // //         }

// // // // //         // Move: track tail to possibly drop poop there
// // // // //         Point tail_before = snake.back();
// // // // //         snake.push_front(nh);
// // // // //         snake.pop_back();

// // // // //         // Drop poop on the cell that just vacated (tail) — one per move
// // // // //         if (poop_to_drop > 0) {
// // // // //             poops.push_back(Poop{tail_before, POOP_TTL});
// // // // //             poop_to_drop--;
// // // // //         }
// // // // //     }

// // // // //     // -------- Pac-Man style circular overlay ----------
// // // // //     // Draws a 5x5 circle centered on the current head with a directional mouth wedge
// // // // //     // that narrows over frames (open -> close). Returns true if (r,c) is part of overlay.
// // // // //     bool pac_overlay(int r, int c, const char*& outGlyph) const {
// // // // //         if (!consuming || snake.empty()) return false;

// // // // //         int phase = CHOMP_TOTAL - chomp_frames; // 0..7
// // // // //         Point h = snake.front();

// // // // //         auto wrap_delta = [](int d, int maxv){
// // // // //             if (d >  maxv/2) d -= maxv;
// // // // //             if (d < -maxv/2) d += maxv;
// // // // //             return d;
// // // // //         };
// // // // //         int dy = wrap_delta(r - h.r, ROWS);
// // // // //         int dx = wrap_delta(c - h.c, COLS);

// // // // //         if (abs(dx) > 2 || abs(dy) > 2) return false;

// // // // //         double radius =
// // // // //             (phase <= 1) ? 2.4 :
// // // // //             (phase <= 3) ? 2.2 :
// // // // //                            2.0;
// // // // //         double r2 = dx*dx + dy*dy;
// // // // //         if (r2 > radius*radius) return false;

// // // // //         int vx = 0, vy = 0;
// // // // //         switch (dir) {
// // // // //             case Dir::Right: vx = 1; vy = 0; break;
// // // // //             case Dir::Left:  vx = -1; vy = 0; break;
// // // // //             case Dir::Up:    vx = 0; vy = -1; break;
// // // // //             case Dir::Down:  vx = 0; vy = 1; break;
// // // // //         }

// // // // //         int mouth_band =
// // // // //             (phase <= 1) ? 2 :
// // // // //             (phase <= 3) ? 1 :
// // // // //                            0;
// // // // //         int forward_thresh =
// // // // //             (phase <= 1) ? 0 :
// // // // //             (phase <= 3) ? 1 :
// // // // //                            99;

// // // // //         int forward = vx*dx + vy*dy;
// // // // //         int perp    = (-vy)*dx + (vx)*dy;

// // // // //         bool in_mouth_open =
// // // // //             (forward >= forward_thresh) &&
// // // // //             (abs(perp) <= mouth_band);

// // // // //         if (in_mouth_open) {
// // // // //             return false; // let blue background show through
// // // // //         }

// // // // //         outGlyph = "█";
// // // // //         return true;
// // // // //     }

// // // // //     bool cell_has_poop(int rr, int cc) const {
// // // // //         for (const auto& p : poops) {
// // // // //             if (p.p.r == rr && p.p.c == cc) return true;
// // // // //         }
// // // // //         return false;
// // // // //     }

// // // // //     void render() const {
// // // // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // // // //         // Top border + score
// // // // //         cout << '+';
// // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // //         cout << "+  Score: " << score
// // // // //              << (consuming ? "   (CHOMP!)" : "")
// // // // //              << (poop_to_drop > 0 ? "   (Dropping...)" : "")
// // // // //              << "\n";

// // // // //         for (int r = 0; r < ROWS; ++r) {
// // // // //             cout << '|';
// // // // //             cout << BG_BLUE << FG_WHITE;

// // // // //             for (int c = 0; c < COLS; ++c) {
// // // // //                 // Pac head overlay has highest priority during chomp
// // // // //                 const char* overlayGlyph = nullptr;
// // // // //                 if (pac_overlay(r, c, overlayGlyph)) {
// // // // //                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
// // // // //                     continue;
// // // // //                 }

// // // // //                 // Food (visible until the final lunge overwrites it)
// // // // //                 if (food.r == r && food.c == c) {
// // // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // // //                     continue;
// // // // //                 }

// // // // //                 // Snake has priority over poop
// // // // //                 bool on_snake = false;
// // // // //                 for (const auto& seg : snake) {
// // // // //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// // // // //                 }
// // // // //                 if (on_snake) {
// // // // //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // // // //                     continue;
// // // // //                 }

// // // // //                 // Poop (brown, fades over time)
// // // // //                 if (cell_has_poop(r, c)) {
// // // // //                     cout << FG_BROWN_256 << "●" << FG_WHITE;
// // // // //                 } else {
// // // // //                     cout << ' '; // empty playfield (blue bg shows)
// // // // //                 }
// // // // //             }

// // // // //             cout << RESET << "|\n";
// // // // //         }

// // // // //         // Bottom border
// // // // //         cout << '+';
// // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // //         cout << "+\n";

// // // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // // //         cout.flush();
// // // // //     }
// // // // // };

// // // // // int main() {
// // // // //     ios::sync_with_stdio(false);
// // // // //     cin.tie(nullptr);

// // // // //     RawTerm raw;
// // // // //     Game game;
// // // // //     auto next_tick = chrono::steady_clock::now();

// // // // //     while (running.load()) {
// // // // //         // read all pending keys
// // // // //         while (true) {
// // // // //             auto k = read_key_now();
// // // // //             if (!k) break;
// // // // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // // // //             enqueue(*k);
// // // // //         }

// // // // //         if (auto key = poll_key()) {
// // // // //             if (*key == 'Q') running.store(false);
// // // // //             else game.change_dir(*key);
// // // // //         }

// // // // //         auto now = chrono::steady_clock::now();
// // // // //         if (now >= next_tick) {
// // // // //             while (now >= next_tick) {
// // // // //                 game.update();
// // // // //                 next_tick += TICK;
// // // // //             }
// // // // //             game.render();
// // // // //         } else {
// // // // //             this_thread::sleep_until(next_tick);
// // // // //         }
// // // // //     }

// // // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // // //     cout << "Thanks for playing.\n";
// // // // //     return 0;
// // // // // }



// // // // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // // // Playfield interior = blue
// // // // // // // Snake body = bright green dots
// // // // // // // Food = bright yellow dot
// // // // // // // Bite animation: a round 5x5 green head grows and closes a directional mouth on the food, then lunges.

// // // // // // #include <algorithm>
// // // // // // #include <atomic>
// // // // // // #include <chrono>
// // // // // // #include <cctype>
// // // // // // #include <cmath>
// // // // // // #include <deque>
// // // // // // #include <iostream>
// // // // // // #include <mutex>
// // // // // // #include <optional>
// // // // // // #include <queue>
// // // // // // #include <random>
// // // // // // #include <thread>
// // // // // // #include <vector>
// // // // // // #include <termios.h>
// // // // // // #include <unistd.h>

// // // // // // using namespace std;

// // // // // // // --- ANSI colors ---
// // // // // // static constexpr const char* RESET = "\x1b[0m";
// // // // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// // // // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // // // // // // --- Config ---
// // // // // // static constexpr int ROWS = 20;
// // // // // // static constexpr int COLS = 80;
// // // // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // // // --- Raw terminal guard (RAII) ---
// // // // // // struct RawTerm {
// // // // // //     termios orig{};
// // // // // //     bool ok{false};
// // // // // //     RawTerm() {
// // // // // //         if (!isatty(STDIN_FILENO)) return;
// // // // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // // // //         termios raw = orig;
// // // // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // // // //         raw.c_cc[VMIN]  = 0;
// // // // // //         raw.c_cc[VTIME] = 0;
// // // // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // // // //         ok = true;
// // // // // //     }
// // // // // //     ~RawTerm() {
// // // // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // // // //     }
// // // // // // };

// // // // // // // --- Input queue ---
// // // // // // mutex in_mtx;
// // // // // // queue<char> in_q;
// // // // // // atomic<bool> running{true};

// // // // // // optional<char> read_key_now() {
// // // // // //     unsigned char ch;
// // // // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // // // //     if (n == 1) return static_cast<char>(ch);
// // // // // //     return nullopt;
// // // // // // }
// // // // // // void enqueue(char ch) {
// // // // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // // // //         lock_guard<mutex> lk(in_mtx);
// // // // // //         in_q.push(ch);
// // // // // //     }
// // // // // // }
// // // // // // optional<char> poll_key() {
// // // // // //     lock_guard<mutex> lk(in_mtx);
// // // // // //     if (in_q.empty()) return nullopt;
// // // // // //     char c = in_q.front(); in_q.pop();
// // // // // //     return c;
// // // // // // }

// // // // // // // --- Game model ---
// // // // // // struct Point { int r, c; };
// // // // // // enum class Dir { Up, Down, Left, Right };

// // // // // // struct Game {
// // // // // //     deque<Point> snake; // front = head
// // // // // //     Dir dir = Dir::Right;
// // // // // //     Point food{0, 0};
// // // // // //     bool game_over = false;
// // // // // //     int score = 0;

// // // // // //     bool consuming = false;
// // // // // //     int chomp_frames = 0;
// // // // // //     static constexpr int CHOMP_TOTAL = 8; // ~0.8s at 10 FPS

// // // // // //     mt19937 rng{random_device{}()};

// // // // // //     Game() {
// // // // // //         int r = ROWS / 2, c = COLS / 2;
// // // // // //         snake.push_back({r, c});
// // // // // //         snake.push_back({r, c - 1});
// // // // // //         snake.push_back({r, c - 2});
// // // // // //         place_food();
// // // // // //     }

// // // // // //     Point wrap(Point p) const {
// // // // // //         if (p.r < 0) p.r = ROWS - 1;
// // // // // //         if (p.r >= ROWS) p.r = 0;
// // // // // //         if (p.c < 0) p.c = COLS - 1;
// // // // // //         if (p.c >= COLS) p.c = 0;
// // // // // //         return p;
// // // // // //     }

// // // // // //     Point next_head(Point head) const {
// // // // // //         switch (dir) {
// // // // // //             case Dir::Up:    head.r--; break;
// // // // // //             case Dir::Down:  head.r++; break;
// // // // // //             case Dir::Left:  head.c--; break;
// // // // // //             case Dir::Right: head.c++; break;
// // // // // //         }
// // // // // //         return wrap(head);
// // // // // //     }

// // // // // //     void place_food() {
// // // // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // // // //         while (true) {
// // // // // //             Point p{R(rng), C(rng)};
// // // // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // // // //             if (!on_snake) { food = p; return; }
// // // // // //         }
// // // // // //     }

// // // // // //     void change_dir(char key) {
// // // // // //         auto opp = [&](Dir a, Dir b) {
// // // // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // // // //                    (a == Dir::Right && b == Dir::Left);
// // // // // //         };
// // // // // //         Dir ndir = dir;
// // // // // //         if (key == 'W') ndir = Dir::Up;
// // // // // //         else if (key == 'S') ndir = Dir::Down;
// // // // // //         else if (key == 'A') ndir = Dir::Left;
// // // // // //         else if (key == 'D') ndir = Dir::Right;
// // // // // //         if (!opp(dir, ndir)) dir = ndir;
// // // // // //     }

// // // // // //     void update() {
// // // // // //         if (game_over) return;

// // // // // //         if (consuming) {
// // // // // //             if (--chomp_frames <= 0) {
// // // // // //                 Point nh = next_head(snake.front());
// // // // // //                 if (any_of(snake.begin(), snake.end(),
// // // // // //                            [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // // //                     game_over = true; return;
// // // // // //                 }
// // // // // //                 snake.push_front(nh);
// // // // // //                 score += 10;
// // // // // //                 place_food();
// // // // // //                 consuming = false;
// // // // // //             }
// // // // // //             return;
// // // // // //         }

// // // // // //         Point head = snake.front();
// // // // // //         Point nh = next_head(head);

// // // // // //         if (nh.r == food.r && nh.c == food.c) {
// // // // // //             consuming = true;
// // // // // //             chomp_frames = CHOMP_TOTAL;
// // // // // //             return;
// // // // // //         }

// // // // // //         if (any_of(snake.begin(), snake.end(),
// // // // // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // // //             game_over = true; return;
// // // // // //         }

// // // // // //         snake.push_front(nh);
// // // // // //         snake.pop_back();
// // // // // //     }

// // // // // //     bool pac_overlay(int r, int c, const char*& outGlyph) const {
// // // // // //         if (!consuming || snake.empty()) return false;

// // // // // //         int phase = CHOMP_TOTAL - chomp_frames;
// // // // // //         Point h = snake.front();

// // // // // //         auto wrap_delta = [](int d, int maxv){
// // // // // //             if (d >  maxv/2) d -= maxv;
// // // // // //             if (d < -maxv/2) d += maxv;
// // // // // //             return d;
// // // // // //         };
// // // // // //         int dy = wrap_delta(r - h.r, ROWS);
// // // // // //         int dx = wrap_delta(c - h.c, COLS);

// // // // // //         if (abs(dx) > 2 || abs(dy) > 2) return false;

// // // // // //         double radius =
// // // // // //             (phase <= 1) ? 2.4 :
// // // // // //             (phase <= 3) ? 2.2 :
// // // // // //                            2.0;
// // // // // //         double r2 = dx*dx + dy*dy;
// // // // // //         if (r2 > radius*radius) return false;

// // // // // //         int vx = 0, vy = 0;
// // // // // //         switch (dir) {
// // // // // //             case Dir::Right: vx = 1; vy = 0; break;
// // // // // //             case Dir::Left:  vx = -1; vy = 0; break;
// // // // // //             case Dir::Up:    vx = 0; vy = -1; break;
// // // // // //             case Dir::Down:  vx = 0; vy = 1; break;
// // // // // //         }

// // // // // //         int mouth_band =
// // // // // //             (phase <= 1) ? 2 :
// // // // // //             (phase <= 3) ? 1 :
// // // // // //                            0;
// // // // // //         int forward_thresh =
// // // // // //             (phase <= 1) ? 0 :
// // // // // //             (phase <= 3) ? 1 :
// // // // // //                            99;

// // // // // //         int forward = vx*dx + vy*dy;
// // // // // //         int perp    = (-vy)*dx + (vx)*dy;

// // // // // //         bool in_mouth_open =
// // // // // //             (forward >= forward_thresh) &&
// // // // // //             (abs(perp) <= mouth_band);

// // // // // //         if (in_mouth_open) {
// // // // // //             return false; // let blue background show through
// // // // // //         }

// // // // // //         outGlyph = "█";
// // // // // //         return true;
// // // // // //     }

// // // // // //     void render() const {
// // // // // //         cout << "\x1b[2J\x1b[H";
// // // // // //         cout << '+';
// // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // //         cout << "+  Score: " << score << (consuming ? "   (CHOMP!)" : "") << "\n";

// // // // // //         for (int r = 0; r < ROWS; ++r) {
// // // // // //             cout << '|';
// // // // // //             cout << BG_BLUE << FG_WHITE;

// // // // // //             for (int c = 0; c < COLS; ++c) {
// // // // // //                 const char* overlayGlyph = nullptr;
// // // // // //                 if (pac_overlay(r, c, overlayGlyph)) {
// // // // // //                     cout << FG_BRIGHT_GREEN << overlayGlyph << FG_WHITE;
// // // // // //                     continue;
// // // // // //                 }

// // // // // //                 if (food.r == r && food.c == c) {
// // // // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // // // //                     continue;
// // // // // //                 }

// // // // // //                 bool on_snake = false;
// // // // // //                 for (const auto& seg : snake) {
// // // // // //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// // // // // //                 }
// // // // // //                 if (on_snake) {
// // // // // //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // // // // //                 } else {
// // // // // //                     cout << ' ';
// // // // // //                 }
// // // // // //             }

// // // // // //             cout << RESET << "|\n";
// // // // // //         }

// // // // // //         cout << '+';
// // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // //         cout << "+\n";
// // // // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // // // //         cout.flush();
// // // // // //     }
// // // // // // };

// // // // // // int main() {
// // // // // //     ios::sync_with_stdio(false);
// // // // // //     cin.tie(nullptr);

// // // // // //     RawTerm raw;
// // // // // //     Game game;
// // // // // //     auto next_tick = chrono::steady_clock::now();

// // // // // //     while (running.load()) {
// // // // // //         while (true) {
// // // // // //             auto k = read_key_now();
// // // // // //             if (!k) break;
// // // // // //             if (*k == 3) { running.store(false); break; }
// // // // // //             enqueue(*k);
// // // // // //         }

// // // // // //         if (auto key = poll_key()) {
// // // // // //             if (*key == 'Q') running.store(false);
// // // // // //             else game.change_dir(*key);
// // // // // //         }

// // // // // //         auto now = chrono::steady_clock::now();
// // // // // //         if (now >= next_tick) {
// // // // // //             while (now >= next_tick) {
// // // // // //                 game.update();
// // // // // //                 next_tick += TICK;
// // // // // //             }
// // // // // //             game.render();
// // // // // //         } else {
// // // // // //             this_thread::sleep_until(next_tick);
// // // // // //         }
// // // // // //     }

// // // // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // // // //     cout << "Thanks for playing.\n";
// // // // // //     return 0;
// // // // // // }


// // // // // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // // // // Playfield interior = blue
// // // // // // // // Snake body = bright green dots
// // // // // // // // Food = bright yellow dot
// // // // // // // // Bite animation: 2x2 circular head (◜◝◟◞) that opens toward the food, closes, then the snake lunges.

// // // // // // // #include <algorithm>
// // // // // // // #include <atomic>
// // // // // // // #include <chrono>
// // // // // // // #include <cctype>
// // // // // // // #include <deque>
// // // // // // // #include <iostream>
// // // // // // // #include <mutex>
// // // // // // // #include <optional>
// // // // // // // #include <queue>
// // // // // // // #include <random>
// // // // // // // #include <thread>
// // // // // // // #include <vector>
// // // // // // // #include <termios.h>
// // // // // // // #include <unistd.h>

// // // // // // // using namespace std;

// // // // // // // // --- ANSI colors ---
// // // // // // // static constexpr const char* RESET = "\x1b[0m";
// // // // // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// // // // // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // // // // // // // --- Config ---
// // // // // // // static constexpr int ROWS = 20;
// // // // // // // static constexpr int COLS = 80;
// // // // // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // // // // --- Raw terminal guard (RAII) ---
// // // // // // // struct RawTerm {
// // // // // // //     termios orig{};
// // // // // // //     bool ok{false};
// // // // // // //     RawTerm() {
// // // // // // //         if (!isatty(STDIN_FILENO)) return;
// // // // // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // // // // //         termios raw = orig;
// // // // // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // // // // //         raw.c_cc[VMIN]  = 0;
// // // // // // //         raw.c_cc[VTIME] = 0;
// // // // // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // // // // //         ok = true;
// // // // // // //     }
// // // // // // //     ~RawTerm() {
// // // // // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // // // // //     }
// // // // // // // };

// // // // // // // // --- Input queue ---
// // // // // // // mutex in_mtx;
// // // // // // // queue<char> in_q;
// // // // // // // atomic<bool> running{true};

// // // // // // // optional<char> read_key_now() {
// // // // // // //     unsigned char ch;
// // // // // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // // // // //     if (n == 1) return static_cast<char>(ch);
// // // // // // //     return nullopt;
// // // // // // // }
// // // // // // // void enqueue(char ch) {
// // // // // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // // // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // // // // //         lock_guard<mutex> lk(in_mtx);
// // // // // // //         in_q.push(ch);
// // // // // // //     }
// // // // // // // }
// // // // // // // optional<char> poll_key() {
// // // // // // //     lock_guard<mutex> lk(in_mtx);
// // // // // // //     if (in_q.empty()) return nullopt;
// // // // // // //     char c = in_q.front(); in_q.pop();
// // // // // // //     return c;
// // // // // // // }

// // // // // // // // --- Game model ---
// // // // // // // struct Point { int r, c; };
// // // // // // // enum class Dir { Up, Down, Left, Right };

// // // // // // // struct Game {
// // // // // // //     deque<Point> snake; // front = head
// // // // // // //     Dir dir = Dir::Right;
// // // // // // //     Point food{0, 0};
// // // // // // //     bool game_over = false;
// // // // // // //     int score = 0;

// // // // // // //     // Bite animation state
// // // // // // //     bool consuming = false;
// // // // // // //     Point food_target{0,0};
// // // // // // //     int chomp_frames = 0;                 // 6..1 while animating
// // // // // // //     static constexpr int CHOMP_TOTAL = 6; // ~0.6s at 10 FPS

// // // // // // //     mt19937 rng{random_device{}()};

// // // // // // //     Game() {
// // // // // // //         int r = ROWS / 2, c = COLS / 2;
// // // // // // //         snake.push_back({r, c});
// // // // // // //         snake.push_back({r, c - 1});
// // // // // // //         snake.push_back({r, c - 2});
// // // // // // //         place_food();
// // // // // // //     }

// // // // // // //     Point wrap(Point p) const {
// // // // // // //         if (p.r < 0) p.r = ROWS - 1;
// // // // // // //         if (p.r >= ROWS) p.r = 0;
// // // // // // //         if (p.c < 0) p.c = COLS - 1;
// // // // // // //         if (p.c >= COLS) p.c = 0;
// // // // // // //         return p;
// // // // // // //     }

// // // // // // //     Point next_head(Point head) const {
// // // // // // //         switch (dir) {
// // // // // // //             case Dir::Up:    head.r--; break;
// // // // // // //             case Dir::Down:  head.r++; break;
// // // // // // //             case Dir::Left:  head.c--; break;
// // // // // // //             case Dir::Right: head.c++; break;
// // // // // // //         }
// // // // // // //         return wrap(head);
// // // // // // //     }

// // // // // // //     void place_food() {
// // // // // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // // // // //         while (true) {
// // // // // // //             Point p{R(rng), C(rng)};
// // // // // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // // // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // // // // //             if (!on_snake) { food = p; return; }
// // // // // // //         }
// // // // // // //     }

// // // // // // //     void change_dir(char key) {
// // // // // // //         auto opp = [&](Dir a, Dir b) {
// // // // // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // // // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // // // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // // // // //                    (a == Dir::Right && b == Dir::Left);
// // // // // // //         };
// // // // // // //         Dir ndir = dir;
// // // // // // //         if (key == 'W') ndir = Dir::Up;
// // // // // // //         else if (key == 'S') ndir = Dir::Down;
// // // // // // //         else if (key == 'A') ndir = Dir::Left;
// // // // // // //         else if (key == 'D') ndir = Dir::Right;
// // // // // // //         if (!opp(dir, ndir)) dir = ndir;
// // // // // // //     }

// // // // // // //     void update() {
// // // // // // //         if (game_over) return;

// // // // // // //         // If we're mid-chomp, finish the animation without moving
// // // // // // //         if (consuming) {
// // // // // // //             if (--chomp_frames <= 0) {
// // // // // // //                 // Finalize: move head into the food cell, grow, score, new food
// // // // // // //                 Point head = next_head(snake.front());
// // // // // // //                 // self-collision guard
// // // // // // //                 if (any_of(snake.begin(), snake.end(),
// // // // // // //                            [&](const Point& p){ return p.r == head.r && p.c == head.c; })) {
// // // // // // //                     game_over = true; return;
// // // // // // //                 }
// // // // // // //                 snake.push_front(head); // grow (no pop)
// // // // // // //                 score += 10;
// // // // // // //                 place_food();
// // // // // // //                 consuming = false;
// // // // // // //             }
// // // // // // //             return;
// // // // // // //         }

// // // // // // //         // Normal step
// // // // // // //         Point head = snake.front();
// // // // // // //         Point nh = next_head(head);

// // // // // // //         // If next step eats food, start chomp (pause movement)
// // // // // // //         if (nh.r == food.r && nh.c == food.c) {
// // // // // // //             consuming = true;
// // // // // // //             food_target = food;
// // // // // // //             chomp_frames = CHOMP_TOTAL;
// // // // // // //             return;
// // // // // // //         }

// // // // // // //         // Self collision
// // // // // // //         if (any_of(snake.begin(), snake.end(),
// // // // // // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // // // //             game_over = true; return;
// // // // // // //         }

// // // // // // //         snake.push_front(nh);
// // // // // // //         snake.pop_back();
// // // // // // //     }

// // // // // // //     // Compute 2x2 overlay positions around the current head for the circular mouth
// // // // // // //     // Returns true and outputs a glyph if this cell is part of the overlay for the current frame.
// // // // // // //     bool overlay_glyph(int r, int c, const char*& outGlyph) const {
// // // // // // //         if (!consuming || chomp_frames <= 0 || snake.empty()) return false;

// // // // // // //         // Phase: open (0-1), partial close (2-3), closed (4-5)
// // // // // // //         int phase = CHOMP_TOTAL - chomp_frames; // 0..5

// // // // // // //         // Current head position
// // // // // // //         Point h = snake.front();

// // // // // // //         // Helper to wrap coords
// // // // // // //         auto wrap_rc = [&](int rr, int cc)->Point{
// // // // // // //             if (rr < 0) rr = ROWS - 1; if (rr >= ROWS) rr = 0;
// // // // // // //             if (cc < 0) cc = COLS - 1; if (cc >= COLS) cc = 0;
// // // // // // //             return {rr, cc};
// // // // // // //         };

// // // // // // //         // Determine the 2x2 block positions and which quadrants are "mouth side"
// // // // // // //         // Quadrant glyphs:
// // // // // // //         static constexpr const char* TL = "◜";
// // // // // // //         static constexpr const char* TR = "◝";
// // // // // // //         static constexpr const char* BL = "◟";
// // // // // // //         static constexpr const char* BR = "◞";

// // // // // // //         // Positions for the four quadrants, depending on direction.
// // // // // // //         Point pTL, pTR, pBL, pBR;
// // // // // // //         // For each direction we anchor the 2x2 so that the "mouth side"
// // // // // // //         // faces the target food cell.
// // // // // // //         switch (dir) {
// // // // // // //             case Dir::Right:
// // // // // // //                 // 2x2 spans rows (h.r-1, h.r), cols (h.c, h.c+1)
// // // // // // //                 pTL = wrap_rc(h.r - 1, h.c);
// // // // // // //                 pTR = wrap_rc(h.r - 1, h.c + 1);
// // // // // // //                 pBL = wrap_rc(h.r,     h.c);
// // // // // // //                 pBR = wrap_rc(h.r,     h.c + 1);
// // // // // // //                 break;
// // // // // // //             case Dir::Left:
// // // // // // //                 // spans rows (h.r-1, h.r), cols (h.c-1, h.c)
// // // // // // //                 pTL = wrap_rc(h.r - 1, h.c - 1);
// // // // // // //                 pTR = wrap_rc(h.r - 1, h.c);
// // // // // // //                 pBL = wrap_rc(h.r,     h.c - 1);
// // // // // // //                 pBR = wrap_rc(h.r,     h.c);
// // // // // // //                 break;
// // // // // // //             case Dir::Up:
// // // // // // //                 // spans rows (h.r-1, h.r), cols (h.c-1, h.c)
// // // // // // //                 pTL = wrap_rc(h.r - 1, h.c - 1);
// // // // // // //                 pTR = wrap_rc(h.r - 1, h.c);
// // // // // // //                 pBL = wrap_rc(h.r,     h.c - 1);
// // // // // // //                 pBR = wrap_rc(h.r,     h.c);
// // // // // // //                 break;
// // // // // // //             case Dir::Down:
// // // // // // //                 // spans rows (h.r, h.r+1), cols (h.c-1, h.c)
// // // // // // //                 pTL = wrap_rc(h.r,     h.c - 1);
// // // // // // //                 pTR = wrap_rc(h.r,     h.c);
// // // // // // //                 pBL = wrap_rc(h.r + 1, h.c - 1);
// // // // // // //                 pBR = wrap_rc(h.r + 1, h.c);
// // // // // // //                 break;
// // // // // // //         }

// // // // // // //         // Which quadrants are "kept" per phase (open -> partial -> closed)
// // // // // // //         bool keepTL=false, keepTR=false, keepBL=false, keepBR=false;

// // // // // // //         // Start with an open mouth: keep the side *away* from the food (back side)
// // // // // // //         auto open_right = [&]{ keepTL=true; keepBL=true; };             // mouth on right: drop TR, BR
// // // // // // //         auto open_left  = [&]{ keepTR=true; keepBR=true; };             // mouth on left:  drop TL, BL
// // // // // // //         auto open_up    = [&]{ keepBL=true; keepBR=true; };             // mouth on top:   drop TL, TR
// // // // // // //         auto open_down  = [&]{ keepTL=true; keepTR=true; };             // mouth on bottom:drop BL, BR

// // // // // // //         // Partial close: add one mouth-side quadrant (nearer to center)
// // // // // // //         auto partial_right = [&]{ keepTL=true; keepBL=true; keepTR=true; };
// // // // // // //         auto partial_left  = [&]{ keepTR=true; keepBR=true; keepTL=true; };
// // // // // // //         auto partial_up    = [&]{ keepBL=true; keepBR=true; keepTL=true; };
// // // // // // //         auto partial_down  = [&]{ keepTL=true; keepTR=true; keepBL=true; };

// // // // // // //         // Closed: keep all
// // // // // // //         auto closed_all = [&]{ keepTL=keepTR=keepBL=keepBR=true; };

// // // // // // //         if (phase <= 1) {
// // // // // // //             switch (dir) {
// // // // // // //                 case Dir::Right: open_right(); break;
// // // // // // //                 case Dir::Left:  open_left();  break;
// // // // // // //                 case Dir::Up:    open_up();    break;
// // // // // // //                 case Dir::Down:  open_down();  break;
// // // // // // //             }
// // // // // // //         } else if (phase <= 3) {
// // // // // // //             switch (dir) {
// // // // // // //                 case Dir::Right: partial_right(); break;
// // // // // // //                 case Dir::Left:  partial_left();  break;
// // // // // // //                 case Dir::Up:    partial_up();    break;
// // // // // // //                 case Dir::Down:  partial_down();  break;
// // // // // // //             }
// // // // // // //         } else {
// // // // // // //             closed_all();
// // // // // // //         }

// // // // // // //         // Now decide if current (r,c) is one of these quadrants
// // // // // // //         if (r == pTL.r && c == pTL.c && keepTL) { outGlyph = TL; return true; }
// // // // // // //         if (r == pTR.r && c == pTR.c && keepTR) { outGlyph = TR; return true; }
// // // // // // //         if (r == pBL.r && c == pBL.c && keepBL) { outGlyph = BL; return true; }
// // // // // // //         if (r == pBR.r && c == pBR.c && keepBR) { outGlyph = BR; return true; }

// // // // // // //         // If it's part of the 2x2 but not kept (i.e., the open mouth area), we want blue background to show.
// // // // // // //         if ((r == pTL.r && c == pTL.c) || (r == pTR.r && c == pTR.c) ||
// // // // // // //             (r == pBL.r && c == pBL.c) || (r == pBR.r && c == pBR.c)) {
// // // // // // //             outGlyph = " "; // mouth wedge (blue background through)
// // // // // // //             return true;
// // // // // // //         }

// // // // // // //         return false;
// // // // // // //     }

// // // // // // //     void render() const {
// // // // // // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // // // // // //         // Top border + score
// // // // // // //         cout << '+';
// // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // //         cout << "+  Score: " << score << "\n";

// // // // // // //         for (int r = 0; r < ROWS; ++r) {
// // // // // // //             cout << '|';
// // // // // // //             cout << BG_BLUE << FG_WHITE;

// // // // // // //             for (int c = 0; c < COLS; ++c) {
// // // // // // //                 // Overlay bite head takes precedence
// // // // // // //                 const char* og = nullptr;
// // // // // // //                 if (overlay_glyph(r, c, og)) {
// // // // // // //                     cout << FG_BRIGHT_GREEN << og << FG_WHITE;
// // // // // // //                     continue;
// // // // // // //                 }

// // // // // // //                 // Food (still visible during early bite frames unless covered by overlay)
// // // // // // //                 if (food.r == r && food.c == c) {
// // // // // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // // // // //                     continue;
// // // // // // //                 }

// // // // // // //                 // Snake?
// // // // // // //                 bool on_snake = false;
// // // // // // //                 for (const auto& seg : snake) {
// // // // // // //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// // // // // // //                 }
// // // // // // //                 if (on_snake) {
// // // // // // //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // // // // // //                 } else {
// // // // // // //                     cout << ' '; // empty playfield (blue bg shows)
// // // // // // //                 }
// // // // // // //             }

// // // // // // //             cout << RESET << "|\n";
// // // // // // //         }

// // // // // // //         // Bottom border
// // // // // // //         cout << '+';
// // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // //         cout << "+\n";

// // // // // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // // // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // // // // //         cout.flush();
// // // // // // //     }
// // // // // // // };

// // // // // // // int main() {
// // // // // // //     ios::sync_with_stdio(false);
// // // // // // //     cin.tie(nullptr);

// // // // // // //     RawTerm raw;
// // // // // // //     Game game;
// // // // // // //     auto next_tick = chrono::steady_clock::now();

// // // // // // //     while (running.load()) {
// // // // // // //         // read all pending keys
// // // // // // //         while (true) {
// // // // // // //             auto k = read_key_now();
// // // // // // //             if (!k) break;
// // // // // // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // // // // // //             enqueue(*k);
// // // // // // //         }

// // // // // // //         if (auto key = poll_key()) {
// // // // // // //             if (*key == 'Q') running.store(false);
// // // // // // //             else game.change_dir(*key);
// // // // // // //         }

// // // // // // //         auto now = chrono::steady_clock::now();
// // // // // // //         if (now >= next_tick) {
// // // // // // //             while (now >= next_tick) {
// // // // // // //                 game.update();
// // // // // // //                 next_tick += TICK;
// // // // // // //             }
// // // // // // //             game.render();
// // // // // // //         } else {
// // // // // // //             this_thread::sleep_until(next_tick);
// // // // // // //         }
// // // // // // //     }

// // // // // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // // // // //     cout << "Thanks for playing.\n";
// // // // // // //     return 0;
// // // // // // // }


// // // // // // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // // // // // Playfield interior = blue
// // // // // // // // // Snake body = bright green dots
// // // // // // // // // Food = bright yellow dot
// // // // // // // // // Head animates like Pac-Man (round dot that opens toward the food)

// // // // // // // // #include <algorithm>
// // // // // // // // #include <atomic>
// // // // // // // // #include <chrono>
// // // // // // // // #include <cctype>
// // // // // // // // #include <deque>
// // // // // // // // #include <iostream>
// // // // // // // // #include <mutex>
// // // // // // // // #include <optional>
// // // // // // // // #include <queue>
// // // // // // // // #include <random>
// // // // // // // // #include <thread>
// // // // // // // // #include <vector>
// // // // // // // // #include <termios.h>
// // // // // // // // #include <unistd.h>

// // // // // // // // using namespace std;

// // // // // // // // // --- ANSI colors ---
// // // // // // // // static constexpr const char* RESET = "\x1b[0m";
// // // // // // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // // // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // // // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// // // // // // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // // // // // // // // --- Config ---
// // // // // // // // static constexpr int ROWS = 20;
// // // // // // // // static constexpr int COLS = 80;
// // // // // // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // // // // // --- Raw terminal guard (RAII) ---
// // // // // // // // struct RawTerm {
// // // // // // // //     termios orig{};
// // // // // // // //     bool ok{false};
// // // // // // // //     RawTerm() {
// // // // // // // //         if (!isatty(STDIN_FILENO)) return;
// // // // // // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // // // // // //         termios raw = orig;
// // // // // // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // // // // // //         raw.c_cc[VMIN]  = 0;
// // // // // // // //         raw.c_cc[VTIME] = 0;
// // // // // // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // // // // // //         ok = true;
// // // // // // // //     }
// // // // // // // //     ~RawTerm() {
// // // // // // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // // // // // //     }
// // // // // // // // };

// // // // // // // // // --- Input queue ---
// // // // // // // // mutex in_mtx;
// // // // // // // // queue<char> in_q;
// // // // // // // // atomic<bool> running{true};

// // // // // // // // optional<char> read_key_now() {
// // // // // // // //     unsigned char ch;
// // // // // // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // // // // // //     if (n == 1) return static_cast<char>(ch);
// // // // // // // //     return nullopt;
// // // // // // // // }
// // // // // // // // void enqueue(char ch) {
// // // // // // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // // // // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // // // // // //         lock_guard<mutex> lk(in_mtx);
// // // // // // // //         in_q.push(ch);
// // // // // // // //     }
// // // // // // // // }
// // // // // // // // optional<char> poll_key() {
// // // // // // // //     lock_guard<mutex> lk(in_mtx);
// // // // // // // //     if (in_q.empty()) return nullopt;
// // // // // // // //     char c = in_q.front(); in_q.pop();
// // // // // // // //     return c;
// // // // // // // // }

// // // // // // // // // --- Game model ---
// // // // // // // // struct Point { int r, c; };
// // // // // // // // enum class Dir { Up, Down, Left, Right };

// // // // // // // // // Simple animation timer
// // // // // // // // static unsigned long g_frame = 0;

// // // // // // // // struct Game {
// // // // // // // //     deque<Point> snake; // front = head
// // // // // // // //     Dir dir = Dir::Right;
// // // // // // // //     Point food{0, 0};
// // // // // // // //     bool game_over = false;
// // // // // // // //     int score = 0;

// // // // // // // //     // Pac-Man mouth animation
// // // // // // // //     int mouth_frames = 0;            // >0 while mouth is shown open
// // // // // // // //     static constexpr int MOUTH_OPEN_FRAMES = 6; // ~0.6s at 10 FPS

// // // // // // // //     mt19937 rng{random_device{}()};

// // // // // // // //     Game() {
// // // // // // // //         int r = ROWS / 2, c = COLS / 2;
// // // // // // // //         snake.push_back({r, c});
// // // // // // // //         snake.push_back({r, c - 1});
// // // // // // // //         snake.push_back({r, c - 2});
// // // // // // // //         place_food();
// // // // // // // //     }

// // // // // // // //     Point wrap(Point p) const {
// // // // // // // //         if (p.r < 0) p.r = ROWS - 1;
// // // // // // // //         if (p.r >= ROWS) p.r = 0;
// // // // // // // //         if (p.c < 0) p.c = COLS - 1;
// // // // // // // //         if (p.c >= COLS) p.c = 0;
// // // // // // // //         return p;
// // // // // // // //     }

// // // // // // // //     Point next_head(Point head) const {
// // // // // // // //         switch (dir) {
// // // // // // // //             case Dir::Up:    head.r--; break;
// // // // // // // //             case Dir::Down:  head.r++; break;
// // // // // // // //             case Dir::Left:  head.c--; break;
// // // // // // // //             case Dir::Right: head.c++; break;
// // // // // // // //         }
// // // // // // // //         return wrap(head);
// // // // // // // //     }

// // // // // // // //     void place_food() {
// // // // // // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // // // // // //         while (true) {
// // // // // // // //             Point p{R(rng), C(rng)};
// // // // // // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // // // // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // // // // // //             if (!on_snake) { food = p; return; }
// // // // // // // //         }
// // // // // // // //     }

// // // // // // // //     void change_dir(char key) {
// // // // // // // //         auto opp = [&](Dir a, Dir b) {
// // // // // // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // // // // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // // // // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // // // // // //                    (a == Dir::Right && b == Dir::Left);
// // // // // // // //         };
// // // // // // // //         Dir ndir = dir;
// // // // // // // //         if (key == 'W') ndir = Dir::Up;
// // // // // // // //         else if (key == 'S') ndir = Dir::Down;
// // // // // // // //         else if (key == 'A') ndir = Dir::Left;
// // // // // // // //         else if (key == 'D') ndir = Dir::Right;
// // // // // // // //         if (!opp(dir, ndir)) dir = ndir;
// // // // // // // //     }

// // // // // // // //     void update() {
// // // // // // // //         if (game_over) return;

// // // // // // // //         Point head = snake.front();
// // // // // // // //         Point nh = next_head(head);

// // // // // // // //         // If the next move hits food, start/open the mouth
// // // // // // // //         if (nh.r == food.r && nh.c == food.c) {
// // // // // // // //             mouth_frames = MOUTH_OPEN_FRAMES;
// // // // // // // //         }

// // // // // // // //         // Self collision check with nh
// // // // // // // //         if (any_of(snake.begin(), snake.end(),
// // // // // // // //                    [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
// // // // // // // //             game_over = true; return;
// // // // // // // //         }

// // // // // // // //         snake.push_front(nh);

// // // // // // // //         if (nh.r == food.r && nh.c == food.c) {
// // // // // // // //             score += 10;
// // // // // // // //             place_food(); // ate it; keep mouth open briefly for the gulp
// // // // // // // //         } else {
// // // // // // // //             snake.pop_back();
// // // // // // // //         }

// // // // // // // //         if (mouth_frames > 0) mouth_frames--;
// // // // // // // //     }

// // // // // // // //     void render() const {
// // // // // // // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // // // // // // //         // Top border + score
// // // // // // // //         cout << '+';
// // // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // // //         cout << "+  Score: " << score << "\n";

// // // // // // // //         for (int r = 0; r < ROWS; ++r) {
// // // // // // // //             cout << '|';
// // // // // // // //             cout << BG_BLUE << FG_WHITE;

// // // // // // // //             for (int c = 0; c < COLS; ++c) {
// // // // // // // //                 // Food
// // // // // // // //                 if (food.r == r && food.c == c) {
// // // // // // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // // // // // //                     continue;
// // // // // // // //                 }

// // // // // // // //                 // Snake occupancy + head index
// // // // // // // //                 bool on_snake = false;
// // // // // // // //                 size_t seg_index = 0;
// // // // // // // //                 for (size_t i = 0; i < snake.size(); ++i) {
// // // // // // // //                     if (snake[i].r == r && snake[i].c == c) {
// // // // // // // //                         on_snake = true; seg_index = i; break;
// // // // // // // //                     }
// // // // // // // //                 }

// // // // // // // //                 if (on_snake) {
// // // // // // // //                     if (seg_index == 0) {
// // // // // // // //                         // Head glyph: closed or Pac-Man style open toward direction
// // // // // // // //                         const bool open = (mouth_frames > 0) && ((g_frame % 2) == 0);
// // // // // // // //                         const char* headGlyph = "●";
// // // // // // // //                         if (open) {
// // // // // // // //                             // Use half-filled circles: empty side = mouth opening
// // // // // // // //                             switch (dir) {
// // // // // // // //                                 case Dir::Right: headGlyph = "◐"; break; // empty right
// // // // // // // //                                 case Dir::Left:  headGlyph = "◑"; break; // empty left
// // // // // // // //                                 case Dir::Up:    headGlyph = "◒"; break; // empty top
// // // // // // // //                                 case Dir::Down:  headGlyph = "◓"; break; // empty bottom
// // // // // // // //                             }
// // // // // // // //                         }
// // // // // // // //                         cout << FG_BRIGHT_GREEN << headGlyph << FG_WHITE;
// // // // // // // //                     } else {
// // // // // // // //                         cout << FG_BRIGHT_GREEN << "●" << FG_WHITE; // body
// // // // // // // //                     }
// // // // // // // //                 } else {
// // // // // // // //                     cout << ' ';
// // // // // // // //                 }
// // // // // // // //             }

// // // // // // // //             cout << RESET << "|\n";
// // // // // // // //         }

// // // // // // // //         // Bottom border
// // // // // // // //         cout << '+';
// // // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // // //         cout << "+\n";

// // // // // // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // // // // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // // // // // //         cout.flush();

// // // // // // // //         g_frame++;
// // // // // // // //     }
// // // // // // // // };

// // // // // // // // int main() {
// // // // // // // //     ios::sync_with_stdio(false);
// // // // // // // //     cin.tie(nullptr);

// // // // // // // //     RawTerm raw;
// // // // // // // //     Game game;
// // // // // // // //     auto next_tick = chrono::steady_clock::now();

// // // // // // // //     while (running.load()) {
// // // // // // // //         // read all pending keys
// // // // // // // //         while (true) {
// // // // // // // //             auto k = read_key_now();
// // // // // // // //             if (!k) break;
// // // // // // // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // // // // // // //             enqueue(*k);
// // // // // // // //         }

// // // // // // // //         if (auto key = poll_key()) {
// // // // // // // //             if (*key == 'Q') running.store(false);
// // // // // // // //             else game.change_dir(*key);
// // // // // // // //         }

// // // // // // // //         auto now = chrono::steady_clock::now();
// // // // // // // //         if (now >= next_tick) {
// // // // // // // //             while (now >= next_tick) {
// // // // // // // //                 game.update();
// // // // // // // //                 next_tick += TICK;
// // // // // // // //             }
// // // // // // // //             game.render();
// // // // // // // //         } else {
// // // // // // // //             this_thread::sleep_until(next_tick);
// // // // // // // //         }
// // // // // // // //     }

// // // // // // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // // // // // //     cout << "Thanks for playing.\n";
// // // // // // // //     return 0;
// // // // // // // // }


// // // // // // // // // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // // // // // // // // Playfield interior = blue
// // // // // // // // // // Snake = bright green dots
// // // // // // // // // // Food = bright yellow dot
// // // // // // // // // // Snake head enlarges briefly when eating to show a "bite" animation

// // // // // // // // // #include <algorithm>
// // // // // // // // // #include <atomic>
// // // // // // // // // #include <chrono>
// // // // // // // // // #include <cctype>
// // // // // // // // // #include <deque>
// // // // // // // // // #include <iostream>
// // // // // // // // // #include <mutex>
// // // // // // // // // #include <optional>
// // // // // // // // // #include <queue>
// // // // // // // // // #include <random>
// // // // // // // // // #include <thread>
// // // // // // // // // #include <vector>
// // // // // // // // // #include <termios.h>
// // // // // // // // // #include <unistd.h>

// // // // // // // // // using namespace std;

// // // // // // // // // // --- ANSI colors ---
// // // // // // // // // static constexpr const char* RESET = "\x1b[0m";
// // // // // // // // // static constexpr const char* BG_BLUE = "\x1b[44m";
// // // // // // // // // static constexpr const char* FG_WHITE = "\x1b[37m";
// // // // // // // // // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// // // // // // // // // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // // // // // // // // // --- Config ---
// // // // // // // // // static constexpr int ROWS = 20;
// // // // // // // // // static constexpr int COLS = 80;
// // // // // // // // // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // // // // // // // // // --- Raw terminal guard (RAII) ---
// // // // // // // // // struct RawTerm {
// // // // // // // // //     termios orig{};
// // // // // // // // //     bool ok{false};
// // // // // // // // //     RawTerm() {
// // // // // // // // //         if (!isatty(STDIN_FILENO)) return;
// // // // // // // // //         if (tcgetattr(STDIN_FILENO, &orig) != 0) return;
// // // // // // // // //         termios raw = orig;
// // // // // // // // //         raw.c_lflag &= ~(ICANON | ECHO);
// // // // // // // // //         raw.c_cc[VMIN]  = 0;
// // // // // // // // //         raw.c_cc[VTIME] = 0;
// // // // // // // // //         if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
// // // // // // // // //         ok = true;
// // // // // // // // //     }
// // // // // // // // //     ~RawTerm() {
// // // // // // // // //         if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
// // // // // // // // //     }
// // // // // // // // // };

// // // // // // // // // // --- Input queue ---
// // // // // // // // // mutex in_mtx;
// // // // // // // // // queue<char> in_q;
// // // // // // // // // atomic<bool> running{true};

// // // // // // // // // optional<char> read_key_now() {
// // // // // // // // //     unsigned char ch;
// // // // // // // // //     ssize_t n = read(STDIN_FILENO, &ch, 1);
// // // // // // // // //     if (n == 1) return static_cast<char>(ch);
// // // // // // // // //     return nullopt;
// // // // // // // // // }
// // // // // // // // // void enqueue(char ch) {
// // // // // // // // //     ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
// // // // // // // // //     if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D' || ch == 'Q') {
// // // // // // // // //         lock_guard<mutex> lk(in_mtx);
// // // // // // // // //         in_q.push(ch);
// // // // // // // // //     }
// // // // // // // // // }
// // // // // // // // // optional<char> poll_key() {
// // // // // // // // //     lock_guard<mutex> lk(in_mtx);
// // // // // // // // //     if (in_q.empty()) return nullopt;
// // // // // // // // //     char c = in_q.front(); in_q.pop();
// // // // // // // // //     return c;
// // // // // // // // // }

// // // // // // // // // // --- Game model ---
// // // // // // // // // struct Point { int r, c; };
// // // // // // // // // enum class Dir { Up, Down, Left, Right };

// // // // // // // // // struct Game {
// // // // // // // // //     deque<Point> snake; // front = head
// // // // // // // // //     Dir dir = Dir::Right;
// // // // // // // // //     Point food{0, 0};
// // // // // // // // //     bool game_over = false;
// // // // // // // // //     int score = 0;
// // // // // // // // //     int chomp_frames = 0; // >0 during bite animation
// // // // // // // // //     mt19937 rng{random_device{}()};

// // // // // // // // //     Game() {
// // // // // // // // //         int r = ROWS / 2, c = COLS / 2;
// // // // // // // // //         snake.push_back({r, c});
// // // // // // // // //         snake.push_back({r, c - 1});
// // // // // // // // //         snake.push_back({r, c - 2});
// // // // // // // // //         place_food();
// // // // // // // // //     }

// // // // // // // // //     void place_food() {
// // // // // // // // //         uniform_int_distribution<int> R(0, ROWS - 1), C(0, COLS - 1);
// // // // // // // // //         while (true) {
// // // // // // // // //             Point p{R(rng), C(rng)};
// // // // // // // // //             bool on_snake = any_of(snake.begin(), snake.end(),
// // // // // // // // //                                    [&](const Point& s){ return s.r == p.r && s.c == p.c; });
// // // // // // // // //             if (!on_snake) { food = p; return; }
// // // // // // // // //         }
// // // // // // // // //     }

// // // // // // // // //     void change_dir(char key) {
// // // // // // // // //         auto opp = [&](Dir a, Dir b) {
// // // // // // // // //             return (a == Dir::Up && b == Dir::Down) ||
// // // // // // // // //                    (a == Dir::Down && b == Dir::Up) ||
// // // // // // // // //                    (a == Dir::Left && b == Dir::Right) ||
// // // // // // // // //                    (a == Dir::Right && b == Dir::Left);
// // // // // // // // //         };
// // // // // // // // //         Dir ndir = dir;
// // // // // // // // //         if (key == 'W') ndir = Dir::Up;
// // // // // // // // //         else if (key == 'S') ndir = Dir::Down;
// // // // // // // // //         else if (key == 'A') ndir = Dir::Left;
// // // // // // // // //         else if (key == 'D') ndir = Dir::Right;
// // // // // // // // //         if (!opp(dir, ndir)) dir = ndir;
// // // // // // // // //     }

// // // // // // // // //     void update() {
// // // // // // // // //         if (game_over) return;
// // // // // // // // //         Point head = snake.front();
// // // // // // // // //         switch (dir) {
// // // // // // // // //             case Dir::Up:    head.r--; break;
// // // // // // // // //             case Dir::Down:  head.r++; break;
// // // // // // // // //             case Dir::Left:  head.c--; break;
// // // // // // // // //             case Dir::Right: head.c++; break;
// // // // // // // // //         }
// // // // // // // // //         if (head.r < 0) head.r = ROWS - 1;
// // // // // // // // //         if (head.r >= ROWS) head.r = 0;
// // // // // // // // //         if (head.c < 0) head.c = COLS - 1;
// // // // // // // // //         if (head.c >= COLS) head.c = 0;

// // // // // // // // //         // Collision with self
// // // // // // // // //         if (any_of(snake.begin(), snake.end(),
// // // // // // // // //                    [&](const Point& p){ return p.r == head.r && p.c == head.c; })) {
// // // // // // // // //             game_over = true; return;
// // // // // // // // //         }

// // // // // // // // //         snake.push_front(head);

// // // // // // // // //         // Check food
// // // // // // // // //         if (head.r == food.r && head.c == food.c) {
// // // // // // // // //             score += 10;
// // // // // // // // //             chomp_frames = 5;  // head grows for a short time
// // // // // // // // //             place_food();
// // // // // // // // //         } else {
// // // // // // // // //             snake.pop_back();
// // // // // // // // //             if (chomp_frames > 0) chomp_frames--;
// // // // // // // // //         }
// // // // // // // // //     }

// // // // // // // // //     void render() const {
// // // // // // // // //         cout << "\x1b[2J\x1b[H"; // Clear and home cursor

// // // // // // // // //         // Top border + score
// // // // // // // // //         cout << '+';
// // // // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // // // //         cout << "+  Score: " << score << "\n";

// // // // // // // // //         // Game field
// // // // // // // // //         for (int r = 0; r < ROWS; ++r) {
// // // // // // // // //             cout << '|';
// // // // // // // // //             cout << BG_BLUE << FG_WHITE;

// // // // // // // // //             for (int c = 0; c < COLS; ++c) {
// // // // // // // // //                 // Food
// // // // // // // // //                 if (food.r == r && food.c == c) {
// // // // // // // // //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// // // // // // // // //                     continue;
// // // // // // // // //                 }

// // // // // // // // //                 bool on_snake = false;
// // // // // // // // //                 size_t seg_index = 0;
// // // // // // // // //                 for (size_t i = 0; i < snake.size(); ++i) {
// // // // // // // // //                     if (snake[i].r == r && snake[i].c == c) {
// // // // // // // // //                         on_snake = true;
// // // // // // // // //                         seg_index = i;
// // // // // // // // //                         break;
// // // // // // // // //                     }
// // // // // // // // //                 }

// // // // // // // // //                 if (on_snake) {
// // // // // // // // //                     if (seg_index == 0 && chomp_frames > 0) {
// // // // // // // // //                         // Enlarged head (2-wide)
// // // // // // // // //                         cout << FG_BRIGHT_GREEN << "OO" << FG_WHITE;
// // // // // // // // //                         ++c; // skip next column
// // // // // // // // //                     } else {
// // // // // // // // //                         cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// // // // // // // // //                     }
// // // // // // // // //                 } else {
// // // // // // // // //                     cout << ' ';
// // // // // // // // //                 }
// // // // // // // // //             }

// // // // // // // // //             cout << RESET;
// // // // // // // // //             cout << "|\n";
// // // // // // // // //         }

// // // // // // // // //         // Bottom border
// // // // // // // // //         cout << '+';
// // // // // // // // //         for (int c = 0; c < COLS; ++c) cout << '-';
// // // // // // // // //         cout << "+\n";

// // // // // // // // //         cout << "W/A/S/D to move, Q to quit.\n";
// // // // // // // // //         if (game_over) cout << "Game Over. Press Q to exit.\n";
// // // // // // // // //         cout.flush();
// // // // // // // // //     }
// // // // // // // // // };

// // // // // // // // // int main() {
// // // // // // // // //     ios::sync_with_stdio(false);
// // // // // // // // //     cin.tie(nullptr);

// // // // // // // // //     RawTerm raw;
// // // // // // // // //     Game game;
// // // // // // // // //     auto next_tick = chrono::steady_clock::now();

// // // // // // // // //     while (running.load()) {
// // // // // // // // //         // read all pending keys
// // // // // // // // //         while (true) {
// // // // // // // // //             auto k = read_key_now();
// // // // // // // // //             if (!k) break;
// // // // // // // // //             if (*k == 3) { running.store(false); break; } // Ctrl-C
// // // // // // // // //             enqueue(*k);
// // // // // // // // //         }

// // // // // // // // //         if (auto key = poll_key()) {
// // // // // // // // //             if (*key == 'Q') running.store(false);
// // // // // // // // //             else game.change_dir(*key);
// // // // // // // // //         }

// // // // // // // // //         auto now = chrono::steady_clock::now();
// // // // // // // // //         if (now >= next_tick) {
// // // // // // // // //             while (now >= next_tick) {
// // // // // // // // //                 game.update();
// // // // // // // // //                 next_tick += TICK;
// // // // // // // // //             }
// // // // // // // // //             game.render();
// // // // // // // // //         } else {
// // // // // // // // //             this_thread::sleep_until(next_tick);
// // // // // // // // //         }
// // // // // // // // //     }

// // // // // // // // //     cout << RESET << "\x1b[2J\x1b[H";
// // // // // // // // //     cout << "Thanks for playing.\n";
// // // // // // // // //     return 0;
// // // // // // // // // }