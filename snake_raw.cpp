// snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// Playfield interior = blue
// Snake = bright green dots
// Food = bright yellow dot
// Snake head enlarges briefly when eating to show a "bite" animation

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <vector>
#include <termios.h>
#include <unistd.h>

using namespace std;

// --- ANSI colors ---
static constexpr const char* RESET = "\x1b[0m";
static constexpr const char* BG_BLUE = "\x1b[44m";
static constexpr const char* FG_WHITE = "\x1b[37m";
static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// --- Config ---
static constexpr int ROWS = 20;
static constexpr int COLS = 80;
static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// --- Raw terminal guard (RAII) ---
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

// --- Game model ---
struct Point { int r, c; };
enum class Dir { Up, Down, Left, Right };

struct Game {
    deque<Point> snake; // front = head
    Dir dir = Dir::Right;
    Point food{0, 0};
    bool game_over = false;
    int score = 0;
    int chomp_frames = 0; // >0 during bite animation
    mt19937 rng{random_device{}()};

    Game() {
        int r = ROWS / 2, c = COLS / 2;
        snake.push_back({r, c});
        snake.push_back({r, c - 1});
        snake.push_back({r, c - 2});
        place_food();
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

    void update() {
        if (game_over) return;
        Point head = snake.front();
        switch (dir) {
            case Dir::Up:    head.r--; break;
            case Dir::Down:  head.r++; break;
            case Dir::Left:  head.c--; break;
            case Dir::Right: head.c++; break;
        }
        if (head.r < 0) head.r = ROWS - 1;
        if (head.r >= ROWS) head.r = 0;
        if (head.c < 0) head.c = COLS - 1;
        if (head.c >= COLS) head.c = 0;

        // Collision with self
        if (any_of(snake.begin(), snake.end(),
                   [&](const Point& p){ return p.r == head.r && p.c == head.c; })) {
            game_over = true; return;
        }

        snake.push_front(head);

        // Check food
        if (head.r == food.r && head.c == food.c) {
            score += 10;
            chomp_frames = 5;  // head grows for a short time
            place_food();
        } else {
            snake.pop_back();
            if (chomp_frames > 0) chomp_frames--;
        }
    }

    void render() const {
        cout << "\x1b[2J\x1b[H"; // Clear and home cursor

        // Top border + score
        cout << '+';
        for (int c = 0; c < COLS; ++c) cout << '-';
        cout << "+  Score: " << score << "\n";

        // Game field
        for (int r = 0; r < ROWS; ++r) {
            cout << '|';
            cout << BG_BLUE << FG_WHITE;

            for (int c = 0; c < COLS; ++c) {
                // Food
                if (food.r == r && food.c == c) {
                    cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
                    continue;
                }

                bool on_snake = false;
                size_t seg_index = 0;
                for (size_t i = 0; i < snake.size(); ++i) {
                    if (snake[i].r == r && snake[i].c == c) {
                        on_snake = true;
                        seg_index = i;
                        break;
                    }
                }

                if (on_snake) {
                    if (seg_index == 0 && chomp_frames > 0) {
                        // Enlarged head (2-wide)
                        cout << FG_BRIGHT_GREEN << "OO" << FG_WHITE;
                        ++c; // skip next column
                    } else {
                        cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
                    }
                } else {
                    cout << ' ';
                }
            }

            cout << RESET;
            cout << "|\n";
        }

        // Bottom border
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
    Game game;
    auto next_tick = chrono::steady_clock::now();

    while (running.load()) {
        // read all pending keys
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
// // Playfield interior = blue, Food = bright yellow dot,
// // Snake body = solid green dots, Head = chomp animation when eating

// #include <algorithm>
// #include <atomic>
// #include <chrono>
// #include <cctype>
// #include <deque>
// #include <iostream>
// #include <mutex>
// #include <optional>
// #include <queue>
// #include <random>
// #include <thread>
// #include <vector>
// #include <termios.h>
// #include <unistd.h>

// using namespace std;

// // --- ANSI colors ---
// static constexpr const char* RESET = "\x1b[0m";
// static constexpr const char* BG_BLUE = "\x1b[44m";
// static constexpr const char* FG_WHITE = "\x1b[37m";
// static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // --- Config ---
// static constexpr int ROWS = 20;
// static constexpr int COLS = 80;
// static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

// // --- Raw terminal guard (RAII) ---
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

// // --- Game model ---
// struct Point { int r, c; };
// enum class Dir { Up, Down, Left, Right };

// // global-ish frame counter for simple animation toggling
// static unsigned long g_frame = 0;

// struct Game {
//     deque<Point> snake; // front = head
//     Dir dir = Dir::Right;
//     Point food{0, 0};
//     bool game_over = false;
//     int score = 0;
//     int chomp_frames = 0; // >0 during bite animation
//     mt19937 rng{random_device{}()};

//     Game() {
//         int r = ROWS / 2, c = COLS / 2;
//         snake.push_back({r, c});
//         snake.push_back({r, c - 1});
//         snake.push_back({r, c - 2});
//         place_food();
//     }

//     Point next_head(Point head) const {
//         switch (dir) {
//             case Dir::Up:    head.r--; break;
//             case Dir::Down:  head.r++; break;
//             case Dir::Left:  head.c--; break;
//             case Dir::Right: head.c++; break;
//         }
//         if (head.r < 0) head.r = ROWS - 1;
//         if (head.r >= ROWS) head.r = 0;
//         if (head.c < 0) head.c = COLS - 1;
//         if (head.c >= COLS) head.c = 0;
//         return head;
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

//     void update() {
//         if (game_over) return;

//         // If next step will land on food, start the bite animation
//         Point head = snake.front();
//         Point nh = next_head(head);
//         if (nh.r == food.r && nh.c == food.c) {
//             // a handful of frames so the mouth toggles open/closed as it reaches food
//             chomp_frames = 6; // ~0.6s at 10 FPS
//         }

//         // advance
//         head = nh;

//         // self collision
//         if (any_of(snake.begin(), snake.end(),
//                    [&](const Point& p){ return p.r == head.r && p.c == head.c; })) {
//             game_over = true; return;
//         }

//         snake.push_front(head);

//         if (head.r == food.r && head.c == food.c) {
//             score += 10;
//             place_food();
//         } else {
//             snake.pop_back();
//         }

//         if (chomp_frames > 0) chomp_frames--;
//     }

//     void render() const {
//         // Clear screen & move cursor home
//         cout << "\x1b[2J\x1b[H";

//         // Top border + score (default background)
//         cout << '+';
//         for (int c = 0; c < COLS; ++c) cout << '-';
//         cout << "+  Score: " << score << "\n";

//         // Each row: left border (default), interior (blue bg), right border (default)
//         for (int r = 0; r < ROWS; ++r) {
//             cout << '|';                 // left border on default bg
//             cout << BG_BLUE << FG_WHITE; // start blue background for playfield

//             for (int c = 0; c < COLS; ++c) {
//                 // Food
//                 if (food.r == r && food.c == c) {
//                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
//                     continue;
//                 }

//                 // Snake?
//                 bool on_snake = false;
//                 size_t seg_index = 0;
//                 for (size_t i = 0; i < snake.size(); ++i) {
//                     if (snake[i].r == r && snake[i].c == c) {
//                         on_snake = true; seg_index = i; break;
//                     }
//                 }

//                 if (on_snake) {
//                     if (seg_index == 0) {
//                         // Head: chomp animation
//                         // Toggle between 'closed' and 'open' based on chomp_frames & frame parity
//                         bool open = (chomp_frames > 0) && ((g_frame % 2) == 0);
//                         const char* glyph = "●"; // closed mouth
//                         if (open) {
//                             switch (dir) {
//                                 case Dir::Right: glyph = "▶"; break;
//                                 case Dir::Left:  glyph = "◀"; break;
//                                 case Dir::Up:    glyph = "▲"; break;
//                                 case Dir::Down:  glyph = "▼"; break;
//                             }
//                         }
//                         cout << FG_BRIGHT_GREEN << glyph << FG_WHITE;
//                     } else {
//                         // Body
//                         cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
//                     }
//                 } else {
//                     cout << ' '; // blue background shows through
//                 }
//             }

//             cout << RESET;              // stop blue background
//             cout << "|\n";              // right border on default bg
//         }

//         // Bottom border (default background)
//         cout << '+';
//         for (int c = 0; c < COLS; ++c) cout << '-';
//         cout << "+\n";

//         cout << "W/A/S/D to move, Q to quit.\n";
//         if (game_over) cout << "Game Over. Press Q to exit.\n";
//         cout.flush();

//         // advance the simple animation clock
//         g_frame++;
//     }
// };

// int main() {
//     ios::sync_with_stdio(false);
//     cin.tie(nullptr);

//     RawTerm raw; // raw mode
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

//     // Clear and reset colors on exit
//     cout << RESET << "\x1b[2J\x1b[H";
//     cout << "Thanks for playing.\n";
//     return 0;
// }


// // // snake_raw.cpp — macOS-friendly console Snake with raw keyboard input
// // // Playfield interior = blue, Food = bright yellow dot, Snake = solid green dots

// // #include <algorithm>
// // #include <atomic>
// // #include <chrono>
// // #include <cctype>
// // #include <deque>
// // #include <iostream>
// // #include <mutex>
// // #include <optional>
// // #include <queue>
// // #include <random>
// // #include <thread>
// // #include <vector>
// // #include <termios.h>
// // #include <unistd.h>

// // using namespace std;

// // // --- ANSI colors ---
// // static constexpr const char* RESET = "\x1b[0m";
// // static constexpr const char* BG_BLUE = "\x1b[44m";
// // static constexpr const char* FG_WHITE = "\x1b[37m";
// // static constexpr const char* FG_BRIGHT_YELLOW = "\x1b[93m"; // food
// // static constexpr const char* FG_BRIGHT_GREEN  = "\x1b[92m"; // snake

// // // --- Config ---
// // static constexpr int ROWS = 20;
// // static constexpr int COLS = 80;
// // static constexpr chrono::milliseconds TICK{100}; // 10 ticks/sec

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
// //         raw.c_cc[VTIME] = 0;
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

// // // --- Game model ---
// // struct Point { int r, c; };
// // enum class Dir { Up, Down, Left, Right };

// // struct Game {
// //     deque<Point> snake; // front = head
// //     Dir dir = Dir::Right;
// //     Point food{0, 0};
// //     bool game_over = false;
// //     int score = 0;
// //     mt19937 rng{random_device{}()};

// //     Game() {
// //         int r = ROWS / 2, c = COLS / 2;
// //         snake.push_back({r, c});
// //         snake.push_back({r, c - 1});
// //         snake.push_back({r, c - 2});
// //         place_food();
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

// //     void update() {
// //         if (game_over) return;
// //         Point head = snake.front();
// //         switch (dir) {
// //             case Dir::Up:    head.r--; break;
// //             case Dir::Down:  head.r++; break;
// //             case Dir::Left:  head.c--; break;
// //             case Dir::Right: head.c++; break;
// //         }
// //         if (head.r < 0) head.r = ROWS - 1;
// //         if (head.r >= ROWS) head.r = 0;
// //         if (head.c < 0) head.c = COLS - 1;
// //         if (head.c >= COLS) head.c = 0;

// //         if (any_of(snake.begin(), snake.end(),
// //                    [&](const Point& p){ return p.r == head.r && p.c == head.c; })) {
// //             game_over = true; return;
// //         }

// //         snake.push_front(head);

// //         if (head.r == food.r && head.c == food.c) {
// //             score += 10;
// //             place_food();
// //         } else {
// //             snake.pop_back();
// //         }
// //     }

// //     void render() const {
// //         // Clear screen & move cursor home
// //         cout << "\x1b[2J\x1b[H";

// //         // Top border + score (default background)
// //         cout << '+';
// //         for (int c = 0; c < COLS; ++c) cout << '-';
// //         cout << "+  Score: " << score << "\n";

// //         // Each row: left border (default), interior (blue bg), right border (default)
// //         for (int r = 0; r < ROWS; ++r) {
// //             cout << '|';                 // left border on default bg
// //             cout << BG_BLUE << FG_WHITE; // start blue background for playfield

// //             for (int c = 0; c < COLS; ++c) {
// //                 if (food.r == r && food.c == c) {
// //                     // bright yellow food
// //                     cout << FG_BRIGHT_YELLOW << "●" << FG_WHITE;
// //                     continue;
// //                 }
// //                 bool on_snake = false;
// //                 for (const auto& seg : snake) {
// //                     if (seg.r == r && seg.c == c) { on_snake = true; break; }
// //                 }
// //                 if (on_snake) {
// //                     // snake as solid green dots
// //                     cout << FG_BRIGHT_GREEN << "●" << FG_WHITE;
// //                 } else {
// //                     cout << ' '; // blue background shows through
// //                 }
// //             }

// //             cout << RESET;              // stop blue background
// //             cout << "|\n";              // right border on default bg
// //         }

// //         // Bottom border (default background)
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

// //     RawTerm raw; // raw mode
// //     Game game;
// //     auto next_tick = chrono::steady_clock::now();

// //     while (running.load()) {
// //         // read all pending keys
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

// //     // Clear and reset colors on exit
// //     cout << RESET << "\x1b[2J\x1b[H";
// //     cout << "Thanks for playing.\n";
// //     return 0;
// // }