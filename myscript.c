/* myscript.c v2 - for loop + built-ins */
#include <stdint.h>

#define TOK_MAX 2048
#define VAR_MAX 64
#define NAME_MAX 32
#define STR_MAX 64

enum {
    T_EOF = 0, T_NUM, T_STR, T_IDENT,
    T_LET, T_PRINT, T_IF, T_THEN, T_ELSE, T_END, T_WHILE, T_DO,
    T_FOR, T_IN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE, T_ASSIGN,
    T_LPAREN, T_RPAREN, T_COMMA, T_DOTDOT, T_NEWLINE
};

typedef struct { int type; int num; char str[STR_MAX]; } token_t;
typedef struct { int type; int num; char str[STR_MAX]; } value_t;
typedef struct { char name[NAME_MAX]; value_t val; } var_t;

static token_t toks[TOK_MAX];
static int tok_count = 0;
static int pos = 0;
static var_t vars[VAR_MAX];
static int var_count = 0;
static void (*out_fn)(const char*) = 0;

void msc_set_output(void (*fn)(const char*)){ out_fn = fn; }
static void out(const char* s){ if(out_fn) out_fn(s); }

static int _slen(const char* s){ int n = 0; while(s[n]) n++; return n; }
static void _scpy(char* d, const char* s, int max){ int i = 0; for(; i < max-1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int _scmp(const char* a, const char* b){ while(*a && *a == *b){ a++; b++; } return *a - *b; }
static int is_digit(char c){ return c >= '0' && c <= '9'; }
static int is_alpha(char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c){ return is_alpha(c) || is_digit(c); }
static int is_space(char c){ return c == ' ' || c == '\t' || c == '\r'; }

static int lex(const char* src){
    tok_count = 0;
    int i = 0;
    while(src[i]){
        char c = src[i];
        if(is_space(c)){ i++; continue; }
        if(c == '\n'){
            if(tok_count < TOK_MAX){ toks[tok_count].type = T_NEWLINE; tok_count++; }
            i++; continue;
        }
        if(c == '#'){
            while(src[i] && src[i] != '\n') i++;
            continue;
        }
        if(is_digit(c)){
            int v = 0;
            while(is_digit(src[i])){ v = v*10 + (src[i] - '0'); i++; }
            if(tok_count < TOK_MAX){ toks[tok_count].type = T_NUM; toks[tok_count].num = v; tok_count++; }
            continue;
        }
        if(c == '"'){
            i++;
            int j = 0;
            char buf[STR_MAX];
            while(src[i] && src[i] != '"' && j < STR_MAX-1) buf[j++] = src[i++];
            buf[j] = 0;
            if(src[i] == '"') i++;
            if(tok_count < TOK_MAX){ toks[tok_count].type = T_STR; _scpy(toks[tok_count].str, buf, STR_MAX); tok_count++; }
            continue;
        }
        if(is_alpha(c)){
            int j = 0;
            char buf[NAME_MAX];
            while(is_alnum(src[i]) && j < NAME_MAX-1) buf[j++] = src[i++];
            buf[j] = 0;
            int t = T_IDENT;
            if(_scmp(buf, "let") == 0) t = T_LET;
            else if(_scmp(buf, "print") == 0) t = T_PRINT;
            else if(_scmp(buf, "if") == 0) t = T_IF;
            else if(_scmp(buf, "then") == 0) t = T_THEN;
            else if(_scmp(buf, "else") == 0) t = T_ELSE;
            else if(_scmp(buf, "end") == 0) t = T_END;
            else if(_scmp(buf, "while") == 0) t = T_WHILE;
            else if(_scmp(buf, "do") == 0) t = T_DO;
            else if(_scmp(buf, "for") == 0) t = T_FOR;
            else if(_scmp(buf, "in") == 0) t = T_IN;
            if(tok_count < TOK_MAX){ toks[tok_count].type = t; _scpy(toks[tok_count].str, buf, STR_MAX); tok_count++; }
            continue;
        }
        if(c == '.' && src[i+1] == '.'){
            if(tok_count < TOK_MAX){ toks[tok_count].type = T_DOTDOT; tok_count++; }
            i += 2; continue;
        }
        int t = -1;
        if(c == '+') t = T_PLUS;
        else if(c == '-') t = T_MINUS;
        else if(c == '*') t = T_STAR;
        else if(c == '/') t = T_SLASH;
        else if(c == '%') t = T_PERCENT;
        else if(c == '(') t = T_LPAREN;
        else if(c == ')') t = T_RPAREN;
        else if(c == ',') t = T_COMMA;
        else if(c == '=' && src[i+1] == '='){ t = T_EQ; i++; }
        else if(c == '!' && src[i+1] == '='){ t = T_NEQ; i++; }
        else if(c == '<' && src[i+1] == '='){ t = T_LE; i++; }
        else if(c == '>' && src[i+1] == '='){ t = T_GE; i++; }
        else if(c == '<') t = T_LT;
        else if(c == '>') t = T_GT;
        else if(c == '=') t = T_ASSIGN;
        if(t >= 0 && tok_count < TOK_MAX){ toks[tok_count].type = t; tok_count++; }
        i++;
    }
    if(tok_count < TOK_MAX){ toks[tok_count].type = T_EOF; tok_count++; }
    return 0;
}

static value_t mk_num(int n){ value_t v; v.type = 0; v.num = n; v.str[0] = 0; return v; }
static value_t mk_str(const char* s){ value_t v; v.type = 1; v.num = 0; _scpy(v.str, s, STR_MAX); return v; }

static int var_find(const char* name){
    for(int i = 0; i < var_count; i++) if(_scmp(vars[i].name, name) == 0) return i;
    return -1;
}
static int var_set(const char* name, value_t v){
    int i = var_find(name);
    if(i < 0){
        if(var_count >= VAR_MAX) return -1;
        i = var_count++;
        _scpy(vars[i].name, name, NAME_MAX);
    }
    vars[i].val = v;
    return 0;
}
static value_t var_get(const char* name){
    int i = var_find(name);
    if(i < 0) return mk_num(0);
    return vars[i].val;
}

static value_t num_to_str(value_t v){
    if(v.type == 1) return v;
    char buf[STR_MAX];
    int n = v.num, neg = 0;
    if(n < 0){ neg = 1; n = -n; }
    int i = 0;
    if(n == 0) buf[i++] = '0';
    else {
        char t[16]; int j = 0;
        while(n){ t[j++] = '0' + (n % 10); n /= 10; }
        if(neg) buf[i++] = '-';
        while(j) buf[i++] = t[--j];
    }
    buf[i] = 0;
    return mk_str(buf);
}
static value_t do_binop(int op, value_t a, value_t b){
    if(op == T_EQ || op == T_NEQ){
        int eq;
        if(a.type == 0 && b.type == 0) eq = (a.num == b.num);
        else { value_t sa = num_to_str(a), sb = num_to_str(b); eq = (_scmp(sa.str, sb.str) == 0); }
        return mk_num(op == T_EQ ? eq : !eq);
    }
    if(op == T_PLUS && (a.type == 1 || b.type == 1)){
        value_t sa = num_to_str(a), sb = num_to_str(b);
        char buf[STR_MAX];
        int i = 0;
        for(int k = 0; sa.str[k] && i < STR_MAX-1; k++) buf[i++] = sa.str[k];
        for(int k = 0; sb.str[k] && i < STR_MAX-1; k++) buf[i++] = sb.str[k];
        buf[i] = 0;
        return mk_str(buf);
    }
    int x = a.num, y = b.num, r = 0;
    switch(op){
        case T_PLUS: r = x + y; break;
        case T_MINUS: r = x - y; break;
        case T_STAR: r = x * y; break;
        case T_SLASH: r = y ? (x / y) : 0; break;
        case T_PERCENT: r = y ? (x % y) : 0; break;
        case T_LT: r = x < y; break;
        case T_GT: r = x > y; break;
        case T_LE: r = x <= y; break;
        case T_GE: r = x >= y; break;
    }
    return mk_num(r);
}

/* ===== built-ins ===== */
static value_t builtin_call(const char* name, value_t* args, int n){
    if(_scmp(name, "abs") == 0 && n >= 1){
        if(args[0].type == 0){ int v = args[0].num; if(v < 0) v = -v; return mk_num(v); }
        return mk_num(0);
    }
    if(_scmp(name, "min") == 0 && n >= 2) return mk_num(args[0].num < args[1].num ? args[0].num : args[1].num);
    if(_scmp(name, "max") == 0 && n >= 2) return mk_num(args[0].num > args[1].num ? args[0].num : args[1].num);
    if(_scmp(name, "len") == 0 && n >= 1){
        value_t s = num_to_str(args[0]);
        return mk_num(_slen(s.str));
    }
    if(_scmp(name, "str") == 0 && n >= 1) return num_to_str(args[0]);
    if(_scmp(name, "int") == 0 && n >= 1){
        if(args[0].type == 0) return args[0];
        int v = 0, i = 0, neg = 0;
        if(args[0].str[0] == '-'){ neg = 1; i = 1; }
        while(args[0].str[i] >= '0' && args[0].str[i] <= '9'){ v = v*10 + (args[0].str[i] - '0'); i++; }
        return mk_num(neg ? -v : v);
    }
    return mk_num(0);
}

static value_t parse_expr(void);
static void parse_stmt(void);
static void skip_newlines(void){
    while(pos < tok_count && toks[pos].type == T_NEWLINE) pos++;
}
static int is_truthy(value_t v){ return v.type == 0 ? (v.num != 0) : (v.str[0] != 0); }

static void skip_to_else_or_end(void){
    int depth = 0;
    while(pos < tok_count){
        int tt = toks[pos].type;
        if(tt == T_IF || tt == T_WHILE || tt == T_FOR){ depth++; pos++; continue; }
        if(tt == T_END){
            if(depth == 0) return;
            depth--; pos++; continue;
        }
        if(tt == T_ELSE && depth == 0) return;
        if(tt == T_EOF) return;
        pos++;
    }
}
static void skip_to_matching_end(void){
    int depth = 0;
    while(pos < tok_count){
        int tt = toks[pos].type;
        if(tt == T_IF || tt == T_WHILE || tt == T_FOR){ depth++; pos++; continue; }
        if(tt == T_END){
            if(depth == 0){ pos++; return; }
            depth--; pos++; continue;
        }
        pos++;
    }
}

static value_t parse_primary(void){
    if(pos >= tok_count) return mk_num(0);
    token_t t = toks[pos];
    if(t.type == T_NUM){ pos++; return mk_num(t.num); }
    if(t.type == T_STR){ pos++; return mk_str(t.str); }
    if(t.type == T_IDENT){
        char name[NAME_MAX];
        _scpy(name, t.str, NAME_MAX);
        pos++;
        if(pos < tok_count && toks[pos].type == T_LPAREN){
            pos++;
            value_t args[4];
            int nargs = 0;
            skip_newlines();
            if(pos < tok_count && toks[pos].type != T_RPAREN){
                while(pos < tok_count && nargs < 4){
                    args[nargs++] = parse_expr();
                    skip_newlines();
                    if(pos < tok_count && toks[pos].type == T_COMMA){ pos++; skip_newlines(); continue; }
                    break;
                }
            }
            if(pos < tok_count && toks[pos].type == T_RPAREN) pos++;
            return builtin_call(name, args, nargs);
        }
        return var_get(name);
    }
    if(t.type == T_LPAREN){
        pos++;
        value_t v = parse_expr();
        if(pos < tok_count && toks[pos].type == T_RPAREN) pos++;
        return v;
    }
    if(t.type == T_MINUS){
        pos++;
        value_t v = parse_primary();
        if(v.type == 0) v.num = -v.num;
        return v;
    }
    return mk_num(0);
}
static value_t parse_mul(void){
    value_t a = parse_primary();
    while(pos < tok_count){
        int t = toks[pos].type;
        if(t != T_STAR && t != T_SLASH && t != T_PERCENT) break;
        pos++;
        a = do_binop(t, a, parse_primary());
    }
    return a;
}
static value_t parse_add(void){
    value_t a = parse_mul();
    while(pos < tok_count){
        int t = toks[pos].type;
        if(t != T_PLUS && t != T_MINUS) break;
        pos++;
        a = do_binop(t, a, parse_mul());
    }
    return a;
}
static value_t parse_cmp(void){
    value_t a = parse_add();
    while(pos < tok_count){
        int t = toks[pos].type;
        if(t != T_LT && t != T_GT && t != T_LE && t != T_GE && t != T_EQ && t != T_NEQ) break;
        pos++;
        a = do_binop(t, a, parse_add());
    }
    return a;
}
static value_t parse_expr(void){ return parse_cmp(); }

static void parse_stmt(void){
    skip_newlines();
    if(pos >= tok_count) return;
    token_t t = toks[pos];

    if(t.type == T_LET){
        pos++;
        if(pos >= tok_count || toks[pos].type != T_IDENT) return;
        char name[NAME_MAX];
        _scpy(name, toks[pos].str, NAME_MAX);
        pos++;
        if(pos < tok_count && toks[pos].type == T_ASSIGN) pos++;
        value_t v = parse_expr();
        var_set(name, v);
        return;
    }
    if(t.type == T_PRINT){
        pos++;
        value_t v = num_to_str(parse_expr());
        out(v.str);
        out("\n");
        return;
    }
    if(t.type == T_IF){
        pos++;
        value_t cond = parse_expr();
        if(pos < tok_count && toks[pos].type == T_THEN) pos++;
        if(is_truthy(cond)){
            while(1){
                skip_newlines();
                if(pos >= tok_count) break;
                int tt = toks[pos].type;
                if(tt == T_END){ pos++; break; }
                if(tt == T_ELSE){ pos++; skip_to_matching_end(); break; }
                if(tt == T_EOF) break;
                parse_stmt();
            }
        } else {
            skip_to_else_or_end();
            if(pos < tok_count && toks[pos].type == T_ELSE){
                pos++;
                while(1){
                    skip_newlines();
                    if(pos >= tok_count) break;
                    int tt = toks[pos].type;
                    if(tt == T_END){ pos++; break; }
                    if(tt == T_EOF) break;
                    parse_stmt();
                }
            } else if(pos < tok_count && toks[pos].type == T_END){
                pos++;
            }
        }
        return;
    }
    if(t.type == T_WHILE){
        pos++;
        int cond_start = pos;
        value_t cond = parse_expr();
        if(pos < tok_count && toks[pos].type == T_DO) pos++;
        int body_start = pos;
        while(is_truthy(cond)){
            pos = body_start;
            while(1){
                skip_newlines();
                if(pos >= tok_count) break;
                int tt = toks[pos].type;
                if(tt == T_END) break;
                if(tt == T_EOF) break;
                parse_stmt();
            }
            pos = cond_start;
            cond = parse_expr();
            if(pos < tok_count && toks[pos].type == T_DO) pos++;
        }
        pos = body_start;
        skip_to_matching_end();
        return;
    }
    if(t.type == T_FOR){
        pos++;
        if(pos >= tok_count || toks[pos].type != T_IDENT) return;
        char varname[NAME_MAX];
        _scpy(varname, toks[pos].str, NAME_MAX);
        pos++;
        if(pos < tok_count && toks[pos].type == T_IN) pos++;
        value_t va = parse_expr();
        if(pos < tok_count && toks[pos].type == T_DOTDOT) pos++;
        value_t vb = parse_expr();
        if(pos < tok_count && toks[pos].type == T_DO) pos++;
        int body_start = pos;
        int a = va.num, b = vb.num;
        if(a <= b){
            for(int i = a; i <= b; i++){
                var_set(varname, mk_num(i));
                pos = body_start;
                while(1){
                    skip_newlines();
                    if(pos >= tok_count) break;
                    int tt = toks[pos].type;
                    if(tt == T_END) break;
                    if(tt == T_EOF) break;
                    parse_stmt();
                }
            }
        }
        pos = body_start;
        skip_to_matching_end();
        return;
    }
    if(t.type == T_END || t.type == T_ELSE || t.type == T_THEN || t.type == T_DO) return;
    parse_expr();
}

int msc_run(const char* src){
    lex(src);
    pos = 0;
    var_count = 0;
    while(pos < tok_count && toks[pos].type != T_EOF){
        skip_newlines();
        if(pos >= tok_count || toks[pos].type == T_EOF) break;
        parse_stmt();
    }
    return 0;
}
