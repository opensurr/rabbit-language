#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <setjmp.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#include <sys/wait.h>

#define RABBIT_VERSION "1.0.0"

#if defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

static const char *g_file = "<input>";

typedef struct PcallFrame {
    jmp_buf jb;
    struct PcallFrame *prev;
    int depth;
} PcallFrame;

static PcallFrame *g_pcall_top = NULL;
static int g_call_depth = 0;
#define MAX_CALL_DEPTH 2000
#define MAX_NEST_DEPTH 300
static int g_nest_depth = 0;
static char *g_pcall_err = NULL;

static int safe_int(double d) {
    if (d != d) return 0;
    if (d > 1000000000.0) return 1000000000;
    if (d < -1000000000.0) return -1000000000;
    return (int)d;
}

static NORETURN void fail(int line, const char *fmt, ...) {
    char msg[1024];
    char full[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(full, sizeof full, "rabbit: %s:%d: error: %s", g_file, line, msg);
    if (g_pcall_top) {
        g_pcall_err = malloc(strlen(full) + 1);
        strcpy(g_pcall_err, full);
        longjmp(g_pcall_top->jb, 1);
    }
    fprintf(stderr, "%s\n", full);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "rabbit: out of memory\n");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *old, size_t n) {
    void *p = realloc(old, n);
    if (!p) {
        fprintf(stderr, "rabbit: out of memory\n");
        exit(1);
    }
    return p;
}

static char *g_arena = NULL;
static size_t g_arena_left = 0;

static void *arena_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n > g_arena_left) {
        size_t chunk = n > 65536 ? n : 65536;
        g_arena = xmalloc(chunk);
        g_arena_left = chunk;
    }
    void *p = g_arena;
    g_arena += n;
    g_arena_left -= n;
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

typedef struct { char *s; size_t len, cap; } SB;

static void sb_reserve(SB *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < b->len + extra + 1) nc *= 2;
    b->s = xrealloc(b->s, nc);
    b->cap = nc;
}

static void sb_add(SB *b, const char *s) {
    size_t n = strlen(s);
    sb_reserve(b, n);
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

static void sb_addc(SB *b, char c) {
    char t[2] = {c, '\0'};
    sb_add(b, t);
}

static char *sb_take(SB *b) {
    if (!b->s) return xstrdup("");
    return b->s;
}

static const char *intern(const char *s);

typedef enum { T_EOF, T_NUMBER, T_STRING, T_IDENT, T_KEYWORD, T_OP } TokType;

typedef struct {
    TokType type;
    char *text;
    double num;
    int line;
} Token;

static const char *KEYWORDS[] = {
    "let", "func", "if", "elseif", "else", "while", "repeat", "until",
    "for", "in", "do", "return", "break", "continue", "and", "or", "not",
    "true", "false", "nil", NULL
};

static const char *TWO_CHAR_OPS[] = {
    "==", "~=", "!=", "<=", ">=", "..", "+=", "-=", "*=", "/=", "??", NULL
};

static int is_keyword(const char *s) {
    for (int i = 0; KEYWORDS[i]; i++)
        if (strcmp(KEYWORDS[i], s) == 0) return 1;
    return 0;
}

static Token *lex(const char *src, int *count) {
    size_t cap = 128, n = 0;
    Token *toks = xmalloc(cap * sizeof(Token));
    const char *p = src;
    int line = 1;

    while (*p) {
        char c = *p;
        Token t;
        memset(&t, 0, sizeof t);
        t.line = line;

        if (c == '\n') { line++; p++; continue; }
        if (isspace((unsigned char)c)) { p++; continue; }
        if (c == ';') { p++; continue; }
        if (c == '-' && p[1] == '-' && p[2] == '[' && p[3] == '[') {
            p += 4;
            while (*p && !(p[0] == ']' && p[1] == ']')) {
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p += 2;
            continue;
        }
        if (c == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (isdigit((unsigned char)c)) {
            char *end;
            t.type = T_NUMBER;
            t.num = strtod(p, &end);
            p = end;
        } else if (c == '"') {
            size_t len = 0, bufcap = 32;
            char *buf = xmalloc(bufcap);
            p++;
            while (*p && *p != '"') {
                char ch = *p++;
                if (ch == '\n') line++;
                if (ch == '\\' && *p) {
                    char e = *p++;
                    ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : e;
                }
                if (len + 1 >= bufcap) {
                    bufcap *= 2;
                    buf = xrealloc(buf, bufcap);
                }
                buf[len++] = ch;
            }
            if (*p != '"') fail(t.line, "text is missing its closing quote");
            p++;
            buf[len] = '\0';
            t.type = T_STRING;
            t.text = buf;
        } else if (isalpha((unsigned char)c) || c == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t len = (size_t)(p - start);
            char *word = xmalloc(len + 1);
            memcpy(word, start, len);
            word[len] = '\0';
            t.type = is_keyword(word) ? T_KEYWORD : T_IDENT;
            t.text = (t.type == T_IDENT) ? (char *)intern(word) : word;
        } else {
            int len = 1;
            for (int i = 0; TWO_CHAR_OPS[i]; i++) {
                if (p[0] == TWO_CHAR_OPS[i][0] && p[1] == TWO_CHAR_OPS[i][1]) {
                    len = 2;
                    break;
                }
            }
            if (len == 1 && !strchr("+-*/%(){}[],=<>#.:", c))
                fail(line, "unexpected character '%c'", c);
            t.type = T_OP;
            t.text = xmalloc(3);
            memcpy(t.text, p, (size_t)len);
            p += len;
        }

        if (n + 1 >= cap) {
            cap *= 2;
            toks = xrealloc(toks, cap * sizeof(Token));
        }
        toks[n++] = t;
    }

    if (n + 1 >= cap) {
        cap *= 2;
        toks = xrealloc(toks, cap * sizeof(Token));
    }
    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.type = T_EOF;
    eof.line = line;
    toks[n++] = eof;
    *count = (int)n;
    return toks;
}

typedef enum {
    N_NUM, N_STR, N_BOOL, N_NIL, N_VAR, N_FUNC, N_ARRAY, N_TABLE, N_INDEX,
    N_UNARY, N_BINARY, N_CALL, N_LET, N_ASSIGN, N_IF, N_WHILE, N_REPEAT,
    N_FOR, N_FORIN, N_DO, N_RETURN, N_BREAK, N_CONTINUE, N_BLOCK
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    int line;
    double num;
    char *name;
    char *name2;
    Node *a, *b, *c, *d;
    Node **list;
    int count;
    char **params;
    int nparams;
    int reuse_scope;
};

static Node *new_node(NodeKind k, int line) {
    Node *n = xmalloc(sizeof(Node));
    n->kind = k;
    n->line = line;
    return n;
}

static void push_node(Node *n, Node *child) {
    n->list = xrealloc(n->list, sizeof(Node *) * (size_t)(n->count + 1));
    n->list[n->count++] = child;
}

static void push_param(Node *n, char *name) {
    n->params = xrealloc(n->params, sizeof(char *) * (size_t)(n->nparams + 1));
    n->params[n->nparams++] = name;
}

typedef struct {
    Token *toks;
    int count;
    int pos;
} Parser;

static const char *tok_desc(const Token *t) {
    static char buf[256];
    switch (t->type) {
    case T_EOF:
        return "end of file";
    case T_NUMBER:
        snprintf(buf, sizeof buf, "number %g", t->num);
        return buf;
    case T_STRING:
        snprintf(buf, sizeof buf, "text \"%s\"", t->text);
        return buf;
    case T_IDENT:
        snprintf(buf, sizeof buf, "name '%s'", t->text);
        return buf;
    default:
        snprintf(buf, sizeof buf, "'%s'", t->text);
        return buf;
    }
}

static Token *peek(Parser *ps) { return &ps->toks[ps->pos]; }

static Token *peek2(Parser *ps) {
    int i = ps->pos + 1;
    return &ps->toks[i < ps->count ? i : ps->count - 1];
}

static Token *advance(Parser *ps) {
    Token *t = &ps->toks[ps->pos];
    if (ps->pos < ps->count - 1) ps->pos++;
    return t;
}

static int check_op(Parser *ps, const char *op) {
    Token *t = peek(ps);
    return t->type == T_OP && strcmp(t->text, op) == 0;
}

static int check_kw(Parser *ps, const char *kw) {
    Token *t = peek(ps);
    return t->type == T_KEYWORD && strcmp(t->text, kw) == 0;
}

static void expect_op(Parser *ps, const char *op) {
    if (!check_op(ps, op))
        fail(peek(ps)->line, "expected '%s' but found %s", op, tok_desc(peek(ps)));
    advance(ps);
}

static void expect_kw(Parser *ps, const char *kw) {
    if (!check_kw(ps, kw))
        fail(peek(ps)->line, "expected '%s' but found %s", kw, tok_desc(peek(ps)));
    advance(ps);
}

static char *expect_ident(Parser *ps) {
    Token *t = peek(ps);
    if (t->type != T_IDENT)
        fail(t->line, "expected a name but found %s", tok_desc(t));
    advance(ps);
    return t->text;
}

static Node *parse_expr(Parser *ps);
static Node *parse_statement(Parser *ps);
static Node *parse_block(Parser *ps);

static Node *make_binary(Node *l, const char *op, Node *r, int line) {
    Node *n = new_node(N_BINARY, line);
    n->name = (char *)op;
    n->a = l;
    n->b = r;
    return n;
}

static Node *make_string(const char *text, int line) {
    Node *n = new_node(N_STR, line);
    n->name = (char *)text;
    return n;
}

static int g_closure_count = 0;

static Node *parse_function(Parser *ps, int named) {
    g_closure_count++;
    Token *kw = advance(ps);
    Node *n = new_node(N_FUNC, kw->line);
    n->name = named ? expect_ident(ps) : (char *)"anonymous";
    expect_op(ps, "(");
    if (!check_op(ps, ")")) {
        push_param(n, expect_ident(ps));
        while (check_op(ps, ",")) {
            advance(ps);
            push_param(n, expect_ident(ps));
        }
    }
    expect_op(ps, ")");
    n->b = parse_block(ps);
    return n;
}

static Node *parse_array(Parser *ps) {
    Token *open = advance(ps);
    Node *n = new_node(N_ARRAY, open->line);
    while (!check_op(ps, "]")) {
        push_node(n, parse_expr(ps));
        if (check_op(ps, ",")) advance(ps);
        else if (!check_op(ps, "]"))
            fail(peek(ps)->line, "expected ',' or ']' but found %s", tok_desc(peek(ps)));
    }
    advance(ps);
    return n;
}

static Node *parse_table(Parser *ps) {
    Token *open = advance(ps);
    Node *n = new_node(N_TABLE, open->line);
    while (!check_op(ps, "}")) {
        Token *t = peek(ps);
        Node *key;
        int is_name_key = (t->type == T_IDENT || t->type == T_KEYWORD ||
                           t->type == T_STRING) &&
                          (strcmp(peek2(ps)->text, ":") == 0 ||
                           strcmp(peek2(ps)->text, "=") == 0);
        if (is_name_key) {
            advance(ps);
            key = make_string(t->text, t->line);
        } else if (check_op(ps, "[")) {
            advance(ps);
            key = parse_expr(ps);
            expect_op(ps, "]");
        } else {
            fail(t->line, "expected 'name: value' inside { } but found %s", tok_desc(t));
        }
        if (check_op(ps, ":") || check_op(ps, "=")) advance(ps);
        else fail(peek(ps)->line, "expected ':' after the key but found %s", tok_desc(peek(ps)));
        push_node(n, key);
        push_node(n, parse_expr(ps));
        if (check_op(ps, ",")) advance(ps);
        else if (!check_op(ps, "}"))
            fail(peek(ps)->line, "expected ',' or '}' but found %s", tok_desc(peek(ps)));
    }
    advance(ps);
    return n;
}

static Node *parse_primary(Parser *ps) {
    Token *t = peek(ps);
    int line = t->line;

    if (t->type == T_NUMBER) {
        advance(ps);
        Node *n = new_node(N_NUM, line);
        n->num = t->num;
        return n;
    }
    if (t->type == T_STRING) {
        advance(ps);
        return make_string(t->text, line);
    }
    if (check_kw(ps, "true") || check_kw(ps, "false")) {
        Node *n = new_node(N_BOOL, line);
        n->num = check_kw(ps, "true") ? 1 : 0;
        advance(ps);
        return n;
    }
    if (check_kw(ps, "nil")) {
        advance(ps);
        return new_node(N_NIL, line);
    }
    if (check_kw(ps, "func")) return parse_function(ps, 0);
    if (check_op(ps, "[")) return parse_array(ps);
    if (check_op(ps, "{")) return parse_table(ps);
    if (t->type == T_IDENT) {
        advance(ps);
        Node *n = new_node(N_VAR, line);
        n->name = t->text;
        return n;
    }
    if (check_op(ps, "(")) {
        advance(ps);
        Node *e = parse_expr(ps);
        expect_op(ps, ")");
        return e;
    }
    fail(line, "expected a value but found %s", tok_desc(t));
}

static Node *parse_postfix(Parser *ps) {
    Node *e = parse_primary(ps);
    for (;;) {
        if (check_op(ps, "(")) {
            Token *t = advance(ps);
            Node *c = new_node(N_CALL, t->line);
            c->a = e;
            if (!check_op(ps, ")")) {
                push_node(c, parse_expr(ps));
                while (check_op(ps, ",")) {
                    advance(ps);
                    push_node(c, parse_expr(ps));
                }
            }
            expect_op(ps, ")");
            e = c;
        } else if (check_op(ps, "[")) {
            Token *t = advance(ps);
            Node *ix = new_node(N_INDEX, t->line);
            ix->a = e;
            ix->b = parse_expr(ps);
            expect_op(ps, "]");
            e = ix;
        } else if (check_op(ps, ":")) {
            Token *colon = advance(ps);
            Token *name = peek(ps);
            if (name->type != T_IDENT)
                fail(name->line, "expected a method name after ':' but found %s", tok_desc(name));
            advance(ps);
            expect_op(ps, "(");
            Node *c = new_node(N_CALL, colon->line);
            Node *ix = new_node(N_INDEX, colon->line);
            ix->a = e;
            ix->b = make_string(name->text, name->line);
            c->a = ix;
            if (!check_op(ps, ")")) {
                push_node(c, parse_expr(ps));
                while (check_op(ps, ",")) {
                    advance(ps);
                    push_node(c, parse_expr(ps));
                }
            }
            expect_op(ps, ")");
            e = c;
        } else if (check_op(ps, ".")) {
            Token *dot = advance(ps);
            Token *name = peek(ps);
            if (name->type != T_IDENT && name->type != T_KEYWORD)
                fail(name->line, "expected a field name after '.' but found %s", tok_desc(name));
            advance(ps);
            Node *ix = new_node(N_INDEX, dot->line);
            ix->a = e;
            ix->b = make_string(name->text, name->line);
            e = ix;
        } else {
            break;
        }
    }
    return e;
}

static Node *parse_unary(Parser *ps) {
    if (check_op(ps, "-") || check_op(ps, "#")) {
        Token *t = advance(ps);
        Node *n = new_node(N_UNARY, t->line);
        n->name = t->text;
        n->a = parse_unary(ps);
        return n;
    }
    return parse_postfix(ps);
}

static Node *parse_mul(Parser *ps) {
    Node *l = parse_unary(ps);
    while (check_op(ps, "*") || check_op(ps, "/") || check_op(ps, "%")) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_unary(ps), op->line);
    }
    return l;
}

static Node *parse_add(Parser *ps) {
    Node *l = parse_mul(ps);
    while (check_op(ps, "+") || check_op(ps, "-")) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_mul(ps), op->line);
    }
    return l;
}

static Node *parse_concat(Parser *ps) {
    Node *l = parse_add(ps);
    if (check_op(ps, "..")) {
        Token *op = advance(ps);
        return make_binary(l, op->text, parse_concat(ps), op->line);
    }
    return l;
}

static int is_compare_op(Token *t) {
    if (t->type != T_OP) return 0;
    return strcmp(t->text, "==") == 0 || strcmp(t->text, "~=") == 0 ||
           strcmp(t->text, "!=") == 0 || strcmp(t->text, "<") == 0 ||
           strcmp(t->text, "<=") == 0 || strcmp(t->text, ">") == 0 ||
           strcmp(t->text, ">=") == 0;
}

static Node *parse_compare(Parser *ps) {
    Node *l = parse_concat(ps);
    while (is_compare_op(peek(ps))) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_concat(ps), op->line);
    }
    return l;
}

static Node *parse_not(Parser *ps) {
    if (check_kw(ps, "not")) {
        Token *t = advance(ps);
        Node *n = new_node(N_UNARY, t->line);
        n->name = t->text;
        n->a = parse_not(ps);
        return n;
    }
    return parse_compare(ps);
}

static Node *parse_and(Parser *ps) {
    Node *l = parse_not(ps);
    while (check_kw(ps, "and")) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_not(ps), op->line);
    }
    return l;
}

static Node *parse_or(Parser *ps) {
    Node *l = parse_and(ps);
    while (check_kw(ps, "or")) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_and(ps), op->line);
    }
    return l;
}

static Node *parse_nullish(Parser *ps) {
    Node *l = parse_or(ps);
    while (check_op(ps, "??")) {
        Token *op = advance(ps);
        l = make_binary(l, op->text, parse_or(ps), op->line);
    }
    return l;
}

static Node *parse_expr(Parser *ps) {
    if (++g_nest_depth > MAX_NEST_DEPTH)
        fail(peek(ps)->line, "expression is nested too deeply");
    Node *e = parse_nullish(ps);
    g_nest_depth--;
    return e;
}

static Node *parse_condition(Parser *ps, const char *kw, int line) {
    if (!check_op(ps, "("))
        fail(line, "'%s' needs its condition in parentheses, like: %s (x > 1) { ... }",
             kw, kw);
    advance(ps);
    Node *e = parse_expr(ps);
    expect_op(ps, ")");
    return e;
}

static int body_can_reuse(Node *body, int closures_before) {
    if (g_closure_count != closures_before) return 0;
    for (int i = 0; i < body->count; i++) {
        NodeKind k = body->list[i]->kind;
        if (k == N_LET || k == N_FUNC) return 0;
    }
    return 1;
}

static Node *parse_block(Parser *ps) {
    Token *open = peek(ps);
    expect_op(ps, "{");
    Node *b = new_node(N_BLOCK, open->line);
    while (!check_op(ps, "}")) {
        if (peek(ps)->type == T_EOF)
            fail(open->line, "this '{' is never closed; add a matching '}'");
        push_node(b, parse_statement(ps));
    }
    advance(ps);
    return b;
}

static Node *parse_if_rest(Parser *ps, int line, const char *kw) {
    Node *n = new_node(N_IF, line);
    n->a = parse_condition(ps, kw, line);
    int before = g_closure_count;
    n->b = parse_block(ps);
    n->reuse_scope = body_can_reuse(n->b, before);
    if (check_kw(ps, "elseif")) {
        Token *t = advance(ps);
        n->c = new_node(N_BLOCK, t->line);
        push_node(n->c, parse_if_rest(ps, t->line, "elseif"));
    } else if (check_kw(ps, "else")) {
        Token *t = advance(ps);
        if (check_kw(ps, "if")) {
            Token *ifTok = advance(ps);
            n->c = new_node(N_BLOCK, t->line);
            push_node(n->c, parse_if_rest(ps, ifTok->line, "if"));
        } else {
            n->c = parse_block(ps);
        }
    }
    return n;
}

static Node *parse_statement(Parser *ps) {
    Token *t = peek(ps);
    int line = t->line;

    if (check_kw(ps, "let")) {
        advance(ps);
        Node *n = new_node(N_LET, line);
        n->name = expect_ident(ps);
        if (check_op(ps, "=")) {
            advance(ps);
            n->a = parse_expr(ps);
        }
        return n;
    }

    if (check_kw(ps, "func") && peek2(ps)->type == T_IDENT)
        return parse_function(ps, 1);

    if (check_kw(ps, "if")) {
        Token *kw = advance(ps);
        return parse_if_rest(ps, kw->line, "if");
    }

    if (check_kw(ps, "while")) {
        advance(ps);
        Node *n = new_node(N_WHILE, line);
        n->a = parse_condition(ps, "while", line);
        int before = g_closure_count;
        n->b = parse_block(ps);
        n->reuse_scope = body_can_reuse(n->b, before);
        return n;
    }

    if (check_kw(ps, "repeat")) {
        advance(ps);
        Node *n = new_node(N_REPEAT, line);
        int before = g_closure_count;
        n->b = parse_block(ps);
        n->reuse_scope = body_can_reuse(n->b, before);
        expect_kw(ps, "until");
        n->a = parse_condition(ps, "until", line);
        return n;
    }

    if (check_kw(ps, "for")) {
        advance(ps);
        char *first = expect_ident(ps);
        if (check_op(ps, "=")) {
            advance(ps);
            Node *n = new_node(N_FOR, line);
            n->name = first;
            n->a = parse_expr(ps);
            expect_op(ps, ",");
            n->b = parse_expr(ps);
            if (check_op(ps, ",")) {
                advance(ps);
                n->d = parse_expr(ps);
            }
            int before = g_closure_count;
            n->c = parse_block(ps);
            n->reuse_scope = body_can_reuse(n->c, before);
            return n;
        }
        Node *n = new_node(N_FORIN, line);
        n->name = first;
        if (check_op(ps, ",")) {
            advance(ps);
            n->name2 = expect_ident(ps);
        }
        expect_kw(ps, "in");
        n->a = parse_expr(ps);
        int before = g_closure_count;
        n->c = parse_block(ps);
        n->reuse_scope = body_can_reuse(n->c, before);
        return n;
    }

    if (check_kw(ps, "do")) {
        advance(ps);
        Node *n = new_node(N_DO, line);
        n->b = parse_block(ps);
        return n;
    }

    if (check_kw(ps, "return")) {
        advance(ps);
        Node *n = new_node(N_RETURN, line);
        if (!check_op(ps, "}") && peek(ps)->type != T_EOF)
            n->a = parse_expr(ps);
        return n;
    }

    if (check_kw(ps, "break")) {
        advance(ps);
        return new_node(N_BREAK, line);
    }

    if (check_kw(ps, "continue")) {
        advance(ps);
        return new_node(N_CONTINUE, line);
    }

    Node *e = parse_expr(ps);
    if (check_op(ps, "=") || check_op(ps, "+=") || check_op(ps, "-=") ||
        check_op(ps, "*=") || check_op(ps, "/=")) {
        if (e->kind != N_VAR && e->kind != N_INDEX)
            fail(e->line, "only a name, or a field like data[0] or user.name, can be assigned");
        Token *op = advance(ps);
        Node *n = new_node(N_ASSIGN, line);
        n->name = op->text;
        n->a = e;
        n->b = parse_expr(ps);
        return n;
    }
    if (e->kind != N_CALL)
        fail(e->line, "only function calls can stand alone; "
                      "use 'let' to create a variable or 'print(...)' to show one");
    return e;
}

typedef struct Env Env;
typedef struct Value Value;
typedef struct List List;
typedef struct Map Map;

typedef Value (*NativeFn)(Value *args, int argc, int line);

typedef enum {
    V_NIL, V_BOOL, V_NUM, V_STR, V_FUNC, V_NATIVE, V_ARRAY, V_TABLE
} ValueType;

typedef struct Closure {
    Node *fn;
    Env *env;
} Closure;

struct Value {
    ValueType type;
    union {
        double num;
        char *str;
        Closure *clo;
        NativeFn native;
        List *list;
        Map *map;
    };
};

struct List {
    Value *items;
    int count, cap;
};

struct Map {
    Value *keys;
    Value *vals;
    int count, cap;
    int *index;
    int icap;
    int tombs;
};

typedef struct Var {
    const char *name;
    Value val;
    struct Var *next;
} Var;

struct Env {
    Var *vars;
    Env *parent;
};

static Env *g_globals;
static Value g_return;
static Map *g_string_lib, *g_math_lib, *g_table_lib, *g_os_lib, *g_array_methods;
static Map *g_loading = NULL;
static Map *g_http_lib, *g_fs_lib, *g_io_lib, *g_net_lib, *g_modules, *g_builtin_libs;
static char g_script_dir[1024] = ".";

#define SIG_NONE     0
#define SIG_RETURN   1
#define SIG_BREAK    2
#define SIG_CONTINUE 3

static Value mk(ValueType t) {
    Value v;
    memset(&v, 0, sizeof v);
    v.type = t;
    return v;
}

static Value v_nil(void) { return mk(V_NIL); }

static Value make_closure(Node *fn, Env *env) {
    Closure *c = arena_alloc(sizeof(Closure));
    c->fn = fn;
    c->env = env;
    Value v = mk(V_FUNC);
    v.clo = c;
    return v;
}

static Value v_num(double n) {
    Value v = mk(V_NUM);
    v.num = n;
    return v;
}

static Value v_bool(int b) {
    Value v = mk(V_BOOL);
    v.num = b ? 1 : 0;
    return v;
}

static Value v_str(char *s) {
    Value v = mk(V_STR);
    v.str = s;
    return v;
}

static Value v_native(NativeFn f) {
    Value v = mk(V_NATIVE);
    v.native = f;
    return v;
}

static Value v_list(List *l) {
    Value v = mk(V_ARRAY);
    v.list = l;
    return v;
}

static Value v_map(Map *m) {
    Value v = mk(V_TABLE);
    v.map = m;
    return v;
}

static List *new_list(void) { return xmalloc(sizeof(List)); }
static Map *new_map(void) { return xmalloc(sizeof(Map)); }

static void list_push(List *l, Value v) {
    if (l->count + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, sizeof(Value) * (size_t)l->cap);
    }
    l->items[l->count++] = v;
}

static void list_insert(List *l, int pos, Value v) {
    list_push(l, v_nil());
    memmove(&l->items[pos + 1], &l->items[pos],
            sizeof(Value) * (size_t)(l->count - 1 - pos));
    l->items[pos] = v;
}

static Value list_remove(List *l, int pos) {
    Value v = l->items[pos];
    memmove(&l->items[pos], &l->items[pos + 1],
            sizeof(Value) * (size_t)(l->count - pos - 1));
    l->count--;
    return v;
}

typedef struct NameEntry {
    char *name;
    struct NameEntry *next;
} NameEntry;

#define NAME_BUCKETS 4096
static NameEntry *g_names[NAME_BUCKETS];

static const char *intern(const char *s) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        h = h * 33 + *p;
    NameEntry **slot = &g_names[h & (NAME_BUCKETS - 1)];
    for (NameEntry *e = *slot; e; e = e->next)
        if (strcmp(e->name, s) == 0) return e->name;
    NameEntry *e = arena_alloc(sizeof(NameEntry));
    e->name = xstrdup(s);
    e->next = *slot;
    *slot = e;
    return e->name;
}

static Env *new_env(Env *parent) {
    Env *e = arena_alloc(sizeof(Env));
    e->parent = parent;
    return e;
}

static Var *find_local(Env *e, const char *name) {
    for (Var *v = e->vars; v; v = v->next)
        if (v->name == name) return v;
    return NULL;
}

static Var *lookup(Env *e, const char *name) {
    for (; e; e = e->parent) {
        Var *v = find_local(e, name);
        if (v) return v;
    }
    return NULL;
}

static void define_interned(Env *e, const char *name, Value val) {
    Var *v = find_local(e, name);
    if (v) {
        v->val = val;
        return;
    }
    v = arena_alloc(sizeof(Var));
    v->name = name;
    v->val = val;
    v->next = e->vars;
    e->vars = v;
}

static void define(Env *e, const char *name, Value val) {
    define_interned(e, intern(name), val);
}

static int truthy(Value v) {
    if (v.type == V_NIL) return 0;
    if (v.type == V_BOOL) return v.num != 0;
    return 1;
}

static const char *type_name(Value v) {
    switch (v.type) {
    case V_NIL: return "nil";
    case V_BOOL: return "boolean";
    case V_NUM: return "number";
    case V_STR: return "string";
    case V_FUNC: return "function";
    case V_NATIVE: return "function";
    case V_ARRAY: return "array";
    case V_TABLE: return "table";
    }
    return "unknown";
}

static void num_text(char *buf, size_t size, double d) {
    if (d >= -1e15 && d <= 1e15 && d == (long long)d)
        snprintf(buf, size, "%lld", (long long)d);
    else
        snprintf(buf, size, "%.10g", d);
}

static void text_into(SB *b, Value v, int depth) {
    char buf[64];
    switch (v.type) {
    case V_NIL:
        sb_add(b, "nil");
        return;
    case V_BOOL:
        sb_add(b, v.num ? "true" : "false");
        return;
    case V_NUM:
        num_text(buf, sizeof buf, v.num);
        sb_add(b, buf);
        return;
    case V_STR:
        if (depth > 0) sb_addc(b, '"');
        sb_add(b, v.str);
        if (depth > 0) sb_addc(b, '"');
        return;
    case V_FUNC:
        sb_add(b, "<function ");
        sb_add(b, v.clo->fn->name);
        sb_addc(b, '>');
        return;
    case V_NATIVE:
        sb_add(b, "<built-in function>");
        return;
    case V_ARRAY:
        if (depth > 3) {
            sb_add(b, "[...]");
            return;
        }
        sb_addc(b, '[');
        for (int i = 0; i < v.list->count; i++) {
            if (i > 0) sb_add(b, ", ");
            text_into(b, v.list->items[i], depth + 1);
        }
        sb_addc(b, ']');
        return;
    case V_TABLE:
        if (depth > 3) {
            sb_add(b, "{...}");
            return;
        }
        sb_addc(b, '{');
        for (int i = 0; i < v.map->count; i++) {
            if (i > 0) sb_add(b, ", ");
            text_into(b, v.map->keys[i], 0);
            sb_add(b, ": ");
            text_into(b, v.map->vals[i], depth + 1);
        }
        sb_addc(b, '}');
        return;
    }
}

static char *to_text(Value v) {

    if (v.type == V_STR) return v.str;
    SB b = {NULL, 0, 0};
    text_into(&b, v, 0);
    return sb_take(&b);
}

static int values_equal(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case V_NIL: return 1;
    case V_BOOL:
    case V_NUM: return a.num == b.num;
    case V_STR: return strcmp(a.str, b.str) == 0;
    case V_FUNC: return a.clo == b.clo;
    case V_NATIVE: return a.native == b.native;
    case V_ARRAY: return a.list == b.list;
    case V_TABLE: return a.map == b.map;
    }
    return 0;
}

static unsigned long hash_value(Value v) {
    switch (v.type) {
    case V_NUM: {
        double d = v.num;
        if (d == 0) d = 0;
        uint64_t b;
        memcpy(&b, &d, sizeof b);
        return (unsigned long)((b ^ (b >> 29)) * 0x9E3779B97F4A7C15ULL >> 32);
    }
    case V_STR: {
        unsigned long h = 2166136261UL;
        for (const unsigned char *p = (const unsigned char *)v.str; *p; p++)
            h = (h ^ *p) * 16777619UL;
        return h;
    }
    case V_BOOL: return v.num ? 3 : 2;
    case V_ARRAY: return (unsigned long)(uintptr_t)v.list;
    case V_TABLE: return (unsigned long)(uintptr_t)v.map;
    case V_FUNC: return (unsigned long)(uintptr_t)v.clo;
    case V_NATIVE: return (unsigned long)(uintptr_t)v.native;
    default: return 0;
    }
}

static int map_probe(Map *m, Value key, int *entry, int *free_slot) {
    *entry = -1;
    *free_slot = -1;
    if (m->icap == 0) return -1;
    unsigned long mask = (unsigned long)m->icap - 1;
    unsigned long h = hash_value(key) & mask;
    for (;;) {
        int slot = m->index[h];
        if (slot == 0) {
            if (*free_slot < 0) *free_slot = (int)h;
            return -1;
        }
        if (slot == -1) {
            if (*free_slot < 0) *free_slot = (int)h;
        } else if (values_equal(m->keys[slot - 1], key)) {
            *entry = slot - 1;
            return (int)h;
        }
        h = (h + 1) & mask;
    }
}

static void map_rebuild(Map *m, int want) {
    int icap = 8;
    while (icap < want * 4) icap *= 2;
    m->index = xrealloc(NULL, sizeof(int) * (size_t)icap);
    memset(m->index, 0, sizeof(int) * (size_t)icap);
    m->icap = icap;
    m->tombs = 0;
    unsigned long mask = (unsigned long)icap - 1;
    for (int j = 0; j < m->count; j++) {
        unsigned long h = hash_value(m->keys[j]) & mask;
        while (m->index[h] != 0) h = (h + 1) & mask;
        m->index[h] = j + 1;
    }
}

static Value *map_find(Map *m, Value key) {
    int entry, free_slot;
    if (map_probe(m, key, &entry, &free_slot) < 0) return NULL;
    return &m->vals[entry];
}

static void map_delete_at(Map *m, int pos, int entry) {
    m->index[pos] = -1;
    m->tombs++;
    int last = m->count - 1;
    if (entry != last) {
        m->keys[entry] = m->keys[last];
        m->vals[entry] = m->vals[last];
        int e2, f2;
        int lp = map_probe(m, m->keys[entry], &e2, &f2);
        m->index[lp] = entry + 1;
    }
    m->count--;
}

static void map_set(Map *m, Value key, Value val) {
    int entry, free_slot;
    int pos = map_probe(m, key, &entry, &free_slot);
    if (pos >= 0) {
        if (val.type == V_NIL) map_delete_at(m, pos, entry);
        else m->vals[entry] = val;
        return;
    }
    if (val.type == V_NIL) return;
    if (m->count + 1 > m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->keys = xrealloc(m->keys, sizeof(Value) * (size_t)m->cap);
        m->vals = xrealloc(m->vals, sizeof(Value) * (size_t)m->cap);
    }
    if (m->icap == 0 || (m->count + m->tombs + 1) * 2 > m->icap) {
        map_rebuild(m, m->count + 1);
        map_probe(m, key, &entry, &free_slot);
    }
    if (m->index[free_slot] == -1) m->tombs--;
    m->keys[m->count] = key;
    m->vals[m->count] = val;
    m->index[free_slot] = m->count + 1;
    m->count++;
}

static double need_num(Value v, int line, const char *op) {
    if (v.type != V_NUM)
        fail(line, "'%s' needs a number, but got a %s", op, type_name(v));
    return v.num;
}

static Value binary_op(const char *op, Value l, Value r, int line) {
    if (strcmp(op, "==") == 0) return v_bool(values_equal(l, r));
    if (strcmp(op, "~=") == 0 || strcmp(op, "!=") == 0)
        return v_bool(!values_equal(l, r));

    if (strcmp(op, "..") == 0) {
        char *a = to_text(l), *b = to_text(r);
        char *out = xmalloc(strlen(a) + strlen(b) + 1);
        strcpy(out, a);
        strcat(out, b);
        return v_str(out);
    }

    if (strcmp(op, "+") == 0) {
        if (l.type == V_STR || r.type == V_STR)
            fail(line, "'+' adds numbers; use '..' to join text");
        return v_num(need_num(l, line, op) + need_num(r, line, op));
    }
    if (strcmp(op, "-") == 0)
        return v_num(need_num(l, line, op) - need_num(r, line, op));
    if (strcmp(op, "*") == 0)
        return v_num(need_num(l, line, op) * need_num(r, line, op));
    if (strcmp(op, "/") == 0) {
        double d = need_num(r, line, op);
        if (d == 0) fail(line, "division by zero");
        return v_num(need_num(l, line, op) / d);
    }
    if (strcmp(op, "%") == 0) {
        double d = need_num(r, line, op);
        if (d == 0) fail(line, "division by zero");
        return v_num(fmod(need_num(l, line, op), d));
    }

    if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        int c;
        if (l.type == V_NUM && r.type == V_NUM)
            c = (l.num < r.num) ? -1 : (l.num > r.num) ? 1 : 0;
        else if (l.type == V_STR && r.type == V_STR)
            c = strcmp(l.str, r.str);
        else
            fail(line, "can't compare a %s with a %s", type_name(l), type_name(r));
        if (strcmp(op, "<") == 0) return v_bool(c < 0);
        if (strcmp(op, "<=") == 0) return v_bool(c <= 0);
        if (strcmp(op, ">") == 0) return v_bool(c > 0);
        return v_bool(c >= 0);
    }

    fail(line, "unknown operator '%s'", op);
}

static Value index_value(Value obj, Value key, int line) {
    switch (obj.type) {
    case V_ARRAY:
        if (key.type == V_NUM) {
            int i = safe_int(key.num);
            if (key.num == i && i >= 0 && i < obj.list->count)
                return obj.list->items[i];
            return v_nil();
        }
        if (key.type == V_STR && strcmp(key.str, "length") == 0)
            return v_num(obj.list->count);
        return v_nil();
    case V_TABLE: {
        Value *slot = map_find(obj.map, key);
        return slot ? *slot : v_nil();
    }
    case V_STR:
        if (key.type == V_NUM) {
            int i = safe_int(key.num);
            if (key.num == i && i >= 0 && i < (int)strlen(obj.str)) {
                char *c = xmalloc(2);
                c[0] = obj.str[i];
                return v_str(c);
            }
            return v_nil();
        }
        if (key.type == V_STR && strcmp(key.str, "length") == 0)
            return v_num((double)strlen(obj.str));
        return v_nil();
    default:
        fail(line, "can't read a field of a %s value", type_name(obj));
    }
}

static void set_index(Value obj, Value key, Value val, int line) {
    if (obj.type == V_ARRAY) {
        if (key.type != V_NUM)
            fail(line, "array index must be a number, but got a %s", type_name(key));
        int i = safe_int(key.num);
        if (key.num != i || i < 0)
            fail(line, "array index must be a whole number starting at 0");
        if (i < obj.list->count) {
            obj.list->items[i] = val;
        } else if (i == obj.list->count) {
            if (val.type != V_NIL) list_push(obj.list, val);
        } else {
            fail(line, "index %d is past the end of the array (length %d)",
                 i, obj.list->count);
        }
    } else if (obj.type == V_TABLE) {
        if (key.type == V_NIL) fail(line, "a table key can't be nil");
        map_set(obj.map, key, val);
    } else {
        fail(line, "can't set a field on a %s value", type_name(obj));
    }
}

static int exec_block(Node *b, Env *env);
static int exec(Node *n, Env *env);

static Value call_value(Value f, Value *args, int argc, int line) {
    if (++g_call_depth > MAX_CALL_DEPTH)
        fail(line, "too many nested function calls (the limit is %d)", MAX_CALL_DEPTH);
    Node *fn = f.clo->fn;
    Env *local = new_env(f.clo->env);
    for (int i = 0; i < fn->nparams; i++)
        define_interned(local, fn->params[i], i < argc ? args[i] : v_nil());

    int sig = exec_block(fn->b, local);
    g_call_depth--;
    if (sig == SIG_BREAK || sig == SIG_CONTINUE)
        fail(line, "'break' or 'continue' used outside of a loop");
    if (sig == SIG_RETURN) {
        Value r = g_return;
        g_return = v_nil();
        return r;
    }
    return v_nil();
}

static Value call_callable(Value f, Value *args, int argc, int line) {
    if (f.type == V_FUNC) return call_value(f, args, argc, line);
    if (f.type == V_NATIVE) return f.native(args, argc, line);
    fail(line, "tried to call a %s value", type_name(f));
}

static NativeFn method_for(Value obj, const char *name) {
    Map *m;
    if (obj.type == V_ARRAY) m = g_array_methods;
    else if (obj.type == V_STR) m = g_string_lib;
    else return NULL;
    Value *f = map_find(m, v_str((char *)name));
    if (f && f->type == V_NATIVE) return f->native;
    return NULL;
}

static Value eval(Node *n, Env *env) {
    switch (n->kind) {
    case N_NUM:
        return v_num(n->num);
    case N_STR:
        return v_str(n->name);
    case N_BOOL:
        return v_bool((int)n->num);
    case N_NIL:
        return v_nil();

    case N_VAR: {
        Var *v = lookup(env, n->name);
        if (!v) fail(n->line, "'%s' is not defined", n->name);
        return v->val;
    }

    case N_FUNC: {
        return make_closure(n, env);
    }

    case N_ARRAY: {
        List *l = new_list();
        for (int i = 0; i < n->count; i++) list_push(l, eval(n->list[i], env));
        return v_list(l);
    }

    case N_TABLE: {
        Map *m = new_map();
        for (int i = 0; i + 1 < n->count; i += 2) {
            Value k = eval(n->list[i], env);
            Value v = eval(n->list[i + 1], env);
            if (k.type == V_NIL) fail(n->line, "a table key can't be nil");
            map_set(m, k, v);
        }
        return v_map(m);
    }

    case N_INDEX: {
        Value obj = eval(n->a, env);
        Value key = eval(n->b, env);
        return index_value(obj, key, n->line);
    }

    case N_UNARY: {
        Value v = eval(n->a, env);
        if (strcmp(n->name, "-") == 0)
            return v_num(-need_num(v, n->line, "-"));
        if (strcmp(n->name, "#") == 0) {
            if (v.type == V_ARRAY) return v_num(v.list->count);
            if (v.type == V_STR) return v_num((double)strlen(v.str));
            if (v.type == V_TABLE) return v_num(v.map->count);
            fail(n->line, "'#' measures arrays and text, but got a %s", type_name(v));
        }
        return v_bool(!truthy(v));
    }

    case N_BINARY: {
        if (strcmp(n->name, "??") == 0) {
            Value l = eval(n->a, env);
            return (l.type != V_NIL) ? l : eval(n->b, env);
        }
        if (strcmp(n->name, "and") == 0) {
            Value l = eval(n->a, env);
            return truthy(l) ? eval(n->b, env) : l;
        }
        if (strcmp(n->name, "or") == 0) {
            Value l = eval(n->a, env);
            return truthy(l) ? l : eval(n->b, env);
        }
        Value l = eval(n->a, env);
        Value r = eval(n->b, env);
        return binary_op(n->name, l, r, n->line);
    }

    case N_CALL: {
        Node *callee = n->a;
        Value stack_args[8];
        Value *args = (n->count + 1 <= 8) ? stack_args
                                          : xmalloc(sizeof(Value) * (size_t)(n->count + 1));
        Value fnv = v_nil();
        NativeFn method = NULL;
        int argc = 0;

        if (callee->kind == N_INDEX) {
            Value obj = eval(callee->a, env);
            Value key = eval(callee->b, env);
            if (key.type == V_STR) method = method_for(obj, key.str);
            if (method) args[argc++] = obj;
            else fnv = index_value(obj, key, callee->line);
        } else {
            fnv = eval(callee, env);
        }

        for (int i = 0; i < n->count; i++) args[argc++] = eval(n->list[i], env);

        if (method) return method(args, argc, n->line);
        return call_callable(fnv, args, argc, n->line);
    }

    default:
        fail(n->line, "internal error: unexpected expression");
    }
}

static int exec_scoped(Node *b, Env *env) {
    return exec_block(b, new_env(env));
}

static int exec_block(Node *b, Env *env) {
    for (int i = 0; i < b->count; i++) {
        int sig = exec(b->list[i], env);
        if (sig != SIG_NONE) return sig;
    }
    return SIG_NONE;
}

static int exec(Node *n, Env *env) {
    switch (n->kind) {
    case N_LET:
        define_interned(env, n->name, n->a ? eval(n->a, env) : v_nil());
        return SIG_NONE;

    case N_ASSIGN: {
        const char *op = n->name;
        int compound = strcmp(op, "=") != 0;
        char bop[2] = {op[0], '\0'};
        Node *t = n->a;

        if (t->kind == N_VAR) {
            Var *var = lookup(env, t->name);
            if (compound && !var)
                fail(n->line, "'%s' is not defined", t->name);
            Value cur = var ? var->val : v_nil();
            Value rhs = eval(n->b, env);
            Value nv = compound ? binary_op(bop, cur, rhs, n->line) : rhs;
            if (var) {
                var->val = nv;
            } else {

                Env *g = env;
                while (g->parent) g = g->parent;
                define(g, t->name, nv);
            }
        } else {
            Value obj = eval(t->a, env);
            Value key = eval(t->b, env);
            Value cur = compound ? index_value(obj, key, n->line) : v_nil();
            Value rhs = eval(n->b, env);
            Value nv = compound ? binary_op(bop, cur, rhs, n->line) : rhs;
            set_index(obj, key, nv, n->line);
        }
        return SIG_NONE;
    }

    case N_FUNC:
        define_interned(env, n->name, eval(n, env));
        return SIG_NONE;

    case N_IF:
        if (truthy(eval(n->a, env))) {
            return n->reuse_scope ? exec_block(n->b, env) : exec_scoped(n->b, env);
        }
        if (n->c) return exec_scoped(n->c, env);
        return SIG_NONE;

    case N_WHILE: {
        Env *shared = n->reuse_scope ? new_env(env) : NULL;
        while (truthy(eval(n->a, env))) {
            int sig = shared ? exec_block(n->b, shared) : exec_scoped(n->b, env);
            if (sig == SIG_BREAK) break;
            if (sig == SIG_RETURN) return sig;
        }
        return SIG_NONE;
    }

    case N_REPEAT: {
        Env *shared = n->reuse_scope ? new_env(env) : NULL;
        for (;;) {
            int sig = shared ? exec_block(n->b, shared) : exec_scoped(n->b, env);
            if (sig == SIG_BREAK) break;
            if (sig == SIG_RETURN) return sig;
            if (truthy(eval(n->a, env))) break;
        }
        return SIG_NONE;
    }

    case N_FOR: {
        double from = need_num(eval(n->a, env), n->line, "for");
        double to = need_num(eval(n->b, env), n->line, "for");
        double step = n->d ? need_num(eval(n->d, env), n->line, "for") : 1;
        if (step == 0) fail(n->line, "the 'for' step cannot be zero");
        Env *shared = n->reuse_scope ? new_env(env) : NULL;
        for (double i = from; step > 0 ? i <= to : i >= to; i += step) {
            Env *loop = shared ? shared : new_env(env);
            define(loop, n->name, v_num(i));
            int sig = exec_block(n->c, loop);
            if (sig == SIG_BREAK) break;
            if (sig == SIG_RETURN) return sig;
        }
        return SIG_NONE;
    }

    case N_FORIN: {
        Value it = eval(n->a, env);
        Env *shared = n->reuse_scope ? new_env(env) : NULL;
        if (it.type == V_ARRAY) {
            for (int i = 0; i < it.list->count; i++) {
                Env *loop = shared ? shared : new_env(env);
                if (n->name2) {
                    define(loop, n->name, v_num(i));
                    define(loop, n->name2, it.list->items[i]);
                } else {
                    define(loop, n->name, it.list->items[i]);
                }
                int sig = exec_block(n->c, loop);
                if (sig == SIG_BREAK) break;
                if (sig == SIG_RETURN) return sig;
            }
        } else if (it.type == V_TABLE) {
            for (int i = 0; i < it.map->count; i++) {
                Env *loop = shared ? shared : new_env(env);
                define(loop, n->name, it.map->keys[i]);
                if (n->name2) define(loop, n->name2, it.map->vals[i]);
                int sig = exec_block(n->c, loop);
                if (sig == SIG_BREAK) break;
                if (sig == SIG_RETURN) return sig;
            }
        } else if (it.type == V_STR) {
            size_t len = strlen(it.str);
            for (size_t i = 0; i < len; i++) {
                Env *loop = shared ? shared : new_env(env);
                char *ch = xmalloc(2);
                ch[0] = it.str[i];
                define(loop, n->name, v_str(ch));
                int sig = exec_block(n->c, loop);
                if (sig == SIG_BREAK) break;
                if (sig == SIG_RETURN) return sig;
            }
        } else {
            fail(n->line, "'for ... in' needs an array, table or text, but got a %s",
                 type_name(it));
        }
        return SIG_NONE;
    }

    case N_DO:
        return exec_scoped(n->b, env);

    case N_RETURN: {
        Value v = n->a ? eval(n->a, env) : v_nil();
        g_return = v;
        return SIG_RETURN;
    }

    case N_BREAK:
        return SIG_BREAK;

    case N_CONTINUE:
        return SIG_CONTINUE;

    case N_BLOCK:
        return exec_block(n, env);

    default:

        eval(n, env);
        return SIG_NONE;
    }
}

static void check_argc(const char *name, int argc, int min, int max, int line) {
    if (argc < min || argc > max) {
        if (min == max)
            fail(line, "'%s' takes %d argument(s), but got %d", name, min, argc);
        fail(line, "'%s' takes %d to %d arguments, but got %d", name, min, max, argc);
    }
}

static List *need_list(Value v, int line, const char *name) {
    if (v.type != V_ARRAY)
        fail(line, "%s needs an array, but got a %s", name, type_name(v));
    return v.list;
}

static Map *need_table(Value v, int line, const char *name) {
    if (v.type != V_TABLE)
        fail(line, "%s needs a table, but got a %s", name, type_name(v));
    return v.map;
}

static const char *need_string(Value v, int line, const char *name) {
    if (v.type != V_STR)
        fail(line, "%s needs text, but got a %s", name, type_name(v));
    return v.str;
}

static Value lib_print(Value *a, int n, int line) {
    (void)line;
    for (int i = 0; i < n; i++) {
        char *t = to_text(a[i]);
        if (i > 0) putchar('\t');
        fputs(t, stdout);
    }
    putchar('\n');
    return v_nil();
}

static Value lib_input(Value *a, int n, int line) {
    char buf[4096];
    check_argc("input", n, 0, 1, line);
    if (n == 1) {
        fputs(to_text(a[0]), stdout);
        fflush(stdout);
    }
    if (!fgets(buf, sizeof buf, stdin)) return v_nil();
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    return v_str(xstrdup(buf));
}

static Value lib_type(Value *a, int n, int line) {
    check_argc("type", n, 1, 1, line);
    return v_str((char *)type_name(a[0]));
}

static Value lib_tostring(Value *a, int n, int line) {
    check_argc("tostring", n, 1, 1, line);
    return v_str(to_text(a[0]));
}

static Value lib_tonumber(Value *a, int n, int line) {
    check_argc("tonumber", n, 1, 1, line);
    if (a[0].type == V_NUM) return a[0];
    if (a[0].type == V_STR) {
        char *end;
        double d = strtod(a[0].str, &end);
        if (end != a[0].str && *end == '\0') return v_num(d);
    }
    return v_nil();
}

static Value lib_error(Value *a, int n, int line) {
    char *msg = n >= 1 ? to_text(a[0]) : xstrdup("error");
    fail(line, "%s", msg);
}

static Value lib_assert(Value *a, int n, int line) {
    if (n < 1 || !truthy(a[0]))
        fail(line, "%s", n >= 2 ? to_text(a[1]) : "assertion failed!");
    return a[0];
}

static Value lib_pairs(Value *a, int n, int line) {
    check_argc("pairs", n, 1, 1, line);
    if (a[0].type != V_TABLE && a[0].type != V_ARRAY)
        fail(line, "pairs needs a table or array, but got a %s", type_name(a[0]));
    return a[0];
}

static Value tbl_insert(Value *a, int n, int line) {
    check_argc("table.insert", n, 2, 3, line);
    List *l = need_list(a[0], line, "table.insert");
    if (n == 2) {
        list_push(l, a[1]);
        return v_nil();
    }
    int pos = safe_int(need_num(a[1], line, "table.insert"));
    if (pos < 0) pos = 0;
    if (pos > l->count) pos = l->count;
    list_insert(l, pos, a[2]);
    return v_nil();
}

static Value tbl_remove(Value *a, int n, int line) {
    check_argc("table.remove", n, 1, 2, line);
    List *l = need_list(a[0], line, "table.remove");
    if (l->count == 0) return v_nil();
    int pos = (n == 2) ? safe_int(need_num(a[1], line, "table.remove")) : l->count - 1;
    if (pos < 0 || pos >= l->count) return v_nil();
    return list_remove(l, pos);
}

static Value tbl_concat(Value *a, int n, int line) {
    check_argc("table.concat", n, 1, 2, line);
    List *l = need_list(a[0], line, "table.concat");
    const char *sep = (n == 2) ? to_text(a[1]) : "";
    SB sb = {NULL, 0, 0};
    for (int i = 0; i < l->count; i++) {
        if (i > 0) sb_add(&sb, sep);
        sb_add(&sb, to_text(l->items[i]));
    }
    return v_str(sb_take(&sb));
}

static Value g_sort_fn;
static int g_sort_line;

static int sort_by_fn(const void *x, const void *y) {
    Value args[2];
    args[0] = *(const Value *)x;
    args[1] = *(const Value *)y;
    if (truthy(call_callable(g_sort_fn, args, 2, g_sort_line))) return -1;
    args[0] = *(const Value *)y;
    args[1] = *(const Value *)x;
    if (truthy(call_callable(g_sort_fn, args, 2, g_sort_line))) return 1;
    return 0;
}

static int sort_default(const void *x, const void *y) {
    Value a = *(const Value *)x, b = *(const Value *)y;
    if (truthy(binary_op("<", a, b, g_sort_line))) return -1;
    if (truthy(binary_op("<", b, a, g_sort_line))) return 1;
    return 0;
}

static Value tbl_sort(Value *a, int n, int line) {
    check_argc("table.sort", n, 1, 2, line);
    List *l = need_list(a[0], line, "table.sort");
    if (l->count < 2) return v_nil();
    g_sort_line = line;
    if (n == 2 && a[1].type != V_NIL) {
        if (a[1].type != V_FUNC && a[1].type != V_NATIVE)
            fail(line, "table.sort's second argument must be a function");
        g_sort_fn = a[1];
        qsort(l->items, (size_t)l->count, sizeof(Value), sort_by_fn);
    } else {
        qsort(l->items, (size_t)l->count, sizeof(Value), sort_default);
    }
    return v_nil();
}

static Value tbl_keys(Value *a, int n, int line) {
    check_argc("table.keys", n, 1, 1, line);
    Map *m = need_table(a[0], line, "table.keys");
    List *out = new_list();
    for (int i = 0; i < m->count; i++) list_push(out, m->keys[i]);
    return v_list(out);
}

static Value tbl_values(Value *a, int n, int line) {
    check_argc("table.values", n, 1, 1, line);
    Map *m = need_table(a[0], line, "table.values");
    List *out = new_list();
    for (int i = 0; i < m->count; i++) list_push(out, m->vals[i]);
    return v_list(out);
}

static Value str_len(Value *a, int n, int line) {
    check_argc("string.len", n, 1, 1, line);
    return v_num((double)strlen(need_string(a[0], line, "string.len")));
}

static Value str_upper(Value *a, int n, int line) {
    check_argc("string.upper", n, 1, 1, line);
    const char *s = need_string(a[0], line, "string.upper");
    size_t len = strlen(s);
    char *o = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) o[i] = (char)toupper((unsigned char)s[i]);
    return v_str(o);
}

static Value str_lower(Value *a, int n, int line) {
    check_argc("string.lower", n, 1, 1, line);
    const char *s = need_string(a[0], line, "string.lower");
    size_t len = strlen(s);
    char *o = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) o[i] = (char)tolower((unsigned char)s[i]);
    return v_str(o);
}

static Value str_sub(Value *a, int n, int line) {
    check_argc("string.sub", n, 1, 3, line);
    const char *s = need_string(a[0], line, "string.sub");
    int len = (int)strlen(s);
    int start = n >= 2 ? safe_int(need_num(a[1], line, "string.sub")) : 0;
    int stop = n >= 3 ? safe_int(need_num(a[2], line, "string.sub")) : len;
    if (start < 0) start += len;
    if (stop < 0) stop += len;
    if (start < 0) start = 0;
    if (stop > len) stop = len;
    if (start >= stop) return v_str(xstrdup(""));
    char *o = xmalloc((size_t)(stop - start) + 1);
    memcpy(o, s + start, (size_t)(stop - start));
    o[stop - start] = '\0';
    return v_str(o);
}

static Value str_rep(Value *a, int n, int line) {
    check_argc("string.rep", n, 2, 2, line);
    const char *s = need_string(a[0], line, "string.rep");
    int count = safe_int(need_num(a[1], line, "string.rep"));
    if (count < 0) count = 0;
    size_t len = strlen(s);
    if (len > 0 && (size_t)count > (size_t)(1 << 28) / len)
        fail(line, "string.rep would make a text that is too large");
    char *o = xmalloc(len * (size_t)count + 1);
    for (int i = 0; i < count; i++) memcpy(o + (size_t)i * len, s, len);
    o[len * (size_t)count] = '\0';
    return v_str(o);
}

static Value str_reverse(Value *a, int n, int line) {
    check_argc("string.reverse", n, 1, 1, line);
    const char *s = need_string(a[0], line, "string.reverse");
    size_t len = strlen(s);
    char *o = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) o[i] = s[len - 1 - i];
    return v_str(o);
}

#define PAT_MAXCAP 32
#define PAT_MAXDEPTH 200
#define CAP_UNFINISHED (-1)
#define CAP_POSITION (-2)

typedef struct {
    const char *src_init;
    const char *src_end;
    const char *p_end;
    int level;
    int depth;
    int line;
    struct { const char *init; ptrdiff_t len; } cap[PAT_MAXCAP];
} MatchState;

static const char *pat_match(MatchState *ms, const char *s, const char *p);

static int pat_class_match(int c, int cl) {
    int res;
    switch (tolower(cl)) {
    case 'a': res = isalpha(c); break;
    case 'c': res = iscntrl(c); break;
    case 'd': res = isdigit(c); break;
    case 'g': res = isgraph(c); break;
    case 'l': res = islower(c); break;
    case 'p': res = ispunct(c); break;
    case 's': res = isspace(c); break;
    case 'u': res = isupper(c); break;
    case 'w': res = isalnum(c); break;
    case 'x': res = isxdigit(c); break;
    default: return cl == c;
    }
    if (isupper(cl)) res = !res;
    return res;
}

static const char *pat_class_end(MatchState *ms, const char *p) {
    char c = *p++;
    if (c == '%') {
        if (p >= ms->p_end) fail(ms->line, "malformed pattern (ends with '%%')");
        return p + 1;
    }
    if (c == '[') {
        if (p < ms->p_end && *p == '^') p++;
        do {
            if (p >= ms->p_end) fail(ms->line, "malformed pattern (missing ']')");
            if (*(p++) == '%' && p < ms->p_end) p++;
        } while (*p != ']');
        return p + 1;
    }
    return p;
}

static int pat_bracket_match(MatchState *ms, int c, const char *p, const char *ec) {
    int sig = 1;
    (void)ms;
    if (*(p + 1) == '^') {
        sig = 0;
        p++;
    }
    while (++p < ec) {
        if (*p == '%') {
            p++;
            if (pat_class_match(c, (unsigned char)*p)) return sig;
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            if ((unsigned char)*(p - 2) <= c && c <= (unsigned char)*p) return sig;
        } else if ((unsigned char)*p == c) {
            return sig;
        }
    }
    return !sig;
}

static int pat_single(MatchState *ms, const char *s, const char *p, const char *ep) {
    if (s >= ms->src_end) return 0;
    int c = (unsigned char)*s;
    switch (*p) {
    case '.': return 1;
    case '%': return pat_class_match(c, (unsigned char)*(p + 1));
    case '[': return pat_bracket_match(ms, c, p, ep - 1);
    default: return (unsigned char)*p == c;
    }
}

static const char *max_expand(MatchState *ms, const char *s, const char *p, const char *ep) {
    ptrdiff_t i = 0;
    while (pat_single(ms, s + i, p, ep)) i++;
    while (i >= 0) {
        const char *res = pat_match(ms, s + i, ep + 1);
        if (res) return res;
        i--;
    }
    return NULL;
}

static const char *min_expand(MatchState *ms, const char *s, const char *p, const char *ep) {
    for (;;) {
        const char *res = pat_match(ms, s, ep + 1);
        if (res) return res;
        if (pat_single(ms, s, p, ep)) s++;
        else return NULL;
    }
}

static int capture_to_close(MatchState *ms) {
    for (int level = ms->level - 1; level >= 0; level--)
        if (ms->cap[level].len == CAP_UNFINISHED) return level;
    fail(ms->line, "invalid pattern capture");
}

static const char *start_capture(MatchState *ms, const char *s, const char *p, ptrdiff_t what) {
    if (ms->level >= PAT_MAXCAP) fail(ms->line, "too many captures in pattern");
    ms->cap[ms->level].init = s;
    ms->cap[ms->level].len = what;
    ms->level++;
    const char *res = pat_match(ms, s, p);
    if (!res) ms->level--;
    return res;
}

static const char *end_capture(MatchState *ms, const char *s, const char *p) {
    int l = capture_to_close(ms);
    ms->cap[l].len = s - ms->cap[l].init;
    const char *res = pat_match(ms, s, p);
    if (!res) ms->cap[l].len = CAP_UNFINISHED;
    return res;
}

static const char *match_balance(MatchState *ms, const char *s, const char *p) {
    if (p + 1 >= ms->p_end) fail(ms->line, "malformed pattern (%%b needs two characters)");
    if (s >= ms->src_end || *s != *p) return NULL;
    int b = *p, e = *(p + 1), cont = 1;
    while (++s < ms->src_end) {
        if (*s == e) {
            if (--cont == 0) return s + 1;
        } else if (*s == b) {
            cont++;
        }
    }
    return NULL;
}

static const char *match_capture(MatchState *ms, const char *s, int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->cap[l].len < 0)
        fail(ms->line, "invalid capture reference in pattern");
    size_t len = (size_t)ms->cap[l].len;
    if ((size_t)(ms->src_end - s) >= len && memcmp(ms->cap[l].init, s, len) == 0)
        return s + len;
    return NULL;
}

static const char *pat_match(MatchState *ms, const char *s, const char *p) {
    const char *res = NULL;
    if (--ms->depth == 0) fail(ms->line, "pattern is too complicated");
    for (;;) {
        if (p == ms->p_end) {
            res = s;
            break;
        }
        if (*p == '(') {
            if (p + 1 < ms->p_end && *(p + 1) == ')')
                res = start_capture(ms, s, p + 2, CAP_POSITION);
            else
                res = start_capture(ms, s, p + 1, CAP_UNFINISHED);
            break;
        }
        if (*p == ')') {
            res = end_capture(ms, s, p + 1);
            break;
        }
        if (*p == '$' && p + 1 == ms->p_end) {
            res = (s == ms->src_end) ? s : NULL;
            break;
        }
        if (*p == '%' && p + 1 < ms->p_end) {
            if (*(p + 1) == 'b') {
                s = match_balance(ms, s, p + 2);
                if (s) {
                    p += 4;
                    continue;
                }
                res = NULL;
                break;
            }
            if (*(p + 1) == 'f') {
                p += 2;
                if (*p != '[') fail(ms->line, "missing '[' after '%%f' in pattern");
                const char *ep = pat_class_end(ms, p);
                int prev = (s == ms->src_init) ? 0 : (unsigned char)*(s - 1);
                int cur = (s < ms->src_end) ? (unsigned char)*s : 0;
                if (!pat_bracket_match(ms, prev, p, ep - 1) &&
                    pat_bracket_match(ms, cur, p, ep - 1)) {
                    p = ep;
                    continue;
                }
                res = NULL;
                break;
            }
            if (isdigit((unsigned char)*(p + 1))) {
                s = match_capture(ms, s, (unsigned char)*(p + 1));
                if (s) {
                    p += 2;
                    continue;
                }
                res = NULL;
                break;
            }
        }

        const char *ep = pat_class_end(ms, p);
        if (!pat_single(ms, s, p, ep)) {
            if (*ep == '*' || *ep == '?' || *ep == '-') {
                p = ep + 1;
                continue;
            }
            res = NULL;
            break;
        }
        if (*ep == '?') {
            const char *r = pat_match(ms, s + 1, ep + 1);
            if (r) {
                res = r;
                break;
            }
            p = ep + 1;
            continue;
        }
        if (*ep == '+') {
            res = max_expand(ms, s + 1, p, ep);
            break;
        }
        if (*ep == '*') {
            res = max_expand(ms, s, p, ep);
            break;
        }
        if (*ep == '-') {
            res = min_expand(ms, s, p, ep);
            break;
        }
        s++;
        p = ep;
    }
    ms->depth++;
    return res;
}

static void ms_init(MatchState *ms, const char *s, size_t ls, const char *p, size_t lp, int line) {
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
    ms->level = 0;
    ms->depth = PAT_MAXDEPTH;
    ms->line = line;
}

static const char *pat_search(MatchState *ms, const char *s, const char *p, const char **end_out) {
    int anchor = (p < ms->p_end && *p == '^');
    if (anchor) p++;
    const char *s1 = s;
    do {
        ms->level = 0;
        ms->depth = PAT_MAXDEPTH;
        const char *e = pat_match(ms, s1, p);
        if (e) {
            *end_out = e;
            return s1;
        }
    } while (s1++ < ms->src_end && !anchor);
    return NULL;
}

static Value sub_value(const char *s, const char *e) {
    size_t len = (size_t)(e - s);
    char *w = xmalloc(len + 1);
    memcpy(w, s, len);
    w[len] = '\0';
    return v_str(w);
}

static Value get_capture(MatchState *ms, int i, const char *s, const char *e) {
    if (i >= ms->level) {
        if (i == 0) return sub_value(s, e);
        fail(ms->line, "invalid capture index %d in pattern", i + 1);
    }
    ptrdiff_t l = ms->cap[i].len;
    if (l == CAP_UNFINISHED) fail(ms->line, "unfinished capture in pattern");
    if (l == CAP_POSITION) return v_num((double)(ms->cap[i].init - ms->src_init));
    return sub_value(ms->cap[i].init, ms->cap[i].init + l);
}

static Value str_find(Value *a, int n, int line) {
    check_argc("string.find", n, 2, 4, line);
    const char *s = need_string(a[0], line, "string.find");
    const char *p = need_string(a[1], line, "string.find");
    size_t ls = strlen(s), lp = strlen(p);
    int init = (n >= 3 && a[2].type != V_NIL) ? safe_int(need_num(a[2], line, "string.find")) : 0;
    int plain = (n >= 4) && truthy(a[3]);
    if (init < 0) init += (int)ls;
    if (init < 0) init = 0;
    if (init > (int)ls) return v_nil();
    if (plain || strpbrk(p, "^$*+?.([%-") == NULL) {
        const char *q = strstr(s + init, p);
        return q ? v_num((double)(q - s)) : v_nil();
    }
    MatchState ms;
    ms_init(&ms, s, ls, p, lp, line);
    const char *e = NULL;
    const char *st = pat_search(&ms, s + init, p, &e);
    return st ? v_num((double)(st - s)) : v_nil();
}

static Value str_match(Value *a, int n, int line) {
    check_argc("string.match", n, 2, 3, line);
    const char *s = need_string(a[0], line, "string.match");
    const char *p = need_string(a[1], line, "string.match");
    size_t ls = strlen(s), lp = strlen(p);
    int init = (n == 3) ? safe_int(need_num(a[2], line, "string.match")) : 0;
    if (init < 0) init += (int)ls;
    if (init < 0) init = 0;
    if (init > (int)ls) return v_nil();
    MatchState ms;
    ms_init(&ms, s, ls, p, lp, line);
    const char *e = NULL;
    const char *st = pat_search(&ms, s + init, p, &e);
    if (!st) return v_nil();
    return get_capture(&ms, 0, st, e);
}

static Value str_gmatch(Value *a, int n, int line) {
    check_argc("string.gmatch", n, 2, 2, line);
    const char *s = need_string(a[0], line, "string.gmatch");
    const char *p = need_string(a[1], line, "string.gmatch");
    MatchState ms;
    ms_init(&ms, s, strlen(s), p, strlen(p), line);
    List *out = new_list();
    const char *src = s;
    const char *lastmatch = NULL;
    while (src <= ms.src_end) {
        ms.level = 0;
        ms.depth = PAT_MAXDEPTH;
        const char *e = pat_match(&ms, src, p);
        if (e && e != lastmatch) {
            list_push(out, get_capture(&ms, 0, src, e));
            src = lastmatch = e;
        } else {
            src++;
        }
    }
    return v_list(out);
}

static void add_replacement(MatchState *ms, SB *out, const char *s, const char *e,
                            Value repl, int line) {
    if (repl.type == V_STR) {
        for (const char *r = repl.str; *r; r++) {
            if (*r != '%') {
                sb_addc(out, *r);
                continue;
            }
            r++;
            if (*r == '%') {
                sb_addc(out, '%');
            } else if (*r == '0') {
                sb_add(out, sub_value(s, e).str);
            } else if (isdigit((unsigned char)*r)) {
                sb_add(out, to_text(get_capture(ms, *r - '1', s, e)));
            } else {
                fail(line, "invalid use of '%%' in the replacement text");
            }
        }
        return;
    }
    Value first = get_capture(ms, 0, s, e);
    Value result;
    if (repl.type == V_TABLE) {
        result = index_value(repl, first, line);
    } else {
        Value args[1];
        args[0] = first;
        result = call_callable(repl, args, 1, line);
    }
    if (result.type == V_NIL || (result.type == V_BOOL && result.num == 0))
        sb_add(out, sub_value(s, e).str);
    else
        sb_add(out, to_text(result));
}

static Value str_gsub(Value *a, int n, int line) {
    check_argc("string.gsub", n, 3, 4, line);
    const char *s = need_string(a[0], line, "string.gsub");
    const char *p = need_string(a[1], line, "string.gsub");
    Value repl = a[2];
    if (repl.type != V_STR && repl.type != V_TABLE && repl.type != V_FUNC && repl.type != V_NATIVE)
        fail(line, "string.gsub's replacement must be text, a table or a function");
    int max = (n == 4) ? safe_int(need_num(a[3], line, "string.gsub")) : INT_MAX;
    MatchState ms;
    ms_init(&ms, s, strlen(s), p, strlen(p), line);
    int anchor = (*p == '^');
    const char *pp = anchor ? p + 1 : p;
    SB out = {NULL, 0, 0};
    const char *src = s;
    const char *lastmatch = NULL;
    int count = 0;
    while (count < max) {
        ms.level = 0;
        ms.depth = PAT_MAXDEPTH;
        const char *e = pat_match(&ms, src, pp);
        if (e && e != lastmatch) {
            count++;
            add_replacement(&ms, &out, src, e, repl, line);
            src = lastmatch = e;
        } else if (src < ms.src_end) {
            sb_addc(&out, *src++);
        } else {
            break;
        }
        if (anchor) break;
    }
    sb_add(&out, src);
    return v_str(sb_take(&out));
}

static Value str_split(Value *a, int n, int line) {
    check_argc("string.split", n, 1, 2, line);
    const char *s = need_string(a[0], line, "string.split");
    const char *sep = (n == 2) ? need_string(a[1], line, "string.split") : "";
    List *out = new_list();
    size_t seplen = strlen(sep);
    if (seplen == 0) {
        for (size_t i = 0; s[i]; i++) {
            char *ch = xmalloc(2);
            ch[0] = s[i];
            list_push(out, v_str(ch));
        }
        return v_list(out);
    }
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, sep);
        if (!q) {
            list_push(out, v_str(xstrdup(p)));
            break;
        }
        size_t len = (size_t)(q - p);
        char *piece = xmalloc(len + 1);
        memcpy(piece, p, len);
        piece[len] = '\0';
        list_push(out, v_str(piece));
        p = q + seplen;
    }
    return v_list(out);
}

static Value str_trim(Value *a, int n, int line) {
    check_argc("string.trim", n, 1, 1, line);
    const char *s = need_string(a[0], line, "string.trim");
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *o = xmalloc(len + 1);
    memcpy(o, s, len);
    o[len] = '\0';
    return v_str(o);
}

static Value str_replace(Value *a, int n, int line) {
    check_argc("string.replace", n, 3, 3, line);
    const char *s = need_string(a[0], line, "string.replace");
    const char *from = need_string(a[1], line, "string.replace");
    const char *to = need_string(a[2], line, "string.replace");
    size_t flen = strlen(from);
    SB sb = {NULL, 0, 0};
    if (flen == 0) return v_str(xstrdup(s));
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, from);
        if (!q) break;
        size_t len = (size_t)(q - p);
        char *piece = xmalloc(len + 1);
        memcpy(piece, p, len);
        piece[len] = '\0';
        sb_add(&sb, piece);
        sb_add(&sb, to);
        p = q + flen;
    }
    sb_add(&sb, p);
    return v_str(sb_take(&sb));
}

static Value str_starts(Value *a, int n, int line) {
    check_argc("string.starts_with", n, 2, 2, line);
    const char *s = need_string(a[0], line, "string.starts_with");
    const char *p = need_string(a[1], line, "string.starts_with");
    return v_bool(strncmp(s, p, strlen(p)) == 0);
}

static Value str_ends(Value *a, int n, int line) {
    check_argc("string.ends_with", n, 2, 2, line);
    const char *s = need_string(a[0], line, "string.ends_with");
    const char *p = need_string(a[1], line, "string.ends_with");
    size_t ls = strlen(s), lp = strlen(p);
    return v_bool(lp <= ls && strcmp(s + ls - lp, p) == 0);
}

static Value m_unary(Value *a, int n, int line, const char *name, double (*f)(double)) {
    check_argc(name, n, 1, 1, line);
    return v_num(f(need_num(a[0], line, name)));
}

static double d_floor(double x) { return floor(x); }
static double d_ceil(double x) { return ceil(x); }
static double d_abs(double x) { return fabs(x); }
static double d_sqrt(double x) { return sqrt(x); }
static double d_sin(double x) { return sin(x); }
static double d_cos(double x) { return cos(x); }
static double d_tan(double x) { return tan(x); }
static double d_exp(double x) { return exp(x); }
static double d_log(double x) { return log(x); }

static Value m_floor(Value *a, int n, int line) { return m_unary(a, n, line, "math.floor", d_floor); }
static Value m_ceil(Value *a, int n, int line) { return m_unary(a, n, line, "math.ceil", d_ceil); }
static Value m_abs(Value *a, int n, int line) { return m_unary(a, n, line, "math.abs", d_abs); }
static Value m_sqrt(Value *a, int n, int line) { return m_unary(a, n, line, "math.sqrt", d_sqrt); }
static Value m_sin(Value *a, int n, int line) { return m_unary(a, n, line, "math.sin", d_sin); }
static Value m_cos(Value *a, int n, int line) { return m_unary(a, n, line, "math.cos", d_cos); }
static Value m_tan(Value *a, int n, int line) { return m_unary(a, n, line, "math.tan", d_tan); }
static Value m_exp(Value *a, int n, int line) { return m_unary(a, n, line, "math.exp", d_exp); }
static Value m_log(Value *a, int n, int line) { return m_unary(a, n, line, "math.log", d_log); }

static Value m_pow(Value *a, int n, int line) {
    check_argc("math.pow", n, 2, 2, line);
    return v_num(pow(need_num(a[0], line, "math.pow"), need_num(a[1], line, "math.pow")));
}

static Value m_max(Value *a, int n, int line) {
    check_argc("math.max", n, 1, 1000, line);
    double best = need_num(a[0], line, "math.max");
    for (int i = 1; i < n; i++) {
        double v = need_num(a[i], line, "math.max");
        if (v > best) best = v;
    }
    return v_num(best);
}

static Value m_min(Value *a, int n, int line) {
    check_argc("math.min", n, 1, 1000, line);
    double best = need_num(a[0], line, "math.min");
    for (int i = 1; i < n; i++) {
        double v = need_num(a[i], line, "math.min");
        if (v < best) best = v;
    }
    return v_num(best);
}

static Value m_random(Value *a, int n, int line) {
    if (n == 0) return v_num(rand() / ((double)RAND_MAX + 1.0));
    check_argc("math.random", n, 1, 2, line);
    int lo = 1, hi;
    if (n == 1) {
        hi = safe_int(need_num(a[0], line, "math.random"));
    } else {
        lo = safe_int(need_num(a[0], line, "math.random"));
        hi = safe_int(need_num(a[1], line, "math.random"));
    }
    if (hi < lo) fail(line, "math.random: the range is empty");
    return v_num(lo + rand() % (hi - lo + 1));
}

static Value os_date(Value *a, int n, int line) {
    check_argc("os.date", n, 0, 1, line);
    const char *fmt = (n == 1) ? need_string(a[0], line, "os.date") : "%Y-%m-%d %H:%M:%S";
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    char buf[256];
    buf[0] = '\0';
    strftime(buf, sizeof buf, fmt, tmv);
    return v_str(xstrdup(buf));
}

static Value os_clock(Value *a, int n, int line) {
    (void)a;
    check_argc("os.clock", n, 0, 0, line);
    return v_num((double)clock() / CLOCKS_PER_SEC);
}

static Value os_time(Value *a, int n, int line) {
    (void)a;
    check_argc("os.time", n, 0, 0, line);
    return v_num((double)time(NULL));
}

static void sb_printf(SB *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = xmalloc((size_t)len + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);
    sb_add(b, buf);
}

static Value str_format(Value *a, int n, int line) {
    check_argc("string.format", n, 1, 1000, line);
    const char *f = need_string(a[0], line, "string.format");
    SB sb = {NULL, 0, 0};
    int argi = 1;
    for (const char *p = f; *p; p++) {
        if (*p != '%') {
            sb_addc(&sb, *p);
            continue;
        }
        p++;
        if (*p == '%') {
            sb_addc(&sb, '%');
            continue;
        }
        char spec[40];
        int si = 0;
        spec[si++] = '%';
        while (*p && strchr("-+ #0123456789.", *p) && si < 30) spec[si++] = *p++;
        spec[si] = '\0';
        if (!*p) fail(line, "string.format: the format ends too early");
        char conv = *p;
        if (argi >= n) fail(line, "string.format: not enough values for '%%%c'", conv);
        Value v = a[argi++];
        char f2[64];
        switch (conv) {
        case 'd':
        case 'i': {
            long long x = (long long)need_num(v, line, "string.format");
            snprintf(f2, sizeof f2, "%slld", spec);
            sb_printf(&sb, f2, x);
            break;
        }
        case 'x':
        case 'X':
        case 'o':
        case 'u': {
            unsigned long long x = (unsigned long long)(long long)need_num(v, line, "string.format");
            snprintf(f2, sizeof f2, "%sll%c", spec, conv);
            sb_printf(&sb, f2, x);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double x = need_num(v, line, "string.format");
            snprintf(f2, sizeof f2, "%s%c", spec, conv);
            sb_printf(&sb, f2, x);
            break;
        }
        case 'c': {
            int x = safe_int(need_num(v, line, "string.format"));
            snprintf(f2, sizeof f2, "%sc", spec);
            sb_printf(&sb, f2, x);
            break;
        }
        case 's': {
            char *t = to_text(v);
            snprintf(f2, sizeof f2, "%ss", spec);
            sb_printf(&sb, f2, t);
            break;
        }
        default:
            fail(line, "string.format: unknown format '%%%c'", conv);
        }
    }
    return v_str(sb_take(&sb));
}

static Value str_byte(Value *a, int n, int line) {
    check_argc("string.byte", n, 1, 2, line);
    const char *s = need_string(a[0], line, "string.byte");
    int i = (n == 2) ? safe_int(need_num(a[1], line, "string.byte")) : 0;
    if (i < 0 || i >= (int)strlen(s)) return v_nil();
    return v_num((unsigned char)s[i]);
}

static Value str_char(Value *a, int n, int line) {
    char *o = xmalloc((size_t)n + 1);
    for (int i = 0; i < n; i++)
        o[i] = (char)safe_int(need_num(a[i], line, "string.char"));
    o[n] = '\0';
    return v_str(o);
}

static Value m_fmod(Value *a, int n, int line) {
    check_argc("math.fmod", n, 2, 2, line);
    return v_num(fmod(need_num(a[0], line, "math.fmod"), need_num(a[1], line, "math.fmod")));
}

static Value m_randomseed(Value *a, int n, int line) {
    check_argc("math.randomseed", n, 1, 1, line);
    srand((unsigned)need_num(a[0], line, "math.randomseed"));
    return v_nil();
}

static Value os_getenv(Value *a, int n, int line) {
    check_argc("os.getenv", n, 1, 1, line);
    const char *v = getenv(need_string(a[0], line, "os.getenv"));
    return v ? v_str(xstrdup(v)) : v_nil();
}

static Value os_exit(Value *a, int n, int line) {
    check_argc("os.exit", n, 0, 1, line);
    int code = (n == 1) ? safe_int(need_num(a[0], line, "os.exit")) : 0;
    fflush(stdout);
    exit(code);
}

static Value io_write(Value *a, int n, int line) {
    (void)line;
    for (int i = 0; i < n; i++) fputs(to_text(a[i]), stdout);
    fflush(stdout);
    return v_nil();
}

static Value io_read(Value *a, int n, int line) {
    (void)a;
    check_argc("io.read", n, 0, 0, line);
    fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return v_nil();
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[--len] = '\0';
    return v_str(xstrdup(buf));
}

static char *try_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap + 1);
    for (;;) {
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (len < cap) break;
        cap *= 2;
        buf = xrealloc(buf, cap + 1);
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static Value fs_read(Value *a, int n, int line) {
    check_argc("fs.read", n, 1, 1, line);
    char *t = try_read(need_string(a[0], line, "fs.read"));
    return t ? v_str(t) : v_nil();
}

static Value fs_write(Value *a, int n, int line) {
    check_argc("fs.write", n, 2, 2, line);
    FILE *f = fopen(need_string(a[0], line, "fs.write"), "wb");
    if (!f) return v_bool(0);
    fputs(to_text(a[1]), f);
    fclose(f);
    return v_bool(1);
}

static Value fs_exists(Value *a, int n, int line) {
    check_argc("fs.exists", n, 1, 1, line);
    FILE *f = fopen(need_string(a[0], line, "fs.exists"), "rb");
    if (f) fclose(f);
    return v_bool(f != NULL);
}

static Value fs_remove(Value *a, int n, int line) {
    check_argc("fs.remove", n, 1, 1, line);
    return v_bool(remove(need_string(a[0], line, "fs.remove")) == 0);
}

static char *shell_quote(const char *s) {
    SB sb = {NULL, 0, 0};
    sb_addc(&sb, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_add(&sb, "'\\''");
        else sb_addc(&sb, *s);
    }
    sb_addc(&sb, '\'');
    return sb_take(&sb);
}

static Value run_command_output(const char *cmd, int line) {
    FILE *p = popen(cmd, "r");
    if (!p) fail(line, "could not start a network command");
    SB sb = {NULL, 0, 0};
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) {
        sb_reserve(&sb, got);
        memcpy(sb.s + sb.len, buf, got);
        sb.len += got;
        sb.s[sb.len] = '\0';
    }
    if (pclose(p) != 0) return v_nil();
    return v_str(sb_take(&sb));
}

static void require_web_address(const char *url, int line) {
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        fail(line, "only http:// and https:// addresses are allowed");
}

static Value http_get(Value *a, int n, int line) {
    check_argc("http.get", n, 1, 1, line);
    require_web_address(need_string(a[0], line, "http.get"), line);
    char *url = shell_quote(need_string(a[0], line, "http.get"));
    SB c = {NULL, 0, 0};
    sb_add(&c, "curl -sfL --max-time 20 ");
    sb_add(&c, url);
    return run_command_output(sb_take(&c), line);
}

static Value http_post(Value *a, int n, int line) {
    check_argc("http.post", n, 2, 2, line);
    require_web_address(need_string(a[0], line, "http.post"), line);
    char *url = shell_quote(need_string(a[0], line, "http.post"));
    char *body = shell_quote(to_text(a[1]));
    SB c = {NULL, 0, 0};
    sb_add(&c, "curl -sfL --max-time 300 -X POST -H 'Content-Type: application/json' --data-binary ");
    sb_add(&c, body);
    sb_addc(&c, ' ');
    sb_add(&c, url);
    return run_command_output(sb_take(&c), line);
}

static Node *parse_program(const char *src) {
    int count = 0;
    Token *toks = lex(src, &count);
    Parser ps = {toks, count, 0};
    Node *program = new_node(N_BLOCK, 1);
    while (peek(&ps)->type != T_EOF)
        push_node(program, parse_statement(&ps));
    return program;
}

static int ends_with_rab(const char *s) {
    size_t L = strlen(s);
    return L >= 4 && strcmp(s + L - 4, ".rab") == 0;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static char *with_rab(const char *name) {
    SB sb = {NULL, 0, 0};
    sb_add(&sb, name);
    if (!ends_with_rab(name)) sb_add(&sb, ".rab");
    return sb_take(&sb);
}

static char *in_dir(const char *dir, const char *name) {
    SB sb = {NULL, 0, 0};
    sb_add(&sb, dir);
    sb_add(&sb, "/");
    sb_add(&sb, with_rab(name));
    return sb_take(&sb);
}

static char *find_in(const char *dir, const char *name) {
    char *cand = in_dir(dir, name);
    return file_exists(cand) ? cand : NULL;
}

static char *resolve_module_path(const char *name) {
    const char *home = getenv("HOME");
    const char *lib_env = getenv("RABBIT_LIB");
    char *found = NULL;
    char buf[1200];

    if (name[0] == '/') return with_rab(name);
    if (name[0] == '~' && name[1] == '/' && home) return in_dir(home, name + 2);

    if ((found = find_in(g_script_dir, name))) return found;
    snprintf(buf, sizeof buf, "%s/lib", g_script_dir);
    if ((found = find_in(buf, name))) return found;
    snprintf(buf, sizeof buf, "%s/../lib", g_script_dir);
    if ((found = find_in(buf, name))) return found;
    if ((found = find_in(".", name))) return found;
    if ((found = find_in("./lib", name))) return found;
    if (lib_env && (found = find_in(lib_env, name))) return found;
    if ((found = find_in("/usr/local/share/rabbit/lib", name))) return found;
    if ((found = find_in("/usr/share/rabbit/lib", name))) return found;
    return in_dir(g_script_dir, name);
}

static Value load_module_file(const char *path, int line) {
    Value *cached = map_find(g_modules, v_str((char *)path));
    if (cached) return *cached;
    if (map_find(g_loading, v_str((char *)path)))
        fail(line, "circular require: '%s' is already being loaded", path);
    char *src = try_read(path);
    if (!src) fail(line, "can't find the module '%s'", path);
    map_set(g_loading, v_str((char *)path), v_bool(1));
    const char *old_file = g_file;
    char old_dir[1024];
    memcpy(old_dir, g_script_dir, sizeof old_dir);
    g_file = path;
    size_t plen = strlen(path);
    if (plen >= sizeof g_script_dir) plen = sizeof g_script_dir - 1;
    memcpy(g_script_dir, path, plen);
    g_script_dir[plen] = '\0';
    char *mslash = strrchr(g_script_dir, '/');
    if (mslash) *mslash = '\0';
    else strcpy(g_script_dir, ".");
    Node *prog = parse_program(src);
    Env *menv = new_env(g_globals);
    int sig = exec_block(prog, menv);
    g_file = old_file;
    memcpy(g_script_dir, old_dir, sizeof old_dir);
    map_set(g_loading, v_str((char *)path), v_nil());
    Value result = (sig == SIG_RETURN) ? g_return : v_nil();
    g_return = v_nil();
    map_set(g_modules, v_str((char *)path), result);
    return result;
}

static Value lib_require(Value *a, int n, int line) {
    check_argc("require", n, 1, 1, line);
    const char *name = need_string(a[0], line, "require");
    return load_module_file(resolve_module_path(name), line);
}

static Value lib_getlib(Value *a, int n, int line) {
    check_argc("getlib", n, 1, 1, line);
    const char *name = need_string(a[0], line, "getlib");
    Value *lib = map_find(g_builtin_libs, v_str((char *)name));
    if (lib) return *lib;
    return load_module_file(resolve_module_path(name), line);
}

static Value lib_load(Value *a, int n, int line) {
    check_argc("load", n, 1, 1, line);
    const char *src = need_string(a[0], line, "load");
    Node *fn = new_node(N_FUNC, line);
    fn->name = (char *)"chunk";
    fn->b = parse_program(src);
    return make_closure(fn, g_globals);
}

static Value lib_pcall(Value *a, int n, int line) {
    check_argc("pcall", n, 1, 1000, line);
    PcallFrame frame;
    frame.prev = g_pcall_top;
    frame.depth = g_call_depth;
    g_pcall_top = &frame;
    Map *m = new_map();
    if (setjmp(frame.jb) == 0) {
        Value result = call_callable(a[0], a + 1, n - 1, line);
        g_pcall_top = frame.prev;
        map_set(m, v_str((char *)"ok"), v_bool(1));
        map_set(m, v_str((char *)"value"), result);
    } else {
        g_pcall_top = frame.prev;
        g_call_depth = frame.depth;
        map_set(m, v_str((char *)"ok"), v_bool(0));
        map_set(m, v_str((char *)"error"), v_str(g_pcall_err));
    }
    return v_map(m);
}

static Value lib_rawget(Value *a, int n, int line) {
    check_argc("rawget", n, 2, 2, line);
    return index_value(a[0], a[1], line);
}

static Value lib_rawset(Value *a, int n, int line) {
    check_argc("rawset", n, 3, 3, line);
    set_index(a[0], a[1], a[2], line);
    return a[0];
}

static Value lib_rawequal(Value *a, int n, int line) {
    check_argc("rawequal", n, 2, 2, line);
    return v_bool(values_equal(a[0], a[1]));
}

static Value io_getchar(Value *a, int n, int line) {
    (void)a;
    check_argc("io.getchar", n, 0, 0, line);
    fflush(stdout);
    int c = getchar();
    if (c == EOF) return v_nil();
    char *s = xmalloc(2);
    s[0] = (char)c;
    return v_str(s);
}

static Value os_execute(Value *a, int n, int line) {
    check_argc("os.execute", n, 1, 1, line);
    fflush(stdout);
    int st = system(need_string(a[0], line, "os.execute"));
    if (st == -1) return v_nil();
    return v_num(WIFEXITED(st) ? WEXITSTATUS(st) : st);
}

static Value os_capture(Value *a, int n, int line) {
    check_argc("os.capture", n, 1, 1, line);
    fflush(stdout);
    return run_command_output(need_string(a[0], line, "os.capture"), line);
}

static char *dup_range(const char *s, size_t len) {
    char *o = xmalloc(len + 1);
    memcpy(o, s, len);
    o[len] = '\0';
    return o;
}

static char *lower_text(const char *s) {
    size_t len = strlen(s);
    char *o = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) o[i] = (char)tolower((unsigned char)s[i]);
    o[len] = '\0';
    return o;
}

static char *trim_range(const char *s, size_t len) {
    while (len > 0 && isspace((unsigned char)*s)) { s++; len--; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    return dup_range(s, len);
}

static const char *status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

static void send_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t w = send(fd, data, len, 0);
        if (w <= 0) return;
        data += w;
        len -= (size_t)w;
    }
}

static int read_request(int fd, char **method, char **target, Map **headers,
                        char **body, size_t *body_len) {
    SB buf = {NULL, 0, 0};
    char chunk[4096];
    size_t header_end = 0;
    for (;;) {
        ssize_t r = recv(fd, chunk, sizeof chunk, 0);
        if (r <= 0) return 0;
        sb_reserve(&buf, (size_t)r);
        memcpy(buf.s + buf.len, chunk, (size_t)r);
        buf.len += (size_t)r;
        buf.s[buf.len] = '\0';
        char *p = strstr(buf.s, "\r\n\r\n");
        if (p) {
            header_end = (size_t)(p - buf.s) + 4;
            break;
        }
        if (buf.len > 65536) return 0;
    }

    char *line_end = strstr(buf.s, "\r\n");
    char *sp1 = memchr(buf.s, ' ', (size_t)(line_end - buf.s));
    if (!sp1) return 0;
    char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) return 0;
    *method = dup_range(buf.s, (size_t)(sp1 - buf.s));
    *target = dup_range(sp1 + 1, (size_t)(sp2 - sp1 - 1));

    *headers = new_map();
    char *cur = line_end + 2;
    char *block_end = buf.s + header_end - 2;
    while (cur < block_end) {
        char *eol = strstr(cur, "\r\n");
        if (!eol) eol = block_end;
        char *colon = memchr(cur, ':', (size_t)(eol - cur));
        if (colon) {
            char *key = lower_text(trim_range(cur, (size_t)(colon - cur)));
            char *val = trim_range(colon + 1, (size_t)(eol - colon - 1));
            map_set(*headers, v_str(key), v_str(val));
        }
        cur = eol + 2;
    }

    size_t want = 0;
    Value *cl = map_find(*headers, v_str((char *)"content-length"));
    if (cl && cl->type == V_STR) want = (size_t)strtoul(cl->str, NULL, 10);
    if (want > 1048576) return 0;

    *body = xmalloc(want + 1);
    size_t have = buf.len - header_end;
    if (have > want) have = want;
    memcpy(*body, buf.s + header_end, have);
    while (have < want) {
        ssize_t r = recv(fd, *body + have, want - have, 0);
        if (r <= 0) return 0;
        have += (size_t)r;
    }
    (*body)[want] = '\0';
    *body_len = want;
    return 1;
}

static int call_protected(Value f, Value *args, int argc, int line, Value *out, char **err) {
    PcallFrame frame;
    frame.prev = g_pcall_top;
    frame.depth = g_call_depth;
    g_pcall_top = &frame;
    if (setjmp(frame.jb) == 0) {
        *out = call_callable(f, args, argc, line);
        g_pcall_top = frame.prev;
        return 1;
    }
    g_pcall_top = frame.prev;
    g_call_depth = frame.depth;
    *err = g_pcall_err;
    return 0;
}

static char *header_safe(const char *s) {
    size_t len = strlen(s);
    char *o = xmalloc(len + 1);
    for (size_t i = 0; i < len; i++) o[i] = (s[i] == '\r' || s[i] == '\n') ? ' ' : s[i];
    o[len] = '\0';
    return o;
}

static void write_response(int fd, int code, const char *ctype, const char *body,
                           Map *extra) {
    SB head = {NULL, 0, 0};
    char line[256];
    size_t blen = strlen(body);
    snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", code, status_text(code));
    sb_add(&head, line);
    snprintf(line, sizeof line, "Content-Type: %s\r\n", ctype);
    sb_add(&head, line);
    snprintf(line, sizeof line, "Content-Length: %zu\r\n", blen);
    sb_add(&head, line);
    sb_add(&head, "Connection: close\r\n");
    if (extra) {
        for (int i = 0; i < extra->count; i++) {
            char *k = header_safe(to_text(extra->keys[i]));
            char *v = header_safe(to_text(extra->vals[i]));
            sb_add(&head, k);
            sb_add(&head, ": ");
            sb_add(&head, v);
            sb_add(&head, "\r\n");
        }
    }
    sb_add(&head, "\r\n");
    send_all(fd, head.s, head.len);
    send_all(fd, body, blen);
}

static void respond_with(int fd, Value result, const char *method, const char *target,
                         int *code_out) {
    int code = 200;
    const char *ctype = "text/plain; charset=utf-8";
    char *body = "";
    Map *extra = NULL;

    if (result.type == V_STR) {
        body = result.str;
        if (result.str[0] == '<') ctype = "text/html; charset=utf-8";
    } else if (result.type == V_TABLE) {
        Value *st = map_find(result.map, v_str((char *)"status"));
        if (st && st->type == V_NUM) code = safe_int(st->num);
        Value *bd = map_find(result.map, v_str((char *)"body"));
        if (bd && bd->type != V_NIL) body = to_text(*bd);
        Value *ty = map_find(result.map, v_str((char *)"type"));
        if (ty && ty->type == V_STR) ctype = ty->str;
        Value *hd = map_find(result.map, v_str((char *)"headers"));
        if (hd && hd->type == V_TABLE) extra = hd->map;
    } else if (result.type != V_NIL) {
        body = to_text(result);
    }
    write_response(fd, code, ctype, body, extra);
    fprintf(stderr, "%s %s %d\n", method, target, code);
    *code_out = code;
}

static void handle_connection(int fd, Value handler, int line) {
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char *method = NULL, *target = NULL, *body = NULL;
    Map *headers = NULL;
    size_t blen = 0;
    if (!read_request(fd, &method, &target, &headers, &body, &blen)) {
        write_response(fd, 400, "text/plain; charset=utf-8", "Bad Request", NULL);
        return;
    }

    char *path = target;
    char *query = "";
    char *qmark = strchr(target, '?');
    if (qmark) {
        path = dup_range(target, (size_t)(qmark - target));
        query = qmark + 1;
    }
    Map *req = new_map();
    map_set(req, v_str((char *)"method"), v_str(method));
    map_set(req, v_str((char *)"path"), v_str(path));
    map_set(req, v_str((char *)"query"), v_str(query));
    map_set(req, v_str((char *)"headers"), v_map(headers));
    map_set(req, v_str((char *)"body"), v_str(body));

    Value args[1];
    args[0] = v_map(req);
    Value result = v_nil();
    char *err = NULL;
    if (call_protected(handler, args, 1, line, &result, &err)) {
        int code = 200;
        respond_with(fd, result, method, target, &code);
    } else {
        fprintf(stderr, "rabbit: error in request handler: %s\n", err);
        write_response(fd, 500, "text/plain; charset=utf-8", "Internal Server Error", NULL);
        fprintf(stderr, "%s %s 500\n", method, target);
    }
}

static Value net_serve(Value *a, int n, int line) {
    check_argc("net.serve", n, 2, 4, line);
    int port = safe_int(need_num(a[0], line, "net.serve"));
    if (a[1].type != V_FUNC && a[1].type != V_NATIVE)
        fail(line, "net.serve needs a handler function as its second argument");
    const char *host = (n >= 3 && a[2].type == V_STR) ? a[2].str : "127.0.0.1";
    long max = (n >= 4 && a[3].type == V_NUM) ? (long)a[3].num : -1;

    signal(SIGPIPE, SIG_IGN);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) fail(line, "could not create a network socket");
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        fail(line, "net.serve: '%s' is not a valid IP address (try 127.0.0.1)", host);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0)
        fail(line, "could not listen on %s:%d (is the port already in use?)", host, port);
    if (listen(srv, 64) < 0) fail(line, "could not start listening");

    fflush(stdout);
    fprintf(stderr, "rabbit: serving on http://%s:%d  (Ctrl+C to stop)\n", host, port);
    long served = 0;
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        handle_connection(c, a[1], line);
        close(c);
        served++;
        if (max >= 0 && served >= max) break;
    }
    close(srv);
    return v_nil();
}

static Value os_sleep(Value *a, int n, int line) {
    check_argc("os.sleep", n, 1, 1, line);
    double secs = need_num(a[0], line, "os.sleep");
    if (secs < 0) secs = 0;
    fflush(stdout);
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) == -1) {}
    return v_nil();
}

static Value env_getglobal(Value *a, int n, int line) {
    check_argc("getglobal", n, 1, 1, line);
    Var *v = lookup(g_globals, intern(need_string(a[0], line, "getglobal")));
    return v ? v->val : v_nil();
}

static Value env_setglobal(Value *a, int n, int line) {
    check_argc("setglobal", n, 2, 2, line);
    define_interned(g_globals, intern(need_string(a[0], line, "setglobal")), a[1]);
    return a[1];
}

static Value env_hasglobal(Value *a, int n, int line) {
    check_argc("hasglobal", n, 1, 1, line);
    return v_bool(lookup(g_globals, intern(need_string(a[0], line, "hasglobal"))) != NULL);
}

static Value env_getgenv(Value *a, int n, int line) {
    (void)a;
    check_argc("getgenv", n, 0, 0, line);
    Map *m = new_map();
    for (Var *v = g_globals->vars; v; v = v->next)
        map_set(m, v_str((char *)v->name), v->val);
    return v_map(m);
}

static Value env_setgenv(Value *a, int n, int line) {
    check_argc("setgenv", n, 1, 1, line);
    Map *m = need_table(a[0], line, "setgenv");
    for (int i = 0; i < m->count; i++) {
        if (m->keys[i].type != V_STR)
            fail(line, "setgenv: every key must be text, but found a %s", type_name(m->keys[i]));
        define_interned(g_globals, intern(m->keys[i].str), m->vals[i]);
    }
    return v_nil();
}

static int cmp_names(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

static Value env_globals(Value *a, int n, int line) {
    (void)a;
    check_argc("globals", n, 0, 0, line);
    int count = 0;
    for (Var *v = g_globals->vars; v; v = v->next) count++;
    const char **names = xmalloc(sizeof(char *) * (size_t)(count + 1));
    int i = 0;
    for (Var *v = g_globals->vars; v; v = v->next) names[i++] = v->name;
    qsort(names, (size_t)count, sizeof(char *), cmp_names);
    List *out = new_list();
    for (i = 0; i < count; i++) list_push(out, v_str((char *)names[i]));
    return v_list(out);
}

static Value tbl_find(Value *a, int n, int line) {
    check_argc("table.find", n, 2, 2, line);
    List *l = need_list(a[0], line, "table.find");
    for (int i = 0; i < l->count; i++)
        if (values_equal(l->items[i], a[1])) return v_num(i);
    return v_nil();
}

static Value tbl_contains(Value *a, int n, int line) {
    check_argc("table.contains", n, 2, 2, line);
    List *l = need_list(a[0], line, "table.contains");
    for (int i = 0; i < l->count; i++)
        if (values_equal(l->items[i], a[1])) return v_bool(1);
    return v_bool(0);
}

static Value tbl_map(Value *a, int n, int line) {
    check_argc("table.map", n, 2, 2, line);
    List *l = need_list(a[0], line, "table.map");
    List *out = new_list();
    for (int i = 0; i < l->count; i++) {
        Value args[1];
        args[0] = l->items[i];
        list_push(out, call_callable(a[1], args, 1, line));
    }
    return v_list(out);
}

static Value tbl_filter(Value *a, int n, int line) {
    check_argc("table.filter", n, 2, 2, line);
    List *l = need_list(a[0], line, "table.filter");
    List *out = new_list();
    for (int i = 0; i < l->count; i++) {
        Value args[1];
        args[0] = l->items[i];
        if (truthy(call_callable(a[1], args, 1, line))) list_push(out, l->items[i]);
    }
    return v_list(out);
}

static Value tbl_reduce(Value *a, int n, int line) {
    check_argc("table.reduce", n, 2, 3, line);
    List *l = need_list(a[0], line, "table.reduce");
    int i = 0;
    Value acc;
    if (n == 3) {
        acc = a[2];
    } else {
        if (l->count == 0) return v_nil();
        acc = l->items[0];
        i = 1;
    }
    for (; i < l->count; i++) {
        Value args[2];
        args[0] = acc;
        args[1] = l->items[i];
        acc = call_callable(a[1], args, 2, line);
    }
    return acc;
}

static Value str_pad(Value *a, int n, int line, int left) {
    check_argc(left ? "string.lpad" : "string.rpad", n, 2, 3, line);
    const char *s = need_string(a[0], line, "string.pad");
    int width = safe_int(need_num(a[1], line, "string.pad"));
    const char *fill = (n == 3) ? need_string(a[2], line, "string.pad") : " ";
    if (fill[0] == '\0') fill = " ";
    size_t len = strlen(s);
    if (width <= (int)len) return v_str(xstrdup(s));
    size_t padn = (size_t)width - len;
    SB sb = {NULL, 0, 0};
    if (!left) sb_add(&sb, s);
    for (size_t i = 0; i < padn; i++) sb_addc(&sb, fill[i % strlen(fill)]);
    if (left) sb_add(&sb, s);
    return v_str(sb_take(&sb));
}
static Value str_lpad(Value *a, int n, int line) { return str_pad(a, n, line, 1); }
static Value str_rpad(Value *a, int n, int line) { return str_pad(a, n, line, 0); }

static Value m_clamp(Value *a, int n, int line) {
    check_argc("math.clamp", n, 3, 3, line);
    double x = need_num(a[0], line, "math.clamp");
    double lo = need_num(a[1], line, "math.clamp");
    double hi = need_num(a[2], line, "math.clamp");
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return v_num(x);
}

static Value m_round(Value *a, int n, int line) {
    check_argc("math.round", n, 1, 1, line);
    return v_num(floor(need_num(a[0], line, "math.round") + 0.5));
}

static Value fs_mkdir(Value *a, int n, int line) {
    check_argc("fs.mkdir", n, 1, 1, line);
    return v_bool(mkdir(need_string(a[0], line, "fs.mkdir"), 0755) == 0);
}

static Value fs_list(Value *a, int n, int line) {
    check_argc("fs.list", n, 1, 1, line);
    DIR *d = opendir(need_string(a[0], line, "fs.list"));
    if (!d) return v_nil();
    int cap = 16, count = 0;
    char **names = xmalloc(sizeof(char *) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (count == cap) {
            cap *= 2;
            names = xrealloc(names, sizeof(char *) * (size_t)cap);
        }
        names[count++] = xstrdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof(char *), cmp_names);
    List *out = new_list();
    for (int i = 0; i < count; i++) list_push(out, v_str(names[i]));
    return v_list(out);
}

static Value os_platform(Value *a, int n, int line) {
    (void)a;
    check_argc("os.platform", n, 0, 0, line);
#if defined(__APPLE__)
    return v_str(xstrdup("macos"));
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return v_str(xstrdup("bsd"));
#elif defined(__linux__)
    return v_str(xstrdup("linux"));
#else
    return v_str(xstrdup("other"));
#endif
}

static void lib_add(Map *m, const char *name, NativeFn f) {
    map_set(m, v_str((char *)name), v_native(f));
}

static void lib_set(Map *m, const char *name, Value v) {
    map_set(m, v_str((char *)name), v);
}

static void init_globals(void) {
    g_globals = new_env(NULL);

    g_table_lib = new_map();
    lib_add(g_table_lib, "insert", tbl_insert);
    lib_add(g_table_lib, "remove", tbl_remove);
    lib_add(g_table_lib, "concat", tbl_concat);
    lib_add(g_table_lib, "sort", tbl_sort);
    lib_add(g_table_lib, "keys", tbl_keys);
    lib_add(g_table_lib, "values", tbl_values);

    g_string_lib = new_map();
    lib_add(g_string_lib, "len", str_len);
    lib_add(g_string_lib, "upper", str_upper);
    lib_add(g_string_lib, "lower", str_lower);
    lib_add(g_string_lib, "sub", str_sub);
    lib_add(g_string_lib, "rep", str_rep);
    lib_add(g_string_lib, "reverse", str_reverse);
    lib_add(g_string_lib, "find", str_find);
    lib_add(g_string_lib, "split", str_split);
    lib_add(g_string_lib, "trim", str_trim);
    lib_add(g_string_lib, "replace", str_replace);
    lib_add(g_string_lib, "starts_with", str_starts);
    lib_add(g_string_lib, "ends_with", str_ends);

    g_math_lib = new_map();
    lib_add(g_math_lib, "floor", m_floor);
    lib_add(g_math_lib, "ceil", m_ceil);
    lib_add(g_math_lib, "abs", m_abs);
    lib_add(g_math_lib, "sqrt", m_sqrt);
    lib_add(g_math_lib, "sin", m_sin);
    lib_add(g_math_lib, "cos", m_cos);
    lib_add(g_math_lib, "tan", m_tan);
    lib_add(g_math_lib, "exp", m_exp);
    lib_add(g_math_lib, "log", m_log);
    lib_add(g_math_lib, "pow", m_pow);
    lib_add(g_math_lib, "max", m_max);
    lib_add(g_math_lib, "min", m_min);
    lib_add(g_math_lib, "random", m_random);
    lib_set(g_math_lib, "pi", v_num(3.14159265358979323846));
    lib_set(g_math_lib, "huge", v_num(HUGE_VAL));

    g_os_lib = new_map();
    lib_add(g_os_lib, "clock", os_clock);
    lib_add(g_os_lib, "time", os_time);

    g_array_methods = new_map();
    lib_add(g_array_methods, "push", tbl_insert);
    lib_add(g_array_methods, "pop", tbl_remove);
    lib_add(g_array_methods, "join", tbl_concat);
    lib_add(g_array_methods, "sort", tbl_sort);

    g_modules = new_map();
    g_loading = new_map();
    g_builtin_libs = new_map();

    g_net_lib = new_map();
    lib_add(g_net_lib, "serve", net_serve);
    lib_set(g_builtin_libs, "net", v_map(g_net_lib));
    define(g_globals, "net", v_map(g_net_lib));

    g_http_lib = new_map();
    lib_add(g_http_lib, "get", http_get);
    lib_add(g_http_lib, "post", http_post);

    g_fs_lib = new_map();
    lib_add(g_fs_lib, "read", fs_read);
    lib_add(g_fs_lib, "write", fs_write);
    lib_add(g_fs_lib, "exists", fs_exists);
    lib_add(g_fs_lib, "remove", fs_remove);

    g_io_lib = new_map();
    lib_add(g_io_lib, "write", io_write);
    lib_add(g_io_lib, "read", io_read);
    lib_add(g_table_lib, "find", tbl_find);
    lib_add(g_table_lib, "contains", tbl_contains);
    lib_add(g_table_lib, "map", tbl_map);
    lib_add(g_table_lib, "filter", tbl_filter);
    lib_add(g_table_lib, "reduce", tbl_reduce);
    lib_add(g_string_lib, "lpad", str_lpad);
    lib_add(g_string_lib, "rpad", str_rpad);
    lib_add(g_math_lib, "clamp", m_clamp);
    lib_add(g_math_lib, "round", m_round);
    lib_add(g_fs_lib, "mkdir", fs_mkdir);
    lib_add(g_fs_lib, "list", fs_list);
    lib_add(g_os_lib, "platform", os_platform);
    define(g_globals, "getglobal", v_native(env_getglobal));
    define(g_globals, "setglobal", v_native(env_setglobal));
    define(g_globals, "hasglobal", v_native(env_hasglobal));
    define(g_globals, "getgenv", v_native(env_getgenv));
    define(g_globals, "setgenv", v_native(env_setgenv));
    define(g_globals, "globals", v_native(env_globals));
    lib_add(g_io_lib, "getchar", io_getchar);
    lib_add(g_os_lib, "execute", os_execute);
    lib_add(g_os_lib, "capture", os_capture);

    lib_add(g_string_lib, "format", str_format);
    lib_add(g_string_lib, "match", str_match);
    lib_add(g_string_lib, "gmatch", str_gmatch);
    lib_add(g_string_lib, "gsub", str_gsub);
    lib_add(g_os_lib, "date", os_date);
    lib_add(g_string_lib, "byte", str_byte);
    lib_add(g_string_lib, "char", str_char);
    lib_add(g_math_lib, "fmod", m_fmod);
    lib_add(g_math_lib, "randomseed", m_randomseed);
    lib_add(g_os_lib, "getenv", os_getenv);
    lib_add(g_os_lib, "exit", os_exit);
    lib_add(g_os_lib, "sleep", os_sleep);
    define(g_globals, "wait", v_native(os_sleep));

    lib_set(g_builtin_libs, "table", v_map(g_table_lib));
    lib_set(g_builtin_libs, "string", v_map(g_string_lib));
    lib_set(g_builtin_libs, "math", v_map(g_math_lib));
    lib_set(g_builtin_libs, "os", v_map(g_os_lib));
    lib_set(g_builtin_libs, "io", v_map(g_io_lib));
    lib_set(g_builtin_libs, "http", v_map(g_http_lib));
    lib_set(g_builtin_libs, "fs", v_map(g_fs_lib));

    define(g_globals, "io", v_map(g_io_lib));
    define(g_globals, "http", v_map(g_http_lib));
    define(g_globals, "fs", v_map(g_fs_lib));
    define(g_globals, "require", v_native(lib_require));
    define(g_globals, "getlib", v_native(lib_getlib));
    define(g_globals, "load", v_native(lib_load));
    define(g_globals, "pcall", v_native(lib_pcall));
    define(g_globals, "rawget", v_native(lib_rawget));
    define(g_globals, "rawset", v_native(lib_rawset));
    define(g_globals, "rawequal", v_native(lib_rawequal));

    define(g_globals, "print", v_native(lib_print));
    define(g_globals, "input", v_native(lib_input));
    define(g_globals, "type", v_native(lib_type));
    define(g_globals, "tostring", v_native(lib_tostring));
    define(g_globals, "tonumber", v_native(lib_tonumber));
    define(g_globals, "error", v_native(lib_error));
    define(g_globals, "assert", v_native(lib_assert));
    define(g_globals, "pairs", v_native(lib_pairs));
    define(g_globals, "ipairs", v_native(lib_pairs));

    define(g_globals, "table", v_map(g_table_lib));
    define(g_globals, "string", v_map(g_string_lib));
    define(g_globals, "math", v_map(g_math_lib));
    define(g_globals, "os", v_map(g_os_lib));

    srand((unsigned)time(NULL));
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "rabbit: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) len = 0;
    char *buf = xmalloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void usage(FILE *out) {
    fprintf(out,
            "Rabbit %s - a small, friendly programming language\n\n"
            "usage: rabbit <file.rab>\n"
            "       rabbit --version\n"
            "       rabbit --help\n",
            RABBIT_VERSION);
}

int main(int argc, char **argv) {
    if (!isatty(fileno(stdout))) setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("rabbit %s\n", RABBIT_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "--check") == 0) {
        if (argc < 3) {
            usage(stderr);
            return 1;
        }
        if (strcmp(argv[2], "-") == 0) {

            size_t cap = 65536, len = 0;
            char *buf = xmalloc(cap + 1);
            for (;;) {
                size_t got = fread(buf + len, 1, cap - len, stdin);
                len += got;
                if (len < cap) break;
                cap *= 2;
                buf = xrealloc(buf, cap + 1);
            }
            buf[len] = '\0';
            g_file = "<editor>";
            parse_program(buf);
            return 0;
        }
        g_file = argv[2];
        parse_program(read_file(argv[2]));
        return 0;
    }

    g_file = argv[1];
    strncpy(g_script_dir, argv[1], sizeof g_script_dir - 1);
    g_script_dir[sizeof g_script_dir - 1] = '\0';
    char *slash = strrchr(g_script_dir, '/');
    if (slash) *slash = '\0';
    else strcpy(g_script_dir, ".");
    char *src = read_file(argv[1]);

    int count = 0;
    Token *toks = lex(src, &count);

    Parser ps = {toks, count, 0};
    Node *program = new_node(N_BLOCK, 1);
    while (peek(&ps)->type != T_EOF)
        push_node(program, parse_statement(&ps));

    init_globals();
    int sig = exec_block(program, g_globals);
    if (sig == SIG_BREAK || sig == SIG_CONTINUE)
        fail(0, "'break' or 'continue' used outside of a loop");

    return 0;
}
