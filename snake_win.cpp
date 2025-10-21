// snake_win.cpp — Windows-native console Snake
// Features:
// - Centered playfield; blue interior, green snake ("●"), yellow food ("●")
// - Pac-Man-ish gulp overlay when eating (round head overlay)
// - Poop trail: 3 brown dots after each eat, fading over time
// - Sounds (Windows Beep-based): bite (random tones), fart (low rumble), splash fanfare, level-up ping
// - Splash: centered ASCII art (stays in terminal; no external image viewer)
// - LEVEL UP: every +100 points → screen blink + "LEVEL UP!" + faster speed

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <conio.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
#include <iostream>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ---------- ANSI helpers (Windows) ----------
static bool enable_vt_mode() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return false;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    return SetConsoleMode(hOut, mode) != 0;
}
static void set_utf8() {
    SetConsoleOutputCP(CP_UTF8);
}

// ---------- ANSI colors ----------
static constexpr const char* RESET = "\x1b[0m";
static constexpr const char* BG_BLUE = "\x1b[44m";
static constexpr const char* FG_WHITE = "\x1b[37m";
static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m";
static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m";
static constexpr const char* FG_BROWN_256     = "\x1b[38;5;130m";
static constexpr const char* FG_GREEN         = "\x1b[32m";

// ---------- Game + timing config ----------
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
static constexpr int BASE_TICK_MS = 100; // start speed (10 FPS)
static constexpr int MIN_TICK_MS  = 30;  // cap (~33 FPS)
static constexpr int TICK_DECR_MS = 15;  // faster by 15ms each level-up
static constexpr int POOP_TTL     = 12;  // poop fades

// ---------- Input queue ----------
queue<char> in_q;

// ---------- Helpers ----------
static int term_cols() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return COLS;
}
static int term_rows() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    return ROWS + 6;
}
static void center_line(const std::string& s) {
    int w = term_cols();
    int pad = std::max(0, (int)(w - (int)s.size()) / 2);
    for (int i = 0; i < pad; ++i) std::cout << ' ';
    std::cout << s << "\n";
}
static void clr() { std::cout << "\x1b[2J\x1b[H"; }

// ---------- Sounds (Beep-based) ----------
static inline void tone(int freq, int ms) { Beep(freq, ms); }

// French-horn-ish fanfare at splash (simple triad swell)
static void play_french_horn_fanfare() {
    // Rough “brassy” envelope
    tone(440, 90);  Sleep(20);
    tone(554, 90);  Sleep(20);
    tone(659, 120); Sleep(40);
    tone(740, 150); Sleep(40);
    tone(659, 120); Sleep(20);
    tone(554, 160);
}

// Bite: random tiny chirps
static void play_random_bite_sound(std::mt19937& rng) {
    std::uniform_int_distribution<int> f(700, 1200);
    int a = f(rng), b = f(rng);
    tone(a, 40); Sleep(5); tone(b, 40);
}

// Fart: low descending rumble
static void play_fart_sound() {
    for (int f = 220; f >= 120; f -= 10) tone(f, 12);
}

// Level up: bright ping
static void play_levelup_sound() {
    tone(1568, 80); Sleep(10); tone(1760, 110);
}

// ---------- Splash (ASCII) ----------
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
    clr();
    std::cout << "\x1b[?25l" << std::flush;
    // fanfare
    std::thread fan(play_french_horn_fanfare);

    ascii_splash_art();

    // Pulsing centered prompt
    bool bright = true;
    auto last = std::chrono::steady_clock::now();
    while (true) {
        // pump keys (any key continues)
        if (_kbhit()) { (void)_getch(); break; }

        auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::milliseconds(400)) {
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
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (fan.joinable()) fan.join();

    std::cout << "\x1b[?25h";
    clr();
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

    // Level-up state
    int level = 1;
    int level_flash = 0;           // frames to blink screen/message
    bool level_up_trigger = false; // consumed in main() to change speed

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
        if (level_flash > 0) level_flash--;

        if (consuming) {
            if (--chomp_frames <= 0) {
                Point nh = next_head(snake.front());
                if (any_of(snake.begin(), snake.end(),
                           [&](const Point& p){ return p.r == nh.r && p.c == nh.c; })) {
                    game_over = true; return;
                }
                snake.push_front(nh); // grow
                score += 10;

                // Level-up every 100 pts
                if (score % 100 == 0) {
                    level++;
                    level_flash = 12;        // ~1.2s at 10 FPS baseline
                    level_up_trigger = true; // main() speeds up
                    play_levelup_sound();
                }

                play_random_bite_sound(rng);
                poop_to_drop = 3;

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
            play_fart_sound();
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
        // center the whole box laterally
        int box_width = COLS + 2;
        int pad = std::max(0, (term_cols() - box_width) / 2);

        // quick screen flash on level-up: reverse-video pulses
        if (level_flash > 0 && ((level_flash / 2) % 2 == 0)) {
            cout << "\x1b[7m";  // reverse video ON
        } else {
            cout << "\x1b[27m"; // reverse video OFF
        }

        clr();

        // centered score/status line
        {
            std::string status = "Score: " + std::to_string(score) + "   Level: " + std::to_string(level);
            if (consuming)    status += "   (CHOMP!)";
            if (poop_to_drop) status += "   (Dropping...)";
            center_line(status);
        }

        // top border
        for (int i = 0; i < pad; ++i) cout << ' ';
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+\n";

        // rows
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

        // bottom border
        for (int i = 0; i < pad; ++i) cout << ' ';
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+\n";

        // controls/help
        center_line("W/A/S/D to move, Q to quit.");
        if (game_over) center_line("Game Over. Press Q to exit.");

        // level-up banner while flashing
        if (level_flash > 0) {
            center_line("\x1b[1m\x1b[93mLEVEL UP!  Speed increased\x1b[0m");
        }

        cout.flush();
    }
};

// ---------- Main (Windows) ----------
int main() {
    set_utf8();
    enable_vt_mode();

    // Splash (ASCII + fanfare)
    cinematic_splash_and_wait();

    Game game;

    int tick_ms = BASE_TICK_MS;
    auto current_tick = chrono::milliseconds(tick_ms);
    auto next_tick = chrono::steady_clock::now();

    while (true) {
        // pump keys quickly (non-blocking)
        while (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) {
                // swallow arrow prefix if needed (ignore for now)
                (void)_getch();
                continue;
            }
            ch = toupper(ch);
            if (ch == 'Q') return 0;
            if (ch == 'W' || ch == 'A' || ch == 'S' || ch == 'D') {
                in_q.push((char)ch);
            }
        }

        // apply input
        if (!in_q.empty()) {
            char k = in_q.front(); in_q.pop();
            game.change_dir(k);
        }

        auto now = chrono::steady_clock::now();
        if (now >= next_tick) {
            while (now >= next_tick) {
                game.update();

                // respond to level-up: speed up (every 100 points)
                if (game.level_up_trigger) {
                    game.level_up_trigger = false;
                    tick_ms = std::max(MIN_TICK_MS, tick_ms - TICK_DECR_MS);
                    current_tick = chrono::milliseconds(tick_ms);
                }

                next_tick += current_tick;
            }
            game.render();
        } else {
            this_thread::sleep_until(next_tick);
        }
    }
    return 0;
}
