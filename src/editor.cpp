#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <ncurses.h>
#ifdef KEY_SAVE
#undef KEY_SAVE
#endif
#define KEY_SAVE userSettings.keys.saveKey
using namespace std;

vector<string> loadFile(const string& filename) {
    vector<string> lines;
    ifstream file(filename);
    if (!file.is_open()) {
        lines.push_back("");
    } else {
        string line;
        while (getline(file, line)) {
            lines.push_back(line);
        }
        file.close();
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

#include <map>

map<string, int> keyword_colors = {
    {"int", 3}, {"float", 3}, {"double", 3}, {"char", 3}, {"void", 3},
    {"short", 3}, {"long", 3}, {"signed", 3}, {"unsigned", 3},
    {"bool", 3}, {"wchar_t", 3}, {"size_t", 3}, {"auto", 3},
    {"const", 3}, {"static", 3}, {"mutable", 3}, {"volatile", 3},
    {"constexpr", 3}, {"inline", 3}, {"typedef", 3}, {"enum", 3},
    {"if", 4}, {"else", 4}, {"for", 4}, {"while", 4}, {"do", 4},
    {"return", 4}, {"switch", 4}, {"case", 4}, {"break", 4},
    {"continue", 4}, {"goto", 4}, {"default", 4},
    {"try", 4}, {"catch", 4}, {"throw", 4},
    {"class", 5}, {"struct", 5}, {"union", 5}, {"public", 5},
    {"private", 5}, {"protected", 5}, {"virtual", 5}, {"override", 5},
    {"final", 5}, {"explicit", 5}, {"friend", 5}, {"template", 5},
    {"typename", 5}, {"operator", 5}, {"namespace", 5}, {"using", 5},
    {"include", 6}, {"define", 6}, {"ifdef", 6}, {"ifndef", 6},
    {"endif", 6}, {"pragma", 6}, {"undef", 6}, {"elif", 6},
    {"new", 4}, {"delete", 4}, {"this", 4}, {"nullptr", 3},
    {"static_cast", 3}, {"dynamic_cast", 3}, {"reinterpret_cast", 3}, {"const_cast", 3},
    {"main", 5}
};

#define KEY_SAVE userSettings.keys.saveKey
#define KEY_QUIT userSettings.keys.quitKey
#define KEY_FIND userSettings.keys.findKey
#define KEY_GOTO userSettings.keys.gotoKey
#define KEY_SAVE_EXIT userSettings.keys.saveAndExitKey

#include <sys/stat.h>

bool fileExists(const string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

bool confirmOverwrite(const string& filename, int max_col) {
    string prompt = "File exists! Overwrite? (y/n): ";
    mvhline(LINES-2, 0, ' ', max_col);
    mvprintw(LINES-2, 0, "%s", prompt.c_str());
    int ch = getch();
    return (ch == 'y' || ch == 'Y');
}

void saveFile(const string& filename, const vector<string>& lines) {
    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);

    if (fileExists(filename)) {
        if (!confirmOverwrite(filename, max_col)) {
            return;
        }
    }
    ofstream file(filename);
    if (!file.is_open()) return;
    for (const auto& line : lines) {
        file << line << endl;
    }
    file.close();
}

void drawStatusBar(const string& filename, bool modified, int row, int col, int max_col, const string& msg) {
    attron(A_REVERSE);
    string status = " " + filename + (modified ? " [*]" : " [Saved]") +
        " | Ctrl+S: Save | Ctrl+Q: Quit | Ctrl+F: Find | Ctrl+G: Goto ";
    status += "| Ln " + to_string(row+1) + ", Col " + to_string(col+1);
    mvhline(LINES-1, 0, ' ', max_col);
    mvprintw(LINES-1, 0, "%s", status.c_str());
    if (!msg.empty()) {
        mvprintw(LINES-1, max_col - msg.size() - 2, "%s", msg.c_str());
    }
    attroff(A_REVERSE);
}

string promptInput(const string& prompt, int max_col) {
    echo();
    curs_set(1);
    char buf[256] = {0};
    mvhline(LINES-2, 0, ' ', max_col);
    mvprintw(LINES-2, 0, "%s", prompt.c_str());
    move(LINES-2, prompt.size());
    getnstr(buf, 255);
    noecho();
    curs_set(0);
    return string(buf);
}

bool searchInLines(const vector<string>& lines, const string& query, int& row, int& col) {
    for (size_t i = row; i < lines.size(); ++i) {
        size_t found = lines[i].find(query, (i == (size_t)row ? col+1 : 0));
        if (found != string::npos) {
            row = i;
            col = found;
            return true;
        }
    }
    return false;
}

bool gotoLine(int& row, int& col, int max_row, int lines_size) {
    string input = promptInput("Goto line: ", max_row);
    try {
        int ln = stoi(input);
        if (ln >= 1 && ln <= lines_size) {
            row = ln - 1;
            col = 0;
            return true;
        }
    } catch (...) {}
    return false;
}

#include <map>

void initSyntaxColors() {
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);
    init_pair(4, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(5, COLOR_GREEN, COLOR_BLACK);
    init_pair(6, COLOR_BLUE, COLOR_BLACK);
    init_pair(7, COLOR_RED, COLOR_BLACK);
    init_pair(8, COLOR_CYAN, COLOR_BLACK);
}
void disableFlicker() {
    leaveok(stdscr, TRUE);
    idlok(stdscr, TRUE);
}
vector<string> preloadFileContent(const string& filename) {
    return loadFile(filename);
}
void printHighlightedLine(WINDOW* pad, int y, int x, const string& line) {
    int i = 0;
    while (i < (int)line.size()) {
        if (line[i] == '/' && i+1 < (int)line.size() && line[i+1] == '/') {
            wattron(pad, COLOR_PAIR(8));
            mvwprintw(pad, y, x+i, "%s", line.substr(i).c_str());
            wattroff(pad, COLOR_PAIR(8));
            break;
        }
        if (line[i] == '"') {
            int start = i++;
            while (i < (int)line.size() && (line[i] != '"' || line[i-1] == '\\')) i++;
            if (i < (int)line.size()) i++;
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, x+start, "%s", line.substr(start, i-start).c_str());
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }
        if (line[i] == '\'') {
            int start = i++;
            while (i < (int)line.size() && (line[i] != '\'' || line[i-1] == '\\')) i++;
            if (i < (int)line.size()) i++;
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, x+start, "%s", line.substr(start, i-start).c_str());
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }
        if (isdigit(line[i]) && (i == 0 || !isalnum(line[i-1]))) {
            int start = i;
            while (i < (int)line.size() && (isdigit(line[i]) || line[i] == '.')) i++;
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, x+start, "%s", line.substr(start, i-start).c_str());
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }
        if (isalpha(line[i]) || line[i] == '_') {
            int start = i;
            while (i < (int)line.size() && (isalnum(line[i]) || line[i] == '_')) i++;
            string word = line.substr(start, i-start);
            auto it = keyword_colors.find(word);
            if (it != keyword_colors.end()) {
                wattron(pad, COLOR_PAIR(it->second));
                mvwprintw(pad, y, x+start, "%s", word.c_str());
                wattroff(pad, COLOR_PAIR(it->second));
            } else {
                mvwprintw(pad, y, x+start, "%s", word.c_str());
            }
            continue;
        }
        mvwaddch(pad, y, x+i, line[i]);
        i++;
    }
}

#undef nanoEditor
void nanoEditor(const string& filename) {
    vector<string> lines = loadFile(filename);
    int row = 0, col = 0;
    int scroll_offset = 0;
    bool modified = false;
    string status_msg;
    int status_msg_timer = 0;

    initscr();
    start_color();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    init_pair(2, COLOR_WHITE, COLOR_BLACK);
    initSyntaxColors();

    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);

    WINDOW* pad = newpad(10000, max_col);

    while (true) {
        getmaxyx(stdscr, max_row, max_col);
        werase(pad);

        if (lines.size() == 1 && lines[0].empty()) {
            string welcome = "-- Quill Editor --";
            mvwprintw(pad, max_row/2, (max_col-welcome.size())/2, "%s", welcome.c_str());
        }

        for (size_t i = scroll_offset; i < lines.size() && i - scroll_offset < max_row - 1; ++i) {
            wattron(pad, COLOR_PAIR(1));
            mvwprintw(pad, i - scroll_offset, 0, "%3d ", (int)i + 1);
            wattroff(pad, COLOR_PAIR(1));
            printHighlightedLine(pad, i - scroll_offset, 4, lines[i]);
        }

        prefresh(pad, 0, 0, 0, 0, max_row-2, max_col-1);
        drawStatusBar(filename, modified, row, col, max_col, status_msg);

        int cursor_y = row - scroll_offset;
        int cursor_x = col + 4;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_y >= max_row-1) cursor_y = max_row-2;
        move(cursor_y, cursor_x);

        refresh();

        int ch = getch();

        if (status_msg_timer > 0) {
            status_msg_timer--;
            if (status_msg_timer == 0) status_msg.clear();
        }

        if (ch == 24) {
            saveFile(filename, lines);
            delwin(pad);
            endwin();
            return;
        }
        if (ch == 19) {
            saveFile(filename, lines);
            modified = false;
            status_msg = "Saved!";
            status_msg_timer = 30;
            continue;
        }
        if (ch == 17) {
            if (modified) {
                status_msg = "Unsaved changes! Ctrl+S to save, Ctrl+Q again to quit.";
                status_msg_timer = 40;
                int confirm = getch();
                if (confirm == 17) {
                    delwin(pad);
                    endwin();
                    return;
                }
                continue;
            }
            delwin(pad);
            endwin();
            return;
        }

        switch (ch) {
            case KEY_UP:
                if (row > 0) row--;
                if (col > lines[row].size()) col = lines[row].size();
                break;
            case KEY_DOWN:
                if (row < (int)lines.size() - 1) row++;
                if (col > lines[row].size()) col = lines[row].size();
                break;
            case KEY_LEFT:
                if (col > 0) col--;
                else if (row > 0) { row--; col = lines[row].size(); }
                break;
            case KEY_RIGHT:
                if (col < lines[row].size()) col++;
                else if (row < (int)lines.size() - 1) { row++; col = 0; }
                break;
            case KEY_BACKSPACE:
            case 127:
            case 8:
                if (col > 0) {
                    lines[row].erase(col - 1, 1);
                    col--;
                    modified = true;
                } else if (row > 0) {
                    col = lines[row - 1].size();
                    lines[row - 1] += lines[row];
                    lines.erase(lines.begin() + row);
                    row--;
                    modified = true;
                }
                break;
            case '\n':
                lines.insert(lines.begin() + row + 1, lines[row].substr(col));
                lines[row] = lines[row].substr(0, col);
                row++;
                col = 0;
                modified = true;
                break;
            case '\t':
                lines[row].insert(col, "    ");
                col += 4;
                modified = true;
                break;
            case KEY_PPAGE:
                if (scroll_offset > 0) scroll_offset--;
                break;
            case KEY_NPAGE:
                if (scroll_offset + max_row < (int)lines.size()) scroll_offset++;
                break;
            case KEY_RESIZE:
                getmaxyx(stdscr, max_row, max_col);
                wresize(pad, 10000, max_col);
                break;
            default:
                if (ch >= 32 && ch <= 126) {
                    lines[row].insert(col, 1, ch);
                    col++;
                    modified = true;
                }
                break;
        }

        if (col < 0) col = 0;
        if (col > lines[row].size()) col = lines[row].size();
        if (row < 0) row = 0;
        if (row >= (int)lines.size()) row = lines.size() - 1;

        if (row - scroll_offset >= max_row - 1) scroll_offset++;
        if (row - scroll_offset < 0) scroll_offset--;
        if (scroll_offset < 0) scroll_offset = 0;
        if (scroll_offset > (int)lines.size() - 1) scroll_offset = lines.size() - 1;
    }
}
int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: quill <filename>" << endl;
        return 1;
    }
    string filename = argv[1];
    nanoEditor(filename);
    return 0;
}
