/*
 * cAI — tiny terminal chat for SharkDeck / Linux.
 * OpenAI-compatible HTTP (xAI default). Needs libcurl.
 *
 *   ./cAI                 # interactive
 *   ./cAI -k              # set / replace API key
 *   ./cAI -m grok-4       # model
 *   ./cAI -u URL          # base URL (no trailing slash)
 *   echo hi | ./cAI -q    # one shot from stdin
 */
#define _GNU_SOURCE
#include <curl/curl.h>
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

struct Buf {
    char *p;
    size_t n, cap;
};

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
        else if (!strncmp(line, "base=", 5))
            snprintf(base, sizeof base, "%s", line + 5);
    }
    fclose(f);
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

static size_t wr(char *ptr, size_t sz, size_t nm, void *ud)
{
    struct Buf *b = ud;
    size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        char *p;
        while (cap < b->n + n + 1)
            cap *= 2;
        if (cap > MAX_BODY)
            cap = MAX_BODY;
        if (b->n + n + 1 > cap)
            return 0;
        p = realloc(b->p, cap);
        if (!p)
            return 0;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, ptr, n);
    b->n += n;
    b->p[b->n] = 0;
    return n;
}

/* pull first "content": "..." after "assistant" if present */
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

static char *chat_once(char **err)
{
    CURL *c;
    CURLcode rc;
    struct Buf b = {0};
    struct curl_slist *hdr = NULL;
    char url[320], auth[300];
    char *body, *reply = NULL;
    long http = 0;

    body = build_req();
    if (!body) {
        *err = strdup("oom");
        return NULL;
    }
    snprintf(url, sizeof url, "%s/chat/completions", base);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);

    c = curl_easy_init();
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, auth);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "cAI/1.0");
    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdr);
    free(body);

    if (rc != CURLE_OK) {
        *err = strdup(curl_easy_strerror(rc));
        free(b.p);
        return NULL;
    }
    if (http / 100 != 2) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "HTTP %ld: %.180s", http, b.p ? b.p : "");
        *err = strdup(tmp);
        free(b.p);
        return NULL;
    }
    reply = extract_content(b.p ? b.p : "");
    free(b.p);
    if (!reply)
        *err = strdup("no content in reply");
    return reply;
}

static void banner(void)
{
    printf("cAI  model=%s  base=%s\n", model, base);
    printf("commands: /quit  /clear  /key  /model NAME  /base URL\n\n");
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
            snprintf(base, sizeof base, "%s", optarg);
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

    curl_global_init(CURL_GLOBAL_DEFAULT);

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
            snprintf(base, sizeof base, "%s", line + 6);
            save_cfg();
            printf("base=%s\n", base);
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
    curl_global_cleanup();
    return 0;
}
