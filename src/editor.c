#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

#define MAX_LINES     10000
#define MAX_LINE_LEN  4096
#define UNDO_LIMIT    128
#define LINE_NUM_W    4   /* "NNN " */

// ─── Utility ────────────────────────────────────────────────────────────────

int fileExists(const char *filename) {
    struct stat buf;
    return stat(filename, &buf) == 0;
}

/* Safe strdup with fallback */
static char *safe_strdup(const char *s) {
    if (!s) s = "";
    char *p = strdup(s);
    if (!p) { perror("strdup"); exit(1); }
    return p;
}

/* Safe malloc */
static void *safe_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

// ─── Line buffer ────────────────────────────────────────────────────────────

typedef struct {
    char **data;
    int    count;
    int    cap;
} LineBuf;

static void lb_ensure(LineBuf *lb, int needed) {
    if (lb->cap >= needed) return;
    int nc = lb->cap ? lb->cap : 256;
    while (nc < needed) nc *= 2;
    lb->data = realloc(lb->data, sizeof(char*) * nc);
    if (!lb->data) { perror("realloc"); exit(1); }
    lb->cap = nc;
}

static LineBuf lb_load(const char *filename) {
    LineBuf lb = {0};
    lb_ensure(&lb, 256);

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        lb.data[0] = safe_strdup("");
        lb.count = 1;
        return lb;
    }

    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || st.st_size == 0) {
        fclose(fp);
        lb.data[0] = safe_strdup("");
        lb.count = 1;
        return lb;
    }

    off_t  sz  = st.st_size;
    char  *buf = safe_malloc(sz + 1);
    size_t rd  = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    /* count lines */
    size_t est = 1;
    for (size_t i = 0; i < rd; i++) if (buf[i] == '\n') est++;
    lb_ensure(&lb, (int)est + 2);

    char *start = buf;
    for (size_t i = 0; i < rd; i++) {
        if (buf[i] == '\r') { buf[i] = '\0'; continue; }
        if (buf[i] == '\n') {
            buf[i] = '\0';
            lb.data[lb.count++] = safe_strdup(start);
            start = buf + i + 1;
        }
    }
    /* last line (may have no trailing \n) */
    lb.data[lb.count++] = safe_strdup(start);
    free(buf);

    if (lb.count == 0) {
        lb.data[0] = safe_strdup("");
        lb.count = 1;
    }
    return lb;
}

static void lb_free(LineBuf *lb) {
    for (int i = 0; i < lb->count; i++) free(lb->data[i]);
    free(lb->data);
    lb->data  = NULL;
    lb->count = lb->cap = 0;
}

/* Deep copy */
static LineBuf lb_clone(const LineBuf *src) {
    LineBuf dst = {0};
    lb_ensure(&dst, src->count + 1);
    for (int i = 0; i < src->count; i++)
        dst.data[i] = safe_strdup(src->data[i]);
    dst.count = src->count;
    return dst;
}

// ─── Undo / Redo ─────────────────────────────────────────────────────────────

typedef struct {
    LineBuf snap;
    int     row, col;
} UndoEntry;

typedef struct {
    UndoEntry entries[UNDO_LIMIT];
    int       head;   /* next write position */
    int       size;   /* how many valid entries */
    int       pos;    /* current position (for redo) */
} UndoStack;

static void undo_push(UndoStack *us, const LineBuf *lb, int row, int col) {
    /* free any redo entries above pos */
    for (int i = us->pos; i < us->size; i++) {
        lb_free(&us->entries[(us->head - us->size + i + UNDO_LIMIT) % UNDO_LIMIT].snap);
    }
    us->size = us->pos;

    int idx = (us->head) % UNDO_LIMIT;
    if (us->size == UNDO_LIMIT) {
        /* overwrite oldest */
        lb_free(&us->entries[idx].snap);
    } else {
        us->size++;
    }
    us->entries[idx].snap = lb_clone(lb);
    us->entries[idx].row  = row;
    us->entries[idx].col  = col;
    us->head = (us->head + 1) % UNDO_LIMIT;
    us->pos  = us->size;
}

static int undo_do(UndoStack *us, LineBuf *lb, int *row, int *col) {
    if (us->pos <= 1) return 0;
    us->pos--;
    int idx = (us->head - us->size + us->pos - 1 + UNDO_LIMIT * 2) % UNDO_LIMIT;
    lb_free(lb);
    *lb  = lb_clone(&us->entries[idx].snap);
    *row = us->entries[idx].row;
    *col = us->entries[idx].col;
    return 1;
}

static int redo_do(UndoStack *us, LineBuf *lb, int *row, int *col) {
    if (us->pos >= us->size) return 0;
    int idx = (us->head - us->size + us->pos + UNDO_LIMIT * 2) % UNDO_LIMIT;
    us->pos++;
    lb_free(lb);
    *lb  = lb_clone(&us->entries[idx].snap);
    *row = us->entries[idx].row;
    *col = us->entries[idx].col;
    return 1;
}

// ─── File I/O ────────────────────────────────────────────────────────────────

static int confirmOverwrite(const char *filename, int max_col) {
    char prompt[512];
    snprintf(prompt, sizeof(prompt), "File '%s' exists! Overwrite? (y/n): ", filename);
    mvhline(LINES-2, 0, ' ', max_col);
    mvprintw(LINES-2, 0, "%s", prompt);
    refresh();
    int ch;
    while (1) {
        ch = getch();
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N') return 0;
    }
}

static void saveFile(const char *filename, const LineBuf *lb) {
    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);
    (void)max_row;

    if (fileExists(filename) && !confirmOverwrite(filename, max_col))
        return;

    size_t total = 0;
    for (int i = 0; i < lb->count; i++)
        total += strlen(lb->data[i]) + 1;

    char *outbuf = safe_malloc(total + 1);
    char *p = outbuf;
    for (int i = 0; i < lb->count; i++) {
        size_t len = strlen(lb->data[i]);
        memcpy(p, lb->data[i], len);
        p += len;
        *p++ = '\n';
    }

    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fwrite(outbuf, 1, total, fp);
        fclose(fp);
    }
    free(outbuf);
}

// ─── Syntax Highlighting ─────────────────────────────────────────────────────

typedef struct { const char *word; int color; } KwColor;

static const KwColor kw_table[] = {
    /* types */
    {"int",8}, {"char",8}, {"void",8}, {"double",8}, {"float",8},
    {"long",8}, {"short",8}, {"unsigned",8}, {"signed",8},
    {"size_t",8}, {"off_t",8}, {"uint8_t",8}, {"uint16_t",8},
    {"uint32_t",8}, {"uint64_t",8}, {"int8_t",8}, {"int16_t",8},
    {"int32_t",8}, {"int64_t",8}, {"bool",8},
    /* keywords */
    {"return",5}, {"if",5}, {"else",5}, {"for",5}, {"while",5},
    {"break",5}, {"continue",5}, {"switch",5}, {"case",5},
    {"default",5}, {"do",5}, {"goto",5}, {"enum",5}, {"union",5},
    {"struct",5}, {"typedef",5}, {"static",5}, {"const",5},
    {"volatile",5}, {"extern",5}, {"inline",5}, {"register",5},
    {"auto",5}, {"asm",5}, {"_Bool",5},
    /* constants */
    {"NULL",3}, {"true",3}, {"false",3},
    /* preprocessor (matched separately via '#') */
    /* stdlib */
    {"printf",6}, {"fprintf",6}, {"sprintf",6}, {"snprintf",6},
    {"scanf",6}, {"sscanf",6}, {"malloc",6}, {"calloc",6},
    {"realloc",6}, {"free",6}, {"memcpy",6}, {"memmove",6},
    {"memset",6}, {"memcmp",6}, {"strlen",6}, {"strcpy",6},
    {"strncpy",6}, {"strdup",6}, {"strcmp",6}, {"strncmp",6},
    {"strstr",6}, {"strchr",6},
    {"fopen",6}, {"fclose",6}, {"fread",6}, {"fwrite",6},
    {"fseek",6}, {"ftell",6}, {"fstat",6}, {"stat",6},
    {"exit",6}, {"perror",6}, {"system",6},
    {"main",6},
    {NULL, 0}
};

static int getKwColor(const char *word) {
    for (int i = 0; kw_table[i].word; i++)
        if (strcmp(word, kw_table[i].word) == 0)
            return kw_table[i].color;
    return 0;
}

/*
 * Render one line with syntax highlighting into pad at row y, starting
 * at pad column (LINE_NUM_W + horiz_scroll is handled by the caller via
 * prefresh — we always write from column LINE_NUM_W).
 *
 * in_mlcomment: 1 if we enter this line already inside a block comment.
 * Returns 1 if we are inside a block comment at end of line.
 */
static int printHighlightedLine(WINDOW *pad, int y, const char *line,
                                 int in_ml)
{
    int i   = 0;
    int len = (int)strlen(line);

    /* still inside a block comment from previous line */
    if (in_ml) {
        wattron(pad, COLOR_PAIR(9));
        int start = 0;
        while (i < len) {
            if (line[i] == '*' && i+1 < len && line[i+1] == '/') {
                i += 2;
                mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i - start, line + start);
                wattroff(pad, COLOR_PAIR(9));
                goto normal;
            }
            i++;
        }
        mvwprintw(pad, y, LINE_NUM_W + start, "%s", line);
        wattroff(pad, COLOR_PAIR(9));
        return 1;
    }

normal:
    while (i < len) {

        /* block comment start */
        if (line[i] == '/' && i+1 < len && line[i+1] == '*') {
            int start = i;
            i += 2;
            wattron(pad, COLOR_PAIR(9));
            while (i < len) {
                if (line[i] == '*' && i+1 < len && line[i+1] == '/') {
                    i += 2;
                    mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i-start, line+start);
                    wattroff(pad, COLOR_PAIR(9));
                    goto normal_cont;
                }
                i++;
            }
            /* comment continues onto next line */
            mvwprintw(pad, y, LINE_NUM_W + start, "%s", line + start);
            wattroff(pad, COLOR_PAIR(9));
            return 1;
        }

        /* line comment */
        if (line[i] == '/' && i+1 < len && line[i+1] == '/') {
            wattron(pad, COLOR_PAIR(9));
            mvwprintw(pad, y, LINE_NUM_W + i, "%s", line + i);
            wattroff(pad, COLOR_PAIR(9));
            return 0;
        }

        /* preprocessor directive */
        if (line[i] == '#' && (i == 0 || isspace(line[i-1]))) {
            int start = i++;
            while (i < len && isalpha(line[i])) i++;
            wattron(pad, COLOR_PAIR(4));
            mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(4));
            continue;
        }

        /* string literal */
        if (line[i] == '"') {
            int start = i++;
            while (i < len) {
                if (line[i] == '\\') { i += 2; continue; }
                if (line[i] == '"')  { i++; break; }
                i++;
            }
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }

        /* char literal */
        if (line[i] == '\'') {
            int start = i++;
            while (i < len) {
                if (line[i] == '\\') { i += 2; continue; }
                if (line[i] == '\'') { i++; break; }
                i++;
            }
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }

        /* number */
        if (isdigit((unsigned char)line[i]) &&
            (i == 0 || !isalnum((unsigned char)line[i-1]))) {
            int start = i;
            while (i < len && (isxdigit((unsigned char)line[i]) ||
                               line[i]=='.' || line[i]=='x' || line[i]=='X' ||
                               line[i]=='u' || line[i]=='U' ||
                               line[i]=='l' || line[i]=='L')) i++;
            wattron(pad, COLOR_PAIR(3));
            mvwprintw(pad, y, LINE_NUM_W + start, "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(3));
            continue;
        }

        /* identifier / keyword */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i]=='_')) i++;
            int wlen = i - start;
            char word[256];
            if (wlen >= (int)sizeof(word)) wlen = (int)sizeof(word) - 1;
            memcpy(word, line + start, wlen);
            word[wlen] = '\0';
            int color = getKwColor(word);
            if (color) wattron(pad, COLOR_PAIR(color));
            mvwprintw(pad, y, LINE_NUM_W + start, "%s", word);
            if (color) wattroff(pad, COLOR_PAIR(color));
            continue;
        }

        /* braces / parens */
        if (line[i] == '{' || line[i] == '}') {
            wattron(pad, COLOR_PAIR(5));
            mvwaddch(pad, y, LINE_NUM_W + i, line[i]);
            wattroff(pad, COLOR_PAIR(5));
            i++;
            continue;
        }
        if (line[i] == '(' || line[i] == ')') {
            wattron(pad, COLOR_PAIR(6));
            mvwaddch(pad, y, LINE_NUM_W + i, line[i]);
            wattroff(pad, COLOR_PAIR(6));
            i++;
            continue;
        }

        mvwaddch(pad, y, LINE_NUM_W + i, line[i]);
        i++;
        normal_cont:;
    }
    return 0;
}

// ─── Status bar ──────────────────────────────────────────────────────────────

static void drawStatusBar(const char *filename, int modified,
                          int row, int col, int max_col,
                          const char *msg)
{
    attron(A_REVERSE);
    mvhline(LINES-1, 0, ' ', max_col);

    char left[256];
    snprintf(left, sizeof(left), " %s %s", filename,
             modified ? "[Modified]" : "[Saved]");
    mvprintw(LINES-1, 0, "%s", left);

    const char *keys = "^S Save  ^Z Undo  ^Y Redo  ^F Find  ^G Goto  ^X Exit";
    int kpos = (max_col - (int)strlen(keys)) / 2;
    if (kpos > 0) mvprintw(LINES-1, kpos, "%s", keys);

    char right[64];
    snprintf(right, sizeof(right), "Ln %d Col %d ", row+1, col+1);
    mvprintw(LINES-1, max_col - (int)strlen(right), "%s", right);

    if (msg && msg[0]) {
        int mpos = max_col - (int)strlen(right) - (int)strlen(msg) - 2;
        if (mpos > 0) mvprintw(LINES-1, mpos, "%s", msg);
    }
    attroff(A_REVERSE);
    refresh();
}

// ─── Editor ──────────────────────────────────────────────────────────────────

void nanoEditor(const char *filename) {
    LineBuf lb = lb_load(filename);

    /* initial capacity headroom */
    lb_ensure(&lb, lb.count + 512);

    int row = 0, col = 0;
    int scroll_row = 0, scroll_col = 0;
    int sticky_col = 0;          /* remembered column for up/down */
    int modified   = 0;
    char status_msg[256] = "";

    UndoStack us = {0};
    undo_push(&us, &lb, row, col);  /* initial snapshot */

    initscr();
    start_color();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    /* color pairs */
    init_pair(1, COLOR_CYAN,    COLOR_BLACK);  /* line numbers */
    init_pair(2, COLOR_WHITE,   COLOR_BLACK);  /* default text */
    init_pair(3, COLOR_YELLOW,  COLOR_BLACK);  /* numbers / constants */
    init_pair(4, COLOR_GREEN,   COLOR_BLACK);  /* preprocessor / keywords2 */
    init_pair(5, COLOR_MAGENTA, COLOR_BLACK);  /* keywords / braces */
    init_pair(6, COLOR_CYAN,    COLOR_BLACK);  /* stdlib / parens */
    init_pair(7, COLOR_RED,     COLOR_BLACK);  /* strings / chars */
    init_pair(8, COLOR_BLUE,    COLOR_BLACK);  /* types */
    init_pair(9, COLOR_GREEN,   COLOR_BLACK);  /* comments */

    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);

    /* pad wide enough for long lines + line-number gutter */
    WINDOW *pad = newpad(MAX_LINES, MAX_LINE_LEN + LINE_NUM_W + 4);

    while (1) {
        getmaxyx(stdscr, max_row, max_col);

        /* ── redraw pad ── */
        werase(pad);

        /* pre-scan multiline comment state up to scroll_row */
        int in_ml = 0;
        for (int i = 0; i < scroll_row && i < lb.count; i++) {
            const char *l = lb.data[i];
            int len = (int)strlen(l);
            for (int j = 0; j < len; j++) {
                if (!in_ml && l[j]=='/' && j+1<len && l[j+1]=='*') { in_ml=1; j++; }
                else if (in_ml && l[j]=='*' && j+1<len && l[j+1]=='/') { in_ml=0; j++; }
            }
        }

        int visible = max_row - 1;
        for (int i = scroll_row;
             i < lb.count && i - scroll_row < visible; i++)
        {
            wattron(pad, COLOR_PAIR(1));
            mvwprintw(pad, i - scroll_row, 0, "%3d ", i + 1);
            wattroff(pad, COLOR_PAIR(1));
            in_ml = printHighlightedLine(pad, i - scroll_row,
                                         lb.data[i], in_ml);
        }

        /* prefresh with horizontal scroll */
        prefresh(pad, 0, scroll_col,
                 0, 0, max_row - 2, max_col - 1);

        drawStatusBar(filename, modified, row, col, max_col, status_msg);

        /* place cursor */
        int screen_col = (LINE_NUM_W + col) - scroll_col;
        if (screen_col < 0) screen_col = 0;
        if (screen_col >= max_col) screen_col = max_col - 1;
        move(row - scroll_row, screen_col);
        refresh();

        int ch = getch();
        status_msg[0] = '\0';

#define PUSH_UNDO() undo_push(&us, &lb, row, col)

        /* ── Exit ── */
        if (ch == 24) {  /* ^X */
            if (modified) {
                mvhline(LINES-2, 0, ' ', max_col);
                mvprintw(LINES-2, 0, "Save changes? (y/n): ");
                refresh();
                int sc = getch();
                if (sc == 'y' || sc == 'Y')
                    saveFile(filename, &lb);
            }
            break;
        }

        /* ── Save ── */
        if (ch == 19) {  /* ^S */
            saveFile(filename, &lb);
            modified = 0;
            snprintf(status_msg, sizeof(status_msg), "Saved!");
            continue;
        }

        /* ── Quit without save ── */
        if (ch == 17) break;  /* ^Q */

        /* ── Undo ^Z ── */
        if (ch == 26) {
            if (undo_do(&us, &lb, &row, &col)) {
                modified = 1;
                snprintf(status_msg, sizeof(status_msg), "Undo");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Nothing to undo");
            }
            sticky_col = col;
            goto scroll_check;
        }

        /* ── Redo ^Y ── */
        if (ch == 25) {
            if (redo_do(&us, &lb, &row, &col)) {
                modified = 1;
                snprintf(status_msg, sizeof(status_msg), "Redo");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Nothing to redo");
            }
            sticky_col = col;
            goto scroll_check;
        }

        /* ── Find ^F ── */
        if (ch == 6) {
            char query[256] = "";
            echo();
            mvhline(LINES-1, 0, ' ', max_col);
            mvprintw(LINES-1, 0, "Search: ");
            refresh();
            mvgetnstr(LINES-1, 8, query, (int)sizeof(query)-1);
            noecho();
            if (!query[0]) { snprintf(status_msg, sizeof(status_msg), "Cancelled"); continue; }
            int found = 0;
            for (int pass = 0; pass < 2 && !found; pass++) {
                int s = pass==0 ? row : 0;
                int e = pass==0 ? lb.count : row+1;
                for (int i = s; i < e && !found; i++) {
                    int start_col = (i == row && pass == 0) ? col+1 : 0;
                    char *p = strstr(lb.data[i] + start_col, query);
                    if (p) {
                        row = i;
                        col = (int)(p - lb.data[i]);
                        sticky_col = col;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Found at Ln %d Col %d", row+1, col+1);
                        found = 1;
                    }
                }
            }
            if (!found) snprintf(status_msg, sizeof(status_msg), "Not found: %s", query);
            goto scroll_check;
        }

        /* ── Goto ^G ── */
        if (ch == 7) {
            char buf[32] = "";
            echo();
            mvhline(LINES-1, 0, ' ', max_col);
            mvprintw(LINES-1, 0, "Goto line: ");
            refresh();
            mvgetnstr(LINES-1, 11, buf, (int)sizeof(buf)-1);
            noecho();
            int target = atoi(buf);
            if (target < 1 || target > lb.count) {
                snprintf(status_msg, sizeof(status_msg), "Invalid: %s", buf);
                continue;
            }
            row = target - 1;
            int llen = (int)strlen(lb.data[row]);
            if (col > llen) col = llen;
            sticky_col = col;
            snprintf(status_msg, sizeof(status_msg), "Jumped to line %d", target);
            goto scroll_check;
        }

        /* ── Navigation ── */
        switch (ch) {
        case KEY_UP:
            if (row > 0) {
                row--;
                int llen = (int)strlen(lb.data[row]);
                col = sticky_col < llen ? sticky_col : llen;
            }
            break;
        case KEY_DOWN:
            if (row < lb.count - 1) {
                row++;
                int llen = (int)strlen(lb.data[row]);
                col = sticky_col < llen ? sticky_col : llen;
            }
            break;
        case KEY_LEFT:
            if (col > 0) { col--; sticky_col = col; }
            else if (row > 0) {
                row--;
                col = sticky_col = (int)strlen(lb.data[row]);
            }
            break;
        case KEY_RIGHT: {
            int llen = (int)strlen(lb.data[row]);
            if (col < llen) { col++; sticky_col = col; }
            else if (row < lb.count-1) { row++; col = sticky_col = 0; }
            break;
        }
        case KEY_HOME:
            col = sticky_col = 0;
            break;
        case KEY_END:
            col = sticky_col = (int)strlen(lb.data[row]);
            break;
        case KEY_PPAGE:  /* Page Up */
            row = (row - (max_row-2) > 0) ? row - (max_row-2) : 0;
            { int llen=(int)strlen(lb.data[row]); if(col>llen) col=llen; sticky_col=col; }
            break;
        case KEY_NPAGE:  /* Page Down */
            row = (row + (max_row-2) < lb.count-1) ? row+(max_row-2) : lb.count-1;
            { int llen=(int)strlen(lb.data[row]); if(col>llen) col=llen; sticky_col=col; }
            break;

        /* ── Backspace ── */
        case 8: case 127: case KEY_BACKSPACE:
            PUSH_UNDO();
            if (col > 0) {
                size_t cur_len = strlen(lb.data[row]);
                memmove(&lb.data[row][col-1], &lb.data[row][col],
                        cur_len - col + 1);
                col--;
                modified = 1;
            } else if (row > 0) {
                int prev_len = (int)strlen(lb.data[row-1]);
                size_t newlen = prev_len + strlen(lb.data[row]) + 1;
                char *merged = realloc(lb.data[row-1], newlen);
                if (!merged) { merged = safe_malloc(newlen); strcpy(merged, lb.data[row-1]); free(lb.data[row-1]); }
                lb.data[row-1] = merged;
                strcat(lb.data[row-1], lb.data[row]);
                free(lb.data[row]);
                for (int i = row; i < lb.count-1; i++) lb.data[i] = lb.data[i+1];
                lb.count--;
                row--;
                col = prev_len;
                modified = 1;
            }
            sticky_col = col;
            break;

        /* ── Delete key ── */
        case KEY_DC:
            PUSH_UNDO();
            {
                int llen = (int)strlen(lb.data[row]);
                if (col < llen) {
                    memmove(&lb.data[row][col], &lb.data[row][col+1], llen-col);
                    modified = 1;
                } else if (row < lb.count-1) {
                    size_t newlen = llen + strlen(lb.data[row+1]) + 1;
                    char *merged = realloc(lb.data[row], newlen);
                    if (!merged) { merged=safe_malloc(newlen); strcpy(merged,lb.data[row]); free(lb.data[row]); }
                    lb.data[row] = merged;
                    strcat(lb.data[row], lb.data[row+1]);
                    free(lb.data[row+1]);
                    for (int i = row+1; i < lb.count-1; i++) lb.data[i]=lb.data[i+1];
                    lb.count--;
                    modified = 1;
                }
            }
            sticky_col = col;
            break;

        /* ── Enter ── */
        case '\n': {
            PUSH_UNDO();
            char *tail = safe_strdup(lb.data[row] + col);
            lb.data[row][col] = '\0';
            lb.count++;
            lb_ensure(&lb, lb.count + 1);
            for (int i = lb.count-1; i > row+1; i--) lb.data[i] = lb.data[i-1];
            lb.data[row+1] = tail;
            row++; col = sticky_col = 0;
            modified = 1;
            break;
        }

        /* ── Printable character ── */
        default:
            if (ch >= 32 && ch <= 126) {
                PUSH_UNDO();
                int llen = (int)strlen(lb.data[row]);
                char *nl = safe_malloc(llen + 2);
                if (col > 0) memcpy(nl, lb.data[row], col);
                nl[col] = (char)ch;
                memcpy(nl + col + 1, lb.data[row] + col, llen - col + 1);
                free(lb.data[row]);
                lb.data[row] = nl;
                col++; sticky_col = col;
                modified = 1;
            }
            break;
        }

scroll_check:
        /* vertical scroll */
        if (row < scroll_row) scroll_row = row;
        else if (row - scroll_row >= max_row - 1)
            scroll_row = row - (max_row - 2);

        /* horizontal scroll */
        int screen_c = LINE_NUM_W + col - scroll_col;
        if (screen_c < LINE_NUM_W) scroll_col = LINE_NUM_W + col - LINE_NUM_W;
        if (scroll_col < 0) scroll_col = 0;
        if (screen_c >= max_col - 1) scroll_col = LINE_NUM_W + col - (max_col - 2);
    }

    delwin(pad);
    endwin();
    lb_free(&lb);
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: quill <filename>\n");
        printf("       quill uninstall\n");
        printf("       quill update\n");
        return 1;
    }

    if (strcmp(argv[1], "uninstall") == 0) {
        if (remove("/usr/local/bin/quill") == 0)
            printf("Quill uninstalled.\n");
        else
            perror("uninstall failed");
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        /* safer than system(): exec directly */
        char *args[] = {"/bin/bash", "update.sh", NULL};
        execv("/bin/bash", args);
        perror("execv update.sh");
        return 1;
    }

    nanoEditor(argv[1]);
    return 0;
}