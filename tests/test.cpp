#include <vector>
#include <string>
#include <cassert>
#include <cstdio>
#include <iostream>

#define __NCURSES_H
// Minimal ncurses stubs
typedef int WINDOW;
static WINDOW* stdscr;
#define LINES 24
#define A_REVERSE 0
#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7
#define COLOR_PAIR(x) (x)
#define KEY_UP 1000
#define KEY_DOWN 1001
#define KEY_LEFT 1002
#define KEY_RIGHT 1003
#define KEY_BACKSPACE 1004
#define KEY_PPAGE 1005
#define KEY_NPAGE 1006
#define KEY_RESIZE 1007
#define TRUE 1

inline void getmaxyx(WINDOW*, int& r, int& c) { r = 24; c = 80; }
inline int mvhline(int,int,char,int){return 0;}
inline int mvprintw(int,int,const char*, ...){return 0;}
inline int getch(){return 'y';}
inline void attron(int){}
inline void attroff(int){}
inline void move(int,int){}
inline int getnstr(char*, int){return 0;}
inline void echo(){}
inline void noecho(){}
inline void curs_set(int){}
inline void wattron(WINDOW*, int){}
inline void wattroff(WINDOW*, int){}
inline int mvwprintw(WINDOW*, int,int,const char*, ...){return 0;}
inline int mvwaddch(WINDOW*, int,int,int){return 0;}
inline void leaveok(WINDOW*, bool){}
inline void idlok(WINDOW*, bool){}
inline void start_color(){}
inline void keypad(WINDOW*, bool){}
inline WINDOW* initscr(){return nullptr;}
inline void endwin(){}
inline WINDOW* newpad(int,int){return nullptr;}
inline void delwin(WINDOW*){}
inline void werase(WINDOW*){}
inline void prefresh(WINDOW*, int,int,int,int,int,int){}
inline void wresize(WINDOW*, int,int){}
inline void refresh(){}
inline void raw(){}
inline void set_escdelay(int){}
inline void init_pair(short,short,short){}

// Use the actual implementations but avoid main definition
#define main quill_main
#include "../src/editor.cpp"
#undef main

int main() {
    const char* infile = "sample_in.txt";
    FILE* f = fopen(infile, "w");
    fprintf(f, "first\nsecond\n");
    fclose(f);

    auto lines = loadFile(infile);
    assert(lines.size() == 2);
    assert(lines[0] == "first");
    assert(lines[1] == "second");

    const char* outfile = "sample_out.txt";
    remove(outfile);
    saveFile(outfile, lines);

    auto saved = loadFile(outfile);
    assert(saved == lines);

    std::cout << "All tests passed" << std::endl;
    return 0;
}
