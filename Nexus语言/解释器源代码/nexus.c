/* nexus.c — the Nexus language, implemented in C11 with the C standard
 * library only.  No host process, no Python/JS runtime: lexer, parser,
 * evaluator, value model, JSON, store, clock and program loader are all
 * implemented here in C.  The engine is a standalone CLI peer of nexus.py.
 *
 * Build:  .\build-c.ps1        (MSVC, falls back to gcc/clang)
 * Usage:  bin\nexus.exe run <file> [<request-json>|-]
 *         bin\nexus.exe ast <file> | tokens <file> | test | repl | version
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#define LANG_NAME "nexus"
#define LANG_VERSION "1.1.0"
#define FILE_EXT ".nx"
#define MAX_SAFE 9007199254740992LL
#define MAX_DEPTH 300
#define MAX_LOOP 5000000L

static void die_oom(void) {
    fputs("nexus: out of memory\n", stderr);
    exit(3);
}
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}
static char *xstrndup(const char *s, size_t n) {
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
static char *xasprintf(const char *fmt, ...) {
    va_list ap;
    int n;
    char *p;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    p = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

typedef struct {
    char *p;
    size_t n, cap;
} Buf;
static void buf_init(Buf *b) { b->p = NULL; b->n = b->cap = 0; }
static void buf_reserve(Buf *b, size_t extra) {
    if (b->n + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->n + extra + 1) cap *= 2;
        b->p = (char *)xrealloc(b->p, cap);
        b->cap = cap;
    }
}
static void buf_addn(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }
static void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }
static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}
static char *buf_take(Buf *b) { return b->p ? b->p : xstrdup(""); }

static int u8_decode(const char *s, size_t n, unsigned long *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && n >= 2) {
        *cp = ((unsigned long)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && n >= 3) {
        *cp = ((unsigned long)(c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) |
              ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && n >= 4) {
        *cp = ((unsigned long)(c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
              (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}
static size_t u8_len(const char *s) {
    size_t i = 0, n = 0, len = strlen(s);
    while (i < len) {
        unsigned long cp;
        i += (size_t)u8_decode(s + i, len - i, &cp);
        n++;
    }
    return n;
}
static size_t u8_offsets(const char *s, size_t *off) {
    size_t i = 0, n = 0, len = strlen(s);
    while (i < len) {
        unsigned long cp;
        int k;
        if (off) off[n] = i;
        k = u8_decode(s + i, len - i, &cp);
        i += (size_t)k;
        n++;
    }
    if (off) off[n] = len;
    return n;
}
static char *u8_substr(const char *s, size_t a, size_t b) {
    size_t n = u8_len(s), *off = (size_t *)xmalloc((n + 1) * sizeof(size_t));
    char *out;
    u8_offsets(s, off);
    if (a > n) a = n;
    if (b > n) b = n;
    out = xstrndup(s + off[a], off[b] - off[a]);
    free(off);
    return out;
}
static unsigned long u8_at(const char *s, size_t idx) {
    size_t len = strlen(s), i = 0, k = 0;
    while (i < len) {
        unsigned long cp;
        int d = u8_decode(s + i, len - i, &cp);
        if (k == idx) return cp;
        i += (size_t)d;
        k++;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
typedef struct Value Value;
typedef struct Env Env;
typedef struct Func Func;
typedef struct Native Native;
typedef struct Interp Interp;
typedef struct Host Host;
typedef struct Ast Ast;

typedef enum { V_VOID = 0, V_BOOL, V_INT, V_FLOAT, V_STR, V_LIST, V_REC, V_FUNC, V_NATIVE } VType;

struct Value {
    VType t;
    union {
        int b;
        long long i;
        double f;
        struct { char *p; size_t n; } s;
        struct { Value *v; size_t n, cap; } list;
        struct { char **k; Value *v; size_t n, cap; } rec;
        Func *fn;
        Native *nat;
    } as;
};

struct Func {
    char *name;
    char **params;
    size_t nparams;
    Ast **body;
    size_t nbody;
    Env *env;
};

typedef Value (*NativeFn)(Interp *, Value *, size_t, void *);

struct Native {
    char *name;
    NativeFn fn;
    void *ud;
};

static Value v_void(void) { Value v; memset(&v, 0, sizeof(v)); v.t = V_VOID; return v; }
static Value v_bool(int b) { Value v; memset(&v, 0, sizeof(v)); v.t = V_BOOL; v.as.b = b ? 1 : 0; return v; }
static Value v_int(long long i) { Value v; memset(&v, 0, sizeof(v)); v.t = V_INT; v.as.i = i; return v; }
static Value v_float(double f) { Value v; memset(&v, 0, sizeof(v)); v.t = V_FLOAT; v.as.f = f; return v; }
static Value v_str_take(char *p) { Value v; memset(&v, 0, sizeof(v)); v.t = V_STR; v.as.s.p = p; v.as.s.n = strlen(p); return v; }
static Value v_str(const char *s) { return v_str_take(xstrdup(s)); }
static Value v_list(void) { Value v; memset(&v, 0, sizeof(v)); v.t = V_LIST; return v; }
static Value v_rec(void) { Value v; memset(&v, 0, sizeof(v)); v.t = V_REC; return v; }
static Value v_func(Func *f) { Value v; memset(&v, 0, sizeof(v)); v.t = V_FUNC; v.as.fn = f; return v; }

static void list_push(Value *lst, Value item) {
    if (lst->as.list.n + 1 > lst->as.list.cap) {
        size_t cap = lst->as.list.cap ? lst->as.list.cap * 2 : 8;
        lst->as.list.v = (Value *)xrealloc(lst->as.list.v, cap * sizeof(Value));
        lst->as.list.cap = cap;
    }
    lst->as.list.v[lst->as.list.n++] = item;
}
static long rec_find(Value *rec, const char *key) {
    size_t i;
    if (rec->t != V_REC) return -1;
    for (i = 0; i < rec->as.rec.n; i++)
        if (strcmp(rec->as.rec.k[i], key) == 0) return (long)i;
    return -1;
}
static Value rec_get(Value *rec, const char *key) {
    long i;
    if (rec->t != V_REC) return v_void();
    i = rec_find(rec, key);
    return i < 0 ? v_void() : rec->as.rec.v[i];
}
static void rec_set(Value *rec, const char *key, Value val) {
    long i;
    if (rec->t != V_REC) return;
    i = rec_find(rec, key);
    if (i >= 0) { rec->as.rec.v[i] = val; return; }
    if (rec->as.rec.n + 1 > rec->as.rec.cap) {
        size_t cap = rec->as.rec.cap ? rec->as.rec.cap * 2 : 8;
        rec->as.rec.k = (char **)xrealloc(rec->as.rec.k, cap * sizeof(char *));
        rec->as.rec.v = (Value *)xrealloc(rec->as.rec.v, cap * sizeof(Value));
        rec->as.rec.cap = cap;
    }
    rec->as.rec.k[rec->as.rec.n] = xstrdup(key);
    rec->as.rec.v[rec->as.rec.n] = val;
    rec->as.rec.n++;
}
static Value value_clone(Value v) {
    size_t i;
    switch (v.t) {
    case V_STR: return v_str(v.as.s.p);
    case V_LIST: {
        Value out = v_list();
        for (i = 0; i < v.as.list.n; i++) list_push(&out, value_clone(v.as.list.v[i]));
        return out;
    }
    case V_REC: {
        Value out = v_rec();
        for (i = 0; i < v.as.rec.n; i++) rec_set(&out, v.as.rec.k[i], value_clone(v.as.rec.v[i]));
        return out;
    }
    default: return v;
    }
}
static int is_num(Value v) { return v.t == V_INT || v.t == V_FLOAT; }
static double num_d(Value v) { return v.t == V_INT ? (double)v.as.i : v.as.f; }
static int truthy(Value v) {
    if (v.t == V_VOID) return 0;
    if (v.t == V_BOOL) return v.as.b != 0;
    return 1;
}
static Value norm_value(Value v) {
    if (v.t == V_FLOAT) {
        double d = v.as.f;
        if (isfinite(d) && floor(d) == d && fabs(d) <= (double)MAX_SAFE) return v_int((long long)d);
    }
    return v;
}
static const char *kind_name(Value v) {
    switch (v.t) {
    case V_VOID: return "void";
    case V_BOOL: return "bool";
    case V_INT: return "int";
    case V_FLOAT: return "float";
    case V_STR: return "string";
    case V_LIST: return "list";
    case V_REC: return "record";
    case V_FUNC: case V_NATIVE: return "function";
    }
    return "void";
}
static int deep_eq(Value a, Value b) {
    size_t i;
    if (is_num(a) && is_num(b)) {
        if (a.t == V_INT && b.t == V_INT) return a.as.i == b.as.i;
        return num_d(a) == num_d(b);
    }
    if (a.t != b.t) return 0;
    switch (a.t) {
    case V_VOID: return 1;
    case V_BOOL: return a.as.b == b.as.b;
    case V_STR: return strcmp(a.as.s.p, b.as.s.p) == 0;
    case V_LIST:
        if (a.as.list.n != b.as.list.n) return 0;
        for (i = 0; i < a.as.list.n; i++)
            if (!deep_eq(a.as.list.v[i], b.as.list.v[i])) return 0;
        return 1;
    case V_REC:
        if (a.as.rec.n != b.as.rec.n) return 0;
        for (i = 0; i < a.as.rec.n; i++) {
            long j = rec_find(&b, a.as.rec.k[i]);
            if (j < 0 || !deep_eq(a.as.rec.v[i], b.as.rec.v[j])) return 0;
        }
        return 1;
    case V_FUNC: return a.as.fn == b.as.fn;
    case V_NATIVE: return a.as.nat == b.as.nat;
    default: return 0;
    }
}

static void float_digits(double x, char *out, size_t outsz) {
    int p;
    if (x == 0) { strcpy(out, "0"); return; }
    for (p = 1; p <= 17; p++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.*g", p, x);
        if (strtod(tmp, NULL) == x) { snprintf(out, outsz, "%s", tmp); return; }
    }
    snprintf(out, outsz, "%.17g", x);
}
static char *float_text(double x) {
    char digits[64];
    Buf b;
    if (isnan(x)) return xstrdup("nan");
    if (isinf(x)) return xstrdup(x < 0 ? "-inf" : "inf");
    if (x == 0) return xstrdup("0.0");
    float_digits(x, digits, sizeof(digits));
    buf_init(&b);
    if (strchr(digits, 'e') || strchr(digits, 'E')) {
        char mant[64];
        char *ep = strchr(digits, 'e');
        int expo = 0;
        if (!ep) ep = strchr(digits, 'E');
        if (ep) {
            size_t mn = (size_t)(ep - digits);
            if (mn >= sizeof(mant)) mn = sizeof(mant) - 1;
            memcpy(mant, digits, mn);
            mant[mn] = 0;
            expo = atoi(ep + 1);
        } else {
            strcpy(mant, digits);
        }
        if (expo >= -4 && expo < 16) {
            char fixed[96];
            snprintf(fixed, sizeof(fixed), "%.*f", expo < 0 ? -expo : 0, strtod(digits, NULL));
            if (strchr(fixed, '.')) {
                size_t L = strlen(fixed);
                while (L > 0 && fixed[L - 1] == '0') fixed[--L] = 0;
                if (L > 0 && fixed[L - 1] == '.') fixed[L] = 0;
            }
            buf_adds(&b, fixed);
            if (!strchr(b.p, '.')) buf_adds(&b, ".0");
            return buf_take(&b);
        }
        buf_adds(&b, mant);
        if (!strchr(b.p, '.')) buf_adds(&b, ".0");
        buf_printf(&b, "e%c%02d", expo < 0 ? '-' : '+', expo < 0 ? -expo : expo);
        return buf_take(&b);
    }
    buf_adds(&b, digits);
    if (!strchr(b.p, '.')) buf_adds(&b, ".0");
    return buf_take(&b);
}
static char *int_text(long long i) { return xasprintf("%lld", i); }
static char *num_text(Value v) { return v.t == V_INT ? int_text(v.as.i) : float_text(v.as.f); }

static int parse_number_text(const char *s, Value *out) {
    const char *t = s;
    char *end = NULL;
    int has_dot = 0, has_exp = 0;
    while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
    if (!*t) return 0;
    {
        const char *q = t;
        if (*q == '+' || *q == '-') q++;
        if (!(*q >= '0' && *q <= '9') && *q != '.') return 0;
        for (; *q; q++) {
            if (*q == '.') has_dot = 1;
            else if (*q == 'e' || *q == 'E') { has_exp = 1; break; }
            else if (!(*q >= '0' && *q <= '9') && *q != '+' && *q != '-') break;
        }
    }
    if (!has_dot && !has_exp) {
        long long iv = strtoll(t, &end, 10);
        while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        if (!end || *end) return 0;
        if (iv > MAX_SAFE || iv < -MAX_SAFE) { *out = v_float((double)iv); return 1; }
        *out = v_int(iv);
        return 1;
    }
    {
        double d = strtod(t, &end);
        while (end && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end++;
        if (!end || *end || !isfinite(d)) return 0;
        *out = norm_value(v_float(d));
        return 1;
    }
}
static int is_numeric_key(const char *k) {
    Value tmp;
    return parse_number_text(k, &tmp);
}

static void json_quote(Buf *b, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    buf_addc(b, '"');
    while (*p) {
        unsigned char c = *p;
        switch (c) {
        case '"': buf_adds(b, "\\\""); break;
        case '\\': buf_adds(b, "\\\\"); break;
        case '\n': buf_adds(b, "\\n"); break;
        case '\t': buf_adds(b, "\\t"); break;
        case '\r': buf_adds(b, "\\r"); break;
        case '\b': buf_adds(b, "\\b"); break;
        case '\f': buf_adds(b, "\\f"); break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else buf_addc(b, (char)c);
        }
        p++;
    }
    buf_addc(b, '"');
}
static void to_json_buf(Buf *b, Value v) {
    size_t i;
    switch (v.t) {
    case V_VOID: buf_adds(b, "null"); break;
    case V_BOOL: buf_adds(b, v.as.b ? "true" : "false"); break;
    case V_INT: buf_printf(b, "%lld", v.as.i); break;
    case V_FLOAT: {
        char *t = num_text(v);
        buf_adds(b, t);
        free(t);
        break;
    }
    case V_STR: json_quote(b, v.as.s.p); break;
    case V_LIST:
        buf_addc(b, '[');
        for (i = 0; i < v.as.list.n; i++) {
            if (i) buf_addc(b, ',');
            to_json_buf(b, v.as.list.v[i]);
        }
        buf_addc(b, ']');
        break;
    case V_REC:
        buf_addc(b, '{');
        for (i = 0; i < v.as.rec.n; i++) {
            if (i) buf_addc(b, ',');
            json_quote(b, v.as.rec.k[i]);
            buf_addc(b, ':');
            to_json_buf(b, v.as.rec.v[i]);
        }
        buf_addc(b, '}');
        break;
    case V_FUNC: case V_NATIVE: {
        const char *nm = v.t == V_FUNC ? v.as.fn->name : v.as.nat->name;
        buf_printf(b, "\"<function: %s>\"", nm ? nm : "?");
        break;
    }
    }
}
static char *to_json_text(Value v) {
    Buf b;
    buf_init(&b);
    to_json_buf(&b, v);
    return buf_take(&b);
}
static char *to_text_val(Value v) {
    switch (v.t) {
    case V_VOID: return xstrdup("");
    case V_BOOL: return xstrdup(v.as.b ? "true" : "false");
    case V_INT: return int_text(v.as.i);
    case V_FLOAT: {
        Value n = norm_value(v);
        if (n.t == V_INT) return int_text(n.as.i);
        return float_text(v.as.f);
    }
    case V_STR: return xstrdup(v.as.s.p);
    case V_FUNC: case V_NATIVE: {
        const char *nm = v.t == V_FUNC ? v.as.fn->name : v.as.nat->name;
        return xasprintf("<function: %s>", nm ? nm : "?");
    }
    default: return to_json_text(v);
    }
}
static void json_pretty(Buf *b, Value v, int ind) {
    size_t i;
    int k;
    switch (v.t) {
    case V_LIST:
        if (!v.as.list.n) { buf_adds(b, "[]"); return; }
        buf_adds(b, "[\n");
        for (i = 0; i < v.as.list.n; i++) {
            for (k = 0; k < ind + 2; k++) buf_addc(b, ' ');
            json_pretty(b, v.as.list.v[i], ind + 2);
            if (i + 1 < v.as.list.n) buf_addc(b, ',');
            buf_addc(b, '\n');
        }
        for (k = 0; k < ind; k++) buf_addc(b, ' ');
        buf_addc(b, ']');
        return;
    case V_REC:
        if (!v.as.rec.n) { buf_adds(b, "{}"); return; }
        buf_adds(b, "{\n");
        for (i = 0; i < v.as.rec.n; i++) {
            for (k = 0; k < ind + 2; k++) buf_addc(b, ' ');
            json_quote(b, v.as.rec.k[i]);
            buf_adds(b, ": ");
            json_pretty(b, v.as.rec.v[i], ind + 2);
            if (i + 1 < v.as.rec.n) buf_addc(b, ',');
            buf_addc(b, '\n');
        }
        for (k = 0; k < ind; k++) buf_addc(b, ' ');
        buf_addc(b, '}');
        return;
    default:
        to_json_buf(b, v);
    }
}

/* @@NEXT@@ */
/* ====================================================================== */
/* 5. JSON input (requests, store file)                                    */
/* ====================================================================== */

typedef struct {
    const char *s;
    size_t n, i;
    char err[256];
    int bad;
} JsonP;

static void jp_ws(JsonP *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int jp_hex4(JsonP *p, unsigned *out) {
    unsigned v = 0;
    int k;
    if (p->i + 4 > p->n) return 0;
    for (k = 0; k < 4; k++) {
        char c = p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void utf8_encode(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_addc(b, (char)cp);
    else if (cp < 0x800) {
        buf_addc(b, (char)(0xC0 | (cp >> 6)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_addc(b, (char)(0xE0 | (cp >> 12)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_addc(b, (char)(0xF0 | (cp >> 18)));
        buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static int jp_string(JsonP *p, char **out) {
    Buf b;
    buf_init(&b);
    p->i++; /* opening quote */
    while (p->i < p->n) {
        char c = p->s[p->i++];
        if (c == '"') { *out = buf_take(&b); return 1; }
        if (c == '\\') {
            char e;
            if (p->i >= p->n) break;
            e = p->s[p->i++];
            switch (e) {
            case '"': buf_addc(&b, '"'); break;
            case '\\': buf_addc(&b, '\\'); break;
            case '/': buf_addc(&b, '/'); break;
            case 'b': buf_addc(&b, '\b'); break;
            case 'f': buf_addc(&b, '\f'); break;
            case 'n': buf_addc(&b, '\n'); break;
            case 'r': buf_addc(&b, '\r'); break;
            case 't': buf_addc(&b, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!jp_hex4(p, &cp)) { strcpy(p->err, "bad-json-unicode"); p->bad = 1; return 0; }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 6 <= p->n &&
                    p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    size_t save = p->i;
                    unsigned lo;
                    p->i += 2;
                    if (jp_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->i = save;
                    }
                }
                utf8_encode(&b, cp);
                break;
            }
            default: strcpy(p->err, "bad-json-escape"); p->bad = 1; return 0;
            }
            continue;
        }
        buf_addc(&b, c);
    }
    strcpy(p->err, "bad-json-string");
    p->bad = 1;
    return 0;
}

static int jp_value(JsonP *p, Value *out, int depth);

static int jp_number(JsonP *p, Value *out) {
    size_t start = p->i;
    int has_dot = 0;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
    if (p->i < p->n && p->s[p->i] == '.') {
        has_dot = 1;
        p->i++;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        size_t save = p->i;
        int ok = 0;
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) { p->i++; ok = 1; }
        if (!ok) p->i = save;
        has_dot = 1;
    }
    if (p->i == start) { strcpy(p->err, "bad-json-number"); p->bad = 1; return 0; }
    {
        char *tmp = xstrndup(p->s + start, p->i - start);
        if (has_dot) *out = v_float(strtod(tmp, NULL));
        else {
            long long iv = strtoll(tmp, NULL, 10);
            if (iv > MAX_SAFE || iv < -MAX_SAFE) *out = v_float((double)iv);
            else *out = v_int(iv);
        }
        free(tmp);
        return 1;
    }
}

static int jp_value(JsonP *p, Value *out, int depth) {
    if (depth > 64) { strcpy(p->err, "json-too-deep"); p->bad = 1; return 0; }
    jp_ws(p);
    if (p->i >= p->n) { strcpy(p->err, "bad-json-eof"); p->bad = 1; return 0; }
    {
        char c = p->s[p->i];
        if (c == '{') {
            Value rec = v_rec();
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == '}') { p->i++; *out = rec; return 1; }
            for (;;) {
                char *key = NULL;
                Value val;
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != '"') { strcpy(p->err, "bad-json-key"); p->bad = 1; return 0; }
                if (!jp_string(p, &key)) return 0;
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') { strcpy(p->err, "bad-json-colon"); p->bad = 1; return 0; }
                p->i++;
                if (!jp_value(p, &val, depth + 1)) return 0;
                rec_set(&rec, key, val);
                free(key);
                jp_ws(p);
                if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
                if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
                strcpy(p->err, "bad-json-object"); p->bad = 1; return 0;
            }
            *out = rec;
            return 1;
        }
        if (c == '[') {
            Value lst = v_list();
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ']') { p->i++; *out = lst; return 1; }
            for (;;) {
                Value val;
                if (!jp_value(p, &val, depth + 1)) return 0;
                list_push(&lst, val);
                jp_ws(p);
                if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
                if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
                strcpy(p->err, "bad-json-array"); p->bad = 1; return 0;
            }
            *out = lst;
            return 1;
        }
        if (c == '"') {
            char *s = NULL;
            if (!jp_string(p, &s)) return 0;
            *out = v_str_take(s);
            return 1;
        }
        if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "true", 4)) { p->i += 4; *out = v_bool(1); return 1; }
        if (p->n - p->i >= 5 && !strncmp(p->s + p->i, "false", 5)) { p->i += 5; *out = v_bool(0); return 1; }
        if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "null", 4)) { p->i += 4; *out = v_void(); return 1; }
        if (c == '-' || isdigit((unsigned char)c)) return jp_number(p, out);
        strcpy(p->err, "bad-json-value");
        p->bad = 1;
        return 0;
    }
}

static int json_parse(const char *src, Value *out, char **err) {
    JsonP p;
    p.s = src;
    p.n = strlen(src);
    p.i = 0;
    p.err[0] = 0;
    p.bad = 0;
    if (!jp_value(&p, out, 0)) {
        if (err) *err = xstrdup(p.err[0] ? p.err : "bad-json");
        return 0;
    }
    jp_ws(&p);
    if (p.i != p.n) {
        if (err) *err = xstrdup("trailing-json");
        return 0;
    }
    return 1;
}

/* ====================================================================== */
/* 6. lexer                                                                */
/* ====================================================================== */

typedef enum {
    T_EOF = 0, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_COMMA,
    T_STRING, T_NUMBER, T_WORD
} TokKind;

typedef struct {
    TokKind kind;
    char *text;
    int line, col;
    long long iv;
    double fv;
    int is_float;
} Token;

typedef struct {
    Token *v;
    size_t n, cap;
} TokList;

static void tok_push(TokList *tl, Token t) {
    if (tl->n + 1 > tl->cap) {
        size_t cap = tl->cap ? tl->cap * 2 : 64;
        tl->v = (Token *)xrealloc(tl->v, cap * sizeof(Token));
        tl->cap = cap;
    }
    tl->v[tl->n++] = t;
}

static int starts3(const char *s, size_t i, size_t n, unsigned char a, unsigned char b, unsigned char c) {
    return i + 2 < n && (unsigned char)s[i] == a && (unsigned char)s[i + 1] == b && (unsigned char)s[i + 2] == c;
}

static int is_ws_char(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

static int is_delim1(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == ',' || c == ';' || c == '"' || c == '\'';
}

/* returns matched length or 0 */
static size_t match_number(const char *s, size_t n) {
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || !isdigit((unsigned char)s[i])) return 0;
        while (i < n && isdigit((unsigned char)s[i])) i++;
    } else if (i < n && isdigit((unsigned char)s[i])) {
        while (i < n && isdigit((unsigned char)s[i])) i++;
        if (i < n && s[i] == '.') {
            i++;
            while (i < n && isdigit((unsigned char)s[i])) i++;
        }
    } else {
        return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t save = i;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i < n && isdigit((unsigned char)s[i])) {
            while (i < n && isdigit((unsigned char)s[i])) i++;
        } else {
            i = save;
        }
    }
    return i;
}

static char *read_file_text(const char *path, size_t *out_len, char **err) {
    FILE *f = fopen(path, "rb");
    Buf b;
    char tmp[8192];
    size_t got;
    if (!f) {
        if (err) *err = xasprintf("file-not-found: %s", path);
        return NULL;
    }
    buf_init(&b);
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) buf_addn(&b, tmp, got);
    fclose(f);
    if (out_len) *out_len = b.n;
    return buf_take(&b);
}

/* strip BOM and normalise CRLF, like _clean_source */
static char *clean_source(char *src) {
    char *p = src;
    char *out;
    size_t i, j = 0, n;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    n = strlen(p);
    out = (char *)xmalloc(n + 1);
    for (i = 0; i < n; i++) {
        if (p[i] == '\r') {
            if (i + 1 < n && p[i + 1] == '\n') continue;
            out[j++] = '\n';
        } else {
            out[j++] = p[i];
        }
    }
    out[j] = 0;
    return out;
}

static TokList lex_source(const char *src, char **err, int *err_line) {
    TokList tl;
    size_t i = 0, n = strlen(src);
    int line = 1, col = 1;
    tl.v = NULL; tl.n = tl.cap = 0;
    while (i < n) {
        char c = src[i];
        if (is_ws_char(c)) { i++; col++; continue; }
        if (c == '\n') { i++; line++; col = 1; continue; }
        if (c == ';') {
            while (i < n && src[i] != '\n') { i++; col++; }
            continue;
        }
        if (starts3(src, i, n, 0xE2, 0xB8, 0xA2)) { /* ⸢ */
            int start_line = line;
            i += 3; col++;
            for (;;) {
                if (i >= n) { *err = xstrdup("unterminated-comment"); *err_line = start_line; free(tl.v); return tl; }
                if (starts3(src, i, n, 0xE2, 0xB8, 0xA3)) { i += 3; col++; break; }
                if (src[i] == '\n') { i++; line++; col = 1; } else { i++; col++; }
            }
            continue;
        }
        if (starts3(src, i, n, 0xE2, 0x9F, 0xA8)) { /* ⟨ */
            Token t; memset(&t, 0, sizeof(t)); t.kind = T_LPAREN; t.text = xstrdup("⟨"); t.line = line; t.col = col;
            tok_push(&tl, t); i += 3; col++; continue;
        }
        if (starts3(src, i, n, 0xE2, 0x9F, 0xA9)) { /* ⟩ */
            Token t; memset(&t, 0, sizeof(t)); t.kind = T_RPAREN; t.text = xstrdup("⟩"); t.line = line; t.col = col;
            tok_push(&tl, t); i += 3; col++; continue;
        }
        if (c == '(') { Token t; memset(&t, 0, sizeof(t)); t.kind = T_LPAREN; t.text = xstrdup("("); t.line = line; t.col = col; tok_push(&tl, t); i++; col++; continue; }
        if (c == ')') { Token t; memset(&t, 0, sizeof(t)); t.kind = T_RPAREN; t.text = xstrdup(")"); t.line = line; t.col = col; tok_push(&tl, t); i++; col++; continue; }
        if (c == '[') { Token t; memset(&t, 0, sizeof(t)); t.kind = T_LBRACKET; t.text = xstrdup("["); t.line = line; t.col = col; tok_push(&tl, t); i++; col++; continue; }
        if (c == ']') { Token t; memset(&t, 0, sizeof(t)); t.kind = T_RBRACKET; t.text = xstrdup("]"); t.line = line; t.col = col; tok_push(&tl, t); i++; col++; continue; }
        if (c == ',') { Token t; memset(&t, 0, sizeof(t)); t.kind = T_COMMA; t.text = xstrdup(","); t.line = line; t.col = col; tok_push(&tl, t); i++; col++; continue; }
        if (c == '"' || c == '\'') {
            char quote = c;
            int start_line = line, start_col = col;
            Buf b;
            size_t text_start = i;
            buf_init(&b);
            i++; col++;
            for (;;) {
                if (i >= n) { *err = xstrdup("unterminated-string"); *err_line = start_line; free(tl.v); return tl; }
                if (src[i] == quote) { i++; col++; break; }
                if (src[i] == '\\') {
                    char e;
                    if (i + 1 >= n) { *err = xstrdup("unterminated-string"); *err_line = start_line; free(tl.v); return tl; }
                    e = src[i + 1];
                    switch (e) {
                    case 'n': buf_addc(&b, '\n'); break;
                    case 't': buf_addc(&b, '\t'); break;
                    case 'r': buf_addc(&b, '\r'); break;
                    case 'b': buf_addc(&b, '\b'); break;
                    case 'f': buf_addc(&b, '\f'); break;
                    default: buf_addc(&b, e); break;
                    }
                    i += 2; col += 2;
                    continue;
                }
                if (src[i] == '\n') { *err = xstrdup("unterminated-string"); *err_line = start_line; free(tl.v); return tl; }
                buf_addc(&b, src[i]); i++; col++;
            }
            {
                Token t;
                memset(&t, 0, sizeof(t));
                t.kind = T_STRING;
                t.text = buf_take(&b);
                t.line = start_line;
                t.col = start_col;
                (void)text_start;
                tok_push(&tl, t);
            }
            continue;
        }
        if (c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            size_t len = match_number(src + i, n - i);
            if (len) {
                char *txt = xstrndup(src + i, len);
                Token t;
                memset(&t, 0, sizeof(t));
                t.kind = T_NUMBER;
                t.text = txt;
                t.line = line;
                t.col = col;
                if (strchr(txt, '.') || strchr(txt, 'e') || strchr(txt, 'E')) {
                    t.is_float = 1;
                    t.fv = strtod(txt, NULL);
                } else {
                    long long iv;
                    errno = 0;
                    iv = strtoll(txt, NULL, 10);
                    if (errno == ERANGE || iv > MAX_SAFE || iv < -MAX_SAFE) {
                        t.is_float = 1;
                        t.fv = strtod(txt, NULL);
                    } else {
                        t.iv = iv;
                    }
                }
                tok_push(&tl, t);
                i += len; col += (int)len;
                continue;
            }
        }
        /* word: run until whitespace / delimiter / comment */
        {
            size_t start = i;
            int start_col = col;
            while (i < n) {
                char ch = src[i];
                if (is_ws_char(ch) || ch == '\n' || is_delim1(ch)) break;
                if (starts3(src, i, n, 0xE2, 0x9F, 0xA8) || starts3(src, i, n, 0xE2, 0x9F, 0xA9) ||
                    starts3(src, i, n, 0xE2, 0xB8, 0xA2) || starts3(src, i, n, 0xE2, 0xB8, 0xA3)) break;
                i++; col++;
            }
            if (i == start) {
                *err = xasprintf("unexpected-character: %c", c);
                *err_line = line;
                free(tl.v);
                return tl;
            }
            {
                Token t;
                memset(&t, 0, sizeof(t));
                t.kind = T_WORD;
                t.text = xstrndup(src + start, i - start);
                t.line = line;
                t.col = start_col;
                tok_push(&tl, t);
            }
        }
    }
    {
        Token t;
        memset(&t, 0, sizeof(t));
        t.kind = T_EOF;
        t.text = xstrdup("");
        t.line = line;
        t.col = col;
        tok_push(&tl, t);
    }
    return tl;
}

/* ====================================================================== */
/* 7. syntax tree and parser                                               */
/* ====================================================================== */

typedef enum { A_PROGRAM, A_NODE, A_SEQ, A_NUM, A_STR, A_BOOL, A_NIL, A_ID } AKind;

typedef struct Ast {
    AKind k;
    int line;
    union {
        struct { struct Ast **items; size_t n; } program;
        struct { char *head; struct Ast **args; size_t n; } node;
        struct { struct Ast **items; size_t n; } seq;
        struct { long long iv; double fv; int is_float; } num;
        char *s;
        int b;
        char *id;
    } as;
} Ast;

static Ast *ast_new(AKind k, int line) {
    Ast *a = (Ast *)xmalloc(sizeof(Ast));
    memset(a, 0, sizeof(*a));
    a->k = k;
    a->line = line;
    return a;
}

typedef struct {
    TokList tl;
    size_t i;
    char *err;
    int err_line;
} Parser;

static Token *p_peek(Parser *p) { return &p->tl.v[p->i]; }
static Token *p_next(Parser *p) { return &p->tl.v[p->i++]; }

static Ast *parse_expr(Parser *p);

static int is_bool_true_word(const char *s) {
    return !strcmp(s, "#t") || !strcmp(s, "\xE2\x8A\xA4") /* ⊤ */ || !strcmp(s, "\xE2\x97\x89"); /* ◉ */
}
static int is_bool_false_word(const char *s) {
    return !strcmp(s, "#f") || !strcmp(s, "\xE2\x8A\xA5") /* ⊥ */ || !strcmp(s, "\xE2\x97\xAF"); /* ◯ */
}
static int is_nil_word(const char *s) {
    return !strcmp(s, "\xE2\x97\x8C") /* ◌ */ || !strcmp(s, "\xE2\x97\x8B") /* ○ */ || !strcmp(s, "\xE2\x8C\x80"); /* ⌀ */
}

static Ast *parse_node(Parser *p) {
    Token *open = p_next(p);
    Ast *a = ast_new(A_NODE, open->line);
    if (p_peek(p)->kind == T_RPAREN) {
        p_next(p);
        a->as.node.head = NULL;
        return a;
    }
    if (p_peek(p)->kind != T_WORD) {
        p->err = xasprintf("bad-head: %s", p_peek(p)->text[0] ? p_peek(p)->text : "?");
        p->err_line = p_peek(p)->line;
        return NULL;
    }
    a->as.node.head = xstrdup(p_next(p)->text);
    {
        size_t cap = 0;
        for (;;) {
            Token *t = p_peek(p);
            if (t->kind == T_RPAREN) { p_next(p); break; }
            if (t->kind == T_EOF) { p->err = xstrdup("unexpected-eof"); p->err_line = t->line; return NULL; }
            if (t->kind == T_COMMA) { p_next(p); continue; }
            {
                Ast *arg = parse_expr(p);
                if (!arg) return NULL;
                if (a->as.node.n + 1 > cap) {
                    cap = cap ? cap * 2 : 4;
                    a->as.node.args = (Ast **)xrealloc(a->as.node.args, cap * sizeof(Ast *));
                }
                a->as.node.args[a->as.node.n++] = arg;
            }
        }
    }
    return a;
}

static Ast *parse_seq(Parser *p) {
    Token *open = p_next(p);
    Ast *a = ast_new(A_SEQ, open->line);
    size_t cap = 0;
    for (;;) {
        Token *t = p_peek(p);
        if (t->kind == T_RBRACKET) { p_next(p); break; }
        if (t->kind == T_EOF) { p->err = xstrdup("unexpected-eof"); p->err_line = t->line; return NULL; }
        if (t->kind == T_COMMA) { p_next(p); continue; }
        {
            Ast *item = parse_expr(p);
            if (!item) return NULL;
            if (a->as.seq.n + 1 > cap) {
                cap = cap ? cap * 2 : 4;
                a->as.seq.items = (Ast **)xrealloc(a->as.seq.items, cap * sizeof(Ast *));
            }
            a->as.seq.items[a->as.seq.n++] = item;
        }
    }
    return a;
}

static Ast *parse_expr(Parser *p) {
    Token *t = p_peek(p);
    if (t->kind == T_LPAREN) return parse_node(p);
    if (t->kind == T_LBRACKET) return parse_seq(p);
    if (t->kind == T_NUMBER) {
        p_next(p);
        {
            Ast *a = ast_new(A_NUM, t->line);
            a->as.num.is_float = t->is_float;
            if (t->is_float) a->as.num.fv = t->fv;
            else a->as.num.iv = t->iv;
            return a;
        }
    }
    if (t->kind == T_STRING) {
        p_next(p);
        {
            Ast *a = ast_new(A_STR, t->line);
            a->as.s = t->text;
            return a;
        }
    }
    if (t->kind == T_WORD) {
        p_next(p);
        if (is_bool_true_word(t->text)) {
            Ast *a = ast_new(A_BOOL, t->line);
            a->as.b = 1;
            return a;
        }
        if (is_bool_false_word(t->text)) {
            Ast *a = ast_new(A_BOOL, t->line);
            a->as.b = 0;
            return a;
        }
        if (is_nil_word(t->text)) return ast_new(A_NIL, t->line);
        {
            Ast *a = ast_new(A_ID, t->line);
            a->as.id = t->text;
            return a;
        }
    }
    if (t->kind == T_EOF) { p->err = xstrdup("unexpected-eof"); p->err_line = t->line; return NULL; }
    p->err = xasprintf("unexpected-token: %s", t->text[0] ? t->text : "?");
    p->err_line = t->line;
    return NULL;
}

static Ast *parse_source(const char *src, char **err, int *err_line) {
    Parser p;
    Ast *prog;
    size_t cap = 0;
    p.tl = lex_source(src, err, err_line);
    if (*err) return NULL;
    p.i = 0;
    p.err = NULL;
    p.err_line = 1;
    prog = ast_new(A_PROGRAM, 1);
    while (p_peek(&p)->kind != T_EOF) {
        Ast *e = parse_expr(&p);
        if (!e) {
            *err = p.err;
            *err_line = p.err_line;
            return NULL;
        }
        if (prog->as.program.n + 1 > cap) {
            cap = cap ? cap * 2 : 8;
            prog->as.program.items = (Ast **)xrealloc(prog->as.program.items, cap * sizeof(Ast *));
        }
        prog->as.program.items[prog->as.program.n++] = e;
    }
    return prog;
}

/* @@APPEND@@ */
/* ====================================================================== */
/* 8. environments, errors, signals                                       */
/* ====================================================================== */

struct Env {
    Env *parent;
    char **names;
    Value *vals;
    size_t n, cap;
};

static Env *env_new(Env *parent) {
    Env *e = (Env *)xmalloc(sizeof(Env));
    memset(e, 0, sizeof(*e));
    e->parent = parent;
    return e;
}
static void env_define(Env *e, const char *name, Value v) {
    size_t i;
    for (i = 0; i < e->n; i++) {
        if (strcmp(e->names[i], name) == 0) { e->vals[i] = v; return; }
    }
    if (e->n + 1 > e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 8;
        e->names = (char **)xrealloc(e->names, cap * sizeof(char *));
        e->vals = (Value *)xrealloc(e->vals, cap * sizeof(Value));
        e->cap = cap;
    }
    e->names[e->n] = xstrdup(name);
    e->vals[e->n] = v;
    e->n++;
}
static int env_get(Env *e, const char *name, Value *out) {
    while (e) {
        size_t i;
        for (i = 0; i < e->n; i++) {
            if (strcmp(e->names[i], name) == 0) { *out = e->vals[i]; return 1; }
        }
        e = e->parent;
    }
    return 0;
}
static int env_set(Env *e, const char *name, Value v) {
    while (e) {
        size_t i;
        for (i = 0; i < e->n; i++) {
            if (strcmp(e->names[i], name) == 0) { e->vals[i] = v; return 1; }
        }
        e = e->parent;
    }
    return 0;
}

typedef struct Frame {
    jmp_buf jb;
    struct Frame *prev;
} Frame;

enum { SIG_NONE = 0, SIG_RETURN, SIG_BREAK, SIG_CONTINUE };

struct Interp {
    Host *host;
    Env *globals;
    int depth;
    long loops;
    Frame *frame;
    Frame *fn_frame;
    Frame *loop_frame;
    Frame *try_frame;
    Frame *top_frame;
    int sig;
    Value ret;
    struct { int set; char kind[16]; char msg[512]; int line; } err;
    void (*out)(const char *);
};

static Interp *interp_new(Host *host, void (*out)(const char *));
static Value eval_expr(Interp *in, Ast *a, Env *env);
static Value eval_body(Interp *in, Ast **items, size_t n, Env *env);
static Value call_function(Interp *in, Func *fn, Value *args, size_t nargs, int line);
static Value call_host(Interp *in, const char *name, Value *args, size_t n, int line);

static void xerr(Interp *in, const char *kind, int line, const char *msg) {
    Frame *target;
    snprintf(in->err.kind, sizeof(in->err.kind), "%s", kind);
    snprintf(in->err.msg, sizeof(in->err.msg), "%s", msg);
    in->err.line = line;
    in->err.set = 1;
    target = in->try_frame ? in->try_frame : in->top_frame;
    if (!target) {
        fprintf(stderr, "nexus: fatal: %s\n", msg);
        exit(3);
    }
    longjmp(target->jb, 1);
}

static void xerrf(Interp *in, const char *kind, int line, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    xerr(in, kind, line, msg);
}

static void raise_sig(Interp *in, int sig, Value value, Frame *target) {
    in->sig = sig;
    in->ret = value;
    longjmp(target->jb, 2);
}

/* ====================================================================== */
/* 9. builtins                                                             */
/* ====================================================================== */

typedef enum {
    B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_POW, B_LT, B_LTE, B_GT, B_GTE, B_EQ,
    B_NE, B_GET, B_SETFIELD, B_NOT, B_LEN, B_TEXT, B_NUM, B_NORM, B_LOWER,
    B_UPPER, B_SPLIT, B_JOIN, B_CONTAINS, B_KEYS, B_VALUES, B_APPEND, B_SLICE,
    B_SORT, B_SUM, B_JSON, B_KIND, B_TRIM
} BCanon;

typedef struct { BCanon c; const char *names[6]; } BAlias;

static const BAlias BUILTIN_ALIASES[] = {
    { B_ADD, { "+", "plus", "\xE2\x8A\x95+", NULL } },
    { B_SUB, { "-", "minus", "\xE2\x8A\x96", NULL } },
    { B_MUL, { "*", "times", "\xE2\x8A\x97*", NULL } },
    { B_DIV, { "/", "over", "\xE2\x8A\x98", NULL } },
    { B_MOD, { "%", "mod", "\xE2\x8C\xA5", NULL } },
    { B_POW, { "**", "pow", "\xE2\x86\x91", NULL } },
    { B_LT, { "<", "lt", "\xE2\x89\xBA", NULL } },
    { B_LTE, { "<=", "lte", "\xE2\x89\xBC", NULL } },
    { B_GT, { ">", "gt", "\xE2\x89\xBB", NULL } },
    { B_GTE, { ">=", "gte", "\xE2\x89\xBD", NULL } },
    { B_EQ, { "==", "eq", "\xE2\x89\xA1", NULL } },
    { B_NE, { "!=", "ne", "\xE2\x89\xA0", NULL } },
    { B_GET, { "get", "at", "index", "lookup", "\xE2\x9F\xB6", NULL } },
    { B_SETFIELD, { "assoc", "with", "setfield", "put", "\xE2\x8C\xB8\xE2\x8C\xB8", NULL } },
    { B_NOT, { "not", "!", "\xE2\x88\xBC", NULL } },
    { B_LEN, { "len", "length", "count", "size", NULL } },
    { B_TEXT, { "text", "str", "show", "repr", NULL } },
    { B_NUM, { "num", "number", "parse", "\xE2\x8C\x97", NULL } },
    { B_NORM, { "norm", "tidy", "\xE2\x87\xA2", "settle", NULL } },
    { B_LOWER, { "lower", "down", "soft", "\xE2\x87\xA3", NULL } },
    { B_UPPER, { "upper", "up", "loud", "\xE2\x87\xA1", NULL } },
    { B_SPLIT, { "split", "cleave", "\xE2\x8B\xAE", "parts", NULL } },
    { B_JOIN, { "join", "stitch", "\xE2\xA7\xBA", "glue", NULL } },
    { B_CONTAINS, { "has", "contains", "\xE2\x88\x8B", "in?", NULL } },
    { B_KEYS, { "keys", "fields", "\xE2\x8C\xB8", "facets", NULL } },
    { B_VALUES, { "values", "contents", "\xE2\x8C\xB9", "beats", NULL } },
    { B_APPEND, { "push", "append", "\xE2\xA4\xB7", "conj", NULL } },
    { B_SLICE, { "slice", "carve", "\xE2\x8C\x97\xE2\x8C\x97", "range", NULL } },
    { B_SORT, { "sort", "order", "\xE2\x87\x85", "rank", NULL } },
    { B_SUM, { "sum", "total", "\xCE\xA3", "tally", NULL } },
    { B_JSON, { "json", "freeze", "\xE2\x8C\x98", "encode", NULL } },
    { B_KIND, { "kind", "type", "typeof", NULL } },
    { B_TRIM, { "trim", "strip", "chomp", NULL } },
    { (BCanon)-1, { NULL } }
};

static int builtin_lookup(const char *name, BCanon *out) {
    int i, j;
    for (i = 0; BUILTIN_ALIASES[i].c != (BCanon)-1; i++) {
        for (j = 0; j < 6 && BUILTIN_ALIASES[i].names[j]; j++) {
            if (strcmp(name, BUILTIN_ALIASES[i].names[j]) == 0) { *out = BUILTIN_ALIASES[i].c; return 1; }
        }
    }
    return 0;
}

static Value need_num(Interp *in, Value v, int line) {
    if (v.t == V_INT || v.t == V_FLOAT) return v;
    xerrf(in, "type", line, "not-a-number: %s", to_text_val(v));
    return v_void();
}
static long long as_index(Interp *in, Value v, int line) {
    if (v.t == V_INT) return v.as.i;
    if (v.t == V_FLOAT && floor(v.as.f) == v.as.f) return (long long)v.as.f;
    xerrf(in, "type", line, "not-an-index: %s", to_text_val(v));
    return 0;
}
static Value arith_norm(Value v) { return norm_value(v); }

static Value add2(Interp *in, Value a, Value b, int line) {
    if (is_num(a) && is_num(b)) {
        if (a.t == V_INT && b.t == V_INT) {
            double d = (double)a.as.i + (double)b.as.i;
            if (fabs(d) <= (double)MAX_SAFE) return v_int((long long)d);
            return arith_norm(v_float(d));
        }
        return arith_norm(v_float(num_d(a) + num_d(b)));
    }
    if (a.t == V_STR && b.t == V_STR) {
        Buf buf;
        buf_init(&buf);
        buf_adds(&buf, a.as.s.p);
        buf_adds(&buf, b.as.s.p);
        return v_str_take(buf_take(&buf));
    }
    if (a.t == V_LIST && b.t == V_LIST) {
        Value out = v_list();
        size_t i;
        for (i = 0; i < a.as.list.n; i++) list_push(&out, a.as.list.v[i]);
        for (i = 0; i < b.as.list.n; i++) list_push(&out, b.as.list.v[i]);
        return out;
    }
    xerrf(in, "type", line, "bad-operand: + %s %s", to_text_val(a), to_text_val(b));
    return v_void();
}

static Value v_get(Interp *in, Value coll, Value key, int line) {
    if (coll.t == V_VOID) return v_void();
    if (coll.t == V_LIST || coll.t == V_STR) {
        long long idx = as_index(in, key, line);
        long long n = coll.t == V_LIST ? (long long)coll.as.list.n : (long long)u8_len(coll.as.s.p);
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) return v_void();
        if (coll.t == V_LIST) return coll.as.list.v[idx];
        {
            size_t *off = (size_t *)xmalloc(((size_t)n + 1) * sizeof(size_t));
            char *piece;
            u8_offsets(coll.as.s.p, off);
            piece = xstrndup(coll.as.s.p + off[idx], off[idx + 1] - off[idx]);
            free(off);
            return v_str_take(piece);
        }
    }
    if (coll.t == V_REC) {
        if (key.t != V_STR) xerrf(in, "type", line, "not-a-key: %s", to_text_val(key));
        return rec_get(&coll, key.as.s.p);
    }
    xerrf(in, "type", line, "not-indexable: %s", to_text_val(coll));
    return v_void();
}

static Value v_setfield(Interp *in, Value rec, Value key, Value val, int line) {
    Value out;
    if (rec.t == V_VOID) rec = v_rec();
    if (rec.t != V_REC) xerrf(in, "type", line, "not-a-record: %s", to_text_val(rec));
    if (key.t != V_STR) xerrf(in, "type", line, "not-a-key: %s", to_text_val(key));
    if (is_numeric_key(key.as.s.p)) xerrf(in, "type", line, "bad-record-key: %s", key.as.s.p);
    out = value_clone(rec);
    rec_set(&out, key.as.s.p, val);
    return out;
}

static int str_contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

static Value b_split(Interp *in, Value s, Value sep, int line) {
    Value out = v_list();
    if (s.t != V_STR) xerrf(in, "type", line, "not-a-string: %s", to_text_val(s));
    if (sep.t != V_STR) xerrf(in, "type", line, "not-a-string: %s", to_text_val(sep));
    if (!sep.as.s.p[0]) {
        size_t n = u8_len(s.as.s.p), i, *off = (size_t *)xmalloc((n + 1) * sizeof(size_t));
        u8_offsets(s.as.s.p, off);
        for (i = 0; i < n; i++) {
            char *piece = xstrndup(s.as.s.p + off[i], off[i + 1] - off[i]);
            list_push(&out, v_str_take(piece));
        }
        free(off);
        return out;
    }
    {
        const char *p = s.as.s.p;
        const char *q;
        size_t seplen = strlen(sep.as.s.p);
        while ((q = strstr(p, sep.as.s.p)) != NULL) {
            list_push(&out, v_str_take(xstrndup(p, (size_t)(q - p))));
            p = q + seplen;
        }
        list_push(&out, v_str(p));
    }
    return out;
}

static Value b_slice(Interp *in, Value x, Value start, Value end, int line) {
    long long n, a, b;
    if (x.t == V_LIST) n = (long long)x.as.list.n;
    else if (x.t == V_STR) n = (long long)u8_len(x.as.s.p);
    else { xerrf(in, "type", line, "not-sliceable: %s", to_text_val(x)); return v_void(); }
    a = as_index(in, start, line);
    if (end.t == V_VOID) b = n;
    else b = as_index(in, end, line);
    if (a < 0) a += n;
    if (b < 0) b += n;
    if (a < 0) a = 0;
    if (b > n) b = n;
    if (b < a) b = a;
    if (x.t == V_LIST) {
        Value out = v_list();
        long long i;
        for (i = a; i < b; i++) list_push(&out, x.as.list.v[i]);
        return out;
    }
    return v_str_take(u8_substr(x.as.s.p, (size_t)a, (size_t)b));
}

static int cmp_values(Interp *in, Value a, Value b, int line) {
    if (is_num(a) && is_num(b)) {
        double x = num_d(a), y = num_d(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.t == V_STR && b.t == V_STR) return strcmp(a.as.s.p, b.as.s.p);
    xerrf(in, "type", line, "mixed-types: sort");
    return 0;
}

static void sort_list(Interp *in, Value *lst, int line) {
    size_t i, j, n = lst->as.list.n;
    for (i = 1; i < n; i++) {
        Value key = lst->as.list.v[i];
        j = i;
        while (j > 0 && cmp_values(in, lst->as.list.v[j - 1], key, line) > 0) {
            lst->as.list.v[j] = lst->as.list.v[j - 1];
            j--;
        }
        lst->as.list.v[j] = key;
    }
}

static Value apply_builtin(Interp *in, BCanon c, Value *a, size_t n, int line) {
    switch (c) {
    case B_ADD: {
        size_t i; Value acc;
        if (n == 0) return v_int(0);
        if (n == 1) return need_num(in, a[0], line);
        acc = a[0];
        for (i = 1; i < n; i++) acc = add2(in, acc, a[i], line);
        return acc;
    }
    case B_SUB: {
        size_t i; Value acc;
        if (n == 0) { xerrf(in, "type", line, "bad-arg-count: - expects 1 or 2, got 0"); }
        if (n == 1) {
            Value x = need_num(in, a[0], line);
            return x.t == V_INT ? v_int(-x.as.i) : arith_norm(v_float(-x.as.f));
        }
        acc = need_num(in, a[0], line);
        for (i = 1; i < n; i++) {
            Value b = need_num(in, a[i], line);
            if (acc.t == V_INT && b.t == V_INT) {
                double d = (double)acc.as.i - (double)b.as.i;
                if (fabs(d) <= (double)MAX_SAFE) acc = v_int((long long)d);
                else acc = arith_norm(v_float(d));
            } else acc = arith_norm(v_float(num_d(acc) - num_d(b)));
        }
        return acc;
    }
    case B_MUL: {
        size_t i; Value acc;
        if (n == 0) return v_int(1);
        acc = need_num(in, a[0], line);
        for (i = 1; i < n; i++) {
            Value b = need_num(in, a[i], line);
            if (acc.t == V_INT && b.t == V_INT) {
                double d = (double)acc.as.i * (double)b.as.i;
                if (fabs(d) <= (double)MAX_SAFE) acc = v_int((long long)d);
                else acc = arith_norm(v_float(d));
            } else acc = arith_norm(v_float(num_d(acc) * num_d(b)));
        }
        return acc;
    }
    case B_DIV: {
        size_t i; Value acc;
        if (n < 2) xerrf(in, "type", line, "bad-arg-count: / expects at least 2, got %d", (int)n);
        acc = need_num(in, a[0], line);
        for (i = 1; i < n; i++) {
            Value b = need_num(in, a[i], line);
            if (num_d(b) == 0) xerrf(in, "runtime", line, "divide-by-zero");
            if (acc.t == V_INT && b.t == V_INT) {
                if (acc.as.i % b.as.i == 0) acc = v_int(acc.as.i / b.as.i);
                else acc = arith_norm(v_float((double)acc.as.i / (double)b.as.i));
            } else acc = arith_norm(v_float(num_d(acc) / num_d(b)));
        }
        return acc;
    }
    case B_MOD: {
        Value a0, b0;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: %% expects 2, got %d", (int)n);
        a0 = need_num(in, a[0], line);
        b0 = need_num(in, a[1], line);
        if (num_d(b0) == 0) xerrf(in, "runtime", line, "divide-by-zero");
        if (a0.t == V_INT && b0.t == V_INT) {
            long long q = a0.as.i / b0.as.i;   /* trunc toward zero, like the reference engine */
            return v_int(a0.as.i - q * b0.as.i);
        }
        return arith_norm(v_float(fmod(num_d(a0), num_d(b0))));
    }
    case B_POW: {
        Value a0, b0;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: ** expects 2, got %d", (int)n);
        a0 = need_num(in, a[0], line);
        b0 = need_num(in, a[1], line);
        if (a0.t == V_INT && b0.t == V_INT && b0.as.i >= 0) {
            long long base = a0.as.i, e = b0.as.i, r = 1;
            int i, ok = 1;
            if (e == 0) return v_int(1);
            for (i = 0; i < (int)e && i < 64; i++) {
                double d = (double)r * (double)base;
                if (fabs(d) > (double)MAX_SAFE) { ok = 0; break; }
                r = (long long)d;
            }
            if (ok) return v_int(r);
        }
        {
            double r = pow(num_d(a0), num_d(b0));
            if (isnan(r)) xerrf(in, "runtime", line, "math-domain");
            if (isinf(r)) xerrf(in, "runtime", line, "overflow");
            return arith_norm(v_float(r));
        }
    }
    case B_LT: case B_LTE: case B_GT: case B_GTE: {
        int r;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: comparison expects 2");
        if (!((is_num(a[0]) && is_num(a[1])) || (a[0].t == V_STR && a[1].t == V_STR)))
            xerrf(in, "type", line, "bad-operand: %s %s", to_text_val(a[0]), to_text_val(a[1]));
        r = cmp_values(in, a[0], a[1], line);
        if (c == B_LT) return v_bool(r < 0);
        if (c == B_LTE) return v_bool(r <= 0);
        if (c == B_GT) return v_bool(r > 0);
        return v_bool(r >= 0);
    }
    case B_EQ:
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: == expects 2");
        return v_bool(deep_eq(a[0], a[1]));
    case B_NE:
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: != expects 2");
        return v_bool(!deep_eq(a[0], a[1]));
    case B_NOT:
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: not expects 1");
        return v_bool(!truthy(a[0]));
    case B_GET:
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: get expects 2");
        return v_get(in, a[0], a[1], line);
    case B_SETFIELD:
        if (n != 3) xerrf(in, "type", line, "bad-arg-count: assoc expects 3");
        return v_setfield(in, a[0], a[1], a[2], line);
    case B_LEN: {
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: len expects 1");
        if (a[0].t == V_STR) return v_int((long long)u8_len(a[0].as.s.p));
        if (a[0].t == V_LIST) return v_int((long long)a[0].as.list.n);
        if (a[0].t == V_REC) return v_int((long long)a[0].as.rec.n);
        xerrf(in, "type", line, "not-measurable: %s", to_text_val(a[0]));
        return v_void();
    }
    case B_TEXT:
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: text expects 1");
        return v_str_take(to_text_val(a[0]));
    case B_NUM: {
        Value out;
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: num expects 1");
        if (a[0].t == V_BOOL) xerrf(in, "type", line, "not-a-number: %s", to_text_val(a[0]));
        if (a[0].t == V_INT || a[0].t == V_FLOAT) return norm_value(a[0]);
        if (a[0].t == V_STR && parse_number_text(a[0].as.s.p, &out)) return out;
        xerrf(in, "type", line, "not-a-number: %s", to_text_val(a[0]));
        return v_void();
    }
    case B_NORM: {
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: norm expects 1");
        if (a[0].t == V_STR) {
            Value out;
            if (parse_number_text(a[0].as.s.p, &out)) return out;
        }
        return norm_value(a[0]);
    }
    case B_LOWER: case B_UPPER: {
        char *s;
        size_t i;
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: case expects 1");
        if (a[0].t != V_STR) xerrf(in, "type", line, "not-a-string: %s", to_text_val(a[0]));
        s = xstrdup(a[0].as.s.p);
        for (i = 0; s[i]; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (c == B_LOWER) { if (ch >= 'A' && ch <= 'Z') s[i] = (char)(ch + 32); }
            else { if (ch >= 'a' && ch <= 'z') s[i] = (char)(ch - 32); }
        }
        return v_str_take(s);
    }
    case B_SPLIT:
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: split expects 2");
        return b_split(in, a[0], a[1], line);
    case B_JOIN: {
        Buf buf;
        size_t i;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: join expects 2");
        if (a[0].t != V_LIST || a[1].t != V_STR)
            xerrf(in, "type", line, "bad-operand: join expects (list, string)");
        buf_init(&buf);
        for (i = 0; i < a[0].as.list.n; i++) {
            char *piece = to_text_val(a[0].as.list.v[i]);
            if (i) buf_adds(&buf, a[1].as.s.p);
            buf_adds(&buf, piece);
        }
        return v_str_take(buf_take(&buf));
    }
    case B_CONTAINS: {
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: contains expects 2");
        if (a[0].t == V_STR) {
            if (a[1].t != V_STR) xerrf(in, "type", line, "not-a-string: %s", to_text_val(a[1]));
            return v_bool(str_contains(a[0].as.s.p, a[1].as.s.p));
        }
        if (a[0].t == V_LIST) {
            size_t i;
            for (i = 0; i < a[0].as.list.n; i++)
                if (deep_eq(a[0].as.list.v[i], a[1])) return v_bool(1);
            return v_bool(0);
        }
        if (a[0].t == V_REC) {
            if (a[1].t != V_STR) xerrf(in, "type", line, "not-a-key: %s", to_text_val(a[1]));
            return v_bool(rec_find(&a[0], a[1].as.s.p) >= 0);
        }
        xerrf(in, "type", line, "not-containable: %s", to_text_val(a[0]));
        return v_void();
    }
    case B_KEYS: case B_VALUES: {
        Value out = v_list();
        size_t i;
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: keys/values expects 1");
        if (a[0].t != V_REC) xerrf(in, "type", line, "not-a-record: %s", to_text_val(a[0]));
        for (i = 0; i < a[0].as.rec.n; i++)
            list_push(&out, c == B_KEYS ? v_str(a[0].as.rec.k[i]) : a[0].as.rec.v[i]);
        return out;
    }
    case B_APPEND: {
        Value out = v_list();
        size_t i;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: append expects 2");
        if (a[0].t != V_LIST) xerrf(in, "type", line, "not-a-list: %s", to_text_val(a[0]));
        for (i = 0; i < a[0].as.list.n; i++) list_push(&out, a[0].as.list.v[i]);
        list_push(&out, a[1]);
        return out;
    }
    case B_SLICE:
        if (n != 3) xerrf(in, "type", line, "bad-arg-count: slice expects 3");
        return b_slice(in, a[0], a[1], a[2], line);
    case B_SORT: {
        Value out;
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: sort expects 1");
        if (a[0].t != V_LIST) xerrf(in, "type", line, "not-a-list: %s", to_text_val(a[0]));
        out = value_clone(a[0]);
        sort_list(in, &out, line);
        return out;
    }
    case B_SUM: {
        size_t i;
        Value acc = v_int(0);
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: sum expects 1");
        if (a[0].t != V_LIST) xerrf(in, "type", line, "not-a-list: %s", to_text_val(a[0]));
        for (i = 0; i < a[0].as.list.n; i++) acc = add2(in, acc, a[0].as.list.v[i], line);
        return acc;
    }
    case B_JSON:
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: json expects 1");
        return v_str_take(to_json_text(a[0]));
    case B_KIND:
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: kind expects 1");
        return v_str(kind_name(a[0]));
    case B_TRIM: {
        const char *s;
        size_t a0, b0;
        char *out;
        if (n != 1) xerrf(in, "type", line, "bad-arg-count: trim expects 1");
        if (a[0].t != V_STR) xerrf(in, "type", line, "not-a-string: %s", to_text_val(a[0]));
        s = a[0].as.s.p;
        a0 = 0;
        b0 = strlen(s);
        while (a0 < b0 && (s[a0] == ' ' || s[a0] == '\t' || s[a0] == '\n' || s[a0] == '\r' || s[a0] == '\f' || s[a0] == '\v')) a0++;
        while (b0 > a0 && (s[b0 - 1] == ' ' || s[b0 - 1] == '\t' || s[b0 - 1] == '\n' || s[b0 - 1] == '\r' || s[b0 - 1] == '\f' || s[b0 - 1] == '\v')) b0--;
        out = xstrndup(s + a0, b0 - a0);
        return v_str_take(out);
    }
    }
    xerrf(in, "runtime", line, "unknown-builtin");
    return v_void();
}

/* @@NEXT@@ */
/* ====================================================================== */
/* 10. evaluator: special forms                                            */
/* ====================================================================== */

static const char *KW_DEFINE[] = { "node", "\xE2\x9C\xA6", "mint", "\xE2\x9C\xA7", NULL };
static const char *KW_BIND[] = { "letv", "\xE2\x8A\x82", "engrave", NULL };
static const char *KW_REBIND[] = { "rebind", "set!", "\xE2\x8A\x83", NULL };
static const char *KW_REC[] = { "rec", "record", "map", "\xE2\x97\xAB", NULL };
static const char *KW_IF[] = { "cond", "branch", "\xE2\x96\xB7", NULL };
static const char *KW_LOOP[] = { "spin", "iterate", "\xE2\x86\xBA", NULL };
static const char *KW_EACH[] = { "scan", "sweep", "\xE2\x9F\xB2", NULL };
static const char *KW_GIVE[] = { "->", "hand", "\xE2\x8A\xB3", NULL };
static const char *KW_NATIVE[] = { "native", "foreign", "ffi", "\xE2\x9A\xA1", NULL };
static const char *KW_PRINT[] = { "print", "log", "\xE2\x97\x87", NULL };
static const char *KW_BREAK[] = { "sever", "escape", "\xE2\x8F\x8F", NULL };
static const char *KW_CONTINUE[] = { "resume", "proceed", "\xE2\x8F\xA9", NULL };
static const char *KW_AND[] = { "conjoin", "both", "\xE2\x8A\x93", "\xE2\x8B\x8F", NULL };
static const char *KW_OR[] = { "alt", "either", "\xE2\x8A\x94", "\xE2\x8B\x8E", NULL };
static const char *KW_TRY[] = { "try", "attempt", "probe", NULL };
static const char *KW_THEN[] = { "then", "do", "thus", NULL };
static const char *KW_ELSE[] = { "lest", "\xE2\x97\x81", "\xE2\x97\x83", NULL };
static const char *KW_STMT[] = {
    "node", "\xE2\x9C\xA6", "mint", "\xE2\x9C\xA7", "letv", "\xE2\x8A\x82",
    "engrave", "rebind", "set!", "\xE2\x8A\x83", "print", "log", "\xE2\x97\x87",
    "spin", "iterate", "\xE2\x86\xBA", "scan", "sweep", "\xE2\x9F\xB2",
    "sever", "escape", "\xE2\x8F\x8F", "resume", "proceed", "\xE2\x8F\xA9",
    "->", "hand", "\xE2\x8A\xB3", NULL
};

static int in_list(const char **list, const char *s) {
    int i;
    for (i = 0; list[i]; i++) if (strcmp(list[i], s) == 0) return 1;
    return 0;
}
static int is_statement(const char *head) { return in_list(KW_STMT, head); }

typedef struct { int kind; int has_then, has_else; } SpecialKind;
enum {
    S_DEFINE = 1, S_BIND, S_REBIND, S_REC, S_IF, S_LOOP, S_EACH, S_GIVE,
    S_NATIVE, S_PRINT, S_BREAK, S_CONTINUE, S_AND, S_OR, S_TRY
};
static int special_of(const char *h) {
    if (in_list(KW_DEFINE, h)) return S_DEFINE;
    if (in_list(KW_BIND, h)) return S_BIND;
    if (in_list(KW_REBIND, h)) return S_REBIND;
    if (in_list(KW_REC, h)) return S_REC;
    if (in_list(KW_IF, h)) return S_IF;
    if (in_list(KW_LOOP, h)) return S_LOOP;
    if (in_list(KW_EACH, h)) return S_EACH;
    if (in_list(KW_GIVE, h)) return S_GIVE;
    if (in_list(KW_NATIVE, h)) return S_NATIVE;
    if (in_list(KW_PRINT, h)) return S_PRINT;
    if (in_list(KW_BREAK, h)) return S_BREAK;
    if (in_list(KW_CONTINUE, h)) return S_CONTINUE;
    if (in_list(KW_AND, h)) return S_AND;
    if (in_list(KW_OR, h)) return S_OR;
    if (in_list(KW_TRY, h)) return S_TRY;
    return 0;
}

static int ast_is_param_list(Ast *a) {
    size_t i;
    if (!a || a->k != A_NODE) return 0;
    if (a->as.node.head == NULL) return a->as.node.n == 0;
    for (i = 0; i < a->as.node.n; i++)
        if (a->as.node.args[i]->k != A_ID) return 0;
    return 1;
}
static Ast **ast_params(Ast *a, size_t *n) {
    size_t i, k = 0;
    Ast **out;
    if (a->as.node.head == NULL) { *n = 0; return NULL; }
    *n = a->as.node.n + 1;
    out = (Ast **)xmalloc(*n * sizeof(Ast *));
    out[0] = ast_new(A_ID, a->line);
    out[0]->as.id = a->as.node.head;
    for (i = 0; i < a->as.node.n; i++) out[++k] = a->as.node.args[i];
    return out;
}

static Value eval_special(Interp *in, int sp, Ast **args, size_t n, Env *env, int line);

static Value eval_node(Interp *in, Ast *a, Env *env) {
    const char *head = a->as.node.head;
    Ast **args = a->as.node.args;
    size_t n = a->as.node.n;
    int line = a->line;
    int sp;
    Value v;
    if (head == NULL) xerr(in, "parse", line, "empty-node");
    sp = special_of(head);
    if (sp) return eval_special(in, sp, args, n, env, line);
    if (env_get(env, head, &v)) {
        Value *vals;
        size_t i;
        if (v.t != V_FUNC) xerrf(in, "name", line, "not-a-function: %s", head);
        vals = n ? (Value *)xmalloc(n * sizeof(Value)) : NULL;
        for (i = 0; i < n; i++) vals[i] = eval_expr(in, args[i], env);
        return call_function(in, v.as.fn, vals, n, line);
    }
    {
        BCanon c;
        if (builtin_lookup(head, &c)) {
            Value *vals;
            size_t i;
            vals = n ? (Value *)xmalloc(n * sizeof(Value)) : NULL;
            for (i = 0; i < n; i++) vals[i] = eval_expr(in, args[i], env);
            return apply_builtin(in, c, vals, n, line);
        }
    }
    xerrf(in, "name", line, "unknown-node: %s", head);
    return v_void();
}

static Value eval_expr(Interp *in, Ast *a, Env *env) {
    switch (a->k) {
    case A_NUM: return a->as.num.is_float ? v_float(a->as.num.fv) : v_int(a->as.num.iv);
    case A_STR: return v_str(a->as.s);
    case A_BOOL: return v_bool(a->as.b);
    case A_NIL: return v_void();
    case A_ID: {
        Value v;
        if (!env_get(env, a->as.id, &v)) xerrf(in, "name", a->line, "unknown-name: %s", a->as.id);
        return v;
    }
    case A_NODE: return eval_node(in, a, env);
    case A_SEQ: {
        Value out = v_list();
        size_t i;
        for (i = 0; i < a->as.seq.n; i++) list_push(&out, eval_expr(in, a->as.seq.items[i], env));
        return out;
    }
    default: return v_void();
    }
}

static Value eval_body(Interp *in, Ast **items, size_t n, Env *env) {
    Value v = v_void();
    size_t i;
    for (i = 0; i < n; i++) v = eval_expr(in, items[i], env);
    return v;
}

static Value call_function(Interp *in, Func *fn, Value *args, size_t nargs, int line) {
    Env *scope;
    Frame fr;
    Frame *old_fn, *old_loop, *old_try;
    Value result;
    size_t i;
    if (nargs != fn->nparams)
        xerrf(in, "type", line, "bad-arg-count: %s expects %d, got %d", fn->name,
              (int)fn->nparams, (int)nargs);
    if (in->depth >= MAX_DEPTH) xerr(in, "runtime", line, "stack-overflow");
    in->depth++;
    scope = env_new(fn->env);
    for (i = 0; i < fn->nparams; i++) env_define(scope, fn->params[i], args ? args[i] : v_void());
    fr.prev = in->frame;
    in->frame = &fr;
    old_fn = in->fn_frame;
    old_loop = in->loop_frame;
    old_try = in->try_frame;
    in->fn_frame = &fr;
    if (setjmp(fr.jb) == 0) {
        result = eval_body(in, fn->body, fn->nbody, scope);
    } else {
        result = in->ret;
        in->sig = SIG_NONE;
    }
    in->frame = fr.prev;
    in->fn_frame = old_fn;
    in->loop_frame = old_loop;
    in->try_frame = old_try;
    in->depth--;
    return result;
}

static Value eval_special(Interp *in, int sp, Ast **args, size_t n, Env *env, int line) {
    switch (sp) {
    case S_DEFINE: {
        Ast *target;
        if (n < 1) xerr(in, "type", line, "bad-arg-count: define expects at least 1, got 0");
        target = args[0];
        if (target->k != A_ID) xerr(in, "parse", line, "bad-define-target");
        if (n >= 3 && ast_is_param_list(args[1])) {
            size_t np = 0, i;
            Ast **ps = ast_params(args[1], &np);
            Func *fn = (Func *)xmalloc(sizeof(Func));
            memset(fn, 0, sizeof(*fn));
            fn->name = xstrdup(target->as.id);
            fn->nparams = np;
            fn->params = (char **)xmalloc((np ? np : 1) * sizeof(char *));
            for (i = 0; i < np; i++) fn->params[i] = xstrdup(ps[i]->as.id);
            fn->body = args + 2;
            fn->nbody = n - 2;
            fn->env = env;
            env_define(env, fn->name, v_func(fn));
            return v_void();
        }
        if (n == 2) {
            Value v = eval_expr(in, args[1], env);
            env_define(env, target->as.id, v);
            return v;
        }
        xerr(in, "type", line, "bad-define-arity");
        return v_void();
    }
    case S_BIND: {
        Value v;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: bind expects 2, got %d", (int)n);
        if (args[0]->k != A_ID) xerr(in, "parse", line, "bad-bind-target");
        v = eval_expr(in, args[1], env);
        env_define(env, args[0]->as.id, v);
        return v;
    }
    case S_REBIND: {
        Value v;
        if (n != 2) xerrf(in, "type", line, "bad-arg-count: rebind expects 2, got %d", (int)n);
        if (args[0]->k != A_ID) xerr(in, "parse", line, "bad-rebind-target");
        v = eval_expr(in, args[1], env);
        if (!env_set(env, args[0]->as.id, v)) xerrf(in, "name", line, "unknown-name: %s", args[0]->as.id);
        return v;
    }
    case S_REC: {
        Value rec = v_rec();
        size_t i;
        if (n % 2 != 0) xerrf(in, "parse", line, "rec-arity: expected key/value pairs, got %d forms", (int)n);
        for (i = 0; i < n; i += 2) {
            const char *key = NULL;
            if (args[i]->k == A_STR) key = args[i]->as.s;
            else if (args[i]->k == A_ID) key = args[i]->as.id;
            else xerr(in, "type", line, "bad-record-key");
            if (is_numeric_key(key)) xerrf(in, "type", line, "bad-record-key: %s", key);
            rec_set(&rec, key, eval_expr(in, args[i + 1], env));
        }
        return rec;
    }
    case S_IF: {
        int cond, marked = 0;
        Ast **then_items = NULL, **else_items = NULL;
        size_t then_n = 0, else_n = 0, i;
        int target_then = 1;
        if (n < 1) xerr(in, "type", line, "bad-arg-count: if expects at least 1, got 0");
        cond = truthy(eval_expr(in, args[0], env));
        for (i = 1; i < n; i++) {
            if (args[i]->k == A_NODE &&
                (in_list(KW_THEN, args[i]->as.node.head) || in_list(KW_ELSE, args[i]->as.node.head))) {
                marked = 1;
                break;
            }
        }
        if (marked) {
            size_t cap = 1;
            for (i = 1; i < n; i++) {
                if (args[i]->k == A_NODE &&
                    (in_list(KW_THEN, args[i]->as.node.head) || in_list(KW_ELSE, args[i]->as.node.head)))
                    cap += args[i]->as.node.n;
                else
                    cap += 1;
            }
            then_items = (Ast **)xmalloc(cap * sizeof(Ast *));
            else_items = (Ast **)xmalloc(cap * sizeof(Ast *));
            for (i = 1; i < n; i++) {
                Ast *a = args[i];
                if (a->k == A_NODE && in_list(KW_THEN, a->as.node.head)) {
                    size_t j;
                    target_then = 1;
                    for (j = 0; j < a->as.node.n; j++) then_items[then_n++] = a->as.node.args[j];
                } else if (a->k == A_NODE && in_list(KW_ELSE, a->as.node.head)) {
                    size_t j;
                    target_then = 0;
                    for (j = 0; j < a->as.node.n; j++) else_items[else_n++] = a->as.node.args[j];
                } else if (target_then) {
                    then_items[then_n++] = a;
                } else {
                    else_items[else_n++] = a;
                }
            }
            return eval_body(in, cond ? then_items : else_items, cond ? then_n : else_n, env);
        }
        if (n - 1 == 2) {
            return eval_expr(in, cond ? args[1] : args[2], env);
        }
        return eval_body(in, args + 1, n - 1, env);
    }
    case S_LOOP: {
        Frame fr;
        Frame *old_loop = in->loop_frame;
        Frame *old_fn = in->fn_frame;
        if (n < 1) xerr(in, "type", line, "bad-arg-count: loop expects a condition");
        fr.prev = in->frame;
        in->frame = &fr;
        in->loop_frame = &fr;
        for (;;) {
            int brk = 0;
            if (!truthy(eval_expr(in, args[0], env))) break;
            in->loops++;
            if (in->loops > MAX_LOOP) xerr(in, "runtime", line, "loop-limit");
            if (setjmp(fr.jb) == 0) {
                eval_body(in, args + 1, n - 1, env);
            } else {
                if (in->sig == SIG_BREAK) { in->sig = SIG_NONE; brk = 1; }
                else if (in->sig == SIG_CONTINUE) { in->sig = SIG_NONE; }
                else { in->frame = fr.prev; in->loop_frame = old_loop; in->fn_frame = old_fn; xerr(in, "runtime", line, "invalid-signal"); }
            }
            if (brk) break;
        }
        in->frame = fr.prev;
        in->loop_frame = old_loop;
        in->fn_frame = old_fn;
        return v_void();
    }
    case S_EACH: {
        Ast *target;
        Value coll, out = v_void();
        Env *scope;
        Frame fr;
        Frame *old_loop = in->loop_frame;
        Frame *old_fn = in->fn_frame;
        int k;
        if (n < 2) xerr(in, "type", line, "bad-arg-count: each expects a name and a collection");
        target = args[0];
        if (target->k != A_ID) xerr(in, "parse", line, "bad-each-target");
        coll = eval_expr(in, args[1], env);
        scope = env_new(env);
        fr.prev = in->frame;
        in->frame = &fr;
        in->loop_frame = &fr;
        if (coll.t == V_LIST) k = 1;
        else if (coll.t == V_REC) k = 2;
        else if (coll.t == V_STR) k = 3;
        else if (coll.t == V_VOID) k = 0;
        else {
            in->frame = fr.prev;
            in->loop_frame = old_loop;
            xerr(in, "type", line, "not-iterable");
            return v_void();
        }
        if (k == 1) {
            size_t i, cnt = coll.as.list.n;
            for (i = 0; i < cnt; i++) {
                int brk = 0;
                in->loops++;
                if (in->loops > MAX_LOOP) xerr(in, "runtime", line, "loop-limit");
                env_define(scope, target->as.id, coll.as.list.v[i]);
                if (setjmp(fr.jb) == 0) out = eval_body(in, args + 2, n - 2, scope);
                else if (in->sig == SIG_BREAK) { in->sig = SIG_NONE; brk = 1; }
                else if (in->sig == SIG_CONTINUE) { in->sig = SIG_NONE; }
                else { in->frame = fr.prev; in->loop_frame = old_loop; in->fn_frame = old_fn; xerr(in, "runtime", line, "invalid-signal"); }
                if (brk) break;
            }
        } else if (k == 2) {
            size_t i, cnt = coll.as.rec.n;
            for (i = 0; i < cnt; i++) {
                int brk = 0;
                in->loops++;
                if (in->loops > MAX_LOOP) xerr(in, "runtime", line, "loop-limit");
                env_define(scope, target->as.id, v_str(coll.as.rec.k[i]));
                if (setjmp(fr.jb) == 0) out = eval_body(in, args + 2, n - 2, scope);
                else if (in->sig == SIG_BREAK) { in->sig = SIG_NONE; brk = 1; }
                else if (in->sig == SIG_CONTINUE) { in->sig = SIG_NONE; }
                else { in->frame = fr.prev; in->loop_frame = old_loop; in->fn_frame = old_fn; xerr(in, "runtime", line, "invalid-signal"); }
                if (brk) break;
            }
        } else if (k == 3) {
            size_t cnt = u8_len(coll.as.s.p), i, *off = (size_t *)xmalloc((cnt + 1) * sizeof(size_t));
            u8_offsets(coll.as.s.p, off);
            for (i = 0; i < cnt; i++) {
                int brk = 0;
                in->loops++;
                if (in->loops > MAX_LOOP) xerr(in, "runtime", line, "loop-limit");
                env_define(scope, target->as.id,
                           v_str_take(xstrndup(coll.as.s.p + off[i], off[i + 1] - off[i])));
                if (setjmp(fr.jb) == 0) out = eval_body(in, args + 2, n - 2, scope);
                else if (in->sig == SIG_BREAK) { in->sig = SIG_NONE; brk = 1; }
                else if (in->sig == SIG_CONTINUE) { in->sig = SIG_NONE; }
                else { in->frame = fr.prev; in->loop_frame = old_loop; in->fn_frame = old_fn; xerr(in, "runtime", line, "invalid-signal"); }
                if (brk) break;
            }
            free(off);
        }
        in->frame = fr.prev;
        in->loop_frame = old_loop;
        in->fn_frame = old_fn;
        (void)out;
        return v_void();
    }
    case S_GIVE: {
        Value v;
        if (n == 0) v = v_void();
        else if (n == 1) v = eval_expr(in, args[0], env);
        else {
            size_t i;
            Value lst = v_list();
            for (i = 0; i < n; i++) list_push(&lst, eval_expr(in, args[i], env));
            v = lst;
        }
        if (!in->fn_frame) xerr(in, "runtime", line, "return-outside-function");
        raise_sig(in, SIG_RETURN, v, in->fn_frame);
        return v_void();
    }
    case S_NATIVE: {
        const char *name;
        Value *vals;
        size_t i;
        if (n < 1) xerr(in, "type", line, "bad-arg-count: native expects a host function name");
        if (args[0]->k == A_ID) name = args[0]->as.id;
        else if (args[0]->k == A_STR) name = args[0]->as.s;
        else { xerr(in, "parse", line, "bad-native-name"); name = ""; }
        vals = (n > 1) ? (Value *)xmalloc((n - 1) * sizeof(Value)) : NULL;
        for (i = 1; i < n; i++) vals[i - 1] = eval_expr(in, args[i], env);
        return call_host(in, name, vals, n - 1, line);
    }
    case S_PRINT: {
        size_t i;
        for (i = 0; i < n; i++) {
            Value v = eval_expr(in, args[i], env);
            char *t = to_text_val(v);
            if (in->out) in->out(t);
            else fprintf(stdout, "%s\n", t);
        }
        return v_void();
    }
    case S_BREAK:
        if (!in->loop_frame) xerr(in, "runtime", line, "break-outside-loop");
        raise_sig(in, SIG_BREAK, v_void(), in->loop_frame);
        return v_void();
    case S_CONTINUE:
        if (!in->loop_frame) xerr(in, "runtime", line, "continue-outside-loop");
        raise_sig(in, SIG_CONTINUE, v_void(), in->loop_frame);
        return v_void();
    case S_AND: {
        size_t i;
        for (i = 0; i < n; i++)
            if (!truthy(eval_expr(in, args[i], env))) return v_bool(0);
        return v_bool(1);
    }
    case S_OR: {
        size_t i;
        for (i = 0; i < n; i++)
            if (truthy(eval_expr(in, args[i], env))) return v_bool(1);
        return v_bool(0);
    }
    case S_TRY: {
        Frame fr;
        Frame *old_try = in->try_frame;
        Frame *old_frame = in->frame;
        Frame *old_fn = in->fn_frame;
        Frame *old_loop = in->loop_frame;
        fr.prev = in->frame;
        in->frame = &fr;
        in->try_frame = &fr;
        if (setjmp(fr.jb) == 0) {
            Value v = eval_body(in, args, n, env);
            Value out = v_rec();
            in->frame = old_frame;
            in->try_frame = old_try;
            in->fn_frame = old_fn;
            in->loop_frame = old_loop;
            rec_set(&out, "ok", v_bool(1));
            rec_set(&out, "value", v);
            return out;
        }
        {
            Value out = v_rec();
            in->frame = old_frame;
            in->try_frame = old_try;
            in->fn_frame = old_fn;
            in->loop_frame = old_loop;
            rec_set(&out, "ok", v_bool(0));
            rec_set(&out, "value", v_void());
            rec_set(&out, "kind", v_str(in->err.kind));
            rec_set(&out, "message", v_str(in->err.msg));
            rec_set(&out, "line", v_int(in->err.line));
            in->err.set = 0;
            return out;
        }
    }
    }
    return v_void();
}

/* ====================================================================== */
/* 11. program evaluation                                                  */
/* ====================================================================== */

static Value interp_eval_ast(Interp *in, Ast *prog) {
    Value last_return = v_void(), last_value = v_void();
    int has_return = 0, has_value = 0;
    size_t i;
    Frame top;
    in->frame = &top;
    in->top_frame = &top;
    in->fn_frame = NULL;
    in->loop_frame = NULL;
    in->try_frame = NULL;
    in->sig = SIG_NONE;
    in->err.set = 0;
    if (setjmp(top.jb) != 0) {
        in->frame = top.prev;
        in->top_frame = NULL;
        in->fn_frame = NULL;
        in->loop_frame = NULL;
        in->try_frame = NULL;
        return v_void();
    }
    for (i = 0; i < prog->as.program.n; i++) {
        Ast *st = prog->as.program.items[i];
        Frame fr;
        Frame *old_fn = in->fn_frame;
        int bare = !(st->k == A_NODE && st->as.node.head && is_statement(st->as.node.head));
        Value v;
        fr.prev = in->frame;
        in->frame = &fr;
        in->fn_frame = &fr;
        if (setjmp(fr.jb) == 0) {
            v = eval_expr(in, st, in->globals);
            if (bare) { last_value = v; has_value = 1; }
            else { has_value = 0; }
        } else {
            last_return = in->ret;
            has_return = 1;
            has_value = 0;
            in->sig = SIG_NONE;
        }
        in->frame = fr.prev;
        in->fn_frame = old_fn;
    }
    in->frame = top.prev;
    in->top_frame = NULL;
    if (has_return) return last_return;
    if (has_value) return last_value;
    return v_void();
}

/* ====================================================================== */
/* 12. host bridge (all implemented in C, no external host)                */
/* ====================================================================== */

struct Host {
    Value cols;              /* record: collection -> list */
    char *data_path;
    char *programs_dir;
    Interp *in;
};

static const char *asset_dir(const char *kind);
static char *asset_file(const char *kind, const char *name);

static void host_load(Host *h) {
    FILE *f;
    char *src, *err = NULL;
    Buf b;
    char tmp[8192];
    size_t got;
    h->cols = v_rec();
    f = fopen(h->data_path, "rb");
    if (!f) return;
    buf_init(&b);
    while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) buf_addn(&b, tmp, got);
    fclose(f);
    src = buf_take(&b);
    if (src && src[0]) {
        Value v;
        if (json_parse(src, &v, &err) && v.t == V_REC) h->cols = v;
    }
}

static void host_save(Host *h) {
    FILE *f;
    char *txt = to_json_text(h->cols);
    f = fopen(h->data_path, "wb");
    if (!f) return;
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
    free(txt);
}

static Value *coll_ptr(Host *h, Value name, int create) {
    long i;
    if (name.t != V_STR) return NULL;
    i = rec_find(&h->cols, name.as.s.p);
    if (i >= 0) {
        if (h->cols.as.rec.v[i].t == V_LIST) return &h->cols.as.rec.v[i];
        if (!create) return NULL;
        h->cols.as.rec.v[i] = v_list();
        return &h->cols.as.rec.v[i];
    }
    if (!create) return NULL;
    rec_set(&h->cols, name.as.s.p, v_list());
    i = rec_find(&h->cols, name.as.s.p);
    return i < 0 ? NULL : &h->cols.as.rec.v[i];
}

static int host_as_int(Value v, long long *out) {
    if (v.t == V_BOOL) return 0;
    if (v.t == V_INT) { *out = v.as.i; return 1; }
    if (v.t == V_FLOAT && floor(v.as.f) == v.as.f) { *out = (long long)v.as.f; return 1; }
    if (v.t == V_STR) {
        long long iv;
        char *end = NULL;
        errno = 0;
        iv = strtoll(v.as.s.p, &end, 10);
        if (end && *end == 0 && errno == 0) { *out = iv; return 1; }
    }
    return 0;
}

static Value h_store_list(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    (void)in; (void)n;
    lst = coll_ptr(h, args[0], 0);
    return (lst && lst->t == V_LIST) ? value_clone(*lst) : v_list();
}
static Value h_store_count(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    (void)in; (void)n;
    lst = coll_ptr(h, args[0], 0);
    return v_int((lst && lst->t == V_LIST) ? (long long)lst->as.list.n : 0);
}
static Value h_store_get(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    long long key;
    size_t i;
    (void)n;
    if (!host_as_int(args[1], &key)) xerr(in, "host", 0, "host-error: store.get: bad-key");
    lst = coll_ptr(h, args[0], 0);
    if (!lst || lst->t != V_LIST) return v_void();
    for (i = 0; i < lst->as.list.n; i++) {
        Value rec = lst->as.list.v[i];
        long long rid;
        if (rec.t != V_REC) continue;
        if (host_as_int(rec_get(&rec, "id"), &rid) && rid == key) return value_clone(rec);
    }
    return v_void();
}
static Value h_store_put(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *coll, rec, idv;
    long long id = 0;
    int have_id = 0;
    size_t i;
    (void)n;
    if (args[0].t != V_STR) xerr(in, "host", 0, "host-error: store.put: collection-must-be-string");
    if (args[1].t != V_REC) xerr(in, "host", 0, "host-error: store.put: record-expected");
    rec = value_clone(args[1]);
    idv = rec_get(&rec, "id");
    if (idv.t != V_VOID) have_id = host_as_int(idv, &id);
    coll = coll_ptr(h, args[0], 1);
    if (have_id) {
        for (i = 0; i < coll->as.list.n; i++) {
            Value old = coll->as.list.v[i];
            long long oid;
            if (old.t == V_REC && host_as_int(rec_get(&old, "id"), &oid) && oid == id) {
                coll->as.list.v[i] = rec;
                host_save(h);
                return value_clone(rec);
            }
        }
    }
    if (!have_id) {
        long long mx = 0;
        for (i = 0; i < coll->as.list.n; i++) {
            Value old = coll->as.list.v[i];
            long long oid;
            if (old.t == V_REC && host_as_int(rec_get(&old, "id"), &oid) && oid > mx) mx = oid;
        }
        id = mx + 1;
        rec_set(&rec, "id", v_int(id));
    }
    list_push(coll, rec);
    host_save(h);
    return value_clone(rec);
}
static Value h_store_patch(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    long long key;
    size_t i;
    (void)n;
    if (!host_as_int(args[1], &key)) xerr(in, "host", 0, "host-error: store.patch: bad-key");
    if (args[2].t != V_REC) xerr(in, "host", 0, "host-error: store.patch: changes-expected");
    lst = coll_ptr(h, args[0], 0);
    if (!lst || lst->t != V_LIST) return v_void();
    for (i = 0; i < lst->as.list.n; i++) {
        Value rec = lst->as.list.v[i];
        long long rid;
        if (rec.t == V_REC && host_as_int(rec_get(&rec, "id"), &rid) && rid == key) {
            Value merged = value_clone(rec);
            size_t j;
            for (j = 0; j < args[2].as.rec.n; j++) {
                rec_set(&merged, args[2].as.rec.k[j], value_clone(args[2].as.rec.v[j]));
            }
            lst->as.list.v[i] = merged;
            host_save(h);
            return value_clone(merged);
        }
    }
    return v_void();
}
static Value h_store_remove(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    long long key;
    size_t i;
    (void)n;
    if (!host_as_int(args[1], &key)) xerr(in, "host", 0, "host-error: store.remove: bad-key");
    lst = coll_ptr(h, args[0], 0);
    if (!lst || lst->t != V_LIST) return v_bool(0);
    for (i = 0; i < lst->as.list.n; i++) {
        Value rec = lst->as.list.v[i];
        long long rid;
        if (rec.t == V_REC && host_as_int(rec_get(&rec, "id"), &rid) && rid == key) {
            memmove(&lst->as.list.v[i], &lst->as.list.v[i + 1],
                    (lst->as.list.n - i - 1) * sizeof(Value));
            lst->as.list.n--;
            host_save(h);
            return v_bool(1);
        }
    }
    return v_bool(0);
}
static Value h_store_clear(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    Value *lst;
    long long cnt;
    (void)in; (void)n;
    lst = coll_ptr(h, args[0], 0);
    cnt = (lst && lst->t == V_LIST) ? (long long)lst->as.list.n : 0;
    if (args[0].t == V_STR) rec_set(&h->cols, args[0].as.s.p, v_list());
    host_save(h);
    return v_int(cnt);
}
static Value h_clock_now(Interp *in, Value *args, size_t n, void *ud) {
    char buf[40];
    time_t t;
    struct tm tmv;
    struct timespec ts;
    (void)in; (void)args; (void)n; (void)ud;
    timespec_get(&ts, TIME_UTC);
    t = ts.tv_sec;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ts.tv_nsec / 1000000));
    return v_str(buf);
}
static Value h_lib_say(Interp *in, Value *args, size_t n, void *ud) {
    char *t;
    (void)ud; (void)n;
    t = to_text_val(args[0]);
    if (in->out) in->out(t);
    else fprintf(stdout, "%s\n", t);
    return v_void();
}
static Value h_lib_emit(Interp *in, Value *args, size_t n, void *ud) {
    (void)in; (void)args; (void)n; (void)ud;
    return v_void();
}
static Value h_lang_eval(Interp *in, Value *args, size_t n, void *ud) {
    Interp *child;
    char *err = NULL;
    int line = 0;
    Ast *prog;
    (void)ud; (void)n;
    if (args[0].t != V_STR) xerr(in, "host", 0, "bad-host-args: lang.eval source must be a string");
    child = interp_new(in->host, in->out);
    prog = parse_source(args[0].as.s.p, &err, &line);
    if (!prog) xerrf(in, "host", 0, "host-error: lang.eval: %s", err ? err : "parse");
    return interp_eval_ast(child, prog);
}
static Value h_lang_names(Interp *in, Value *args, size_t n, void *ud) {
    Value out = v_list();
    size_t i;
    (void)ud; (void)args; (void)n;
    for (i = 0; i < in->globals->n; i++)
        if (in->globals->vals[i].t == V_FUNC) list_push(&out, v_str(in->globals->names[i]));
    return out;
}
static Value h_lang_run(Interp *in, Value *args, size_t n, void *ud) {
    Host *h = (Host *)ud;
    char *path, *src, *err = NULL;
    int line = 0;
    Ast *prog;
    Interp *child;
    Value fnval, *callargs = NULL;
    size_t i, k = 0;
    if (n < 2 || args[0].t != V_STR || args[1].t != V_STR)
        xerr(in, "host", 0, "bad-host-args: lang.run(file, fn, args)");
    path = asset_file("programs", args[0].as.s.p);
    src = read_file_text(path, NULL, &err);
    if (!src) xerrf(in, "host", 0, "host-error: lang.run: %s", err ? err : "file");
    prog = parse_source(clean_source(src), &err, &line);
    if (!prog) xerrf(in, "host", 0, "host-error: lang.run: %s", err ? err : "parse");
    child = interp_new(h, in->out);
    interp_eval_ast(child, prog);
    if (!env_get(child->globals, args[1].as.s.p, &fnval) || fnval.t != V_FUNC)
        xerrf(in, "host", 0, "host-error: lang.run: no function %s", args[1].as.s.p);
    if (n >= 3 && args[2].t == V_LIST) {
        k = args[2].as.list.n;
        callargs = k ? (Value *)xmalloc(k * sizeof(Value)) : NULL;
        for (i = 0; i < k; i++) callargs[i] = args[2].as.list.v[i];
    }
    return call_function(child, fnval.as.fn, callargs, k, 0);
}

typedef struct { const char *name; NativeFn fn; } HostRow;
static HostRow HOST_TABLE[] = {
    { "store.list", h_store_list }, { "store.get", h_store_get },
    { "store.put", h_store_put }, { "store.patch", h_store_patch },
    { "store.remove", h_store_remove }, { "store.count", h_store_count },
    { "store.clear", h_store_clear }, { "clock.now", h_clock_now },
    { "lib.say", h_lib_say }, { "lib.emit", h_lib_emit },
    { "lang.eval", h_lang_eval }, { "lang.run", h_lang_run },
    { "lang.names", h_lang_names }, { NULL, NULL }
};

static Value call_host(Interp *in, const char *name, Value *args, size_t n, int line) {
    int i;
    for (i = 0; HOST_TABLE[i].name; i++) {
        if (strcmp(HOST_TABLE[i].name, name) == 0) {
            return HOST_TABLE[i].fn(in, args, n, in->host);
        }
    }
    xerrf(in, "host", line, "unknown-host-function: %s", name);
    return v_void();
}

/* @@NEXT@@ */
/* ====================================================================== */
/* 13. paths, interpreter construction                                     */
/* ====================================================================== */

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP "/"
#endif

static char *exe_dir(void) {
#ifdef _WIN32
    char buf[4096];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
    if (n == 0) return xstrdup(".");
    buf[n] = 0;
    {
        char *slash = strrchr(buf, '\\');
        if (slash) *slash = 0;
    }
    return xstrdup(buf);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return xstrdup(".");
    buf[n] = 0;
    {
        char *slash = strrchr(buf, '/');
        if (slash) *slash = 0;
    }
    return xstrdup(buf);
#endif
}

static int is_dir(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char *join_path(const char *a, const char *b) {
    return xasprintf("%s" PATH_SEP "%s", a, b);
}

/* find a project asset directory: cwd/kind, exe/kind, exe/../kind, exe/../../kind */
static const char *asset_dir(const char *kind) {
    static char cache[8][4096];
    static int cache_n = 0;
    static const char *cache_key[8];
    int i;
    char *ed;
    const char *cands[4];
    for (i = 0; i < cache_n; i++) {
        if (!strcmp(cache_key[i], kind)) return cache[i];
    }
    ed = exe_dir();
    cands[0] = kind;
    cands[1] = join_path(ed, kind);
    cands[2] = join_path(join_path(ed, ".."), kind);
    cands[3] = join_path(join_path(join_path(ed, ".."), ".."), kind);
    for (i = 0; i < 4; i++) {
        if (is_dir(cands[i])) {
            snprintf(cache[cache_n], sizeof(cache[cache_n]), "%s", cands[i]);
            cache_key[cache_n] = kind;
            cache_n++;
            return cache[cache_n - 1];
        }
    }
    snprintf(cache[cache_n], sizeof(cache[cache_n]), "%s", kind);
    cache_key[cache_n] = kind;
    cache_n++;
    return cache[cache_n - 1];
}

static char *asset_file(const char *kind, const char *name) {
    return join_path(asset_dir(kind), name);
}

static char *data_path(void) {
    const char *env = getenv("NEXUS_DATA");
    char *dir, *p;
    if (env && env[0]) return xstrdup(env);
    dir = (char *)asset_dir("data");
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    p = join_path(dir, "nexus.json");
    return p;
}

static Interp *interp_new(Host *host, void (*out)(const char *)) {
    Interp *in = (Interp *)xmalloc(sizeof(Interp));
    memset(in, 0, sizeof(*in));
    in->host = host;
    in->globals = env_new(NULL);
    in->out = out;
    if (host && !host->in) host->in = in;
    return in;
}

static Value eval_source(Interp *in, const char *src, char **err, int *err_line, const char *module_name) {
    char *clean = clean_source(xstrdup(src));
    Ast *prog;
    *err = NULL;
    *err_line = 0;
    (void)module_name;
    prog = parse_source(clean, err, err_line);
    if (!prog) return v_void();
    return interp_eval_ast(in, prog);
}

/* ====================================================================== */
/* 14. AST / token JSON for the CLI                                        */
/* ====================================================================== */

static void ast_json(Buf *b, Ast *a) {
    switch (a->k) {
    case A_PROGRAM: {
        size_t i;
        buf_printf(b, "{\"k\": \"program\", \"body\": [");
        for (i = 0; i < a->as.program.n; i++) {
            if (i) buf_adds(b, ", ");
            ast_json(b, a->as.program.items[i]);
        }
        buf_adds(b, "]}");
        break;
    }
    case A_NODE: {
        size_t i;
        buf_adds(b, "{\"k\": \"node\", \"head\": ");
        if (a->as.node.head) json_quote(b, a->as.node.head);
        else buf_adds(b, "null");
        buf_adds(b, ", \"args\": [");
        for (i = 0; i < a->as.node.n; i++) {
            if (i) buf_adds(b, ", ");
            ast_json(b, a->as.node.args[i]);
        }
        buf_printf(b, "], \"line\": %d}", a->line);
        break;
    }
    case A_SEQ: {
        size_t i;
        buf_adds(b, "{\"k\": \"seq\", \"items\": [");
        for (i = 0; i < a->as.seq.n; i++) {
            if (i) buf_adds(b, ", ");
            ast_json(b, a->as.seq.items[i]);
        }
        buf_printf(b, "], \"line\": %d}", a->line);
        break;
    }
    case A_NUM:
        buf_adds(b, "{\"k\": \"num\", \"v\": ");
        if (a->as.num.is_float) buf_adds(b, float_text(a->as.num.fv));
        else buf_printf(b, "%lld", a->as.num.iv);
        buf_printf(b, ", \"line\": %d}", a->line);
        break;
    case A_STR:
        buf_adds(b, "{\"k\": \"str\", \"v\": ");
        json_quote(b, a->as.s);
        buf_printf(b, ", \"line\": %d}", a->line);
        break;
    case A_BOOL:
        buf_printf(b, "{\"k\": \"bool\", \"v\": %s, \"line\": %d}", a->as.b ? "true" : "false", a->line);
        break;
    case A_NIL:
        buf_printf(b, "{\"k\": \"nil\", \"line\": %d}", a->line);
        break;
    case A_ID:
        buf_adds(b, "{\"k\": \"id\", \"name\": ");
        json_quote(b, a->as.id);
        buf_printf(b, ", \"line\": %d}", a->line);
        break;
    }
}

static const char *tok_kind_name(TokKind k) {
    switch (k) {
    case T_LPAREN: return "lparen";
    case T_RPAREN: return "rparen";
    case T_LBRACKET: return "lbracket";
    case T_RBRACKET: return "rbracket";
    case T_COMMA: return "comma";
    case T_STRING: return "string";
    case T_NUMBER: return "number";
    case T_WORD: return "word";
    case T_EOF: return "eof";
    }
    return "eof";
}

static void tokens_json(Buf *b, TokList *tl) {
    size_t i;
    buf_adds(b, "[");
    for (i = 0; i < tl->n; i++) {
        Token *t = &tl->v[i];
        if (i) buf_adds(b, ", ");
        buf_adds(b, "{\"kind\": ");
        json_quote(b, tok_kind_name(t->kind));
        buf_adds(b, ", \"text\": ");
        json_quote(b, t->text);
        buf_printf(b, ", \"line\": %d, \"col\": %d}", t->line, t->col);
    }
    buf_adds(b, "]");
}

/* ====================================================================== */
/* 15. CLI                                                                 */
/* ====================================================================== */

static void print_error(Interp *in) {
    Buf b;
    buf_init(&b);
    buf_adds(&b, "{\"error\": {\"kind\": ");
    json_quote(&b, in->err.kind[0] ? in->err.kind : "runtime");
    buf_adds(&b, ", \"message\": ");
    json_quote(&b, in->err.msg);
    if (in->err.line > 0) buf_printf(&b, ", \"line\": %d", in->err.line);
    else buf_adds(&b, ", \"line\": null");
    buf_adds(&b, "}}");
    printf("%s\n", buf_take(&b));
}

static void out_to_stderr(const char *s) { fprintf(stderr, "%s\n", s); }

static int cli_run(int argc, char **argv) {
    char *src, *err = NULL;
    int line = 0;
    Value last;
    Interp *in;
    Host *host;
    (void)argc;
    src = read_file_text(argv[2], NULL, &err);
    if (!src) {
        printf("{\"error\": {\"kind\": \"runtime\", \"message\": ");
        {
            Buf b;
            buf_init(&b);
            json_quote(&b, err ? err : "file-not-found");
            printf("%s", b.p);
        }
        printf(", \"line\": null}}\n");
        return 1;
    }
    host = (Host *)xmalloc(sizeof(Host));
    memset(host, 0, sizeof(*host));
    host->data_path = data_path();
    host->programs_dir = (char *)asset_dir("programs");
    host->cols = v_rec();
    host_load(host);
    in = interp_new(host, out_to_stderr);
    last = eval_source(in, src, &err, &line, "main");
    if (err) {
        printf("{\"error\": {\"kind\": \"%s\", \"message\": ", "parse");
        {
            Buf b;
            buf_init(&b);
            json_quote(&b, err);
            printf("%s", b.p);
        }
        printf(", \"line\": %d}}\n", line);
        return 1;
    }
    if (in->err.set) { print_error(in); return 1; }
    if (argc >= 4) {
        char *req_src = NULL;
        Value req;
        Value fnval;
        if (!strcmp(argv[3], "-")) {
            Buf b;
            char tmp[8192];
            size_t got;
            buf_init(&b);
            while ((got = fread(tmp, 1, sizeof(tmp), stdin)) > 0) buf_addn(&b, tmp, got);
            req_src = buf_take(&b);
        } else {
            req_src = xstrdup(argv[3]);
        }
        if (!json_parse(req_src, &req, &err)) {
            printf("{\"error\": {\"kind\": \"runtime\", \"message\": \"bad-request-json\", \"line\": null}}\n");
            return 1;
        }
        if (!env_get(in->globals, "handle", &fnval) || fnval.t != V_FUNC) {
            printf("{\"error\": {\"kind\": \"name\", \"message\": \"unknown-name: handle\", \"line\": null}}\n");
            return 1;
        }
        last = call_function(in, fnval.as.fn, &req, 1, 0);
        if (in->err.set) { print_error(in); return 1; }
    }
    {
        Buf b;
        buf_init(&b);
        json_pretty(&b, last, 0);
        printf("%s\n", b.p);
    }
    return 0;
}

static int cli_ast(const char *path) {
    char *src = read_file_text(path, NULL, NULL);
    char *err = NULL;
    int line = 0;
    Ast *prog;
    Buf b;
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    prog = parse_source(clean_source(src), &err, &line);
    if (!prog) {
        Buf e;
        buf_init(&e);
        buf_adds(&e, "{\"error\": {\"kind\": \"parse\", \"message\": ");
        json_quote(&e, err);
        buf_printf(&e, ", \"line\": %d}}", line);
        printf("%s\n", e.p);
        return 1;
    }
    buf_init(&b);
    ast_json(&b, prog);
    printf("%s\n", b.p);
    return 0;
}

static int cli_tokens(const char *path) {
    char *src = read_file_text(path, NULL, NULL);
    char *err = NULL;
    int line = 0;
    TokList tl;
    Buf b;
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    tl = lex_source(clean_source(src), &err, &line);
    if (err) {
        Buf e;
        buf_init(&e);
        buf_adds(&e, "{\"error\": {\"kind\": \"lex\", \"message\": ");
        json_quote(&e, err);
        buf_printf(&e, ", \"line\": %d}}", line);
        printf("%s\n", e.p);
        return 1;
    }
    buf_init(&b);
    tokens_json(&b, &tl);
    printf("%s\n", b.p);
    return 0;
}

/* --- built-in smoke tests (language level, no external host) --- */
static int tests_failed = 0;
static int tests_passed = 0;

static void t_ok(const char *src, const char *expect) {
    Interp *in;
    Host host;
    char *err = NULL;
    int line = 0;
    Value v;
    char *got;
    memset(&host, 0, sizeof(host));
    host.cols = v_rec();
    host.data_path = xstrdup("nexus-c-test.json");
    host.programs_dir = (char *)"programs";
    in = interp_new(&host, out_to_stderr);
    v = eval_source(in, src, &err, &line, "test");
    if (err || in->err.set) {
        tests_failed++;
        fprintf(stderr, "FAIL %s -> %s (%s)\n", src, err ? err : in->err.msg, err ? "parse" : in->err.kind);
        return;
    }
    got = to_json_text(v);
    if (strcmp(got, expect) != 0) {
        tests_failed++;
        fprintf(stderr, "FAIL %s -> %s (want %s)\n", src, got, expect);
    } else {
        tests_passed++;
    }
}
static void t_err(const char *src, const char *kind) {
    Interp *in;
    Host host;
    char *err = NULL;
    int line = 0;
    memset(&host, 0, sizeof(host));
    host.cols = v_rec();
    host.data_path = xstrdup("nexus-c-test.json");
    host.programs_dir = (char *)"programs";
    in = interp_new(&host, out_to_stderr);
    (void)eval_source(in, src, &err, &line, "test");
    if (err) {
        tests_failed++;
        fprintf(stderr, "FAIL %s -> parse error %s\n", src, err);
        return;
    }
    if (!in->err.set || strcmp(in->err.kind, kind) != 0) {
        tests_failed++;
        fprintf(stderr, "FAIL %s -> kind %s (want %s)\n", src,
                in->err.set ? in->err.kind : "none", kind);
    } else {
        tests_passed++;
    }
}
static int cli_test(void) {
    t_ok("(+ 1 2)", "3");
    t_ok("(\x2B 1 2)", "3");
    t_ok("(+ (* 2 3) 4)", "10");
    t_ok("(/ 6 2)", "3");
    t_ok("(/ 7 2)", "3.5");
    t_ok("(% -7 3)", "-1");
    t_ok("(** 2 10)", "1024");
    t_ok("(< 1 2)", "true");
    t_ok("(== 1 1.0)", "true");
    t_ok("(not #f)", "true");
    t_ok("(conjoin #t #t)", "true");
    t_ok("(alt #f #t)", "true");
    t_ok("(len \"hello\")", "5");
    t_ok("(len \"\xE4\xBD\xA0\xE5\xA5\xBD\")", "2");
    t_ok("(text 3.5)", "\"3.5\"");
    t_ok("(num \"12\")", "12");
    t_ok("(norm 3.0)", "3");
    t_ok("(upper \"ab\")", "\"AB\"");
    t_ok("(split \"a,b\" \",\")", "[\"a\",\"b\"]");
    t_ok("(join [\"a\" \"b\"] \"-\")", "\"a-b\"");
    t_ok("(contains \"abcdef\" \"cd\")", "true");
    t_ok("(get [10 20] 1)", "20");
    t_ok("(get [10 20] -1)", "20");
    t_ok("(get (rec \"k\" 1) \"k\")", "1");
    t_ok("(assoc (rec \"k\" 1) \"k2\" 2)", "{\"k\":1,\"k2\":2}");
    t_ok("(append [1 2] 3)", "[1,2,3]");
    t_ok("(slice \"hello\" 1 3)", "\"el\"");
    t_ok("(sort [3 1 2])", "[1,2,3]");
    t_ok("(sum [1 2 3])", "6");
    t_ok("(kind 1)", "\"int\"");
    t_ok("(kind 1.5)", "\"float\"");
    t_ok("(json (rec \"a\" 1))", "\"{\\\"a\\\":1}\"");
    t_ok("(trim \"  x  \")", "\"x\"");
    t_ok("(node f (x) (* x x)) (f 5)", "25");
    t_ok("(node add (a b) (+ a b)) (add 2 3)", "5");
    t_ok("(letv x 1) (rebind x 2) x", "2");
    t_ok("(cond #t (then 1) (lest 2))", "1");
    t_ok("(cond #f (then 1) (lest 2))", "2");
    t_ok("(scan x [1 2 3] (print x))", "null");
    t_ok("(letv s 0) (scan x [1 2 3 4] (rebind s (+ s x))) s", "10");
    t_ok("(letv out []) (scan x [1 2 3] (rebind out (append out (* x 2)))) out", "[2,4,6]");
    t_ok("(letv i 0) (spin (< i 3) (rebind i (+ i 1))) i", "3");
    t_ok("(node f (n) (cond (< n 2) (then (hand n))) (hand (+ (f (- n 1)) (f (- n 2))))) (f 10)", "55");
    t_ok("(try (+ 1 2))", "{\"ok\":true,\"value\":3}");
    t_ok("(get (try (/ 1 0)) \"kind\")", "\"runtime\"");
    t_ok("(native lang.names)", "[]");
    t_err("(+ 1 \"a\")", "type");
    t_err("(/ 1 0)", "runtime");
    t_err("(unknown 1)", "name");
    t_err("(get)", "type");
    t_err("(rec \"12\" 1)", "type");
    printf("nexus c tests: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}

/* 交互式解释器：支持多行表达式、:help / :load / :quit。 */
static int repl_depth(const char *s) {
    int depth = 0, i = 0, quote = 0, comment = 0;
    while (s[i]) {
        char c = s[i];
        if (comment) { if (c == '\n') comment = 0; i++; continue; }
        if (quote) {
            if (c == '\\' && s[i + 1]) { i += 2; continue; }
            if (c == quote) quote = 0;
            i++;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; i++; continue; }
        if (c == ';') { comment = 1; i++; continue; }
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        i++;
    }
    return depth;
}

static void repl_help(void) {
    printf("  :help            显示本帮助\n"
           "  :load <file>     在当前解释器中求值一个 .nx 文件\n"
           "  :quit / :q       退出\n"
           "  Ctrl+C / Ctrl+D  退出\n"
           "  直接输入表达式即可；括号未闭合时会继续等待下一行。\n");
}

static int cli_repl(void) {
    char linebuf[4096];
    Host *host = (Host *)xmalloc(sizeof(Host));
    Interp *in;
    Buf pending;
    memset(host, 0, sizeof(*host));
    host->data_path = data_path();
    host->programs_dir = (char *)asset_dir("programs");
    host->cols = v_rec();
    host_load(host);
    in = interp_new(host, out_to_stderr);
    buf_init(&pending);
    printf("Nexus " LANG_VERSION " 交互解释器（C 引擎，无宿主）\n");
    printf("输入 :help 查看命令，:quit 退出。\n");
    for (;;) {
        char *err = NULL;
        int line = 0;
        Value v;
        Buf b;
        size_t L;
        fputs((pending.n == 0) ? "nexus> " : "   ...> ", stdout);
        fflush(stdout);
        if (!fgets(linebuf, sizeof(linebuf), stdin)) break;
        L = strlen(linebuf);
        while (L && (linebuf[L - 1] == '\n' || linebuf[L - 1] == '\r')) linebuf[--L] = 0;
        if (!L) continue;
        if (pending.n == 0 && linebuf[0] == ':') {
            if (!strcmp(linebuf, ":quit") || !strcmp(linebuf, ":q")) break;
            if (!strcmp(linebuf, ":help")) { repl_help(); continue; }
            if (!strncmp(linebuf, ":load ", 6)) {
                const char *path = linebuf + 6;
                char *src = read_file_text(path, NULL, &err);
                if (!src) { fprintf(stderr, "无法读取文件：%s\n", path); continue; }
                v = eval_source(in, src, &err, &line, "repl-load");
                if (err) { fprintf(stderr, "解析失败：%s（第 %d 行）\n", err, line); continue; }
                if (in->err.set) { print_error(in); in->err.set = 0; continue; }
                buf_init(&b); json_pretty(&b, v, 0); printf("%s\n", b.p);
                continue;
            }
            fprintf(stderr, "未知命令：%s（输入 :help）\n", linebuf);
            continue;
        }
        buf_adds(&pending, linebuf);
        buf_addc(&pending, '\n');
        if (repl_depth(pending.p) > 0) continue;
        v = eval_source(in, pending.p, &err, &line, "repl");
        pending.n = 0; pending.p[0] = 0;
        if (err) { fprintf(stderr, "解析失败：%s（第 %d 行）\n", err, line); continue; }
        if (in->err.set) { print_error(in); in->err.set = 0; continue; }
        buf_init(&b);
        json_pretty(&b, v, 0);
        printf("%s\n", b.p);
    }
    printf("\nbye\n");
    return 0;
}

static const char *USAGE =
    "Nexus " LANG_VERSION " - the Nexus language engine (C implementation, no host)\n"
    "\n"
    "usage:\n"
    "  nexus run <file> [<request-json>|-]  evaluate a file; with a request, call handle()\n"
    "  nexus ast <file>                     print the syntax tree as JSON\n"
    "  nexus tokens <file>                  print the token list as JSON\n"
    "  nexus test                           run built-in engine tests\n"
    "  nexus repl                           interactive evaluator (one line at a time)\n"
    "  nexus version                        print version information\n";

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (argc < 2) return cli_repl();   /* 双击 exe 直接进入交互解释器 */
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fputs(USAGE, stderr);
        return 0;
    }
    if (!strcmp(argv[1], "version")) {
        printf("{\"lang\": \"" LANG_NAME "\", \"version\": \"" LANG_VERSION
               "\", \"ext\": \"" FILE_EXT "\", \"engine\": \"c\", \"host\": \"none\"}\n");
        return 0;
    }
    if (!strcmp(argv[1], "run")) {
        if (argc < 3) { fputs(USAGE, stderr); return 2; }
        return cli_run(argc, argv);
    }
    if (!strcmp(argv[1], "ast")) {
        if (argc < 3) { fputs(USAGE, stderr); return 2; }
        return cli_ast(argv[2]);
    }
    if (!strcmp(argv[1], "tokens")) {
        if (argc < 3) { fputs(USAGE, stderr); return 2; }
        return cli_tokens(argv[2]);
    }
    if (!strcmp(argv[1], "test")) return cli_test();
    if (!strcmp(argv[1], "repl")) return cli_repl();
    fputs(USAGE, stderr);
    return 2;
}
