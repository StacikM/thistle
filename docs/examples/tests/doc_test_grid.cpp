// Checks the Sokoban rules in cookbook/grid_puzzle.cpp: pushing, blocking,
// crates on goals, undo by copying, and winning.
#define main example_main_grid
#include "../cookbook/grid_puzzle.cpp"
#undef main

#include <cstdio>

namespace {
int g_failures = 0;
void check(bool cond, const char* msg) {
    if (cond) { std::printf("  ok  %s\n", msg); }
    else { std::printf("  FAIL %s\n", msg); ++g_failures; }
}
}

int main() {
    Board b;
    check(b.px == 2 && b.py == 3, "the player starts where '@' was");
    check(b.cells[3][2] == '.', "and that cell becomes floor");

    check(!b.move(-1, 0) || b.px == 1, "walking left onto floor works");
    b = Board{};

    // Walls block.
    b.px = 1; b.py = 1;
    check(!b.move(-1, 0) && b.px == 1, "can't walk into a wall");
    check(!b.move(0, -1) && b.py == 1, "or the top wall");

    // Pushing a crate: the crate at (3,2) with the player at (2,2) pushed right.
    b = Board{};
    b.px = 2; b.py = 2;
    check(b.move(1, 0), "pushing a free crate moves");
    check(b.px == 3 && b.cells[2][3] == '.' && b.cells[2][4] == 'c', "the crate moves one cell, the player takes its place");

    // Pushing onto a goal: the goal is at (6,2).
    check(b.move(1, 0) && b.cells[2][5] == 'c', "push again");
    check(b.move(1, 0) && b.cells[2][6] == 'C', "pushing a crate onto a goal marks it as done");
    check(b.cells[2][5] == '.', "and leaves floor behind");

    // Pushing it off the goal puts the goal back.
    check(b.move(1, 0) && b.cells[2][6] == 'x' && b.cells[2][7] == 'c', "pushing a crate off a goal uncovers the goal");

    // Two crates in a row can't be pushed.
    Board c;
    c.cells = {"#######", "#@cc..#", "#######"};
    c.px = 1; c.py = 1; c.cells[1][1] = '.';
    check(!c.move(1, 0) && c.px == 1, "can't push a crate into another crate");

    // A crate against a wall can't be pushed into it.
    Board d;
    d.cells = {"#####", "#.@c#", "#####"};
    d.px = 2; d.py = 1; d.cells[1][2] = '.';
    check(!d.move(1, 0) && d.cells[1][3] == 'c', "can't push a crate into a wall");

    // Winning: every crate on a goal.
    Board w;
    w.cells = {"######", "#.@cx#", "######"};
    w.px = 2; w.py = 1; w.cells[1][2] = '.';
    check(!w.won(), "not won with a crate off its goal");
    check(w.move(1, 0), "push it onto the goal");
    check(w.won(), "won when every crate is on a goal");

    // Undo is just copying: history of states.
    Board u;
    u.px = 2; u.py = 2;
    std::vector<Board> history;
    history.push_back(u);
    u.move(1, 0);
    Board restored = history.back();
    check(restored.cells[2][3] == 'c' && restored.px == 2, "an old copy restores the crate and the player");
    check(u.cells[2][4] == 'c', "while the newer state is unaffected");

    if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
}
