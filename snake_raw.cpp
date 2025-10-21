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
#include <sys/ioctl.h>
#include <cstdio>

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

// ---------- Helpers ----------
static bool have_cmd(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
static bool file_exists(const char* p) {
    struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static bool env_set(const char* name) { return std::getenv(name) != nullptr; }

// live terminal size (columns/rows)
static int term_cols() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return COLS; // fallback
}
static int term_rows() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return ROWS + 6; // slack for title/prompt
}

// centered print using current terminal width
static void center_line(const std::string& s) {
    int w = term_cols();
    int pad = std::max(0, (int)(w - (int)s.size()) / 2);
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << s << "\n";
}

// tiny file->string and base64 for iTerm2 inline image (OSC 1337)
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
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
        out.push_back(tbl[(v >> 6)  & 63]);
        out.push_back('=');
    }
    return out;
}
static bool is_iterm() { return std::getenv("ITERM_SESSION_ID") != nullptr; }

// ---------- Splash ----------
static constexpr const char* SPLASH_PATH = "assets/splash.png";
static constexpr int SPLASH_SCALE_PCT = 40; // make image ~40% of terminal width/area

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

    std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;
    play_system_sound(SPLASH_SOUND);

    bool showed_image = false;

    // --- iTerm2: inline PNG (OSC 1337), auto-fit width of terminal ---
    if (!showed_image && is_iterm() && file_exists(SPLASH_PATH)) {
        std::string data;
        if (read_file(SPLASH_PATH, data)) {
            center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
            std::cout << "\n";
            std::string b64 = b64_encode(data);
            // width=100% keeps image within current viewport; aspect preserved
            std::cout << "\x1b]1337;File=name=splash.png;inline=1;width="
            << SPLASH_SCALE_PCT << "%;preserveAspectRatio=1:"
            << b64 << "\x07\n";
        
            showed_image = true;
        }
    }

    // --- kitty: inline PNG via icat, sized to current terminal area ---
    if (!showed_image && std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty") && file_exists(SPLASH_PATH)) {
        showed_image = true;
        int w = std::max(10, (term_cols() * SPLASH_SCALE_PCT) / 100);
        int h = std::max(6,  (term_rows() * SPLASH_SCALE_PCT) / 100);

        std::cout << "\x1b[2J\x1b[H";
        std::string cmd = "kitty +kitten icat --align center --place "
                        + std::to_string(w) + "x" + std::to_string(h) + "@0x0 '" + SPLASH_PATH + "'";
        (void)std::system(cmd.c_str());
        center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
        std::cout << "\n";
    }

    // --- iTerm2 imgcat CLI (still inline), sized to current columns ---
    if (!showed_image && is_iterm() && have_cmd("imgcat") && file_exists(SPLASH_PATH)) {
        showed_image = true;
        int wcols = std::max(20, (term_cols() * SPLASH_SCALE_PCT) / 100);
        std::cout << "\x1b[2J\x1b[H";
        std::string cmd = "imgcat --width=" + std::to_string(wcols) + " '" + SPLASH_PATH + "'";
        int rc = std::system(cmd.c_str());
        if (rc != 0) showed_image = false;
        center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
        std::cout << "\n";
    }

    // --- Fallback: centered ASCII splash ---
    if (!showed_image) {
        std::cout << "\x1b[2J\x1b[H";
        ascii_splash_art();
    }

    // Pulsing centered prompt (re-centers if user resizes)
    bool bright = true;
    auto last = std::chrono::steady_clock::now();
    while (true) {
        if (auto k = read_key_now()) break;
        auto now = std::chrono::steady_clock::now();
        if (now - last >= 400ms) {
            bright = !bright; last = now;
            std::string msg = bright
                ? std::string("\x1b[92m[ Press any key to continue ]\x1b[0m")
                : std::string("\x1b[32m[ Press any key to continue ]\x1b[0m");
            int w = term_cols();
            int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
            std::cout << "\r";
            for (int i = 0; i < pad; ++i) std::cout << ' ';
            std::cout << msg << std::flush;
        }
        std::this_thread::sleep_for(50ms);
    }

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
    // how many spaces to pad so the box (COLS+2 wide including borders) is centered
    int box_width = COLS + 2;
    int pad = std::max(0, (term_cols() - box_width) / 2);

    cout << "\x1b[2J\x1b[H";

    // centered score/status line
    {
        std::string status = "Score: " + std::to_string(score);
        if (consuming)    status += "   (CHOMP!)";
        if (poop_to_drop) status += "   (Dropping...)";
        center_line(status);
    }

    // top border, centered
    for (int i = 0; i < pad; ++i) cout << ' ';
    cout << '+';
    for (int c = 0; c < COLS; ++c) cout << '-';
    cout << "+\n";

    // rows, centered
    for (int r = 0; r < ROWS; ++r) {
        for (int i = 0; i < pad; ++i) cout << ' ';
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
            } else if (cell_has_poop(r, c)) {
                cout << FG_BROWN_256 << "●" << FG_WHITE;
            } else {
                cout << ' ';
            }
        }

        cout << RESET << "|\n";
    }

    // bottom border, centered
    for (int i = 0; i < pad; ++i) cout << ' ';
    cout << '+';
    for (int c = 0; c < COLS; ++c) cout << '-';
    cout << "+\n";

    // centered controls/help
    center_line("W/A/S/D to move, Q to quit.");
    if (game_over) center_line("Game Over. Press Q to exit.");

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
// // Features:
// // - Blue playfield, green snake ("●"), yellow food ("●")
// // - Pac-Man-ish gulp overlay when eating, plus score increment
// // - Poop trail: 3 brown dots after each eat, fading over time
// // - Sounds: randomized bite (Pop/Bottle/Funk/Tink/Ping), "Submarine" on poop
// // - Splash: inline PNG if kitty/iTerm2 supports it; otherwise colored ASCII splash
// //   (No external Preview/Quick Look window; stays inside terminal)

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

// // ---------- Sound config ----------
// static constexpr bool ENABLE_SOUNDS = true;
// static constexpr bool ENABLE_BEEP_FALLBACK = false;

// static const char* BITE_SOUNDS[] = { "Pop", "Bottle", "Funk", "Tink", "Ping" };
// static constexpr const char* FART_SOUND   = "Submarine";
// static constexpr const char* SPLASH_SOUND = "Purr"; // soft hiss/rumble

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

// // ---------- ANSI colors ----------
// static constexpr const char* RESET = "\x1b[0m";
// static constexpr const char* BG_BLUE = "\x1b[44m";
// static constexpr const char* FG_WHITE = "\x1b[37m";
// static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
// static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
// static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
// static constexpr const char* FG_GRAY          = "\x1b[90m";
// static constexpr const char* FG_GREEN         = "\x1b[32m";

// // ---------- Config ----------
// static constexpr int ROWS = 20;
// static constexpr int COLS = 80;
// static constexpr chrono::milliseconds TICK{100}; // 10 FPS
// static constexpr int POOP_TTL = 12;              // poop fades

// // ---------- Raw terminal guard ----------
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

// // ---------- Input queue ----------
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

// // ---------- Small helpers ----------
// static bool have_cmd(const char* name) {
//     std::string cmd = "command -v ";
//     cmd += name;
//     cmd += " >/dev/null 2>&1";
//     return std::system(cmd.c_str()) == 0;
// }
// static bool file_exists(const char* p) {
//     struct stat st{}; return ::stat(p, &st) == 0 && S_ISREG(st.st_mode);
// }
// static void center_line(const std::string& s) {
//     int w = COLS; int pad = std::max(0, (int)(w - (int)s.size()) / 2);
//     for (int i = 0; i < pad; ++i) std::cout << ' ';
//     std::cout << s << "\n";
// }
// static bool env_set(const char* name) { return std::getenv(name) != nullptr; }

// // ---------- Splash (inline PNG if kitty/iTerm2; else ASCII) ----------
// static constexpr const char* SPLASH_PATH = "assets/splash.png";

// static void ascii_splash_art() {
//     const char* G  = "\x1b[92m";   // green
//     const char* Y  = "\x1b[93m";   // yellow
//     const char* R  = "\x1b[91m";   // red
//     const char* Wt = "\x1b[97m";   // white
//     const char* Br = "\x1b[38;5;130m"; // brown
//     const char* Rt = "\x1b[0m";
//     std::vector<std::string> art = {
//         std::string(G) + "           ________                          " + Rt,
//         std::string(G) + "        .-`  ____  `-.                       " + Rt,
//         std::string(G) + "      .'   .`    `.   `.                     " + Rt,
//         std::string(G) + "     /   .'   " + R + "◥◤" + G + "   `.   \\                    " + Rt,
//         std::string(G) + "    ;   /    " + Wt + " __ __ " + G + "   \\   ;                   " + Rt,
//         std::string(G) + "    |  |   " + Wt + " /__V__\\ " + G + "  |  |   " + Y + "   ●" + Rt,
//         std::string(G) + "    |  |  " + R + "  \\____/ " + G + "  " + R + "\\/" + G + " |  |   " + Br + "  ● ● ●" + Rt,
//         std::string(G) + "    ;   \\      " + R + "┏━┓" + G + "      /   ;   " + Br + "  ●●● ●●●" + Rt,
//         std::string(G) + "     \\    `._ " + Wt + "V  V" + G + "  _.'   /                    " + Rt,
//         std::string(G) + "      `.     `-.__.-'     .'                 " + Rt,
//         std::string(G) + "        `-._            _.-'                  " + Rt
//     };
//     center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//     std::cout << "\n";
//     for (auto& line : art) center_line(line);
//     std::cout << "\n";
// }

// // --- tiny file->string and base64 for iTerm2 inline image ---
// static bool read_file(const char* path, std::string& out) {
//     FILE* f = std::fopen(path, "rb");
//     if (!f) return false;
//     std::vector<unsigned char> buf(4096);
//     out.clear();
//     size_t n;
//     while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) out.append((const char*)buf.data(), n);
//     std::fclose(f);
//     return true;
// }
// static std::string b64_encode(const std::string& in) {
//     static const char* tbl =
//         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
//     std::string out; out.reserve((in.size()*4+2)/3);
//     size_t i = 0;
//     while (i + 3 <= in.size()) {
//         unsigned int v = ((unsigned char)in[i] << 16) |
//                          ((unsigned char)in[i+1] << 8) |
//                          ((unsigned char)in[i+2]);
//         out.push_back(tbl[(v >> 18) & 63]);
//         out.push_back(tbl[(v >> 12) & 63]);
//         out.push_back(tbl[(v >> 6)  & 63]);
//         out.push_back(tbl[v & 63]);
//         i += 3;
//     }
//     if (i + 1 == in.size()) {
//         unsigned int v = ((unsigned char)in[i]) << 16;
//         out.push_back(tbl[(v >> 18) & 63]);
//         out.push_back(tbl[(v >> 12) & 63]);
//         out.push_back('=');
//         out.push_back('=');
//     } else if (i + 2 == in.size()) {
//         unsigned int v = (((unsigned char)in[i]) << 16) |
//                          (((unsigned char)in[i+1]) << 8);
//         out.push_back(tbl[(v >> 18) & 63]);
//         out.push_back(tbl[(v >> 12) & 63]);
//         out.push_back(tbl[(v >> 6)  & 63]);
//         out.push_back('=');
//     }
//     return out;
// }

// #include <sys/ioctl.h>

// // live terminal size (columns/rows)
// static int term_cols() {
//     winsize ws{};
//     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
//     return COLS; // fallback to your compiled-in width
// }
// static int term_rows() {
//     winsize ws{};
//     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
//     return ROWS + 6; // bit of slack for borders/title
// }

// // centered print using current terminal width
// static void center_line(const std::string& s) {
//     int w = term_cols();
//     int pad = std::max(0, (int)(w - (int)s.size()) / 2);
//     for (int i = 0; i < pad; ++i) std::cout << ' ';
//     std::cout << s << "\n";
// }

// static bool is_iterm() { return std::getenv("ITERM_SESSION_ID") != nullptr; }

// static void cinematic_splash_and_wait() {
//     using namespace std::chrono;

//     std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;
//     play_system_sound(SPLASH_SOUND);

//     bool showed_image = false;

//     // --- iTerm2: draw PNG inline, scaled to terminal width (100% of columns) ---
//     if (!showed_image && std::getenv("ITERM_SESSION_ID") && file_exists(SPLASH_PATH)) {
//         std::string data;
//         if (read_file(SPLASH_PATH, data)) {
//             // Fit width to terminal, preserve aspect
//             center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//             std::cout << "\n";
//             std::string b64 = b64_encode(data);
//             // width in %, iTerm scales to viewport; use 100% to always fit
//             std::cout << "\x1b]1337;File=name=splash.png;inline=1;width=100%;preserveAspectRatio=1:"
//                       << b64 << "\x07\n";
//             showed_image = true;
//         }
//     }

//     // --- kitty: place image to fill available cells while centered ---
//     if (!showed_image && std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty") && file_exists(SPLASH_PATH)) {
//         showed_image = true;
//         int w = std::max(10, term_cols() - 2);              // leave a small margin
//         int h = std::max(6,  term_rows() - 8);              // room for title + prompt
//         std::cout << "\x1b[2J\x1b[H";
//         // --place WxH@0x0 paints into the grid; --align center keeps it centered
//         std::string cmd = "kitty +kitten icat --align center --place "
//                         + std::to_string(w) + "x" + std::to_string(h) + "@0x0 '" + SPLASH_PATH + "'";
//         (void)std::system(cmd.c_str());
//         center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//         std::cout << "\n";
//     }

//     // --- iTerm2 imgcat CLI fallback (still inside terminal) ---
//     if (!showed_image && std::getenv("ITERM_SESSION_ID") && have_cmd("imgcat") && file_exists(SPLASH_PATH)) {
//         showed_image = true;
//         int wcols = std::max(20, term_cols() - 4); // fit viewport, small margins
//         std::cout << "\x1b[2J\x1b[H";
//         std::string cmd = "imgcat --width=" + std::to_string(wcols) + " '" + SPLASH_PATH + "'";
//         int rc = std::system(cmd.c_str());
//         if (rc != 0) showed_image = false;
//         center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
//         std::cout << "\n";
//     }

//     // --- Fallback: ANSI art (always fits + centered using term_cols) ---
//     if (!showed_image) {
//         std::cout << "\x1b[2J\x1b[H";
//         ascii_splash_art(); // uses center_line() which now reads term width live
//     }

//     // Pulsing centered prompt (uses live width)
//     bool bright = true;
//     auto last = std::chrono::steady_clock::now();
//     while (true) {
//         if (auto k = read_key_now()) break;
//         auto now = std::chrono::steady_clock::now();
//         if (now - last >= 400ms) {
//             bright = !bright; last = now;
//             std::string msg = bright
//                 ? std::string("\x1b[92m[ Press any key to continue ]\x1b[0m")
//                 : std::string("\x1b[32m[ Press any key to continue ]\x1b[0m");
//             // re-center each flash in case the user resizes during splash
//             int w = term_cols();
//             int pad = std::max(0, (int)(w - (int)msg.size()) / 2);
//             std::cout << "\r";
//             for (int i = 0; i < pad; ++i) std::cout << ' ';
//             std::cout << msg << std::flush;
//         }
//         std::this_thread::sleep_for(50ms);
//     }

//     std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
// }



// // ---------- Game model ----------
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

//     // Inline PNG if supported (kitty/iTerm2), else colored ASCII — all inside terminal
//     cinematic_splash_and_wait();

//     Game game;
//     auto next_tick = chrono::steady_clock::now();

//     while (running.load()) {
//         // pump raw keys
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