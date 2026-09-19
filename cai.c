/*
 * cAI — terminal chat for SharkDeck.
 * Uses the curl program (no libcurl headers).
 *
 *   cAI              chat (setup wizard if no key yet)
 *   cAI setup        enter / change API key
 *   cAI -q           one prompt from stdin
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MSG  24
#define MAX_BODY (256 * 1024)
#define MAX_LINE 4096

static char key[256];
static char model[80] = "grok-4";
static char base[256] = "https://api.x.ai/v1";
static char hist_role[MAX_MSG][16];
static char *hist_txt[MAX_MSG];
static int nhist;

static int set_base(const char *s);
static void save_cfg(void);

static char *home_dir(void)
{
    const char *h = getenv("HOME");
    struct passwd *pw;
    if (h && *h)
        return (char *)h;
    pw = getpwuid(getuid());
    return pw ? pw->pw_dir : ".";
}

static void path_join(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", home_dir(), name);
}

static void trim(char *s)
{
    char *a = s, *e;
    while (*a && isspace((unsigned char)*a))
        a++;
    if (a != s)
        memmove(s, a, strlen(a) + 1);
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
}

static int load_file(const char *path, char *dst, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(dst, (int)n, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    trim(dst);
    return dst[0] != 0;
}

static int save_file(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "%s\n", s);
    fclose(f);
    chmod(path, 0600);
    return 1;
}

static int set_base(const char *s)
{
    size_t n;
    if (!s || !*s)
        return 0;
    if (!strcmp(s, "URL") || !strcmp(s, "url") || !strcmp(s, "BASEURL"))
        return 0;
    if (strncmp(s, "http://", 7) && strncmp(s, "https://", 8))
        return 0;
    snprintf(base, sizeof base, "%s", s);
    n = strlen(base);
    while (n && base[n - 1] == '/')
        base[--n] = 0;
    return 1;
}

static void save_cfg(void)
{
    char p[512];
    FILE *f;
    path_join(p, sizeof p, ".cai.conf");
    f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "model=%s\nbase=%s\n", model, base);
    fclose(f);
}

static void load_cfg(void)
{
    char p[512], line[320];
    FILE *f;
    const char *e;
    path_join(p, sizeof p, ".cai.key");
    load_file(p, key, sizeof key);
    e = getenv("XAI_API_KEY");
    if (e && e[0])
        snprintf(key, sizeof key, "%s", e);
    e = getenv("OPENAI_API_KEY");
    if ((!key[0]) && e && e[0])
        snprintf(key, sizeof key, "%s", e);
    path_join(p, sizeof p, ".cai.conf");
    f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!strncmp(line, "model=", 6))
            snprintf(model, sizeof model, "%.*s", (int)sizeof model - 1, line + 6);
        else if (!strncmp(line, "base=", 5)) {
            if (!set_base(line + 5))
                snprintf(base, sizeof base, "https://api.x.ai/v1");
        }
    }
    fclose(f);
}

static void mask_key(char *out, size_t n)
{
    size_t k = strlen(key);
    if (k == 0) {
        snprintf(out, n, "(none)");
        return;
    }
    if (k <= 8)
        snprintf(out, n, "****%s", key + (k > 4 ? k - 4 : 0));
    else
        snprintf(out, n, "%.4s…%s", key, key + k - 4);
}

static int read_line(const char *prompt, char *dst, size_t n)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(dst, (int)n, stdin))
        return 0;
    trim(dst);
    return 1;
}

static int save_key(const char *s)
{
    char p[512];
    if (!s || !s[0])
        return 0;
    snprintf(key, sizeof key, "%s", s);
    path_join(p, sizeof p, ".cai.key");
    if (!save_file(p, key)) {
        printf("could not write %s\n", p);
        return 0;
    }
    printf("key saved to %s (only you can read it)\n", p);
    return 1;
}

static void apply_preset(int n)
{
    if (n == 1) {
        snprintf(base, sizeof base, "https://api.x.ai/v1");
        snprintf(model, sizeof model, "grok-4");
    } else if (n == 2) {
        snprintf(base, sizeof base, "https://api.openai.com/v1");
        snprintf(model, sizeof model, "gpt-4o-mini");
    }
}

static int setup(void)
{
    char buf[256], shown[40];

    puts("");
    puts("======= cAI setup =======");
    puts("1) xAI / Grok     https://api.x.ai/v1");
    puts("2) OpenAI         https://api.openai.com/v1");
    puts("3) keep current / custom");
    if (!read_line("pick 1-3 [1]: ", buf, sizeof buf))
        return 0;
    if (!buf[0] || buf[0] == '1')
        apply_preset(1);
    else if (buf[0] == '2')
        apply_preset(2);
    else if (buf[0] == '3') {
        printf("base now: %s\n", base);
        if (read_line("new base (empty = keep): ", buf, sizeof buf) && buf[0]) {
            if (!set_base(buf)) {
                puts("need https://host/v1 — not the word URL");
                return 0;
            }
        }
        printf("model now: %s\n", model);
        if (read_line("new model (empty = keep): ", buf, sizeof buf) && buf[0])
            snprintf(model, sizeof model, "%.*s", (int)sizeof model - 1, buf);
    } else {
        puts("unknown pick");
        return 0;
    }

    mask_key(shown, sizeof shown);
    printf("current key: %s\n", shown);
    puts("paste API key (empty keeps current):");
    if (!read_line("key> ", buf, sizeof buf))
        return 0;
    if (buf[0]) {
        /* strip accidental "Bearer " prefix */
        if (!strncmp(buf, "Bearer ", 7))
            memmove(buf, buf + 7, strlen(buf + 7) + 1);
        if (!save_key(buf))
            return 0;
    } else if (!key[0]) {
        puts("no key entered");
        return 0;
    }

    save_cfg();
    mask_key(shown, sizeof shown);
    printf("ready. model=%s\n      base=%s\n      key=%s\n", model, base, shown);
    puts("=========================");
    return key[0] != 0;
}

static void hist_add(const char *role, const char *txt)
{
    int i;
    if (nhist == MAX_MSG) {
        free(hist_txt[0]);
        for (i = 1; i < MAX_MSG; i++) {
            memcpy(hist_role[i - 1], hist_role[i], sizeof hist_role[0]);
            hist_txt[i - 1] = hist_txt[i];
        }
        nhist--;
    }
    snprintf(hist_role[nhist], sizeof hist_role[0], "%s", role);
    hist_txt[nhist] = strdup(txt ? txt : "");
    nhist++;
}

static void json_esc(FILE *f, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", f);
        else if (c == '\r')
            fputs("\\r", f);
        else if (c == '\t')
            fputs("\\t", f);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
}

static char *build_req(void)
{
    char *out = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&out, &n);
    int i;
    if (!f)
        return NULL;
    fprintf(f, "{\"model\":\"%s\",\"temperature\":0.7,\"messages\":[", model);
    fprintf(f,
            "{\"role\":\"system\",\"content\":\"You are cAI on a small Linux handheld. Keep answers short unless asked.\"}");
    for (i = 0; i < nhist; i++) {
        fputc(',', f);
        fprintf(f, "{\"role\":\"%s\",\"content\":\"", hist_role[i]);
        json_esc(f, hist_txt[i]);
        fputs("\"}", f);
    }
    fputs("]}", f);
    fclose(f);
    return out;
}

static char *extract_content(const char *js)
{
    const char *p, *q;
    char *out;
    size_t i, n;
    q = NULL;
    p = js;
    while ((p = strstr(p, "\"content\"")) != NULL) {
        q = p;
        p += 9;
    }
    if (!q)
        return NULL;
    q = strchr(q, ':');
    if (!q)
        return NULL;
    q++;
    while (*q && isspace((unsigned char)*q))
        q++;
    if (*q != '"')
        return NULL;
    q++;
    out = malloc(strlen(q) + 1);
    if (!out)
        return NULL;
    n = 0;
    for (i = 0; q[i]; i++) {
        if (q[i] == '\\' && q[i + 1]) {
            i++;
            if (q[i] == 'n')
                out[n++] = '\n';
            else if (q[i] == 'r')
                out[n++] = '\r';
            else if (q[i] == 't')
                out[n++] = '\t';
            else if (q[i] == 'u' && strlen(q + i) >= 5)
                i += 4;
            else
                out[n++] = q[i];
        } else if (q[i] == '"')
            break;
        else
            out[n++] = q[i];
    }
    out[n] = 0;
    return out;
}

static char *slurp(FILE *f)
{
    char *p = NULL;
    size_t n = 0, cap = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 4096;
            if (cap > MAX_BODY)
                cap = MAX_BODY;
            if (n + 2 > cap)
                break;
            p = realloc(p, cap);
            if (!p)
                return NULL;
        }
        p[n++] = (char)c;
    }
    if (p)
        p[n] = 0;
    return p;
}

static char *find_curl(void)
{
    if (access("/usr/bin/curl", X_OK) == 0)
        return "/usr/bin/curl";
    if (access("/bin/curl", X_OK) == 0)
        return "/bin/curl";
    return NULL;
}

static char *chat_once(char **err)
{
    char reqp[] = "/tmp/cai-req.json";
    char cmd[900], url[360];
    char *body, *raw, *reply, *http, *curlbin;
    FILE *f, *p;

    curlbin = find_curl();
    if (!curlbin) {
        *err = strdup("install curl:  apt install curl");
        return NULL;
    }
    body = build_req();
    if (!body) {
        *err = strdup("oom");
        return NULL;
    }
    f = fopen(reqp, "w");
    if (!f) {
        free(body);
        *err = strdup("cannot write /tmp/cai-req.json");
        return NULL;
    }
    fputs(body, f);
    fclose(f);
    free(body);

    snprintf(url, sizeof url, "%s/chat/completions", base);
    snprintf(cmd, sizeof cmd,
             "%s -sS --max-time 90 "
             "-H 'Content-Type: application/json' "
             "-H 'Authorization: Bearer %s' "
             "-d @%s '%s' -w '\\nHTTP:%%{http_code}'",
             curlbin, key, reqp, url);
    p = popen(cmd, "r");
    if (!p) {
        *err = strdup("popen curl failed");
        return NULL;
    }
    raw = slurp(p);
    pclose(p);
    unlink(reqp);
    if (!raw) {
        *err = strdup("empty curl output");
        return NULL;
    }
    http = strstr(raw, "\nHTTP:");
    if (http) {
        int code = atoi(http + 6);
        *http = 0;
        if (code / 100 != 2) {
            char tmp[280];
            snprintf(tmp, sizeof tmp, "HTTP %d %s\n%.200s", code, url, raw);
            *err = strdup(tmp);
            free(raw);
            return NULL;
        }
    }
    reply = extract_content(raw);
    free(raw);
    if (!reply)
        *err = strdup("no content in reply (bad key or model?)");
    return reply;
}

static void banner(void)
{
    char shown[40];
    mask_key(shown, sizeof shown);
    printf("cAI  %s\n", model);
    printf("     %s\n", base);
    printf("     key %s\n", shown);
    puts("type a message, or:  setup  /clear  /quit");
    puts("");
}

static void usage(void)
{
    fputs(
        "cAI — tiny chat\n"
        "  cAI           start chat (asks for key the first time)\n"
        "  cAI setup     change key / provider\n"
        "  cAI -q        one prompt from stdin\n",
        stderr);
}

int main(int argc, char **argv)
{
    int once = 0, i;
    char line[MAX_LINE];

    load_cfg();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-q"))
            once = 1;
        else if (!strcmp(argv[i], "setup") || !strcmp(argv[i], "-k") || !strcmp(argv[i], "--setup")) {
            return setup() ? 0 : 1;
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            snprintf(model, sizeof model, "%s", argv[++i]);
            save_cfg();
        } else if (!strcmp(argv[i], "-u") && i + 1 < argc) {
            if (!set_base(argv[++i])) {
                fputs("base must look like https://api.x.ai/v1\n", stderr);
                return 1;
            }
            save_cfg();
        } else {
            usage();
            return 1;
        }
    }

    if (!key[0]) {
        puts("no API key saved yet.");
        if (!setup())
            return 1;
    }

    if (once) {
        size_t n = fread(line, 1, sizeof line - 1, stdin);
        char *err = NULL, *r;
        line[n] = 0;
        trim(line);
        if (!line[0])
            return 1;
        hist_add("user", line);
        r = chat_once(&err);
        if (!r) {
            fprintf(stderr, "error: %s\n", err ? err : "?");
            free(err);
            return 1;
        }
        puts(r);
        free(r);
        return 0;
    }

    banner();
    for (;;) {
        char *err = NULL, *r;
        fputs("you> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        trim(line);
        if (!line[0])
            continue;
        if (!strcmp(line, "/quit") || !strcmp(line, "/q") || !strcmp(line, "/exit") || !strcmp(line, "quit"))
            break;
        if (!strcmp(line, "/clear")) {
            while (nhist)
                free(hist_txt[--nhist]);
            puts("(cleared)");
            continue;
        }
        if (!strcmp(line, "setup") || !strcmp(line, "/setup") || !strcmp(line, "/key")) {
            setup();
            continue;
        }
        if (!strncmp(line, "/model ", 7)) {
            snprintf(model, sizeof model, "%.*s", (int)sizeof model - 1, line + 7);
            save_cfg();
            printf("model=%s\n", model);
            continue;
        }
        if (!strncmp(line, "/base ", 6)) {
            if (!set_base(line + 6))
                puts("need https://host/v1");
            else {
                save_cfg();
                printf("base=%s\n", base);
            }
            continue;
        }
        hist_add("user", line);
        fputs("cAI> ", stdout);
        fflush(stdout);
        r = chat_once(&err);
        if (!r) {
            printf("[error] %s\n", err ? err : "?");
            free(err);
            continue;
        }
        puts(r);
        hist_add("assistant", r);
        free(r);
    }
    return 0;
}
