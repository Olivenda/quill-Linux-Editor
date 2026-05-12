#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#define MAX_LINES    10000
#define MAX_LINE_LEN 4096
#define UNDO_LIMIT   256
#define LINE_NUM_W   6    /* "NNNNN " */
#define TAB_WIDTH    4

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
/*
 * Every edit calls undo_push AFTER modifying lb.  The stack stores snapshots
 * of lb in order, with pos pointing one past the "current" snapshot.  The
 * initial sentinel push (before any edit) lets undo_do stop at pos==1.
 *
 *   undo_do: load entry[pos-2] after pos--  (go back one step)
 *   redo_do: load entry[pos]   then pos++   (go forward one step)
 */

typedef struct {
    LineBuf snap;
    int     row, col;
} UndoEntry;

typedef struct {
    UndoEntry entries[UNDO_LIMIT];
    int       head;  /* index of next write slot */
    int       size;  /* number of valid entries  */
    int       pos;   /* logical position (1..size); pos==size → newest */
} UndoStack;

static int undo_idx(const UndoStack *us, int logical) {
    int base = (us->head - us->size + UNDO_LIMIT * 4) % UNDO_LIMIT;
    return (base + logical) % UNDO_LIMIT;
}

static void undo_push(UndoStack *us, const LineBuf *lb, int row, int col) {
    /* drop redo entries above pos */
    for (int i = us->pos; i < us->size; i++)
        lb_free(&us->entries[undo_idx(us, i)].snap);
    us->size = us->pos;

    int idx = us->head;
    if (us->size == UNDO_LIMIT)
        lb_free(&us->entries[idx].snap);   /* overwrite oldest */
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

    /* Only ask to overwrite when writing to a path we did NOT load from. */
    if (!loaded_from_disk && fileExists(filename)) {
        if (!confirmOverwrite(filename, max_col)) return 1;
    }

    size_t total = 0;
    for (int i = 0; i < lb->count; i++)
        total += strlen(lb->data[i]) + 1;   /* +1 for '\n' */

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
            /* comment spans to next line */
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

/* ─── Status bar ─────────────────────────────────────────────────────────── */

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

static void drawStatusBar(const char *filename, int modified,
                          int row, int col, int total_lines, int max_col,
                          const char *msg)
{
    attron(A_REVERSE);
    mvhline(LINES-1, 0, ' ', max_col);

    char left[512];
    snprintf(left, sizeof(left), " %s [%s] %s",
             filename, file_type_label(filename),
             modified ? "[Modified]" : "[Saved]");
    mvprintw(LINES-1, 0, "%s", left);

    const char *keys =
        "^S Save  ^Z Undo  ^Y Redo  ^F Find  ^G Goto  ^K Cut  ^U Paste  ^X Exit";
    int kpos = (max_col - (int)strlen(keys)) / 2;
    if (kpos > (int)strlen(left) + 1)
        mvprintw(LINES-1, kpos, "%s", keys);

    char right[80];
    snprintf(right, sizeof(right), "Ln %d/%d  Col %d ", row+1, total_lines, col+1);
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

/* ─── Display column helper ──────────────────────────────────────────────── */

/* Returns visual screen column for byte offset into line (tabs expanded). */
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

/* ─── Editor main loop ───────────────────────────────────────────────────── */

void nanoEditor(const char *filename) {
    int loaded_from_disk = 0;
    LineBuf lb = lb_load(filename, &loaded_from_disk);
    lb_ensure(&lb, lb.count + 512);

    int row = 0, col = 0;
    int scroll_row = 0, scroll_col = 0;
    int sticky_col = 0;
    int modified   = 0;
    char status_msg[256] = "";
    char *cut_buffer = NULL;

    /* Sentinel push: initial state before any edit. */
    UndoStack us = {0};
    undo_push(&us, &lb, 0, 0);

    initscr();
    start_color();
    use_default_colors();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    set_escdelay(25);

    init_pair(1, COLOR_CYAN,    -1);   /* line numbers */
    init_pair(2, COLOR_WHITE,   -1);   /* default text */
    init_pair(3, COLOR_YELLOW,  -1);   /* numbers / constants */
    init_pair(4, COLOR_GREEN,   -1);   /* preprocessor */
    init_pair(5, COLOR_MAGENTA, -1);   /* keywords / braces */
    init_pair(6, COLOR_CYAN,    -1);   /* stdlib / parens */
    init_pair(7, COLOR_RED,     -1);   /* strings / chars */
    init_pair(8, COLOR_BLUE,    -1);   /* types */
    init_pair(9, COLOR_GREEN,   -1);   /* comments */

    /* Map common Ctrl+arrow escape sequences to shift-arrow keycodes. */
    define_key("\033[1;5C", KEY_SRIGHT);
    define_key("\033[1;5D", KEY_SLEFT);

    int max_row, max_col;
    getmaxyx(stdscr, max_row, max_col);
    WINDOW *pad = newpad(MAX_LINES, MAX_LINE_LEN + LINE_NUM_W + 16);

    int last_home_row = -1, last_home_col_nws = -1;

#define PUSH_UNDO() undo_push(&us, &lb, row, col)

    while (1) {
        getmaxyx(stdscr, max_row, max_col);
        werase(pad);

        /* Pre-scan multiline-comment state for lines above the viewport. */
        int in_ml = 0;
        int prescan = scroll_row < lb.count ? scroll_row : lb.count;
        for (int i = 0; i < prescan; i++) {
            const char *l = lb.data[i];
            int len = (int)strlen(l);
            for (int j = 0; j < len; j++) {
                if (!in_ml && l[j]=='/' && j+1<len && l[j+1]=='*')
                    { in_ml=1; j++; }
                else if (in_ml && l[j]=='*' && j+1<len && l[j+1]=='/')
                    { in_ml=0; j++; }
            }
        }

        /* Draw visible lines into pad (guard against MAX_LINES overflow). */
        int draw_limit = lb.count < MAX_LINES ? lb.count : MAX_LINES;
        int visible    = max_row - 1;
        for (int i = scroll_row;
             i < draw_limit && i - scroll_row < visible; i++)
        {
            wattron(pad, COLOR_PAIR(1));
            mvwprintw(pad, i - scroll_row, 0, "%5d ", i + 1);
            wattroff(pad, COLOR_PAIR(1));
            in_ml = printHighlightedLine(pad, i - scroll_row,
                                         lb.data[i], in_ml);
        }

        prefresh(pad, 0, scroll_col, 0, 0, max_row - 2, max_col - 1);
        drawStatusBar(filename, modified, row, col, lb.count, max_col,
                      status_msg);

        /* Position cursor (convert byte offset to visual column). */
        {
            int dcol      = byte_to_display_col(lb.data[row], col);
            int screen_c  = (LINE_NUM_W + dcol) - scroll_col;
            int screen_r  = row - scroll_row;
            if (screen_c < 0) screen_c = 0;
            if (screen_c >= max_col) screen_c = max_col - 1;
            if (screen_r < 0) screen_r = 0;
            if (screen_r > max_row - 2) screen_r = max_row - 2;
            move(screen_r, screen_c);
        }
        refresh();

        int ch = getch();
        status_msg[0] = '\0';

        /* Reset smart-Home state on any key other than Home / ^A. */
        if (ch != 1 && ch != KEY_HOME) {
            last_home_row     = -1;
            last_home_col_nws = -1;
        }

        /* ── Exit ^X ── */
        if (ch == 24) {
            if (modified) {
                mvhline(LINES-2, 0, ' ', max_col);
                mvprintw(LINES-2, 0, "Save changes? (y/n/ESC=cancel): ");
                refresh();
                int sc = getch();
                if (sc == 27) continue;   /* ESC: cancel exit */
                if (sc == 'y' || sc == 'Y') {
                    char err[256] = "";
                    int rc = saveFile(filename, &lb, loaded_from_disk,
                                      err, sizeof(err));
                    if (rc == -1) {
                        snprintf(status_msg, sizeof(status_msg), "%s", err);
                        continue;
                    } else if (rc == 1) {
                        continue;   /* user cancelled overwrite */
                    }
                }
            }
            break;
        }

        /* ── Save ^S ── */
        if (ch == 19) {
            char err[256] = "";
            int rc = saveFile(filename, &lb, loaded_from_disk,
                              err, sizeof(err));
            if (rc == 0) {
                modified = 0;
                loaded_from_disk = 1;
                snprintf(status_msg, sizeof(status_msg),
                         "Saved %d line%s", lb.count,
                         lb.count == 1 ? "" : "s");
            } else if (rc == -1) {
                snprintf(status_msg, sizeof(status_msg), "%s", err);
            } else {
                snprintf(status_msg, sizeof(status_msg), "Save cancelled");
            }
            continue;
        }

        /* ── Quit ^Q (no save prompt) ── */
        if (ch == 17) break;

        /* ── Undo ^Z ── */
        if (ch == 26) {
            if (undo_do(&us, &lb, &row, &col))
                snprintf(status_msg, sizeof(status_msg), "Undo");
            else
                snprintf(status_msg, sizeof(status_msg), "Nothing to undo");
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
            if (!query[0]) {
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
                continue;
            }
            int found = 0;
            for (int pass = 0; pass < 2 && !found; pass++) {
                int s = (pass == 0) ? row       : 0;
                int e = (pass == 0) ? lb.count  : row + 1;
                for (int i = s; i < e && !found; i++) {
                    int llen      = (int)strlen(lb.data[i]);
                    int start_col = (i == row && pass == 0) ? col+1 : 0;
                    if (start_col > llen) continue;
                    char *p = strstr(lb.data[i] + start_col, query);
                    if (p) {
                        row = i;
                        col = (int)(p - lb.data[i]);
                        sticky_col = col;
                        snprintf(status_msg, sizeof(status_msg),
                                 "Found: Ln %d Col %d", row+1, col+1);
                        found = 1;
                    }
                }
            }
            if (!found)
                snprintf(status_msg, sizeof(status_msg),
                         "Not found: %s", query);
            goto scroll_check;
        }

        /* ── Goto line ^G ── */
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
            snprintf(status_msg, sizeof(status_msg),
                     "Jumped to line %d", target);
            goto scroll_check;
        }

        /* ── Smart Home  ^A or KEY_HOME ── */
        if (ch == 1 || ch == KEY_HOME) {
            const char *l    = lb.data[row];
            int   llen       = (int)strlen(l);
            int   first_nws  = 0;
            while (first_nws < llen &&
                   (l[first_nws] == ' ' || l[first_nws] == '\t'))
                first_nws++;
            if (first_nws == llen) first_nws = 0;
            int target;
            if (last_home_row == row &&
                last_home_col_nws == first_nws &&
                col == first_nws)
                target = 0;
            else
                target = (col == first_nws) ? 0 : first_nws;
            last_home_row     = row;
            last_home_col_nws = first_nws;
            col = sticky_col = target;
            goto scroll_check;
        }

        /* ── Kill line ^K ── */
        if (ch == 11) {
            free(cut_buffer);
            int llen = (int)strlen(lb.data[row]);
            if (llen == 0 && lb.count > 1) {
                /* Remove the empty line. */
                cut_buffer = safe_strdup("");
                free(lb.data[row]);
                for (int i = row; i < lb.count - 1; i++)
                    lb.data[i] = lb.data[i+1];
                lb.count--;
                if (row >= lb.count) row = lb.count - 1;
            } else if (llen == 0) {
                cut_buffer = safe_strdup("");
            } else {
                cut_buffer = safe_strdup(lb.data[row]);
                if (lb.count > 1) {
                    free(lb.data[row]);
                    for (int i = row; i < lb.count - 1; i++)
                        lb.data[i] = lb.data[i+1];
                    lb.count--;
                    if (row >= lb.count) row = lb.count - 1;
                } else {
                    lb.data[row][0] = '\0';
                }
            }
            col = sticky_col = 0;
            modified = 1;
            PUSH_UNDO();
            snprintf(status_msg, sizeof(status_msg), "Line cut");
            goto scroll_check;
        }

        /* ── Paste ^U ── */
        if (ch == 21) {
            if (!cut_buffer) {
                snprintf(status_msg, sizeof(status_msg), "Nothing to paste");
                continue;
            }
            lb_ensure(&lb, lb.count + 2);
            for (int i = lb.count; i > row; i--)
                lb.data[i] = lb.data[i-1];
            lb.data[row] = safe_strdup(cut_buffer);
            lb.count++;
            row++;
            col = sticky_col = 0;
            modified = 1;
            PUSH_UNDO();
            snprintf(status_msg, sizeof(status_msg), "Pasted");
            goto scroll_check;
        }

        /* ── Word jump Ctrl+Right / Ctrl+Left ── */
        if (ch == KEY_SRIGHT) {
            word_jump_right(&lb, &row, &col);
            sticky_col = col;
            goto scroll_check;
        }
        if (ch == KEY_SLEFT) {
            word_jump_left(&lb, &row, &col);
            sticky_col = col;
            goto scroll_check;
        }

        /* ── Navigation & editing ── */
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
            else if (row < lb.count - 1) { row++; col = sticky_col = 0; }
            break;
        }
        case KEY_END:
            col = sticky_col = (int)strlen(lb.data[row]);
            break;
        case KEY_PPAGE:
            row = (row - (max_row-2) > 0) ? row - (max_row-2) : 0;
            { int llen=(int)strlen(lb.data[row]);
              if (col>llen) col=llen; sticky_col=col; }
            break;
        case KEY_NPAGE:
            row = (row + (max_row-2) < lb.count-1) ?
                   row + (max_row-2) : lb.count-1;
            { int llen=(int)strlen(lb.data[row]);
              if (col>llen) col=llen; sticky_col=col; }
            break;

        /* ── Backspace ── */
        case 8: case 127: case KEY_BACKSPACE:
            if (col > 0) {
                size_t cur_len = strlen(lb.data[row]);
                memmove(&lb.data[row][col-1], &lb.data[row][col],
                        cur_len - col + 1);
                col--;
                modified = 1;
                PUSH_UNDO();
            } else if (row > 0) {
                int    prev_len = (int)strlen(lb.data[row-1]);
                size_t newlen   = prev_len + strlen(lb.data[row]) + 1;
                char  *merged   = safe_realloc(lb.data[row-1], newlen);
                lb.data[row-1]  = merged;
                strcat(lb.data[row-1], lb.data[row]);
                free(lb.data[row]);
                for (int i = row; i < lb.count-1; i++)
                    lb.data[i] = lb.data[i+1];
                lb.count--;
                row--;
                col = prev_len;
                modified = 1;
                PUSH_UNDO();
            }
            sticky_col = col;
            break;

        /* ── Delete key ── */
        case KEY_DC: {
            int llen = (int)strlen(lb.data[row]);
            if (col < llen) {
                memmove(&lb.data[row][col], &lb.data[row][col+1], llen-col);
                modified = 1;
                PUSH_UNDO();
            } else if (row < lb.count-1) {
                size_t newlen = llen + strlen(lb.data[row+1]) + 1;
                char  *merged = safe_realloc(lb.data[row], newlen);
                lb.data[row]  = merged;
                strcat(lb.data[row], lb.data[row+1]);
                free(lb.data[row+1]);
                for (int i = row+1; i < lb.count-1; i++)
                    lb.data[i] = lb.data[i+1];
                lb.count--;
                modified = 1;
                PUSH_UNDO();
            }
            sticky_col = col;
            break;
        }

        /* ── Enter (with auto-indent) ── */
        case '\n': case '\r': case KEY_ENTER: {
            /* Copy leading whitespace from current line (up to col). */
            const char *cur  = lb.data[row];
            int   indent     = 0;
            int   cur_len    = (int)strlen(cur);
            while (indent < col && indent < cur_len &&
                   (cur[indent] == ' ' || cur[indent] == '\t'))
                indent++;

            char *tail     = safe_strdup(cur + col);
            char *new_next = safe_malloc(indent + strlen(tail) + 1);
            memcpy(new_next, cur, indent);
            strcpy(new_next + indent, tail);
            free(tail);

            lb.data[row][col] = '\0';
            lb_ensure(&lb, lb.count + 2);
            for (int i = lb.count; i > row + 1; i--)
                lb.data[i] = lb.data[i-1];
            lb.data[row+1] = new_next;
            lb.count++;
            row++;
            col = sticky_col = indent;
            modified = 1;
            PUSH_UNDO();
            break;
        }

        /* ── Tab ── */
        case '\t': {
            int   llen = (int)strlen(lb.data[row]);
            char *nl   = safe_malloc(llen + 2);
            if (col > 0) memcpy(nl, lb.data[row], col);
            nl[col] = '\t';
            memcpy(nl + col + 1, lb.data[row] + col, llen - col + 1);
            free(lb.data[row]);
            lb.data[row] = nl;
            col++; sticky_col = col;
            modified = 1;
            PUSH_UNDO();
            break;
        }

        /* ── Printable character ── */
        default:
            if (ch >= 32 && ch <= 126) {
                int   llen = (int)strlen(lb.data[row]);
                char *nl   = safe_malloc(llen + 2);
                if (col > 0) memcpy(nl, lb.data[row], col);
                nl[col] = (char)ch;
                memcpy(nl + col + 1, lb.data[row] + col, llen - col + 1);
                free(lb.data[row]);
                lb.data[row] = nl;
                col++; sticky_col = col;
                modified = 1;
                PUSH_UNDO();
            }
            break;
        }

scroll_check:
        /* Vertical scroll */
        if (row < scroll_row) scroll_row = row;
        else if (row - scroll_row >= max_row - 1)
            scroll_row = row - (max_row - 2);
        if (scroll_row < 0) scroll_row = 0;

        /* Horizontal scroll (display columns, accounts for tabs). */
        {
            int dcol   = byte_to_display_col(lb.data[row], col);
            int sc     = LINE_NUM_W + dcol;
            if (sc - scroll_col < LINE_NUM_W) {
                scroll_col = sc - LINE_NUM_W;
                if (scroll_col < 0) scroll_col = 0;
            }
            if (sc - scroll_col >= max_col - 1) {
                scroll_col = sc - (max_col - 2);
                if (scroll_col < 0) scroll_col = 0;
            }
        }
    }

    delwin(pad);
    endwin();
    free(cut_buffer);
    undo_free(&us);
    lb_free(&lb);
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

    nanoEditor(argv[1]);
    return 0;
}
