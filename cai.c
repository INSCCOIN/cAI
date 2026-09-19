/*
 * cAI — tiny terminal chat for SharkDeck / Linux.
 * OpenAI-compatible HTTP (xAI default). Uses the curl CLI, not libcurl.
 *
 *   ./cAI                 # interactive
 *   ./cAI -k              # set / replace API key
 *   ./cAI -m grok-4       # model
 *   ./cAI -u URL          # base URL (no trailing slash)
 *   echo hi | ./cAI -q    # one shot from stdin
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

#define MAX_MSG  24
#define MAX_BODY (256 * 1024)
#define MAX_LINE 4096

static char key[256];
static char model[64] = "grok-4";
static char base[256] = "https://api.x.ai/v1";
static char hist_role[MAX_MSG][16];
static char *hist_txt[MAX_MSG];
static int nhist;

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
    char *e;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
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

static void load_cfg(void)
{
    char p[512], line[320];
    FILE *f;
    path_join(p, sizeof p, ".cai.key");
    load_file(p, key, sizeof key);
    if (getenv("XAI_API_KEY") && getenv("XAI_API_KEY")[0])
        snprintf(key, sizeof key, "%s", getenv("XAI_API_KEY"));
    else if (getenv("OPENAI_API_KEY") && getenv("OPENAI_API_KEY")[0])
        snprintf(key, sizeof key, "%s", getenv("OPENAI_API_KEY"));
    path_join(p, sizeof p, ".cai.conf");
    f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!strncmp(line, "model=", 6))
            snprintf(model, sizeof model, "%s", line + 6);
        else if (!strncmp(line, "base=", 5)) {
            if (!set_base(line + 5))
                snprintf(base, sizeof base, "https://api.x.ai/v1");
        }
    }
    fclose(f);
}

static int set_base(const char *s)
{
    if (!s || !*s)
        return 0;
    if (!strcmp(s, "URL") || !strcmp(s, "url") || !strcmp(s, "BASEURL"))
        return 0;
    if (strncmp(s, "http://", 7) && strncmp(s, "https://", 8))
        return 0;
    snprintf(base, sizeof base, "%s", s);
    /* strip trailing slash */
    {
        size_t n = strlen(base);
        while (n && base[n - 1] == '/')
            base[--n] = 0;
    }
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

static int ask_key(void)
{
    char p[512], buf[256];
    fprintf(stderr, "API key (xAI / OpenAI compatible): ");
    fflush(stderr);
    if (!fgets(buf, sizeof buf, stdin))
        return 0;
    trim(buf);
    if (!buf[0])
        return 0;
    snprintf(key, sizeof key, "%s", buf);
    path_join(p, sizeof p, ".cai.key");
    if (!save_file(p, key)) {
        fprintf(stderr, "could not write %s\n", p);
        return 0;
    }
    fprintf(stderr, "saved %s (mode 600)\n", p);
    return 1;
}

static void hist_add(const char *role, const char *txt)
{
    int i;
    if (nhist == MAX_MSG) {
        free(hist_txt[0]);
        for (i = 1; i < MAX_MSG; i++) {
            strcpy(hist_role[i - 1], hist_role[i]);
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
    fprintf(f, "{\"role\":\"system\",\"content\":\"You are cAI, a terse assistant on a small Linux handheld. Keep answers short unless asked.\"}");
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

/* last "content": "..." in the JSON */
static char *extract_content(const char *js)
{
    const char *p, *q, *as;
    char *out;
    size_t i, n;
    as = strstr(js, "\"role\"");
    p = js;
    /* last content field is usually the assistant reply */
    q = NULL;
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
                i += 4; /* skip \uXXXX */
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

static char *chat_once(char **err)
{
    char reqp[] = "/tmp/cai-req.json";
    char cmd[768], url[320];
    char *body, *raw, *reply, *http;
    FILE *f, *p;

    if (access("/usr/bin/curl", X_OK) != 0 && access("/bin/curl", X_OK) != 0) {
        *err = strdup("curl not installed. apt install curl");
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
             "curl -sS --max-time 90 "
             "-H 'Content-Type: application/json' "
             "-H 'Authorization: Bearer %s' "
             "-d @%s '%s' -w '\\nHTTP:%%{http_code}'",
             key, reqp, url);
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
            char tmp[256];
            snprintf(tmp, sizeof tmp, "HTTP %d %s: %.160s", code, url, raw);
            *err = strdup(tmp);
            free(raw);
            return NULL;
        }
    }
    reply = extract_content(raw);
    free(raw);
    if (!reply)
        *err = strdup("no content in reply");
    return reply;
}

static void banner(void)
{
    printf("cAI  model=%s  base=%s\n", model, base);
    printf("commands: /quit  /clear  /key  /model grok-4\n");
    printf("          /base https://api.x.ai/v1\n\n");
}

static void usage(void)
{
    fprintf(stderr,
            "cAI — tiny chat (OpenAI-compatible)\n"
            "  cAI            chat\n"
            "  cAI -k         set API key\n"
            "  cAI -m MODEL   (default grok-4)\n"
            "  cAI -u URL     base URL, default https://api.x.ai/v1\n"
            "  cAI -q         one prompt from stdin, print reply\n"
            "key file: ~/.cai.key   conf: ~/.cai.conf\n"
            "env: XAI_API_KEY or OPENAI_API_KEY overrides the file\n");
}

int main(int argc, char **argv)
{
    int opt, once = 0, setkey = 0;
    char line[MAX_LINE];

    load_cfg();
    while ((opt = getopt(argc, argv, "hkm:u:q")) != -1) {
        switch (opt) {
        case 'k':
            setkey = 1;
            break;
        case 'm':
            snprintf(model, sizeof model, "%s", optarg);
            save_cfg();
            break;
        case 'u':
            if (!set_base(optarg)) {
                fprintf(stderr, "bad base (need https://api.x.ai/v1, not the word URL)\n");
                return 1;
            }
            save_cfg();
            break;
        case 'q':
            once = 1;
            break;
        default:
            usage();
            return 1;
        }
    }
    if (setkey) {
        return ask_key() ? 0 : 1;
    }
    if (!key[0]) {
        fprintf(stderr, "no API key. run: cAI -k\n");
        if (!ask_key())
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
        if (!strcmp(line, "/quit") || !strcmp(line, "/q") || !strcmp(line, "/exit"))
            break;
        if (!strcmp(line, "/clear")) {
            while (nhist)
                free(hist_txt[--nhist]);
            puts("(cleared)");
            continue;
        }
        if (!strcmp(line, "/key")) {
            ask_key();
            continue;
        }
        if (!strncmp(line, "/model ", 7)) {
            snprintf(model, sizeof model, "%s", line + 7);
            save_cfg();
            printf("model=%s\n", model);
            continue;
        }
        if (!strncmp(line, "/base ", 6)) {
            if (!set_base(line + 6))
                puts("need a real URL, e.g. /base https://api.x.ai/v1");
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
