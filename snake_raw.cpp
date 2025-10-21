// snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// Poop → Bomb (harmless if eaten) system, with BIG emoji head while chomping
// + Floating text: when a poop activates, “NICE SHIT!” rises up from it.
//
// - 0–15s: Good poop (brown ●). Eat it: slow to base & shrink up to 2.
// - 15–30s: Arms into a flashing red bomb (✹). If you EAT it → neutralize (no penalty).
// - 30s: If it EXPIRES (you didn't reach it), it despawns and queues +2 growth (mild punishment).
// Big chomp: while eating, the head becomes a single wide round emoji (🟢) and then returns to normal.
// Floating text: on poop activation (tail vacates), show “NICE SHIT!” rising for ~2s.
//
// Kept:
// - Blue field, green snake (●), yellow food (●)
// - Poop seeds under tail; 3 drops per food
// - Idle bloat, level-ups every +100, per-growth speed bump, centered board, splash

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
#include <ctime>

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
static constexpr const char* FG_RED           = "\x1b[91m";
static constexpr const char* FG_ORANGE_208    = "\x1b[38;5;208m";
static constexpr const char* FG_YELLOW        = "\x1b[33m";

// ---------- Config ----------
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
// Dynamic speed control:
static constexpr int BASE_TICK_MS = 100; // 10 FPS
static constexpr int MIN_TICK_MS  = 40;  // fastest cap
static constexpr int TICK_DECR_MS = 10;  // level-up speed gain
static constexpr int GROW_DECR_MS = 3;   // per-unit speed gain (food / idle / penalty growth)

// Poop/Bomb timings & penalty
static constexpr auto GOOD_WINDOW = std::chrono::seconds(15);
static constexpr auto BOMB_WINDOW = std::chrono::seconds(15);
static constexpr int  BOMB_GROW_UNITS = 2;

// Wide-head glyph during chomp (double-width in most terminals)
static const std::string WIDE_HEAD = "🟢";

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
    cmd += name; cmd += " >/dev/null 2>&1";
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
    return COLS;
}
static int term_rows() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return ROWS + 6;
}

// centered print
static void center_line(const std::string& s) {
    int w = term_cols();
    int pad = std::max(0, (int)(w - (int)s.size()) / 2);
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << s << "\n";
}

// --- tiny file->string and base64 for iTerm2 inline image ---
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
static constexpr int SPLASH_SCALE_PCT = 40; // ~40% of terminal width

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

    int cols = term_cols();
    int img_cols = std::max(10, (cols * SPLASH_SCALE_PCT) / 100);
    int pad = std::max(0, (cols - img_cols) / 2);

    if (!showed_image && is_iterm() && file_exists(SPLASH_PATH)) {
        std::string data;
        if (read_file(SPLASH_PATH, data)) {
            center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
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

    if (!showed_image && std::getenv("KITTY_WINDOW_ID") && have_cmd("kitty") && file_exists(SPLASH_PATH)) {
        showed_image = true;
        int w = img_cols;
        int h = std::max(6,  (term_rows() * SPLASH_SCALE_PCT) / 100);
        std::cout << "\x1b[2J\x1b[H";
        std::string cmd = "kitty +kitten icat --align center --place "
                        + std::to_string(w) + "x" + std::to_string(h) + "@0x0 '" + SPLASH_PATH + "'";
        (void)std::system(cmd.c_str());
        center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
        std::cout << "\n";
    }

    if (!showed_image && is_iterm() && have_cmd("imgcat") && file_exists(SPLASH_PATH)) {
        showed_image = true;
        std::cout << "\x1b[2J\x1b[H";
        for (int i = 0; i < pad; ++i) std::cout << ' ';
        std::string cmd = "imgcat --width=" + std::to_string(img_cols) + " '" + SPLASH_PATH + "'";
        (void)std::system(cmd.c_str());
        center_line("\x1b[1m\x1b[92mTHE FIERCE POOPING SNAKE\x1b[0m");
        std::cout << "\n";
    }

    if (!showed_image) { std::cout << "\x1b[2J\x1b[H"; ascii_splash_art(); }

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
            int pad2 = std::max(0, (int)(w - (int)msg.size()) / 2);
            std::cout << "\r";
            for (int i = 0; i < pad2; ++i) std::cout << ' ';
            std::cout << msg << std::flush;
        }
        std::this_thread::sleep_for(50ms);
    }

    std::cout << "\x1b[?25h\x1b[2J\x1b[H" << std::flush;
}

// ---------- Game model ----------
struct Point { int r, c; };
enum class Dir { Up, Down, Left, Right };

enum class PoopState { Good, Bomb };
struct Poop {
    Point p;
    std::chrono::steady_clock::time_point activated_at;
    PoopState state{PoopState::Good};
    bool expired_punished{false};
};

struct Explosion {
    Point center;
    int frames_left{5};
    std::vector<Point> ring;
};

// Floating text particle: rises upward and fades
struct FloatText {
    std::string msg;
    int row;            // current row in playfield (0..ROWS-1)
    int col_start;      // starting column for msg (0..COLS-1)
    int age{0};         // ticks since spawn
    int life{20};       // total ticks to live (~2s at base speed)
    int step{3};        // rise one row every 'step' ticks
};

struct Game {
    deque<Point> snake; // front=head
    Dir dir = Dir::Right;
    Point food{0, 0};
    bool game_over = false;
    int score = 0;

    // Bite animation (wide head while true)
    bool consuming = false;
    int chomp_frames = 0;
    static constexpr int CHOMP_TOTAL = 8;

    // Poop / Bombs
    int poop_to_drop = 0;
    vector<Poop>  poops;
    vector<Point> poop_seeds;
    vector<Explosion> booms;

    // Floaters (NICE SHIT!)
    vector<FloatText> floats;

    int growth_pending = 0;    // queued growth (penalties)
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

    mt19937 rng{random_device{}()};

    Game() {
        int r = ROWS / 2, c = COLS / 2;
        snake.push_back({r, c});
        snake.push_back({r, c - 1});
        snake.push_back({r, c - 2});
        place_food();
    }

    void on_player_input() { idle_ticks = 0; }
    void refresh_idle_threshold() {
        idle_bloat_threshold = std::max(80, 120 - (level - 1) * 5);
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

        Explosion e;
        e.center = at;
        e.frames_left = 5;
        e.ring = explosion_ring(at);
        booms.push_back(e);

        play_system_sound(BOMB_SOUND);
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

        // Expire → penalty
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
            if (ft.age % ft.step == 0 && ft.row > 0) ft.row--;  // rise
        }
        floats.erase(remove_if(floats.begin(), floats.end(),
                               [](const FloatText& f){ return f.age >= f.life || f.row < 0; }),
                     floats.end());
    }

    bool cell_on_snake(int rr, int cc) const {
        for (const auto& seg : snake) if (seg.r == rr && seg.c == cc) return true;
        return false;
    }

    bool find_poop_at(Point p, size_t* idx_out=nullptr) const {
        for (size_t i = 0; i < poops.size(); ++i) {
            if (poops[i].p.r == p.r && poops[i].p.c == p.c) {
                if (idx_out) *idx_out = i;
                return true;
            }
        }
        return false;
    }

    void maybe_activate_poops() {
        if (poop_seeds.empty()) return;
        vector<Point> remaining;
        remaining.reserve(poop_seeds.size());
        auto now = std::chrono::steady_clock::now();
        for (const auto& s : poop_seeds) {
            if (!cell_on_snake(s.r, s.c)) {
                Poop pp; pp.p = s; pp.activated_at = now; pp.state = PoopState::Good; pp.expired_punished = false;
                poops.push_back(pp);
                play_system_sound(FART_SOUND);

                // Spawn floating “NICE SHIT!” over this poop, centered
                std::string msg = "NICE SHIT!";
                int len = (int)msg.size();
                int c0 = std::max(0, std::min(COLS - len, s.c - len/2));
                floats.push_back(FloatText{msg, s.r, c0, 0, 20, 3});
            } else {
                remaining.push_back(s);
            }
        }
        poop_seeds.swap(remaining);
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
                if (any_of(snake.begin(), snake.end(),
                           [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
                    game_over = true; return;
                }
                snake.push_front(nh); // grow on food
                score += 10;

                speed_bump_trigger = true;
                speed_bump_amount  += 1;

                if (score % 100 == 0) {
                    level++;
                    level_flash = 12;
                    level_up_trigger = true;
                    play_system_sound(LEVEL_SOUND);
                    refresh_idle_threshold();
                }

                play_random_bite_sound(rng);
                poop_to_drop = 3;

                place_food();
                consuming = false;
            }
            return;
        }

        Point nh = next_head(snake.front());

        // start chomp
        if (nh.r == food.r && nh.c == food.c) {
            consuming = true;
            chomp_frames = CHOMP_TOTAL;
            return;
        }

        // Poop/Bomb at next head cell?
        size_t poop_idx = 0;
        bool on_poop = find_poop_at(nh, &poop_idx);

        // Self-collision
        if (any_of(snake.begin(), snake.end(),
                   [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
            game_over = true; return;
        }

        // Move head
        Point tail_before = snake.back();
        snake.push_front(nh);

        bool grew_this_tick = false;

        if (on_poop) {
            PoopState st = poops[poop_idx].state;
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
                play_system_sound(REWARD_SOUND);
            } else {
                // Bomb eaten → harmless
                play_system_sound(DISARM_SOUND);
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

        if (!grew_this_tick) {
            snake.pop_back();
        }

        // queue poop seed
        if (poop_to_drop > 0) {
            poop_seeds.push_back(tail_before);
            poop_to_drop--;
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

    // Check if a floating text overlays this cell; if yes, print its character and return true.
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

        // top border
        for (int i = 0; i < pad; ++i) cout << ' ';
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+\n";

        // rows
        Point head = snake.front();
        bool show_wide_head = consuming && head.c < COLS - 1; // avoid overflow at far right

        for (int r = 0; r < ROWS; ++r) {
            for (int i = 0; i < pad; ++i) cout << ' ';
            cout << '|';
            bool invert = (level_flash > 0 && ((level_flash / 2) % 2 == 0));
            if (invert) cout << "\x1b[7m";
            cout << BG_BLUE << FG_WHITE;

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

                // 3) Big head (double-width emoji)
                if (show_wide_head && r == head.r && c == head.c) {
                    std::cout << WIDE_HEAD;
                    ++c; // skip next cell
                    continue;
                }

                // 4) Food
                if (food.r == r && food.c == c) {
                    cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
                    continue;
                }

                // 5) Head (normal)
                if ((!show_wide_head || r != head.r || c != head.c) &&
                    r == head.r && c == head.c) {
                    cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
                    continue;
                }

                // 6) Body
                bool on_body = false;
                for (size_t i = 1; i < snake.size(); ++i) {
                    if (snake[i].r == r && snake[i].c == c) { on_body = true; break; }
                }
                if (on_body) {
                    cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
                    continue;
                }

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

            cout << RESET << "|\n";
        }

        // bottom border
        for (int i = 0; i < pad; ++i) cout << ' ';
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+\n";

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

    // Splash
    cinematic_splash_and_wait();

    Game game;
    game.refresh_idle_threshold();

    int tick_ms = BASE_TICK_MS;
    auto current_tick = chrono::milliseconds(tick_ms);
    auto next_tick = chrono::steady_clock::now();

    while (running.load()) {
        // pump raw keys
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

                // Priority: reward slow-down overrides bumps this tick
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
            game.render();
        } else {
            this_thread::sleep_until(next_tick);
        }
    }

    cout << RESET << "\x1b[2J\x1b[H";
    cout << "Thanks for playing.\n";
    return 0;
}
