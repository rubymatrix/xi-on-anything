/* Files over HTTP for the browser build: the game install and host64's own files, read from the local
 * server (tools/webserve.py) as the game asks for them. plat_posix.c sends paths under the mounts
 * here when XI_HTTP_URL is set (the page sets it; the Node build reads the disk instead):
 *
 *   /game/...  the install (the server's /dat/), read only
 *   /app/...   ffxi.reg and the texture packs (the server's /app/)
 *   /dats/N/.. the DAT overlay folders (the server's /dats/), for host64's --dats
 *
 * Each mount's index (every file with its size, every folder) is fetched once: lookups, FindFirstFile
 * and stat need no request, and names match without case, as on Windows. Reads go through a cache of
 * 256 KB blocks, each block one Range request (a synchronous XMLHttpRequest: the game's threads are
 * workers, where that is allowed). */
#include "httpfs_web.h"

#include <emscripten/emscripten.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- requests ----------------------------------------------------------------------------------------- */
/* n bytes at start (n < 0: the whole file) into dst (cap bytes): bytes copied, or -1 */
EM_JS(int, httpfs_xhr, (const char* url, double start, double n, uint8_t* dst, uint32_t cap), {
    const x = new XMLHttpRequest();
    x.open('GET', UTF8ToString(url), false);
    x.responseType = 'arraybuffer';
    if (n >= 0) x.setRequestHeader('Range', 'bytes=' + start + '-' + (start + n - 1));
    try { x.send(); } catch (e) { return -1; }
    if (x.status != 200 && x.status != 206) return -1;
    const b = new Uint8Array(x.response);
    const k = Math.min(b.length, cap);
    new Uint8Array(wasmMemory.buffer).set(b.subarray(0, k), dst);
    return k;
});

/* a whole response, malloc'd (NUL-terminated), or NULL */
EM_JS(uint8_t*, httpfs_get, (const char* url, uint32_t* size), {
    const x = new XMLHttpRequest();
    x.open('GET', UTF8ToString(url), false);
    x.responseType = 'arraybuffer';
    try { x.send(); } catch (e) { return 0; }
    if (x.status != 200) return 0;
    const b = new Uint8Array(x.response);
    const p = _malloc(b.length + 1);
    const m = new Uint8Array(wasmMemory.buffer);
    m.set(b, p);
    m[p + b.length] = 0;
    new DataView(wasmMemory.buffer).setUint32(size, b.length, true);
    return p;
});

/* --- the index ------------------------------------------------------------------------------------------ */
typedef struct Ent
{
    char* name;   /* as written, the last component */
    char* key;    /* lower-case, from the mount's root: "rom/1/2.dat" */
    int64_t size; /* -1: a folder */
    int parent, child, next; /* -1: none */
} Ent;

typedef struct Mount
{
    const char* prefix; /* "/game/" */
    const char* route;  /* "dat" */
    int loaded;
    Ent* ents;
    int n, cap;
    int* table; /* open addressing on key: entry + 1, 0 empty */
    unsigned tcap;
} Mount;

static Mount g_mounts[] = { { "/game/", "dat" }, { "/app/", "app" }, { "/dats/", "dats" } };
#define NMOUNTS (int)(sizeof g_mounts / sizeof *g_mounts)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_base[512], g_token[128];
static int g_on = -1;

static int on(void)
{
    if (g_on < 0)
    {
        const char* b = getenv("XI_HTTP_URL");
        const char* t = getenv("XI_TOKEN");
        g_on = b && *b;
        snprintf(g_base, sizeof g_base, "%s", g_on ? b : "");
        snprintf(g_token, sizeof g_token, "%s", t ? t : "");
    }
    return g_on;
}

static unsigned hash(const char* s)
{
    unsigned h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static int find(Mount* m, const char* key)
{
    for (unsigned i = hash(key) & (m->tcap - 1);; i = (i + 1) & (m->tcap - 1))
    {
        int e = m->table[i] - 1;
        if (e < 0)
            return -1;
        if (!strcmp(m->ents[e].key, key))
            return e;
    }
}

static int add(Mount* m, const char* rel, int64_t size)
{
    if (m->n == m->cap)
    {
        m->cap = m->cap ? m->cap * 2 : 1024;
        m->ents = (Ent*)realloc(m->ents, (size_t)m->cap * sizeof *m->ents);
    }
    Ent* e = &m->ents[m->n];
    e->key = strdup(rel);
    for (char* c = e->key; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
    const char* slash = strrchr(rel, '/');
    e->name = strdup(slash ? slash + 1 : rel);
    e->size = size;
    e->child = e->next = -1;
    e->parent = 0; /* the root, unless a folder above is found below */
    if (slash)
    {
        char* up = strndup(e->key, (size_t)(slash - rel));
        int p = find(m, up);
        free(up);
        if (p >= 0)
            e->parent = p;
    }
    int idx = m->n++;
    if (idx)
    {
        Ent* par = &m->ents[e->parent];
        e->next = par->child; /* listed newest first: FindFirstFile promises no order */
        par->child = idx;
    }
    for (unsigned i = hash(e->key) & (m->tcap - 1);; i = (i + 1) & (m->tcap - 1))
        if (!m->table[i])
        {
            m->table[i] = idx + 1;
            break;
        }
    return idx;
}

static void load(Mount* m)
{
    if (m->loaded)
        return;
    m->loaded = 1;
    char url[700];
    snprintf(url, sizeof url, "%s/%s/index?t=%s", g_base, m->route, g_token);
    uint32_t size = 0;
    char* text = (char*)httpfs_get(url, &size);
    int lines = 1;
    for (uint32_t i = 0; text && i < size; ++i)
        lines += text[i] == '\n';
    m->tcap = 1;
    while (m->tcap < (unsigned)lines * 2 + 16)
        m->tcap <<= 1;
    m->table = (int*)calloc(m->tcap, sizeof *m->table);
    add(m, "", -1); /* the root, entry 0 */
    for (char* line = text; line && *line;)
    {
        char* end = strchr(line, '\n');
        if (end)
            *end = 0;
        char* tab = strchr(line, '\t');
        if (tab)
        {
            *tab = 0;
            add(m, line, strtoll(tab + 1, NULL, 10));
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
}

/* path -> mount and lower-case key (".." and "." folded), or NULL */
static Mount* resolve(const char* path, char* key, size_t n)
{
    if (!on())
        return NULL;
    char norm[1024];
    size_t o = 0;
    norm[0] = 0;
    for (const char* p = path; *p;)
    {
        while (*p == '/' || *p == '\\')
            ++p;
        const char* s = p;
        while (*p && *p != '/' && *p != '\\')
            ++p;
        size_t len = (size_t)(p - s);
        if (!len || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.')
        {
            while (o && norm[o - 1] != '/')
                --o;
            if (o)
                --o;
            norm[o] = 0;
            continue;
        }
        if (o + len + 2 >= sizeof norm)
            return NULL;
        norm[o++] = '/';
        for (size_t i = 0; i < len; ++i)
            norm[o++] = (char)tolower((unsigned char)s[i]);
        norm[o] = 0;
    }
    for (int i = 0; i < NMOUNTS; ++i)
    {
        size_t pl = strlen(g_mounts[i].prefix) - 1; /* "/game" */
        if (!strncmp(norm, g_mounts[i].prefix, pl) && (norm[pl] == '/' || !norm[pl]))
        {
            snprintf(key, n, "%s", norm[pl] ? norm + pl + 1 : "");
            return &g_mounts[i];
        }
    }
    return NULL;
}

static int lookup(const char* path, Mount** mp)
{
    char key[1024];
    Mount* m = resolve(path, key, sizeof key);
    if (!m)
        return -2;
    pthread_mutex_lock(&g_mu);
    load(m);
    int e = find(m, key);
    pthread_mutex_unlock(&g_mu);
    *mp = m;
    return e;
}

int httpfs_owns(const char* path)
{
    char key[1024];
    return resolve(path, key, sizeof key) != NULL;
}

int httpfs_stat(const char* path, int64_t* size, int* is_dir)
{
    Mount* m;
    int e = lookup(path, &m);
    if (e < 0)
        return 0;
    *size = m->ents[e].size < 0 ? 0 : m->ents[e].size;
    *is_dir = m->ents[e].size < 0;
    return 1;
}

/* --- reading -------------------------------------------------------------------------------------------- */
#define BLOCK (256u << 10)
#define SLOTS 128 /* 32 MB */
typedef struct Slot
{
    Mount* m;
    int ent;
    uint32_t block;
    int used, ref;
    uint8_t* data;
    uint32_t len;
} Slot;
static Slot g_slots[SLOTS];
static unsigned g_hand;

static void url_of(Mount* m, int e, char* url, size_t n)
{
    /* the path as the index wrote it: the key with each component's own case */
    char rel[1024] = "";
    int chain[64], depth = 0;
    for (int i = e; i > 0 && depth < 64; i = m->ents[i].parent)
        chain[depth++] = i;
    size_t o = 0;
    while (depth--)
    {
        const char* name = m->ents[chain[depth]].name;
        for (const char* c = name; *c && o + 4 < sizeof rel; ++c)
        {
            unsigned char ch = (unsigned char)*c;
            if (isalnum(ch) || ch == '.' || ch == '-' || ch == '_')
                rel[o++] = (char)ch;
            else
                o += (size_t)snprintf(rel + o, sizeof rel - o, "%%%02X", ch);
        }
        if (depth)
            rel[o++] = '/';
        rel[o] = 0;
    }
    snprintf(url, n, "%s/%s/%s?t=%s", g_base, m->route, rel, g_token);
}

/* the block, held (ref) until release; NULL if it can't be read */
static Slot* block(Mount* m, int e, uint32_t b)
{
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < SLOTS; ++i)
        if (g_slots[i].used && g_slots[i].m == m && g_slots[i].ent == e && g_slots[i].block == b)
        {
            g_slots[i].ref++;
            pthread_mutex_unlock(&g_mu);
            return &g_slots[i];
        }
    Slot* s = NULL;
    for (int tries = 0; tries < 2 * SLOTS && !s; ++tries) /* clock: skip the held */
    {
        Slot* c = &g_slots[g_hand++ % SLOTS];
        if (!c->ref)
            s = c;
    }
    if (!s)
    {
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    if (!s->data)
        s->data = (uint8_t*)malloc(BLOCK);
    s->used = 0, s->ref = 1;
    char url[1400];
    url_of(m, e, url, sizeof url);
    pthread_mutex_unlock(&g_mu);
    int64_t size = m->ents[e].size, at = (int64_t)b * BLOCK;
    uint32_t want = (uint32_t)(size - at < BLOCK ? size - at : BLOCK);
    int got = s->data ? httpfs_xhr(url, (double)at, (double)want, s->data, BLOCK) : -1;
    pthread_mutex_lock(&g_mu);
    if (got < 0)
    {
        s->ref = 0;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    s->m = m, s->ent = e, s->block = b, s->len = (uint32_t)got, s->used = 1;
    pthread_mutex_unlock(&g_mu);
    return s;
}

static void release(Slot* s)
{
    pthread_mutex_lock(&g_mu);
    s->ref--;
    pthread_mutex_unlock(&g_mu);
}

struct HttpFile
{
    Mount* m;
    int ent;
    int64_t pos;
};

HttpFile* httpfs_open(const char* path)
{
    Mount* m;
    int e = lookup(path, &m);
    if (e < 0 || m->ents[e].size < 0)
        return NULL;
    HttpFile* f = (HttpFile*)calloc(1, sizeof *f);
    f->m = m, f->ent = e;
    return f;
}

int64_t httpfs_read(HttpFile* f, void* buf, uint32_t n)
{
    int64_t size = f->m->ents[f->ent].size;
    uint32_t done = 0;
    while (done < n && f->pos < size)
    {
        uint32_t b = (uint32_t)(f->pos / BLOCK), off = (uint32_t)(f->pos % BLOCK);
        Slot* s = block(f->m, f->ent, b);
        if (!s)
            return done ? (int64_t)done : -1;
        uint32_t c = s->len > off ? s->len - off : 0;
        if (c > n - done)
            c = n - done;
        memcpy((uint8_t*)buf + done, s->data + off, c);
        release(s);
        if (!c)
            break;
        done += c, f->pos += c;
    }
    return done;
}

int64_t httpfs_seek(HttpFile* f, int64_t offset, int whence)
{
    int64_t base = whence == 1 ? f->pos : whence == 2 ? f->m->ents[f->ent].size : 0;
    if (base + offset < 0)
        return -1;
    return f->pos = base + offset;
}

int64_t httpfs_size(HttpFile* f) { return f->m->ents[f->ent].size; }
void httpfs_close(HttpFile* f) { free(f); }

unsigned char* httpfs_read_all(const char* path, size_t* size)
{
    HttpFile* f = httpfs_open(path);
    if (!f)
        return NULL;
    int64_t n = httpfs_size(f);
    unsigned char* buf = (unsigned char*)malloc(n > 0 ? (size_t)n : 1);
    if (buf && httpfs_read(f, buf, (uint32_t)n) != n)
    {
        free(buf);
        buf = NULL;
    }
    httpfs_close(f);
    if (buf)
        *size = (size_t)n;
    return buf;
}

/* --- folders -------------------------------------------------------------------------------------------- */
struct HttpDir
{
    Mount* m;
    int at;
};

HttpDir* httpfs_dir_open(const char* path)
{
    Mount* m;
    int e = lookup(path, &m);
    if (e < 0 || m->ents[e].size >= 0)
        return NULL;
    HttpDir* d = (HttpDir*)calloc(1, sizeof *d);
    d->m = m, d->at = m->ents[e].child;
    return d;
}

const char* httpfs_dir_next(HttpDir* d)
{
    if (d->at < 0)
        return NULL;
    const char* name = d->m->ents[d->at].name;
    d->at = d->m->ents[d->at].next;
    return name;
}

void httpfs_dir_close(HttpDir* d) { free(d); }
