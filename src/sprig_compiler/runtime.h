#ifndef SPRIG_RUNTIME_H
#define SPRIG_RUNTIME_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SprigVal SprigVal;
typedef struct {
    SprigVal *data;
    int len, cap;
} SprigList;
typedef struct {
    const char **keys;
    SprigVal *vals;
    int n;
    const char *type_name;
} SprigShape;

#define SV_NIL 0
#define SV_NUM 1
#define SV_STR 2
#define SV_BOOL 3
#define SV_LIST 4
#define SV_SHAPE 5

struct SprigVal {
    int k;
    double num;
    char *str;
    int boolean;
    SprigList *list;
    SprigShape *shape;
};

static SprigVal sv_nil(void) {
    SprigVal v = {0};
    v.k = SV_NIL;
    return v;
}
static SprigVal sv_num(double n) {
    SprigVal v = {0};
    v.k = SV_NUM;
    v.num = n;
    return v;
}
static SprigVal sv_str(const char *s) {
    SprigVal v = {0};
    v.k = SV_STR;
    v.str = (char *)s;
    return v;
}
static SprigVal sv_bool(int b) {
    SprigVal v = {0};
    v.k = SV_BOOL;
    v.boolean = b;
    return v;
}
static SprigVal sv_list(SprigList *l) {
    SprigVal v = {0};
    v.k = SV_LIST;
    v.list = l;
    return v;
}
static SprigVal sv_shape(SprigShape *s) {
    SprigVal v = {0};
    v.k = SV_SHAPE;
    v.shape = s;
    return v;
}

static SprigList *list_new(void) {
    SprigList *l = malloc(sizeof(SprigList));
    l->cap = 4;
    l->len = 0;
    l->data = malloc(sizeof(SprigVal) * 4);
    return l;
}
static void list_append(SprigList *l, SprigVal v) {
    if (l->len == l->cap) {
        l->cap *= 2;
        l->data = realloc(l->data, sizeof(SprigVal) * l->cap);
    }
    l->data[l->len++] = v;
}
static SprigVal list_get(SprigList *l, int i) {
    if (i < 0 || i >= l->len) return sv_nil();
    return l->data[i];
}
static SprigVal list_pop_fn(SprigList *l) {
    if (l->len == 0) return sv_nil();
    return l->data[--l->len];
}
static SprigVal sv_list_of_arr(int n, SprigVal *items) {
    SprigList *l = list_new();
    for (int i = 0; i < n; i++) list_append(l, items[i]);
    return sv_list(l);
}

static SprigShape *shape_new(const char *tname, const char **keys,
                             SprigVal *vals, int n) {
    SprigShape *s = malloc(sizeof(SprigShape));
    s->type_name = tname;
    s->n = n;
    s->keys = malloc(sizeof(char *) * n);
    s->vals = malloc(sizeof(SprigVal) * n);
    for (int i = 0; i < n; i++) {
        s->keys[i] = keys[i];
        s->vals[i] = vals[i];
    }
    return s;
}
static SprigVal shape_get(SprigShape *s, const char *key) {
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->keys[i], key) == 0) return s->vals[i];
    return sv_nil();
}
static void shape_set(SprigShape *s, const char *key, SprigVal val) {
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->keys[i], key) == 0) {
            s->vals[i] = val;
            return;
        }
}

static int sv_truthy(SprigVal v) {
    if (v.k == SV_BOOL) return v.boolean;
    if (v.k == SV_NIL) return 0;
    if (v.k == SV_LIST) return v.list && v.list->len > 0;
    return 1;
}
static int sv_eq(SprigVal a, SprigVal b) {
    if (a.k != b.k) return 0;
    switch (a.k) {
        case SV_NUM:
            return a.num == b.num;
        case SV_STR:
            return a.str && b.str && strcmp(a.str, b.str) == 0;
        case SV_BOOL:
            return a.boolean == b.boolean;
        case SV_NIL:
            return 1;
        default:
            return 0;
    }
}

static char *num_to_str(double n) {
    char *b = malloc(64);
    snprintf(b, 64, "%g", n);
    return b;
}

static SprigVal sv_add(SprigVal a, SprigVal b) {
    if (a.k == SV_STR || b.k == SV_STR) {
        char *sa = a.k == SV_STR
                       ? a.str
                       : (a.k == SV_NUM ? num_to_str(a.num)
                                        : (a.boolean ? "true" : "false"));
        char *sb = b.k == SV_STR
                       ? b.str
                       : (b.k == SV_NUM ? num_to_str(b.num)
                                        : (b.boolean ? "true" : "false"));
        int la = strlen(sa), lb = strlen(sb);
        char *r = malloc(la + lb + 1);
        memcpy(r, sa, la);
        memcpy(r + la, sb, lb);
        r[la + lb] = 0;
        return sv_str(r);
    }
    return sv_num(a.num + b.num);
}
static SprigVal sv_sub(SprigVal a, SprigVal b) { return sv_num(a.num - b.num); }
static SprigVal sv_mul(SprigVal a, SprigVal b) { return sv_num(a.num * b.num); }
static SprigVal sv_div(SprigVal a, SprigVal b) {
    return sv_num(b.num ? a.num / b.num : 0);
}
static SprigVal sv_neg(SprigVal a) { return sv_num(-a.num); }
static SprigVal sv_eq_val(SprigVal a, SprigVal b) {
    return sv_bool(sv_eq(a, b));
}
static SprigVal sv_ne(SprigVal a, SprigVal b) { return sv_bool(!sv_eq(a, b)); }
static SprigVal sv_lt(SprigVal a, SprigVal b) { return sv_bool(a.num < b.num); }
static SprigVal sv_gt(SprigVal a, SprigVal b) { return sv_bool(a.num > b.num); }
static SprigVal sv_lte(SprigVal a, SprigVal b) {
    return sv_bool(a.num <= b.num);
}
static SprigVal sv_gte(SprigVal a, SprigVal b) {
    return sv_bool(a.num >= b.num);
}
static SprigVal sv_and(SprigVal a, SprigVal b) {
    return sv_bool(sv_truthy(a) && sv_truthy(b));
}
static SprigVal sv_or(SprigVal a, SprigVal b) {
    return sv_bool(sv_truthy(a) || sv_truthy(b));
}
static SprigVal sv_not(SprigVal a) { return sv_bool(!sv_truthy(a)); }
static SprigVal sv_index(SprigVal lst, SprigVal idx) {
    if (lst.k == SV_LIST) return list_get(lst.list, (int)idx.num);
    if (lst.k == SV_STR && lst.str) {
        int i = (int)idx.num;
        char *r = malloc(2);
        r[0] = lst.str[i];
        r[1] = 0;
        return sv_str(r);
    }
    return sv_nil();
}

static void print_val(SprigVal v) {
    switch (v.k) {
        case SV_NUM:
            printf("%g", v.num);
            break;
        case SV_STR:
            printf("%s", v.str ? v.str : "");
            break;
        case SV_BOOL:
            printf("%s", v.boolean ? "true" : "false");
            break;
        case SV_NIL:
            printf("nil");
            break;
        case SV_LIST:
            printf("[");
            for (int i = 0; i < v.list->len; i++) {
                if (i) printf(", ");
                print_val(v.list->data[i]);
            }
            printf("]");
            break;
        case SV_SHAPE:
            printf("%s { ", v.shape->type_name ? v.shape->type_name : "shape");
            for (int i = 0; i < v.shape->n; i++) {
                if (i) printf(", ");
                printf("%s: ", v.shape->keys[i]);
                print_val(v.shape->vals[i]);
            }
            printf(" }");
            break;
    }
}
static SprigVal sprig_print(SprigVal v) {
    print_val(v);
    printf("\n");
    return sv_nil();
}
static SprigVal sprig_length(SprigVal v) {
    if (v.k == SV_LIST) return sv_num(v.list->len);
    if (v.k == SV_STR && v.str) return sv_num(strlen(v.str));
    return sv_num(0);
}
static SprigVal sprig_append(SprigVal lst, SprigVal item) {
    list_append(lst.list, item);
    return sv_nil();
}
static SprigVal sprig_first(SprigVal v) {
    if (v.k == SV_LIST && v.list->len > 0) return v.list->data[0];
    return sv_nil();
}
static SprigVal sprig_last(SprigVal v) {
    if (v.k == SV_LIST && v.list->len > 0) return v.list->data[v.list->len - 1];
    return sv_nil();
}
static SprigVal sprig_pop(SprigVal v) {
    if (v.k == SV_LIST) return list_pop_fn(v.list);
    return sv_nil();
}
static SprigVal sprig_to_number(SprigVal v) {
    if (v.k == SV_NUM) return v;
    if (v.k == SV_STR && v.str) return sv_num(atof(v.str));
    return sv_num(0);
}
static SprigVal sprig_to_text(SprigVal v) {
    if (v.k == SV_STR) return v;
    if (v.k == SV_NUM) return sv_str(num_to_str(v.num));
    if (v.k == SV_BOOL) return sv_str(v.boolean ? "true" : "false");
    return sv_str("nil");
}
static SprigVal sprig_char_code(SprigVal v) {
    if (v.k == SV_STR && v.str) return sv_num((double)(unsigned char)v.str[0]);
    return sv_num(0);
}
static SprigVal sprig_char_from_code(SprigVal v) {
    char *s = malloc(2);
    s[0] = (char)(int)v.num;
    s[1] = 0;
    return sv_str(s);
}
static SprigVal sprig_substring(SprigVal s, SprigVal start, SprigVal len) {
    if (s.k != SV_STR || !s.str) return sv_str("");
    int sl = strlen(s.str), st = (int)start.num, ln = (int)len.num;
    if (st < 0) st = 0;
    if (st >= sl) return sv_str("");
    if (st + ln > sl) ln = sl - st;
    char *r = malloc(ln + 1);
    memcpy(r, s.str + st, ln);
    r[ln] = 0;
    return sv_str(r);
}
static SprigVal sprig_string_contains(SprigVal hay, SprigVal ndl) {
    if (hay.k != SV_STR || ndl.k != SV_STR) return sv_bool(0);
    return sv_bool(strstr(hay.str, ndl.str) != NULL);
}
static SprigVal sprig_read_file(SprigVal path) {
    FILE *f = fopen(path.str, "r");
    if (!f) return sv_str("");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = 0;
    return sv_str(buf);
}
static SprigVal sprig_write_file(SprigVal path, SprigVal content) {
    FILE *f = fopen(path.str, "w");
    if (f) {
        fputs(content.str, f);
        fclose(f);
    }
    return sv_nil();
}

static int g_argc = 0;
static char **g_argv = NULL;

static SprigVal sprig_args_count(void) { return sv_num(g_argc); }
static SprigVal sprig_args_get(SprigVal i) {
    int idx = (int)i.num;
    if (idx < 0 || idx >= g_argc) return sv_str("");
    return sv_str(g_argv[idx]);
}
static SprigVal sprig_exit(SprigVal code) {
    exit((int)code.num);
    return sv_nil();
}

static SprigVal sprig_input(SprigVal prompt) {
    if (prompt.k == SV_STR) { fputs(prompt.str, stdout); fflush(stdout); }
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return sv_str("");
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = 0;
    char *owned = malloc(len + 1);
    memcpy(owned, buf, len + 1);
    return sv_str(owned);
}

// Raw pointer ops: addresses are encoded as doubles in SprigVal.num,
// matching the LLVM backend's pointer_to_double representation.
static SprigVal sprig_allocate(SprigVal n) {
    return sv_num((double)(intptr_t)malloc((size_t)n.num));
}
static SprigVal sprig_free(SprigVal addr) {
    free((void *)(intptr_t)addr.num);
    return sv_nil();
}
static SprigVal sprig_read(SprigVal addr) {
    return sv_num(*(double *)(intptr_t)addr.num);
}
static SprigVal sprig_write(SprigVal addr, SprigVal val) {
    *(double *)(intptr_t)addr.num = val.num;
    return sv_nil();
}
static SprigVal sprig_ptr_add(SprigVal addr, SprigVal n) {
    return sv_num((double)((intptr_t)addr.num + (intptr_t)n.num));
}
static SprigVal sprig_ptr_to_number(SprigVal addr) { return addr; }
static SprigVal sprig_number_to_ptr(SprigVal n) { return n; }

#endif /* SPRIG_RUNTIME_H */
