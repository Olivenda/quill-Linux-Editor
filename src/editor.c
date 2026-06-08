#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#define MAX_LINES     10000
#define MAX_LINE_LEN  4096
#define UNDO_LIMIT    256
#define LINE_NUM_W    6    /* "NNNNN " */
#define TAB_WIDTH     4
#define MAX_TABS      8
#define MAX_BOOKMARKS 64
#define MAX_FNAME     1024
#define TAB_BAR_H     1   /* rows reserved for the tab bar */

/* ─── Utility ────────────────────────────────────────────────────────────── */

int fileExists(const char *filename) {
    struct stat buf;
    return stat(filename, &buf) == 0;
}

static char *safe_strdup(const char *s) {
    if (!s) s = "";
    char *p = strdup(s);
    if (!p) { endwin(); perror("strdup"); exit(1); }
    return p;
}

static void *safe_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) { endwin(); perror("malloc"); exit(1); }
    return p;
}

static void *safe_realloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) { endwin(); perror("realloc"); exit(1); }
    return p;
}

/* ─── Line buffer ────────────────────────────────────────────────────────── */

typedef struct {
    char **data;
    int    count;
    int    cap;
} LineBuf;

static void lb_ensure(LineBuf *lb, int needed) {
    if (lb->cap >= needed) return;
    int nc = lb->cap ? lb->cap : 256;
    while (nc < needed) nc *= 2;
    lb->data = safe_realloc(lb->data, sizeof(char *) * nc);
    lb->cap = nc;
}

static LineBuf lb_load(const char *filename, int *existed) {
    LineBuf lb = {0};
    lb_ensure(&lb, 256);
    *existed = 0;

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        lb.data[0] = safe_strdup("");
        lb.count = 1;
        return lb;
    }
    *existed = 1;

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
    lb.data[lb.count++] = safe_strdup(start);
    free(buf);

    if (lb.count == 0) {
        lb.data[0] = safe_strdup("");
        lb.count = 1;
    }
    return lb;
}

static void lb_free(LineBuf *lb) {
    if (!lb->data) return;
    for (int i = 0; i < lb->count; i++) free(lb->data[i]);
    free(lb->data);
    lb->data  = NULL;
    lb->count = lb->cap = 0;
}

static LineBuf lb_clone(const LineBuf *src) {
    LineBuf dst = {0};
    lb_ensure(&dst, src->count + 2);
    for (int i = 0; i < src->count; i++)
        dst.data[i] = safe_strdup(src->data[i]);
    dst.count = src->count;
    return dst;
}

/* ─── Undo / Redo  (push-AFTER semantics) ───────────────────────────────── */

typedef struct {
    LineBuf snap;
    int     row, col;
} UndoEntry;

typedef struct {
    UndoEntry entries[UNDO_LIMIT];
    int       head;
    int       size;
    int       pos;
} UndoStack;

static int undo_idx(const UndoStack *us, int logical) {
    int base = (us->head - us->size + UNDO_LIMIT * 4) % UNDO_LIMIT;
    return (base + logical) % UNDO_LIMIT;
}

static void undo_push(UndoStack *us, const LineBuf *lb, int row, int col) {
    for (int i = us->pos; i < us->size; i++)
        lb_free(&us->entries[undo_idx(us, i)].snap);
    us->size = us->pos;

    int idx = us->head;
    if (us->size == UNDO_LIMIT)
        lb_free(&us->entries[idx].snap);
    else
        us->size++;

    us->entries[idx].snap = lb_clone(lb);
    us->entries[idx].row  = row;
    us->entries[idx].col  = col;
    us->head = (us->head + 1) % UNDO_LIMIT;
    us->pos  = us->size;
}

static int undo_do(UndoStack *us, LineBuf *lb, int *row, int *col) {
    if (us->pos <= 1) return 0;
    us->pos--;
    int idx = undo_idx(us, us->pos - 1);
    lb_free(lb);
    *lb  = lb_clone(&us->entries[idx].snap);
    *row = us->entries[idx].row;
    *col = us->entries[idx].col;
    return 1;
}

static int redo_do(UndoStack *us, LineBuf *lb, int *row, int *col) {
    if (us->pos >= us->size) return 0;
    int idx = undo_idx(us, us->pos);
    us->pos++;
    lb_free(lb);
    *lb  = lb_clone(&us->entries[idx].snap);
    *row = us->entries[idx].row;
    *col = us->entries[idx].col;
    return 1;
}

static void undo_free(UndoStack *us) {
    for (int i = 0; i < us->size; i++)
        lb_free(&us->entries[undo_idx(us, i)].snap);
    us->size = us->pos = us->head = 0;
}

/* ─── File I/O ───────────────────────────────────────────────────────────── */

static int confirmOverwrite(const char *filename, int max_col) {
    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "File '%s' exists! Overwrite? (y/n): ", filename);
    mvhline(LINES-2, 0, ' ', max_col);
    mvprintw(LINES-2, 0, "%s", prompt);
    refresh();
    int ch;
    while (1) {
        ch = getch();
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 27) return 0;
    }
}

/* Returns 0=saved, 1=cancelled, -1=error (errmsg filled). */
static int saveFile(const char *filename, const LineBuf *lb,
                    int loaded_from_disk, char *errmsg, size_t errmsg_sz)
{
    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);
    (void)max_row;

    if (!loaded_from_disk && fileExists(filename)) {
        if (!confirmOverwrite(filename, max_col)) return 1;
    }

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
    if (!fp) {
        snprintf(errmsg, errmsg_sz, "Save failed: %s", strerror(errno));
        free(outbuf);
        return -1;
    }
    size_t wrote  = fwrite(outbuf, 1, total, fp);
    int    closed = fclose(fp);
    free(outbuf);

    if (wrote != total || closed != 0) {
        snprintf(errmsg, errmsg_sz, "Save failed: write error");
        return -1;
    }
    return 0;
}

/* ─── Syntax Highlighting ────────────────────────────────────────────────── */

typedef struct { const char *word; int color; } KwColor;

static const KwColor kw_table[] = {
    /* types */
    {"int",8},{"char",8},{"void",8},{"double",8},{"float",8},
    {"long",8},{"short",8},{"unsigned",8},{"signed",8},
    {"size_t",8},{"off_t",8},{"ptrdiff_t",8},
    {"uint8_t",8},{"uint16_t",8},{"uint32_t",8},{"uint64_t",8},
    {"int8_t",8},{"int16_t",8},{"int32_t",8},{"int64_t",8},
    {"bool",8},{"FILE",8},{"WINDOW",8},
    /* keywords */
    {"return",5},{"if",5},{"else",5},{"for",5},{"while",5},
    {"break",5},{"continue",5},{"switch",5},{"case",5},
    {"default",5},{"do",5},{"goto",5},{"enum",5},{"union",5},
    {"struct",5},{"typedef",5},{"static",5},{"const",5},
    {"volatile",5},{"extern",5},{"inline",5},{"register",5},
    {"auto",5},{"asm",5},{"_Bool",5},{"sizeof",5},{"typeof",5},
    /* constants */
    {"NULL",3},{"true",3},{"false",3},{"EOF",3},
    /* stdlib / ncurses */
    {"printf",6},{"fprintf",6},{"sprintf",6},{"snprintf",6},
    {"scanf",6},{"sscanf",6},{"malloc",6},{"calloc",6},
    {"realloc",6},{"free",6},{"memcpy",6},{"memmove",6},
    {"memset",6},{"memcmp",6},{"strlen",6},{"strcpy",6},
    {"strncpy",6},{"strdup",6},{"strcmp",6},{"strncmp",6},
    {"strstr",6},{"strchr",6},{"strerror",6},
    {"fopen",6},{"fclose",6},{"fread",6},{"fwrite",6},
    {"fseek",6},{"ftell",6},{"fstat",6},{"stat",6},
    {"exit",6},{"perror",6},{"main",6},
    {NULL, 0}
};

static int getKwColor(const char *word) {
    for (int i = 0; kw_table[i].word; i++)
        if (strcmp(word, kw_table[i].word) == 0)
            return kw_table[i].color;
    return 0;
}

/* Expand tabs to TAB_WIDTH-space stops; caller must free() result. */
static char *expand_tabs(const char *src) {
    size_t len = strlen(src);
    size_t cap = len + 16;
    char  *out = safe_malloc(cap + 1);
    size_t oi  = 0;
    int    dc  = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\t') {
            int sp = TAB_WIDTH - (dc % TAB_WIDTH);
            if (oi + sp + 1 > cap) {
                cap = (oi + sp + 16) * 2;
                out = safe_realloc(out, cap + 1);
            }
            for (int s = 0; s < sp; s++) { out[oi++] = ' '; dc++; }
        } else {
            if (oi + 2 > cap) {
                cap = (oi + 16) * 2;
                out = safe_realloc(out, cap + 1);
            }
            out[oi++] = src[i];
            dc++;
        }
    }
    out[oi] = '\0';
    return out;
}

/*
 * Draw one line with syntax highlighting into pad at row y.
 * in_ml: are we already inside a block comment?
 * Returns 1 if still inside a block comment at end of line.
 */
static int printHighlightedLine(WINDOW *pad, int y, const char *raw_line,
                                int in_ml)
{
    char *line   = expand_tabs(raw_line);
    int   i      = 0;
    int   len    = (int)strlen(line);
    int   ret_ml = 0;

    /* ── continuing block comment ── */
    if (in_ml) {
        wattron(pad, COLOR_PAIR(9));
        while (i < len) {
            if (line[i] == '*' && i+1 < len && line[i+1] == '/') {
                i += 2;
                mvwprintw(pad, y, LINE_NUM_W, "%.*s", i, line);
                wattroff(pad, COLOR_PAIR(9));
                goto normal;
            }
            i++;
        }
        mvwprintw(pad, y, LINE_NUM_W, "%s", line);
        wattroff(pad, COLOR_PAIR(9));
        free(line);
        return 1;
    }

normal:
    while (i < len) {

        /* block comment open */
        if (line[i] == '/' && i+1 < len && line[i+1] == '*') {
            int start = i;
            i += 2;
            wattron(pad, COLOR_PAIR(9));
            while (i < len) {
                if (line[i] == '*' && i+1 < len && line[i+1] == '/') {
                    i += 2;
                    mvwprintw(pad, y, LINE_NUM_W + start,
                              "%.*s", i-start, line+start);
                    wattroff(pad, COLOR_PAIR(9));
                    goto normal_cont;
                }
                i++;
            }
            mvwprintw(pad, y, LINE_NUM_W + start, "%s", line + start);
            wattroff(pad, COLOR_PAIR(9));
            ret_ml = 1;
            break;
        }

        /* line comment */
        if (line[i] == '/' && i+1 < len && line[i+1] == '/') {
            wattron(pad, COLOR_PAIR(9));
            mvwprintw(pad, y, LINE_NUM_W + i, "%s", line + i);
            wattroff(pad, COLOR_PAIR(9));
            break;
        }

        /* preprocessor directive */
        if (line[i] == '#' && (i == 0 || isspace((unsigned char)line[i-1]))) {
            int start = i++;
            while (i < len && isalpha((unsigned char)line[i])) i++;
            wattron(pad, COLOR_PAIR(4));
            mvwprintw(pad, y, LINE_NUM_W + start,
                      "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(4));
            continue;
        }

        /* string literal */
        if (line[i] == '"') {
            int start = i++;
            while (i < len) {
                if (line[i] == '\\' && i+1 < len) { i += 2; continue; }
                if (line[i] == '"') { i++; break; }
                i++;
            }
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, LINE_NUM_W + start,
                      "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }

        /* char literal */
        if (line[i] == '\'') {
            int start = i++;
            while (i < len) {
                if (line[i] == '\\' && i+1 < len) { i += 2; continue; }
                if (line[i] == '\'') { i++; break; }
                i++;
            }
            wattron(pad, COLOR_PAIR(7));
            mvwprintw(pad, y, LINE_NUM_W + start,
                      "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(7));
            continue;
        }

        /* numeric literal */
        if (isdigit((unsigned char)line[i]) &&
            (i == 0 || !isalnum((unsigned char)line[i-1])))
        {
            int start = i;
            while (i < len && (isxdigit((unsigned char)line[i]) ||
                               line[i]=='.' || line[i]=='x' || line[i]=='X' ||
                               line[i]=='u' || line[i]=='U' ||
                               line[i]=='l' || line[i]=='L')) i++;
            wattron(pad, COLOR_PAIR(3));
            mvwprintw(pad, y, LINE_NUM_W + start,
                      "%.*s", i-start, line+start);
            wattroff(pad, COLOR_PAIR(3));
            continue;
        }

        /* identifier / keyword */
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            int start = i;
            while (i < len && (isalnum((unsigned char)line[i]) ||
                               line[i] == '_')) i++;
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

        /* braces */
        if (line[i] == '{' || line[i] == '}') {
            wattron(pad, COLOR_PAIR(5));
            mvwaddch(pad, y, LINE_NUM_W + i, line[i]);
            wattroff(pad, COLOR_PAIR(5));
            i++;
            continue;
        }
        /* parentheses */
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

    free(line);
    return ret_ml;
}

/* ─── File type label ────────────────────────────────────────────────────── */

static const char *file_type_label(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return "Plain";
    dot++;
    if (!strcmp(dot,"c"))                                      return "C";
    if (!strcmp(dot,"h"))                                      return "C Header";
    if (!strcmp(dot,"cpp")||!strcmp(dot,"cc")||
        !strcmp(dot,"cxx")||!strcmp(dot,"hpp"))                return "C++";
    if (!strcmp(dot,"py"))                                     return "Python";
    if (!strcmp(dot,"js"))                                     return "JavaScript";
    if (!strcmp(dot,"ts"))                                     return "TypeScript";
    if (!strcmp(dot,"rs"))                                     return "Rust";
    if (!strcmp(dot,"go"))                                     return "Go";
    if (!strcmp(dot,"sh"))                                     return "Shell";
    if (!strcmp(dot,"md"))                                     return "Markdown";
    if (!strcmp(dot,"html")||!strcmp(dot,"htm"))               return "HTML";
    if (!strcmp(dot,"css"))                                    return "CSS";
    if (!strcmp(dot,"json"))                                   return "JSON";
    if (!strcmp(dot,"java"))                                   return "Java";
    if (!strcmp(dot,"txt"))                                    return "Text";
    return "Plain";
}

/* ─── Display column helper ──────────────────────────────────────────────── */

static int byte_to_display_col(const char *line, int byte_col) {
    int dc = 0;
    for (int i = 0; i < byte_col && line[i]; i++) {
        if (line[i] == '\t') dc += TAB_WIDTH - (dc % TAB_WIDTH);
        else                  dc++;
    }
    return dc;
}

/* ─── Word-jump helpers ──────────────────────────────────────────────────── */

static int is_word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static void word_jump_right(const LineBuf *lb, int *row, int *col) {
    int r = *row, c = *col;
    int llen = (int)strlen(lb->data[r]);
    while (c < llen &&  is_word_char((unsigned char)lb->data[r][c])) c++;
    while (c < llen && !is_word_char((unsigned char)lb->data[r][c])) c++;
    if (c >= llen && r < lb->count - 1) {
        r++; c = 0;
        llen = (int)strlen(lb->data[r]);
        while (c < llen && !is_word_char((unsigned char)lb->data[r][c])) c++;
    }
    *row = r; *col = c;
}

static void word_jump_left(const LineBuf *lb, int *row, int *col) {
    int r = *row, c = *col;
    if (c == 0) {
        if (r > 0) { r--; c = (int)strlen(lb->data[r]); }
    } else {
        c--;
        while (c > 0 && !is_word_char((unsigned char)lb->data[r][c])) c--;
        while (c > 0 &&  is_word_char((unsigned char)lb->data[r][c-1])) c--;
    }
    *row = r; *col = c;
}

/* ─── Tab struct ─────────────────────────────────────────────────────────── */
/*
 * Each open file gets one Tab.  All per-buffer state lives here so the
 * editor can switch between tabs without losing any context.
 */
typedef struct {
    LineBuf   lb;
    char      filename[MAX_FNAME];
    int       row, col;
    int       scroll_row, scroll_col;
    int       sticky_col;
    int       modified;
    int       loaded_from_disk;
    UndoStack us;
    char     *cut_buffer;
    int       bookmarks[MAX_BOOKMARKS];
    int       bmark_count;
    int       last_home_row, last_home_col_nws;
} Tab;

static Tab tabs[MAX_TABS];
static int tab_count = 0;
static int cur_tab   = 0;

/* Split-view state: two panes showing different tabs side by side. */
static int split_on    = 0;  /* 1 when split is active            */
static int split_right = 1;  /* tab index shown in the right pane */
static int split_focus = 0;  /* 0 = left pane, 1 = right pane     */

static void tab_init(Tab *t, const char *fname) {
    memset(t, 0, sizeof(*t));
    snprintf(t->filename, MAX_FNAME, "%s", fname);
    t->lb = lb_load(fname, &t->loaded_from_disk);
    lb_ensure(&t->lb, t->lb.count + 512);
    t->last_home_row     = -1;
    t->last_home_col_nws = -1;
    undo_push(&t->us, &t->lb, 0, 0);
}

static void tab_free(Tab *t) {
    lb_free(&t->lb);
    undo_free(&t->us);
    free(t->cut_buffer);
    t->cut_buffer = NULL;
}

/* ─── Bookmark helpers ───────────────────────────────────────────────────── */

static int bmark_has(const Tab *t, int line) {
    for (int i = 0; i < t->bmark_count; i++)
        if (t->bookmarks[i] == line) return 1;
    return 0;
}

static void bmark_toggle(Tab *t, int line) {
    for (int i = 0; i < t->bmark_count; i++) {
        if (t->bookmarks[i] == line) {
            for (int j = i; j < t->bmark_count - 1; j++)
                t->bookmarks[j] = t->bookmarks[j+1];
            t->bmark_count--;
            return;
        }
    }
    if (t->bmark_count < MAX_BOOKMARKS)
        t->bookmarks[t->bmark_count++] = line;
}

/* Returns the nearest bookmarked line after from_line, wrapping around. */
static int bmark_next(const Tab *t, int from_line) {
    int best = -1;
    for (int i = 0; i < t->bmark_count; i++) {
        int bl = t->bookmarks[i];
        if (bl > from_line && (best == -1 || bl < best)) best = bl;
    }
    if (best == -1) {
        /* wrap */
        for (int i = 0; i < t->bmark_count; i++) {
            int bl = t->bookmarks[i];
            if (best == -1 || bl < best) best = bl;
        }
    }
    return best;
}

/* ─── Status bar ─────────────────────────────────────────────────────────── */

static void drawStatusBar(const char *filename, int modified,
                          int row, int col, int total_lines, int max_col,
                          const char *msg, int tab_idx, int n_tabs)
{
    attron(A_REVERSE);
    mvhline(LINES-1, 0, ' ', max_col);

    char left[MAX_FNAME + 64];
    snprintf(left, sizeof(left), " %s [%s] %s",
             filename, file_type_label(filename),
             modified ? "[Modified]" : "[Saved]");
    mvprintw(LINES-1, 0, "%s", left);

    const char *keys =
        "^S Save  ^Z Undo  ^Y Redo  ^F Find  ^G Goto  ^K Cut  ^U Paste  ^X Exit";
    int kpos = (max_col - (int)strlen(keys)) / 2;
    if (kpos > (int)strlen(left) + 1)
        mvprintw(LINES-1, kpos, "%s", keys);

    char right[96];
    snprintf(right, sizeof(right), "Tab %d/%d  Ln %d/%d  Col %d ",
             tab_idx + 1, n_tabs, row + 1, total_lines, col + 1);
    int rpos = max_col - (int)strlen(right);
    if (rpos < 0) rpos = 0;
    mvprintw(LINES-1, rpos, "%s", right);

    if (msg && msg[0]) {
        int mpos = rpos - (int)strlen(msg) - 2;
        if (mpos > (int)strlen(left) + 1)
            mvprintw(LINES-1, mpos, "%s", msg);
    }
    attroff(A_REVERSE);
    refresh();
}

/* ─── Tab bar ────────────────────────────────────────────────────────────── */
/*
 * Renders the top row showing all open tabs.
 * Active tab (receives input) is bold + reversed.
 * The tab shown in the right split pane gets underlined.
 * Modified tabs show a '*' marker.
 */
static void drawTabBar(int max_col) {
    attron(COLOR_PAIR(11));
    mvhline(0, 0, ' ', max_col);

    int x = 0;
    for (int i = 0; i < tab_count; i++) {
        const char *bn = strrchr(tabs[i].filename, '/');
        bn = bn ? bn + 1 : tabs[i].filename;

        char label[36];
        snprintf(label, sizeof(label), " %.24s%s ",
                 bn, tabs[i].modified ? "*" : " ");

        int is_active      = (i == cur_tab && (!split_on || split_focus == 0));
        int is_split_focus = (split_on && i == split_right && split_focus == 1);
        int is_split_other = (split_on && i == split_right && split_focus == 0);
        int is_cur_nofocus = (i == cur_tab && split_on && split_focus == 1);

        attroff(COLOR_PAIR(11));
        if (is_active) {
            attron(A_BOLD | COLOR_PAIR(10));
        } else if (is_split_focus) {
            attron(A_BOLD | COLOR_PAIR(10) | A_UNDERLINE);
        } else if (is_split_other || is_cur_nofocus) {
            attron(COLOR_PAIR(10));
        } else {
            attron(COLOR_PAIR(11));
        }

        if (x + (int)strlen(label) < max_col - 50)
            mvprintw(0, x, "%s", label);

        attroff(A_BOLD | A_UNDERLINE | COLOR_PAIR(10) | COLOR_PAIR(11));
        attron(COLOR_PAIR(11));

        x += (int)strlen(label);
        if (i < tab_count - 1 && x < max_col - 50) {
            mvaddch(0, x++, ACS_VLINE);
        }
    }

    /* Keyboard hint aligned to the right */
    const char *hint = " F2/F3:tabs  F4:open  ^W:close  F5:split  F6:swap  F7:mark  F8:next ";
    int hpos = max_col - (int)strlen(hint);
    if (hpos > x + 2)
        mvprintw(0, hpos, "%s", hint);

    attroff(COLOR_PAIR(11));
}

/* ─── Pane rendering ─────────────────────────────────────────────────────── */
/*
 * Draws a single tab's content into pad and refreshes a rectangular region
 * of the screen [top..bottom, left_col..right_col].
 */
static void drawPaneContent(WINDOW *pad, Tab *t,
                             int top, int left_col, int bottom, int right_col)
{
    int visible = bottom - top + 1;

    werase(pad);

    /* Pre-scan multiline-comment state for lines above the viewport. */
    int in_ml   = 0;
    int prescan = t->scroll_row < t->lb.count ? t->scroll_row : t->lb.count;
    for (int i = 0; i < prescan; i++) {
        const char *l = t->lb.data[i];
        int len = (int)strlen(l);
        for (int j = 0; j < len; j++) {
            if (!in_ml && l[j]=='/' && j+1<len && l[j+1]=='*') { in_ml=1; j++; }
            else if (in_ml && l[j]=='*' && j+1<len && l[j+1]=='/') { in_ml=0; j++; }
        }
    }

    int draw_limit = t->lb.count < MAX_LINES ? t->lb.count : MAX_LINES;
    for (int i = t->scroll_row; i < draw_limit && i - t->scroll_row < visible; i++) {
        int pr = i - t->scroll_row;

        /* Bookmarked lines get a distinct line-number colour. */
        if (bmark_has(t, i)) {
            wattron(pad, COLOR_PAIR(12) | A_BOLD);
            mvwprintw(pad, pr, 0, "%5d ", i + 1);
            wattroff(pad, COLOR_PAIR(12) | A_BOLD);
        } else {
            wattron(pad, COLOR_PAIR(1));
            mvwprintw(pad, pr, 0, "%5d ", i + 1);
            wattroff(pad, COLOR_PAIR(1));
        }

        in_ml = printHighlightedLine(pad, pr, t->lb.data[i], in_ml);
    }

    pnoutrefresh(pad, 0, t->scroll_col, top, left_col, bottom, right_col);
}

/* ─── Editor main loop ───────────────────────────────────────────────────── */

void runEditor(const char *initial_file) {
    tab_init(&tabs[0], initial_file);
    tab_count   = 1;
    cur_tab     = 0;
    split_on    = 0;
    split_right = 0;
    split_focus = 0;

    initscr();
    start_color();
    use_default_colors();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    /* Existing colour pairs (1–9 unchanged) */
    init_pair(1,  COLOR_CYAN,    -1);
    init_pair(2,  COLOR_WHITE,   -1);
    init_pair(3,  COLOR_YELLOW,  -1);
    init_pair(4,  COLOR_GREEN,   -1);
    init_pair(5,  COLOR_MAGENTA, -1);
    init_pair(6,  COLOR_CYAN,    -1);
    init_pair(7,  COLOR_RED,     -1);
    init_pair(8,  COLOR_BLUE,    -1);
    init_pair(9,  COLOR_GREEN,   -1);
    /* New colour pairs */
    init_pair(10, COLOR_BLACK,   COLOR_CYAN);   /* tab bar: active/visible tab  */
    init_pair(11, COLOR_WHITE,   COLOR_BLUE);   /* tab bar: background          */
    init_pair(12, COLOR_YELLOW,  -1);           /* bookmarked line number       */
    init_pair(13, COLOR_WHITE,   -1);           /* split divider                */

    define_key("\033[1;5C", KEY_SRIGHT);
    define_key("\033[1;5D", KEY_SLEFT);

    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);

    WINDOW *pad_left  = newpad(MAX_LINES, MAX_LINE_LEN + LINE_NUM_W + 16);
    WINDOW *pad_right = newpad(MAX_LINES, MAX_LINE_LEN + LINE_NUM_W + 16);

    char status_msg[256] = "";

#define CTAB (tabs[cur_tab])

    while (1) {
        getmaxyx(stdscr, max_row, max_col);

        int content_top = TAB_BAR_H;
        int content_bot = max_row - 2;
        int split_col   = (max_col - 1) / 2;  /* column index of divider bar */

        /* ── Draw tab bar ── */
        drawTabBar(max_col);

        /* ── Draw content pane(s) ── */
        if (split_on && tab_count > 1) {
            /* Guarantee split_right is a different, valid tab. */
            if (split_right >= tab_count || split_right == cur_tab)
                split_right = (cur_tab + 1) % tab_count;

            drawPaneContent(pad_left,  &tabs[cur_tab],
                            content_top, 0, content_bot, split_col - 1);

            /* Divider */
            attron(COLOR_PAIR(13) | A_DIM);
            for (int r = content_top; r <= content_bot; r++)
                mvaddch(r, split_col, ACS_VLINE);
            attroff(COLOR_PAIR(13) | A_DIM);

            drawPaneContent(pad_right, &tabs[split_right],
                            content_top, split_col + 1, content_bot, max_col - 1);
        } else {
            split_on = 0;
            drawPaneContent(pad_left, &tabs[cur_tab],
                            content_top, 0, content_bot, max_col - 1);
        }

        /* ── Status bar: reflects the focused pane ── */
        Tab *focused = (split_on && split_focus == 1) ? &tabs[split_right] : &CTAB;
        int  focused_idx = (split_on && split_focus == 1) ? split_right : cur_tab;
        drawStatusBar(focused->filename, focused->modified,
                      focused->row, focused->col, focused->lb.count,
                      max_col, status_msg, focused_idx, tab_count);

        /* ── Cursor placement ── */
        {
            int dcol     = byte_to_display_col(focused->lb.data[focused->row],
                                               focused->col);
            int base_c, pane_right;
            if (split_on && split_focus == 1) {
                base_c     = split_col + 1;
                pane_right = max_col - 1;
            } else {
                base_c     = 0;
                pane_right = split_on ? split_col - 1 : max_col - 1;
            }
            int screen_c = base_c + (LINE_NUM_W + dcol) - focused->scroll_col;
            int screen_r = (focused->row - focused->scroll_row) + content_top;
            if (screen_c < base_c)      screen_c = base_c;
            if (screen_c > pane_right)  screen_c = pane_right;
            if (screen_r < content_top) screen_r = content_top;
            if (screen_r > content_bot) screen_r = content_bot;
            move(screen_r, screen_c);
        }
        doupdate();

        int ch = getch();
        status_msg[0] = '\0';

        /* Active tab pointer — keyboard input goes to the focused pane. */
        Tab *t = (split_on && split_focus == 1) ? &tabs[split_right] : &CTAB;

        /* Reset smart-Home state on any key other than Home / ^A */
        if (ch != 1 && ch != KEY_HOME) {
            t->last_home_row     = -1;
            t->last_home_col_nws = -1;
        }

#define TPUSH() undo_push(&t->us, &t->lb, t->row, t->col)

        /* ══ Tab navigation ══════════════════════════════════════════════════ */

        /* F2 – next tab */
        if (ch == KEY_F(2)) {
            if (!split_on || split_focus == 0) {
                cur_tab = (cur_tab + 1) % tab_count;
            } else {
                split_right = (split_right + 1) % tab_count;
                if (split_right == cur_tab)
                    split_right = (split_right + 1) % tab_count;
            }
            continue;
        }
        /* F3 – previous tab */
        if (ch == KEY_F(3)) {
            if (!split_on || split_focus == 0) {
                cur_tab = (cur_tab - 1 + tab_count) % tab_count;
            } else {
                split_right = (split_right - 1 + tab_count) % tab_count;
                if (split_right == cur_tab)
                    split_right = (split_right - 1 + tab_count) % tab_count;
            }
            continue;
        }
        /* F4 – open file in a new tab */
        if (ch == KEY_F(4)) {
            if (tab_count >= MAX_TABS) {
                snprintf(status_msg, sizeof(status_msg),
                         "Max %d tabs open", MAX_TABS);
                continue;
            }
            char fname[MAX_FNAME] = "";
            echo();
            mvhline(LINES-1, 0, ' ', max_col);
            mvprintw(LINES-1, 0, "Open file: ");
            refresh();
            mvgetnstr(LINES-1, 11, fname, MAX_FNAME - 1);
            noecho();
            if (!fname[0]) {
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
                continue;
            }
            tab_init(&tabs[tab_count], fname);
            cur_tab     = tab_count++;
            split_focus = 0;
            snprintf(status_msg, sizeof(status_msg), "Opened: %.200s", fname);
            continue;
        }
        /* ^W (23) – close current tab */
        if (ch == 23) {
            if (tab_count == 1) {
                snprintf(status_msg, sizeof(status_msg),
                         "Cannot close last tab");
                continue;
            }
            int close_idx = (split_on && split_focus == 1) ? split_right : cur_tab;
            Tab *ct = &tabs[close_idx];
            if (ct->modified) {
                mvhline(LINES-2, 0, ' ', max_col);
                mvprintw(LINES-2, 0,
                         "Tab modified – close without saving? (y/n): ");
                refresh();
                int sc = getch();
                if (sc != 'y' && sc != 'Y') {
                    snprintf(status_msg, sizeof(status_msg), "Close cancelled");
                    continue;
                }
            }
            tab_free(&tabs[close_idx]);
            for (int i = close_idx; i < tab_count - 1; i++)
                tabs[i] = tabs[i+1];
            memset(&tabs[tab_count-1], 0, sizeof(Tab));
            tab_count--;
            if (cur_tab >= tab_count) cur_tab = tab_count - 1;
            if (split_right >= tab_count) split_right = 0;
            if (split_on && split_right == cur_tab && tab_count > 1)
                split_right = (cur_tab + 1) % tab_count;
            split_focus = 0;
            continue;
        }
        /* F5 – toggle split view */
        if (ch == KEY_F(5)) {
            if (tab_count < 2) {
                snprintf(status_msg, sizeof(status_msg),
                         "Need at least 2 tabs for split view (F4 to open)");
                continue;
            }
            split_on = !split_on;
            if (split_on) {
                split_right = (cur_tab + 1) % tab_count;
                split_focus = 0;
                snprintf(status_msg, sizeof(status_msg), "Split view on");
            } else {
                split_focus = 0;
                snprintf(status_msg, sizeof(status_msg), "Split view off");
            }
            continue;
        }
        /* F6 – swap active pane in split view */
        if (ch == KEY_F(6)) {
            if (!split_on) {
                snprintf(status_msg, sizeof(status_msg),
                         "Not in split mode (F5 to enable)");
                continue;
            }
            split_focus ^= 1;
            snprintf(status_msg, sizeof(status_msg),
                     "Focus: %s pane", split_focus ? "right" : "left");
            continue;
        }
        /* F7 – toggle bookmark on current line */
        if (ch == KEY_F(7)) {
            int had = bmark_has(t, t->row);
            bmark_toggle(t, t->row);
            snprintf(status_msg, sizeof(status_msg),
                     had ? "Bookmark removed" : "Bookmarked Ln %d", t->row + 1);
            continue;
        }
        /* F8 – jump to next bookmark */
        if (ch == KEY_F(8)) {
            if (t->bmark_count == 0) {
                snprintf(status_msg, sizeof(status_msg),
                         "No bookmarks (F7 to add)");
                continue;
            }
            int nxt = bmark_next(t, t->row);
            if (nxt >= 0) {
                t->row = nxt;
                int llen = (int)strlen(t->lb.data[t->row]);
                if (t->col > llen) t->col = llen;
                t->sticky_col = t->col;
                snprintf(status_msg, sizeof(status_msg),
                         "Bookmark: Ln %d", t->row + 1);
            }
            goto scroll_check;
        }

        /* ══ File operations ═════════════════════════════════════════════════ */

        /* ^X – exit (saves all modified tabs with confirmation) */
        if (ch == 24) {
            int any_mod = 0;
            for (int i = 0; i < tab_count; i++)
                if (tabs[i].modified) any_mod = 1;
            if (any_mod) {
                mvhline(LINES-2, 0, ' ', max_col);
                mvprintw(LINES-2, 0,
                         "Save all modified tabs before exit? (y/n/ESC=cancel): ");
                refresh();
                int sc = getch();
                if (sc == 27) continue;
                if (sc == 'y' || sc == 'Y') {
                    for (int i = 0; i < tab_count; i++) {
                        if (tabs[i].modified) {
                            char err[256] = "";
                            saveFile(tabs[i].filename, &tabs[i].lb,
                                     tabs[i].loaded_from_disk, err, sizeof(err));
                        }
                    }
                }
            }
            break;
        }

        /* ^S – save active tab */
        if (ch == 19) {
            char err[256] = "";
            int rc = saveFile(t->filename, &t->lb, t->loaded_from_disk,
                              err, sizeof(err));
            if (rc == 0) {
                t->modified        = 0;
                t->loaded_from_disk = 1;
                snprintf(status_msg, sizeof(status_msg),
                         "Saved %d line%s", t->lb.count,
                         t->lb.count == 1 ? "" : "s");
            } else if (rc == -1) {
                snprintf(status_msg, sizeof(status_msg), "%s", err);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Save cancelled");
            }
            continue;
        }

        /* ^Q – quit without save prompt */
        if (ch == 17) break;

        /* ══ Undo / Redo ═════════════════════════════════════════════════════ */

        if (ch == 26) {   /* ^Z undo */
            if (undo_do(&t->us, &t->lb, &t->row, &t->col))
                snprintf(status_msg, sizeof(status_msg), "Undo");
            else
                snprintf(status_msg, sizeof(status_msg), "Nothing to undo");
            t->sticky_col = t->col;
            goto scroll_check;
        }
        if (ch == 25) {   /* ^Y redo */
            if (redo_do(&t->us, &t->lb, &t->row, &t->col)) {
                t->modified = 1;
                snprintf(status_msg, sizeof(status_msg), "Redo");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Nothing to redo");
            }
            t->sticky_col = t->col;
            goto scroll_check;
        }

        /* ══ Search / Navigation ═════════════════════════════════════════════ */

        if (ch == 6) {   /* ^F find */
            char query[256] = "";
            echo();
            mvhline(LINES-1, 0, ' ', max_col);
            mvprintw(LINES-1, 0, "Search: ");
            refresh();
            mvgetnstr(LINES-1, 8, query, (int)sizeof(query)-1);
            noecho();
            if (!query[0]) {
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
                continue;
            }
            int found = 0;
            for (int pass = 0; pass < 2 && !found; pass++) {
                int s = (pass == 0) ? t->row      : 0;
                int e = (pass == 0) ? t->lb.count : t->row + 1;
                for (int i = s; i < e && !found; i++) {
                    int llen      = (int)strlen(t->lb.data[i]);
                    int start_col = (i == t->row && pass == 0) ? t->col+1 : 0;
                    if (start_col > llen) continue;
                    char *p = strstr(t->lb.data[i] + start_col, query);
                    if (p) {
                        t->row = i;
                        t->col = (int)(p - t->lb.data[i]);
                        t->sticky_col = t->col;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Found: Ln %d Col %d", t->row+1, t->col+1);
                        found = 1;
                    }
                }
            }
            if (!found)
                snprintf(status_msg, sizeof(status_msg),
                         "Not found: %.200s", query);
            goto scroll_check;
        }

        if (ch == 7) {   /* ^G goto line */
            char buf[32] = "";
            echo();
            mvhline(LINES-1, 0, ' ', max_col);
            mvprintw(LINES-1, 0, "Goto line: ");
            refresh();
            mvgetnstr(LINES-1, 11, buf, (int)sizeof(buf)-1);
            noecho();
            int target = atoi(buf);
            if (target < 1 || target > t->lb.count) {
                snprintf(status_msg, sizeof(status_msg), "Invalid: %s", buf);
                continue;
            }
            t->row = target - 1;
            int llen = (int)strlen(t->lb.data[t->row]);
            if (t->col > llen) t->col = llen;
            t->sticky_col = t->col;
            snprintf(status_msg, sizeof(status_msg),
                     "Jumped to line %d", target);
            goto scroll_check;
        }

        /* ══ Smart Home ══════════════════════════════════════════════════════ */

        if (ch == 1 || ch == KEY_HOME) {
            const char *l   = t->lb.data[t->row];
            int llen        = (int)strlen(l);
            int first_nws   = 0;
            while (first_nws < llen &&
                   (l[first_nws] == ' ' || l[first_nws] == '\t'))
                first_nws++;
            if (first_nws == llen) first_nws = 0;
            int target;
            if (t->last_home_row == t->row &&
                t->last_home_col_nws == first_nws &&
                t->col == first_nws)
                target = 0;
            else
                target = (t->col == first_nws) ? 0 : first_nws;
            t->last_home_row     = t->row;
            t->last_home_col_nws = first_nws;
            t->col = t->sticky_col = target;
            goto scroll_check;
        }

        /* ══ Cut / Paste ═════════════════════════════════════════════════════ */

        if (ch == 11) {   /* ^K cut line */
            free(t->cut_buffer);
            int llen = (int)strlen(t->lb.data[t->row]);
            if (llen == 0 && t->lb.count > 1) {
                t->cut_buffer = safe_strdup("");
                free(t->lb.data[t->row]);
                for (int i = t->row; i < t->lb.count - 1; i++)
                    t->lb.data[i] = t->lb.data[i+1];
                t->lb.count--;
                if (t->row >= t->lb.count) t->row = t->lb.count - 1;
            } else if (llen == 0) {
                t->cut_buffer = safe_strdup("");
            } else {
                t->cut_buffer = safe_strdup(t->lb.data[t->row]);
                if (t->lb.count > 1) {
                    free(t->lb.data[t->row]);
                    for (int i = t->row; i < t->lb.count - 1; i++)
                        t->lb.data[i] = t->lb.data[i+1];
                    t->lb.count--;
                    if (t->row >= t->lb.count) t->row = t->lb.count - 1;
                } else {
                    t->lb.data[t->row][0] = '\0';
                }
            }
            t->col = t->sticky_col = 0;
            t->modified = 1;
            TPUSH();
            snprintf(status_msg, sizeof(status_msg), "Line cut");
            goto scroll_check;
        }

        if (ch == 21) {   /* ^U paste */
            if (!t->cut_buffer) {
                snprintf(status_msg, sizeof(status_msg), "Nothing to paste");
                continue;
            }
            lb_ensure(&t->lb, t->lb.count + 2);
            for (int i = t->lb.count; i > t->row; i--)
                t->lb.data[i] = t->lb.data[i-1];
            t->lb.data[t->row] = safe_strdup(t->cut_buffer);
            t->lb.count++;
            t->row++;
            t->col = t->sticky_col = 0;
            t->modified = 1;
            TPUSH();
            snprintf(status_msg, sizeof(status_msg), "Pasted");
            goto scroll_check;
        }

        /* ══ Word jump ═══════════════════════════════════════════════════════ */

        if (ch == KEY_SRIGHT) {
            word_jump_right(&t->lb, &t->row, &t->col);
            t->sticky_col = t->col;
            goto scroll_check;
        }
        if (ch == KEY_SLEFT) {
            word_jump_left(&t->lb, &t->row, &t->col);
            t->sticky_col = t->col;
            goto scroll_check;
        }

        /* ══ Navigation & text editing ═══════════════════════════════════════ */

        switch (ch) {

        case KEY_UP:
            if (t->row > 0) {
                t->row--;
                int llen = (int)strlen(t->lb.data[t->row]);
                t->col = t->sticky_col < llen ? t->sticky_col : llen;
            }
            break;
        case KEY_DOWN:
            if (t->row < t->lb.count - 1) {
                t->row++;
                int llen = (int)strlen(t->lb.data[t->row]);
                t->col = t->sticky_col < llen ? t->sticky_col : llen;
            }
            break;
        case KEY_LEFT:
            if (t->col > 0) { t->col--; t->sticky_col = t->col; }
            else if (t->row > 0) {
                t->row--;
                t->col = t->sticky_col = (int)strlen(t->lb.data[t->row]);
            }
            break;
        case KEY_RIGHT: {
            int llen = (int)strlen(t->lb.data[t->row]);
            if (t->col < llen) { t->col++; t->sticky_col = t->col; }
            else if (t->row < t->lb.count - 1)
                { t->row++; t->col = t->sticky_col = 0; }
            break;
        }
        case KEY_END:
            t->col = t->sticky_col = (int)strlen(t->lb.data[t->row]);
            break;
        case KEY_PPAGE:
            t->row = (t->row - (max_row-3) > 0) ? t->row - (max_row-3) : 0;
            { int llen = (int)strlen(t->lb.data[t->row]);
              if (t->col > llen) t->col = llen;
              t->sticky_col = t->col; }
            break;
        case KEY_NPAGE:
            t->row = (t->row + (max_row-3) < t->lb.count-1)
                   ? t->row + (max_row-3) : t->lb.count-1;
            { int llen = (int)strlen(t->lb.data[t->row]);
              if (t->col > llen) t->col = llen;
              t->sticky_col = t->col; }
            break;

        /* ── Backspace ── */
        case 8: case 127: case KEY_BACKSPACE:
            if (t->col > 0) {
                size_t cur_len = strlen(t->lb.data[t->row]);
                memmove(&t->lb.data[t->row][t->col-1],
                        &t->lb.data[t->row][t->col],
                        cur_len - t->col + 1);
                t->col--;
                t->modified = 1;
                TPUSH();
            } else if (t->row > 0) {
                int    prev_len = (int)strlen(t->lb.data[t->row-1]);
                size_t newlen   = prev_len + strlen(t->lb.data[t->row]) + 1;
                char  *merged   = safe_realloc(t->lb.data[t->row-1], newlen);
                t->lb.data[t->row-1] = merged;
                strcat(t->lb.data[t->row-1], t->lb.data[t->row]);
                free(t->lb.data[t->row]);
                for (int i = t->row; i < t->lb.count-1; i++)
                    t->lb.data[i] = t->lb.data[i+1];
                t->lb.count--;
                t->row--;
                t->col = prev_len;
                t->modified = 1;
                TPUSH();
            }
            t->sticky_col = t->col;
            break;

        /* ── Delete ── */
        case KEY_DC: {
            int llen = (int)strlen(t->lb.data[t->row]);
            if (t->col < llen) {
                memmove(&t->lb.data[t->row][t->col],
                        &t->lb.data[t->row][t->col+1],
                        llen - t->col);
                t->modified = 1;
                TPUSH();
            } else if (t->row < t->lb.count-1) {
                size_t newlen = llen + strlen(t->lb.data[t->row+1]) + 1;
                char  *merged = safe_realloc(t->lb.data[t->row], newlen);
                t->lb.data[t->row] = merged;
                strcat(t->lb.data[t->row], t->lb.data[t->row+1]);
                free(t->lb.data[t->row+1]);
                for (int i = t->row+1; i < t->lb.count-1; i++)
                    t->lb.data[i] = t->lb.data[i+1];
                t->lb.count--;
                t->modified = 1;
                TPUSH();
            }
            t->sticky_col = t->col;
            break;
        }

        /* ── Enter with auto-indent ── */
        case '\n': case '\r': case KEY_ENTER: {
            const char *cur = t->lb.data[t->row];
            int indent  = 0;
            int cur_len = (int)strlen(cur);
            while (indent < t->col && indent < cur_len &&
                   (cur[indent] == ' ' || cur[indent] == '\t'))
                indent++;
            char *tail     = safe_strdup(cur + t->col);
            char *new_next = safe_malloc(indent + strlen(tail) + 1);
            memcpy(new_next, cur, indent);
            strcpy(new_next + indent, tail);
            free(tail);
            t->lb.data[t->row][t->col] = '\0';
            lb_ensure(&t->lb, t->lb.count + 2);
            for (int i = t->lb.count; i > t->row + 1; i--)
                t->lb.data[i] = t->lb.data[i-1];
            t->lb.data[t->row+1] = new_next;
            t->lb.count++;
            t->row++;
            t->col = t->sticky_col = indent;
            t->modified = 1;
            TPUSH();
            break;
        }

        /* ── Tab ── */
        case '\t': {
            int   llen = (int)strlen(t->lb.data[t->row]);
            char *nl   = safe_malloc(llen + 2);
            if (t->col > 0) memcpy(nl, t->lb.data[t->row], t->col);
            nl[t->col] = '\t';
            memcpy(nl + t->col + 1, t->lb.data[t->row] + t->col,
                   llen - t->col + 1);
            free(t->lb.data[t->row]);
            t->lb.data[t->row] = nl;
            t->col++; t->sticky_col = t->col;
            t->modified = 1;
            TPUSH();
            break;
        }

        /* ── Printable character ── */
        default:
            if (ch >= 32 && ch <= 126) {
                int   llen = (int)strlen(t->lb.data[t->row]);
                char *nl   = safe_malloc(llen + 2);
                if (t->col > 0) memcpy(nl, t->lb.data[t->row], t->col);
                nl[t->col] = (char)ch;
                memcpy(nl + t->col + 1, t->lb.data[t->row] + t->col,
                       llen - t->col + 1);
                free(t->lb.data[t->row]);
                t->lb.data[t->row] = nl;
                t->col++; t->sticky_col = t->col;
                t->modified = 1;
                TPUSH();
            }
            break;
        }

scroll_check:
        /* Vertical scroll: keep cursor inside the content area. */
        {
            int visible_rows = content_bot - content_top + 1;
            if (t->row < t->scroll_row)
                t->scroll_row = t->row;
            else if (t->row - t->scroll_row >= visible_rows - 1)
                t->scroll_row = t->row - (visible_rows - 2);
            if (t->scroll_row < 0) t->scroll_row = 0;
        }

        /* Horizontal scroll: keep cursor visible within pane width. */
        {
            int dcol   = byte_to_display_col(t->lb.data[t->row], t->col);
            int sc     = LINE_NUM_W + dcol;
            int pane_w = (split_on && tab_count > 1)
                       ? ((split_focus == 1) ? (max_col - split_col - 1) : split_col)
                       : max_col;
            if (sc - t->scroll_col < LINE_NUM_W) {
                t->scroll_col = sc - LINE_NUM_W;
                if (t->scroll_col < 0) t->scroll_col = 0;
            }
            if (sc - t->scroll_col >= pane_w - 1) {
                t->scroll_col = sc - (pane_w - 2);
                if (t->scroll_col < 0) t->scroll_col = 0;
            }
        }
    }

    delwin(pad_left);
    delwin(pad_right);
    endwin();
    for (int i = 0; i < tab_count; i++)
        tab_free(&tabs[i]);
}

/* ─── Main ───────────────────────────────────────────────────────────────── */

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
        char *args[] = {"/bin/bash", "update.sh", NULL};
        execv("/bin/bash", args);
        perror("execv update.sh");
        return 1;
    }

    runEditor(argv[1]);
    return 0;
}
