#include "compare.h"

typedef struct { const char *p; size_t n; } Slice;
typedef struct { size_t start, end; } Token;
typedef struct {
    Slice name, code;
    size_t first, last;
    int matched;
} Function;
typedef struct {
    char *code, *outside;
    size_t size, count, capacity;
    Function *functions;
} Source;
typedef struct { void *file; int failed; } Writer;
typedef struct {
    Slice text;
    size_t line, column_token, peer_line;
    unsigned hash, line_hash;
    int kind;
} CodeToken;
typedef struct { CodeToken *tokens; size_t count; } CodeTokens;

enum Directive { NO_DIRECTIVE, DIRECTIVE_NAME, INCLUDE_HEADER, DEFINE_NAME, DIRECTIVE_BODY };
enum CodeKind { C_TOKEN, DIRECTIVE_START, DIRECTIVE_TOKEN, MACRO_PARAMETERS };
typedef struct {
    Slice code;
    size_t pos, line, directive_line, macro_name_end;
    enum Directive directive;
    int line_start;
} CodeCursor;

void *grow_array(void *memory, size_t size)
{
    void *result = realloc(memory, size);
    if (!result) memory_error();
    return result;
}

char *copy_bytes(const char *text, size_t size)
{
    char *result = grow_array(0, size + 1);
    if (size) memcpy(result, text, size);
    result[size] = 0;
    return result;
}

char *join_path(const char *directory, const char *name)
{
    size_t left = strlen(directory), right = strlen(name);
    char *result = grow_array(0, left + right + 2);
    memcpy(result, directory, left);
    if (left && directory[left - 1] != '/' && directory[left - 1] != '\\')
        result[left++] = '/';
    memcpy(result + left, name, right + 1);
    return result;
}

const char *base_name(const char *path)
{
    const char *name = path;
    for (; *path; path++) if (*path == '/' || *path == '\\') name = path + 1;
    return name;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; }
int path_compare(const char *left, const char *right)
{
    for (;; left++, right++) {
        int a = lower((unsigned char)*left), b = lower((unsigned char)*right);
        if (a != b || !a) return a - b;
    }
}

int is_source(const char *name)
{
    size_t n = strlen(name);
    return n > 2 && name[n - 2] == '.' && (lower(name[n - 1]) == 'c' || lower(name[n - 1]) == 'h');
}

void add_path(Paths *paths, const char *name)
{
    if (paths->count == paths->capacity) {
        paths->capacity = paths->capacity ? 2 * paths->capacity : 32;
        paths->items = grow_array(paths->items, paths->capacity * sizeof(*paths->items));
    }
    paths->items[paths->count++] = copy_bytes(name, strlen(name));
}

static void sort_paths(char **items, size_t count)
{
    if (count < 2) return;
    size_t i = 0, j = count - 1;
    char *pivot = items[(count - 1) / 2];
    for (;;) {
        while (path_compare(items[i], pivot) < 0) i++;
        while (path_compare(items[j], pivot) > 0) j--;
        if (i >= j) break;
        char *temporary = items[i];
        items[i++] = items[j];
        items[j--] = temporary;
    }
    sort_paths(items, j + 1);
    sort_paths(items + j + 1, count - j - 1);
}

static int equal(Slice a, Slice b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }
static int literal(Slice a, const char *b) { return equal(a, (Slice){b, strlen(b)}); }
static int spacing(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f'; }
static int identifier(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 128;
}

static int digit(char c) { return c >= '0' && c <= '9'; }

static size_t quoted_end(const char *s, size_t n, size_t i)
{
    char quote = s[i++];
    while (i < n) {
        char c = s[i++];
        if (c == quote) break;
        if (c == '\\' && i < n) i++;
    }
    return i;
}

static size_t comment_end(const char *s, size_t n, size_t i, int physical)
{
    char type = s[i + 1];
    i += 2;
    if (type == '/') {
        while (i < n && (s[i] != '\n' || (physical && s[i - 1] == '\\'))) i++;
        return i;
    }
    while (i + 1 < n && (s[i] != '*' || s[i + 1] != '/')) i++;
    return i + 1 < n ? i + 2 : n;
}

static int starts_comment(const char *s, size_t n, size_t i)
{
    return i + 1 < n && s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*');
}

static size_t directive_end(const char *s, size_t n, size_t i, int physical)
{
    while (i < n && s[i] != '\n') {
        if (starts_comment(s, n, i)) i = comment_end(s, n, i, physical);
        else if (s[i] == '"' || s[i] == '\'') i = quoted_end(s, n, i);
        else if (physical && s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') i += 2;
        else i++;
    }
    return i;
}

/* Translation phase 2 precedes both token comparison and redaction. */
static Slice splice_lines(Slice source, char **buffer)
{
    *buffer = 0;
    size_t first = 0;
    while (first + 1 < source.n &&
           (source.p[first] != '\\' || source.p[first + 1] != '\n')) first++;
    if (first + 1 >= source.n) return source;
    *buffer = grow_array(0, source.n);
    memcpy(*buffer, source.p, first);
    size_t used = first;
    for (size_t i = first; i < source.n; i++) {
        if (source.p[i] == '\\' && i + 1 < source.n && source.p[i + 1] == '\n') i++;
        else (*buffer)[used++] = source.p[i];
    }
    return (Slice){*buffer, used};
}

static size_t line_for(const char *s, size_t from, size_t to, size_t line)
{
    for (size_t i = from; i < to; i++) if (s[i] == '\n') line++;
    return line;
}

static size_t code_token_end(const char *s, size_t n, size_t i)
{
    char c = s[i];
    if (c == '"' || c == '\'') return quoted_end(s, n, i);
    if (digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1]))) {
        /* Preprocessing numbers include decimal points and exponent signs. */
        for (i++; i < n; i++) {
            char ch = s[i], previous = s[i - 1];
            if (identifier((unsigned char)ch) || ch == '.') continue;
            if ((ch == '+' || ch == '-') &&
                (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P')) continue;
            break;
        }
        return i;
    }
    if (identifier((unsigned char)c)) {
        size_t start = i++;
        while (i < n && identifier((unsigned char)s[i])) i++;
        if (i < n && (s[i] == '"' || s[i] == '\'')) {
            if ((i - start == 1 && (c == 'L' || c == 'U' || c == 'u')) ||
                (i - start == 2 && c == 'u' && s[start + 1] == '8')) return quoted_end(s, n, i);
        }
        return i;
    }
    static const char punctuators[][5] = {
        "%:%:", "<<=", ">>=", "...", "->", "++", "--", "<<", ">>",
        "<=", ">=", "==", "!=", "&&", "||", "*=", "/=", "%=", "+=", "-=",
        "&=", "^=", "|=", "##", "<:", ":>", "<%", "%>", "%:"
    };
    for (size_t j = 0; j < sizeof(punctuators) / sizeof(*punctuators); j++) {
        if (c != punctuators[j][0]) continue;
        size_t length = strlen(punctuators[j]);
        if (length <= n - i && !memcmp(s + i, punctuators[j], length)) return i + length;
    }
    return i + 1;
}

static int is_directive_start(const char *s, size_t n, size_t i)
{
    return s[i] == '#' || (i + 1 < n && s[i] == '%' && s[i + 1] == ':');
}

static CodeToken next_code_token(CodeCursor *cursor)
{
    const char *s = cursor->code.p;
    size_t n = cursor->code.n, i = cursor->pos;
    while (i < n) {
        if (s[i] == '\n') {
            cursor->line++;
            cursor->line_start = 1;
            cursor->directive = NO_DIRECTIVE;
        } else if (starts_comment(s, n, i)) {
            size_t end = comment_end(s, n, i, 0);
            cursor->line = line_for(s, i, end, cursor->line);
            i = end;
            continue;
        } else if (!spacing((unsigned char)s[i])) break;
        i++;
    }
    if (i == n) return (CodeToken){0};
    size_t end;
    if (cursor->directive == INCLUDE_HEADER && s[i] == '<') {
        end = i + 1;
        while (end < n && s[end] != '>' && s[end] != '\n') end++;
        if (end < n && s[end] == '>') end++;
    } else end = code_token_end(s, n, i);
    Slice word = {s + i, end - i};
    int kind = cursor->directive == NO_DIRECTIVE ? C_TOKEN : DIRECTIVE_TOKEN;
    if (cursor->line_start && (literal(word, "#") || literal(word, "%:"))) {
        kind = DIRECTIVE_START;
        cursor->directive = DIRECTIVE_NAME;
        cursor->directive_line = cursor->line;
    } else if (cursor->directive == DIRECTIVE_NAME) {
        cursor->directive = DIRECTIVE_BODY;
        if (literal(word, "define")) cursor->directive = DEFINE_NAME;
        else if (literal(word, "include") || literal(word, "include_next"))
            cursor->directive = INCLUDE_HEADER;
    } else if (cursor->directive != NO_DIRECTIVE) {
        if (i == cursor->macro_name_end && literal(word, "(")) kind = MACRO_PARAMETERS;
        cursor->macro_name_end = cursor->directive == DEFINE_NAME ? end : 0;
        cursor->directive = DIRECTIVE_BODY;
    }
    cursor->line_start = 0;
    cursor->pos = end;
    CodeToken token = {.text = word, .line = kind == C_TOKEN ? cursor->line : cursor->directive_line,
                       .kind = kind};
    cursor->line = line_for(s, i, end, cursor->line);
    return token;
}

static int same_code(Slice left, Slice right)
{
    if (equal(left, right)) return 1;
    char *a_buffer, *b_buffer;
    CodeCursor a = {.code = splice_lines(left, &a_buffer), .line_start = 1};
    CodeCursor b = {.code = splice_lines(right, &b_buffer), .line_start = 1};
    int same;
    for (;;) {
        CodeToken x = next_code_token(&a), y = next_code_token(&b);
        same = x.kind == y.kind && equal(x.text, y.text);
        if (!same || !x.text.n) break;
    }
    free(a_buffer);
    free(b_buffer);
    return same;
}

static Token next_token(const char *s, size_t n, size_t *position)
{
    size_t i = *position;
    for (;;) {
        while (i < n && spacing((unsigned char)s[i])) i++;
        if (!starts_comment(s, n, i)) break;
        i = comment_end(s, n, i, 1);
    }
    size_t start = i;
    if (i < n) {
        if (is_directive_start(s, n, i)) i = directive_end(s, n, i, 1);
        else i = code_token_end(s, n, i);
    }
    *position = i;
    return (Token){start, i};
}

static Slice find_name(const char *code, Token *tokens, size_t count)
{
    size_t parens = 0;
    Slice name = {0, 0};
    for (size_t i = 0; i < count; i++) {
        const Token token = tokens[i];
        char c = code[token.start];
        if (!parens && (c == '=' || c == ',')) return (Slice){0, 0};
        if (c == '(') {
            if (!parens && i) {
                Token previous = tokens[i - 1];
                if (identifier((unsigned char)code[previous.start])) {
                    Slice candidate = {code + previous.start, previous.end - previous.start};
                    if (literal(candidate, "INTERRUPT") || literal(candidate, "ISR") ||
                        literal(candidate, "SIGNAL")) {
                        if (i + 1 < count && identifier((unsigned char)code[tokens[i + 1].start]))
                            name = (Slice){code + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start};
                    } else if (!literal(candidate, "__attribute__") && !literal(candidate, "__declspec") &&
                               !literal(candidate, "__interrupt") && !literal(candidate, "interrupt") &&
                               !literal(candidate, "typeof") && !literal(candidate, "__typeof__")) {
                        if (!name.n) name = candidate;
                    }
                }
            }
            parens++;
        } else if (c == ')' && parens) parens--;
    }
    return name;
}

static void extract_functions(Source *source)
{
    const char *s = source->code;
    Token *prefix = 0;
    size_t count = 0, cap = 0, pos = 0, depth = 0, consumed = 0, scanned = 0, line = 1;
    size_t function_start = 0, outside_end = 0;
    Slice name = {0, 0};
    source->outside = grow_array(0, source->size + 1);
    while (pos < source->size) {
        Token t = next_token(s, source->size, &pos);
        if (t.start == t.end) break;
        char c = s[t.start];
        if (c == '{') {
            if (!depth) {
                name = find_name(s, prefix, count);
                function_start = count ? prefix[0].start : t.start;
                count = 0;
            }
            depth++;
        } else if (c == '}' && depth) {
            depth--;
            if (depth || !name.n) continue;
            size_t end = t.end;
            while (end < source->size && (s[end] == ' ' || s[end] == '\t')) end++;
            if (end < source->size && s[end] == '\n') end++;
            if (source->count == source->capacity) {
                source->capacity = source->capacity ? 2 * source->capacity : 16;
                source->functions = grow_array(source->functions, source->capacity * sizeof(Function));
            }
            memcpy(source->outside + outside_end, s + consumed, function_start - consumed);
            outside_end += function_start - consumed;
            line = line_for(s, scanned, function_start, line);
            size_t last = line_for(s, function_start, t.end, line);
            source->functions[source->count++] =
                (Function){name, {s + function_start, end - function_start}, line, last, 0};
            scanned = t.end;
            line = last;
            consumed = end;
        } else if (!depth) {
            if (c == ';' || is_directive_start(s, source->size, t.start)) {
                count = 0;
            } else {
                if (count == cap) {
                    cap = cap ? cap * 2 : 32;
                    prefix = grow_array(prefix, cap * sizeof(Token));
                }
                prefix[count++] = t;
            }
        }
    }
    memcpy(source->outside + outside_end, s + consumed, source->size - consumed);
    outside_end += source->size - consumed;
    source->outside[outside_end] = 0;
    free(prefix);
}

static int read_source(const char *path, Source *source)
{
    void *file = open_path(path, 0);
    if (!file) return 0;
    char *data = 0;
    size_t capacity = 0, used = 0, n;
    do {
        if (used + 4096 >= capacity) {
            capacity = capacity ? capacity * 2 : 8192;
            data = grow_array(data, capacity);
        }
        n = read_data(file, data + used, 4096);
        used += n;
    } while (n == 4096);
    int okay = !read_error(file);
    close_path(file);
    if (!okay) { free(data); return 0; }
    data = convert_utf8(data, used, &used);
    if (!data) return 0;
    size_t out = 0;
    for (size_t i = 0; i < used; i++) {
        if (!data[i]) { free(data); return 0; }
        if (data[i] == '\r') {
            if (i + 1 < used && data[i + 1] == '\n') i++;
            data[out++] = '\n';
        } else data[out++] = data[i];
    }
    data[out] = 0;
    source->code = data;
    source->size = out;
    extract_functions(source);
    return 1;
}

static void dispose_source(Source *s)
{
    free(s->code);
    free(s->outside);
    free(s->functions);
}

static void write_slice(Writer *w, Slice s)
{
    if (!w->failed && s.n && !write_data(w->file, s.p, s.n)) w->failed = 1;
}

static void out(Writer *w, const char *s) { write_slice(w, (Slice){s, strlen(s)}); }
static void number(Writer *w, size_t value)
{
    char buffer[32];
    size_t i = sizeof(buffer);
    do {
        buffer[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    write_slice(w, (Slice){buffer + i, sizeof(buffer) - i});
}

static void encoded(Writer *w, Slice s, int json)
{
    static const char hex[] = "0123456789abcdef";
    if (json) out(w, "\"");
    size_t first = 0;
    for (size_t i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        const char *replacement = 0;
        char control[7] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15], 0 };
        if (json) {
            if (c == '"') replacement = "\\\"";
            else if (c == '\\') replacement = "\\\\";
            else if (c == '\n') replacement = "\\n";
            else if (c == '\t') replacement = "\\t";
            else if (c < 32) replacement = control;
        } else {
            if (c == '&') replacement = "&amp;";
            else if (c == '<') replacement = "&lt;";
            else if (c == '>') replacement = "&gt;";
        }
        if (replacement) {
            write_slice(w, (Slice){s.p + first, i - first});
            out(w, replacement);
            first = i + 1;
        }
    }
    if (s.n > first) write_slice(w, (Slice){s.p + first, s.n - first});
    if (json) out(w, "\"");
}

static void text(Writer *w, const char *s, int json) { encoded(w, (Slice){s, strlen(s)}, json); }

/* Export a lexical copy only. The original offsets and comparison stay intact. */
static char *redacted_code(Slice code, size_t *length)
{
    static const char replacement[] = "\"[REDACTED]\"";
    static const char directive[] = "/* 预处理指令已省略 */";
    const char *s = code.p;
    size_t n = code.n;
    if (n > ((size_t)-1 - 1) / sizeof(directive)) memory_error();
    char *redacted = grow_array(0, n * sizeof(directive) + 1);
    size_t i = 0, used = 0;
    while (i < n) {
        char c = s[i];
        if (starts_comment(s, n, i)) {
            size_t end = comment_end(s, n, i, 0);
            redacted[used++] = ' ';
            while (i < end) if (s[i++] == '\n') redacted[used++] = '\n';
        } else if (c == '"' || c == '\'') {
            size_t end = quoted_end(s, n, i);
            if (c == '"') {
                memcpy(redacted + used, replacement, sizeof(replacement) - 1);
                used += sizeof(replacement) - 1;
                while (i < end) if (s[i++] == '\n') redacted[used++] = '\n';
            } else {
                memcpy(redacted + used, s + i, end - i);
                used += end - i;
                i = end;
            }
        } else if (is_directive_start(s, n, i)) {
            size_t end = directive_end(s, n, i, 0);
            memcpy(redacted + used, directive, sizeof(directive) - 1);
            used += sizeof(directive) - 1;
            while (i < end) if (s[i++] == '\n') redacted[used++] = '\n';
        } else redacted[used++] = s[i++];
    }
    *length = used;
    return redacted;
}

static CodeTokens collect_code_tokens(Slice code)
{
    CodeTokens result = {0};
    size_t capacity = 0;
    CodeCursor cursor = {.code = code, .line_start = 1};
    for (CodeToken token = next_code_token(&cursor); token.text.n; token = next_code_token(&cursor)) {
        if (result.count == capacity) {
            if (capacity > (size_t)-1 / (2 * sizeof(*result.tokens))) memory_error();
            capacity = capacity ? capacity * 2 : 32;
            result.tokens = grow_array(result.tokens, capacity * sizeof(*result.tokens));
        }
        unsigned hash = 2166136261u;
        for (size_t i = 0; i < token.text.n; i++)
            hash = (hash ^ (unsigned char)token.text.p[i]) * 16777619u;
        token.hash = hash;
        result.tokens[result.count++] = token;
    }
    for (size_t start = 0; start < result.count;) {
        size_t end = start, line = result.tokens[start].line;
        unsigned hash = 2166136261u;
        while (end < result.count && result.tokens[end].line == line) {
            CodeToken *token = &result.tokens[end];
            hash = (hash ^ token->hash) * 16777619u;
            hash = (hash ^ (unsigned)token->kind) * 16777619u;
            token->column_token = end++ - start;
        }
        while (start < end) result.tokens[start++].line_hash = hash;
    }
    return result;
}

static int same_token(const CodeToken *left, const CodeToken *right)
{
    return left->hash == right->hash && left->kind == right->kind && equal(left->text, right->text);
}

static int same_line_position(const CodeToken *left, const CodeToken *right)
{
    return left->line_hash == right->line_hash && left->column_token == right->column_token;
}

typedef struct { size_t tokens, aligned; } MatchScore;

static int better_match(MatchScore a, MatchScore b)
{
    return a.aligned > b.aligned || (a.aligned == b.aligned && a.tokens > b.tokens);
}

static void pair_tokens(CodeToken *left, CodeToken *right)
{
    left->peer_line = right->line + 1;
    right->peer_line = left->line + 1;
}

static void token_match_scores(const CodeToken *left, size_t n,
                               const CodeToken *right, size_t m,
                               int reverse, MatchScore *scores)
{
    memset(scores, 0, (m + 1) * sizeof(*scores));
    for (size_t i = 0; i < n; i++) {
        const CodeToken *a = &left[reverse ? n - 1 - i : i];
        MatchScore diagonal = {0};
        for (size_t j = 1; j <= m; j++) {
            const CodeToken *b = &right[reverse ? m - j : j - 1];
            MatchScore previous = scores[j];
            if (better_match(scores[j - 1], previous)) scores[j] = scores[j - 1];
            if (same_token(a, b)) {
                MatchScore match = {diagonal.tokens + 1, diagonal.aligned + same_line_position(a, b)};
                if (better_match(match, scores[j])) scores[j] = match;
            }
            diagonal = previous;
        }
    }
}

/* Preserve equivalent lines, then align remaining code across rewrapped lines. */
static void match_code_tokens(CodeToken *left, size_t n, CodeToken *right, size_t m,
                              MatchScore *forward, MatchScore *backward)
{
    while (n && m && same_line_position(left, right) && same_token(left, right)) {
        pair_tokens(left, right);
        left++;
        right++;
        n--;
        m--;
    }
    while (n && m && same_line_position(&left[n - 1], &right[m - 1]) &&
           same_token(&left[n - 1], &right[m - 1])) {
        pair_tokens(&left[--n], &right[--m]);
    }
    if (!n || !m) return;
    if (n == 1) {
        size_t chosen = m;
        for (size_t j = 0; j < m; j++) {
            if (same_token(left, &right[j])) {
                if (chosen == m) chosen = j;
                if (same_line_position(left, &right[j])) {
                    chosen = j;
                    break;
                }
            }
        }
        if (chosen < m) pair_tokens(left, &right[chosen]);
        return;
    }
    size_t middle = n / 2;
    token_match_scores(left, middle, right, m, 0, forward);
    token_match_scores(left + middle, n - middle, right, m, 1, backward);
    size_t split = 0;
    MatchScore best = {0};
    for (size_t j = 0; j <= m; j++) {
        MatchScore count = {forward[j].tokens + backward[m - j].tokens,
                            forward[j].aligned + backward[m - j].aligned};
        if (better_match(count, best)) {
            split = j;
            best = count;
        }
    }
    if (!best.tokens) return;
    match_code_tokens(left, middle, right, split, forward, backward);
    match_code_tokens(left + middle, n - middle, right + split, m - split, forward, backward);
}

static unsigned char *changed_lines(Slice code, CodeTokens tokens)
{
    size_t count = line_for(code.p, 0, code.n, 1);
    unsigned char *marked = grow_array(0, count);
    memset(marked, 0, count);
    for (size_t i = 0; i < tokens.count; i++) {
        if (!tokens.tokens[i].peer_line) marked[tokens.tokens[i].line] = 1;
    }
    return marked;
}

static void mark_counterparts(CodeTokens tokens, const unsigned char *marked,
                              unsigned char *other_marks)
{
    for (size_t i = 0; i < tokens.count; i++) {
        const CodeToken *token = &tokens.tokens[i];
        if (token->peer_line && marked[token->line] == 1 && !other_marks[token->peer_line - 1])
            other_marks[token->peer_line - 1] = 2;
    }
}

static void emit_side(Writer *w, const Function *f, Slice code, const unsigned char *marked,
                      int json, int after)
{
    if (json) {
        out(w, after ? ",\"newStartLine\":" : ",\"oldStartLine\":");
        if (f) number(w, f->first); else out(w, "null");
        out(w, after ? ",\"newEndLine\":" : ",\"oldEndLine\":");
        if (f) number(w, f->last); else out(w, "null");
        out(w, after ? ",\"afterContent\":" : ",\"beforeContent\":");
        if (f) encoded(w, code, 1); else out(w, "null");
        return;
    }
    out(w, after ? "<section><h4>新版" : "<section><h4>旧版");
    if (f) {
        out(w, " · ");
        number(w, f->first);
        out(w, "–");
        number(w, f->last);
        out(w, " 行</h4><pre>");
        for (size_t start = 0, line = 0; start < code.n; line++) {
            size_t end = start;
            while (end < code.n && code.p[end] != '\n') end++;
            if (marked[line]) out(w, "<span class=diff-line>");
            encoded(w, (Slice){code.p + start, end - start}, 0);
            if (marked[line]) out(w, "</span>");
            if (end < code.n) out(w, "\n");
            start = end + 1;
        }
        out(w, "</pre>");
    } else out(w, "</h4><p class=missing>无此函数</p>");
    out(w, "</section>");
}

static void file_header(Writer *w, const char *path, int json, Summary *stats, size_t alias)
{
    const char *suffix = path[strlen(path) - 1] == 'h' || path[strlen(path) - 1] == 'H' ? ".h" : ".c";
    if (json) {
        if (stats->files) out(w, ",");
        out(w, "\n{\"path\":\"File-");
    } else {
        out(w, "<article><h2>File-");
    }
    stats->files++;
    number(w, alias);
    out(w, suffix);
    out(w, json ? "\",\"functions\":[" : "</h2>");
}

static void emit_function(Writer *w, const Function *old, const Function *newer, ReportFormat format,
                          Summary *stats, size_t index)
{
    int json = format != REPORT_HTML;
    const Function *before_function = format == REPORT_JSON_AFTER ? 0 : old;
    Slice name = newer ? newer->name : old->name;
    const char *key = old && newer ? "modified" : newer ? "added" : "deleted";
    const char *label = old && newer ? "修改" : newer ? "新增" : "删除";
    if (old && newer) stats->modified++;
    else if (newer) stats->added++;
    else stats->removed++;
    if (json) {
        if (index) out(w, ",");
        out(w, "\n{\"name\":");
        encoded(w, name, 1);
        out(w, ",\"changeType\":");
        text(w, key, 1);
    } else {
        out(w, "<details open><summary><span class=");
        out(w, key);
        out(w, ">");
        out(w, label);
        out(w, "</span> ");
        encoded(w, name, 0);
        out(w, "</summary><div class=pair>");
    }
    char *old_buffer, *new_buffer;
    Slice a = splice_lines(before_function ? before_function->code : (Slice){0, 0}, &old_buffer);
    Slice b = splice_lines(newer ? newer->code : (Slice){0, 0}, &new_buffer);
    size_t old_length = 0, new_length = 0;
    char *old_code = before_function ? redacted_code(a, &old_length) : 0;
    char *new_code = newer ? redacted_code(b, &new_length) : 0;
    Slice before = {old_code, old_length}, after = {new_code, new_length};
    unsigned char *old_marks = 0, *new_marks = 0;
    if (!json) {
        CodeTokens left = collect_code_tokens(a), right = collect_code_tokens(b);
        if (left.count && right.count) {
            size_t row_size = (right.count + 1) * sizeof(MatchScore);
            MatchScore *forward = grow_array(0, row_size);
            MatchScore *backward = grow_array(0, row_size);
            match_code_tokens(left.tokens, left.count, right.tokens, right.count, forward, backward);
            free(forward);
            free(backward);
        }
        old_marks = changed_lines(a, left);
        new_marks = changed_lines(b, right);
        mark_counterparts(left, old_marks, new_marks);
        mark_counterparts(right, new_marks, old_marks);
        free(left.tokens);
        free(right.tokens);
    }
    if (format != REPORT_JSON_AFTER) emit_side(w, old, before, old_marks, json, 0);
    emit_side(w, newer, after, new_marks, json, 1);
    if (json) out(w, "}");
    else {
        out(w, "</div>");
        if (old && newer && equal(before, after))
            out(w, "<p class=missing>标红行的变更内容已脱敏。</p>");
        out(w, "</details>");
    }
    free(old_marks);
    free(new_marks);
    free(old_code);
    free(new_code);
    free(old_buffer);
    free(new_buffer);
}

static void compare_file(Writer *w, const char *path, Source *old, Source *newer,
                         ReportFormat format, Summary *stats, size_t *last_alias)
{
    int json = format != REPORT_HTML, after_only = format == REPORT_JSON_AFTER;
    size_t alias = *last_alias + 1;
    size_t count = 0;
    /* Pair identical definitions first, so reordering or #if alternative bodies
       do not mask the genuinely changed definition with the same name. */
    for (size_t i = 0; i < old->count; i++) {
        Function *a = &old->functions[i];
        for (size_t j = 0; j < newer->count; j++) {
            Function *b = &newer->functions[j];
            if (!b->matched && equal(a->name, b->name) && same_code(a->code, b->code)) {
                a->matched = b->matched = 1;
                break;
            }
        }
    }
    for (size_t j = 0; j < newer->count; j++) {
        Function *b = &newer->functions[j];
        if (b->matched) continue;
        Function *a = 0;
        for (size_t i = 0; i < old->count; i++) {
            Function *item = &old->functions[i];
            if (!item->matched && equal(item->name, b->name)) {
                a = item;
                a->matched = 1;
                break;
            }
        }
        if (!count) file_header(w, path, json, stats, alias);
        emit_function(w, a, b, format, stats, count++);
    }
    int has_removed = 0;
    for (size_t i = 0; i < old->count; i++) {
        if (old->functions[i].matched) continue;
        has_removed = 1;
        if (after_only) break;
        if (!count) file_header(w, path, json, stats, alias);
        emit_function(w, &old->functions[i], 0, format, stats, count++);
    }
    Slice a = {old->outside, old->outside ? strlen(old->outside) : 0};
    Slice b = {newer->outside, newer->outside ? strlen(newer->outside) : 0};
    int outside = !same_code(a, b);
    /* Keep file aliases consistent across the full and filtered reports. */
    if (count || has_removed || outside) *last_alias = alias;
    if (after_only) outside = 0;
    if (outside && !count) file_header(w, path, json, stats, alias);
    if (!count && !outside) return;
    if (json) out(w, "]");
    if (outside) {
        stats->outside++;
        if (json) {
            out(w, ",\"outsideFunctionsChanged\":true");
        } else {
            out(w, "<p class=missing>函数外内容有变更，已省略原文。</p>");
        }
    }
    out(w, json ? "}" : "</article>");
}

static const char html_header[] =
    "<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>C 函数变更报告</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#f5f7fa;color:#172936;font:16px 'Microsoft YaHei',Arial,sans-serif}"
    "main{max-width:1450px;margin:auto;padding:28px}h1{margin:0 0 12px}h2{font-size:18px;overflow-wrap:anywhere}"
    ".paths{overflow-wrap:anywhere;color:#475c69;margin:0 0 24px;line-height:1.65}"
    "article{border:1px solid #cbd7df;border-radius:8px;background:white;margin:18px 0;padding:12px 18px}"
    "details{border-top:1px solid #dae3e8;margin:12px 0;padding:10px 0}summary{cursor:pointer;font-weight:bold}"
    ".pair{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}.pair section{min-width:0}"
    "h4{font-size:13px;margin:12px 0 5px;color:#475c69}pre{margin:0;background:#f3f6f8;border:1px solid #dae3e8;"
    "overflow:auto;padding:12px;tab-size:4;font:13px/1.5 Consolas,monospace}.missing{color:#536873}"
    ".diff-line{display:inline-block;vertical-align:top;min-width:100%;min-height:1.5em;"
    "background:#ffe5e7;color:#982130}"
    ".added{color:#176641}.deleted{color:#a32635}.modified{color:#185189}footer{padding:14px 0;font-weight:bold}"
    "@media(max-width:780px){main{padding:14px}.pair{grid-template-columns:minmax(0,1fr)}}"
    "</style><main><h1>C 函数变更报告</h1><p class=paths>脱敏导出。文件名使用编号，注释、"
    "字符串内容和函数内预处理指令已隐藏。空白和注释差异已忽略，代码差异行以红色标出。</p>";

static int fail(Summary *r, const char *path, const char *why)
{
    r->problem = why;
    if (path) r->problem_path = copy_bytes(path, strlen(path));
    return 0;
}

static void free_paths(Paths *list)
{
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
}

int compare_paths(const char *old_path, const char *new_path, const char *output,
                  ReportFormat format, Summary *r)
{
    int json = format != REPORT_HTML;
    Paths old = {0}, newer = {0};
    int old_kind = path_kind(old_path), new_kind = path_kind(new_path);
    if (!old_kind) return fail(r, old_path, "旧版路径不存在或无法读取");
    if (!new_kind) return fail(r, new_path, "新版路径不存在或无法读取");
    if (old_kind != new_kind) return fail(r, 0, "请选择两个文件，或选择两个文件夹");
    if (old_kind == 1 && (!is_source(old_path) || !is_source(new_path)))
        return fail(r, 0, "首版支持 C 源文件和头文件（.c、.h）");
    if (is_source(output))
        return fail(r, output, "输出文件请使用 .html 或 .json，避免覆盖源码");
    if (old_kind == 2) {
        if (!collect_paths(old_path, "", &old)) fail(r, old_path, "无法遍历旧版文件夹");
        if (!r->problem && !collect_paths(new_path, "", &newer)) fail(r, new_path, "无法遍历新版文件夹");
        sort_paths(old.items, old.count);
        sort_paths(newer.items, newer.count);
    } else add_path(&newer, base_name(new_path));
    if (!r->problem && !old.count && !newer.count) fail(r, 0, "没有发现 .c 或 .h 文件");
    Writer w = {0, 0};
    if (!r->problem) {
        w.file = open_path(output, 1);
        if (!w.file) fail(r, output, "无法创建报告");
    }
    if (w.file) {
        if (format == REPORT_JSON_AFTER) out(&w, "{\"redacted\":true,\"mode\":\"afterOnly\",\"files\":[");
        else out(&w, json ? "{\"redacted\":true,\"files\":[" : html_header);
        size_t last_alias = 0;
        for (size_t i = 0, j = 0; !r->problem && (i < old.count || j < newer.count);) {
            const char *a = i < old.count ? old.items[i] : 0;
            const char *b = j < newer.count ? newer.items[j] : 0;
            int order = a && b ? path_compare(a, b) : a ? -1 : 1;
            char *left = 0, *right = 0;
            if (old_kind == 1) left = copy_bytes(old_path, strlen(old_path));
            else if (order <= 0) { left = join_path(old_path, a); i++; }
            if (old_kind == 1) { right = copy_bytes(new_path, strlen(new_path)); j++; }
            else if (order >= 0) { right = join_path(new_path, b); j++; }
            Source s = {0}, t = {0};
            if (left && !read_source(left, &s)) fail(r, left, "无法读取或识别源码编码");
            if (!r->problem && right && !read_source(right, &t)) fail(r, right, "无法读取或识别源码编码");
            if (!r->problem) {
                r->compared++;
                compare_file(&w, order < 0 ? a : b, &s, &t, format, r, &last_alias);
            }
            dispose_source(&s);
            dispose_source(&t);
            free(left);
            free(right);
        }
        if (json) {
            out(&w, "],\"summary\":{\"files\":");
            number(&w, r->files);
            out(&w, ",\"added\":"); number(&w, r->added);
            out(&w, ",\"modified\":"); number(&w, r->modified);
            if (format != REPORT_JSON_AFTER) {
                out(&w, ",\"deleted\":"); number(&w, r->removed);
                out(&w, ",\"outsideChanges\":"); number(&w, r->outside);
            }
            out(&w, ",\"comparedFiles\":"); number(&w, r->compared);
            out(&w, "}}\n");
        } else {
            out(&w, "<footer>已比较文件 "); number(&w, r->compared);
            out(&w, "　变更文件 "); number(&w, r->files);
            out(&w, "　新增函数 "); number(&w, r->added);
            out(&w, "　修改函数 "); number(&w, r->modified);
            out(&w, "　删除函数 "); number(&w, r->removed);
            out(&w, "　含函数外变更 "); number(&w, r->outside);
            out(&w, "</footer></main></html>");
        }
        if ((!close_path(w.file) || w.failed) && !r->problem) fail(r, output, "写入报告失败");
    }
    free_paths(&old);
    free_paths(&newer);
    return !r->problem;
}
