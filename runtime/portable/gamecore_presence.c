/* Our own gamecore, the presence group (R3): presence client, handle and character records, mail,
 * text, the Help Desk FAQ and misc slots. Each follows
 * specs/gamecore-slots.presence.txt; its section 3 is the summary this file
 * implements. All cdecl (RETC).
 *
 * The model, from that spec: the online client features (friends, mail, FAQ, server-side settings)
 * are off - slot 836 says so - and every async request FFXiMain can still start finishes at once
 * with an empty result. What world entry needs is real: a valid handle record 0 (173), the
 * presence values 195 sets (189/190/204/206/207), and slot 177's record for the selected
 * character, built from FFXiMain's own character table. Slot 1132 is the text-input conversion
 * every typed character goes through. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h" /* FFXI_CHARS_PTR, per retail build */
#include "gamecore.h"
#include "gamecore_config.h"
#include "plat.h"
#include "vfs.h"

/* --- presence (1b): what 195 last set, read back by 189/190/204/206/207 ---------------------------------- */
static int32_t g_chan = 0, g_sub = -1, g_flag = 1, g_mode = 0;
static uint32_t g_word = 0x3E8;

void presence_update(int32_t chan, int32_t sub, int32_t flag, int32_t mode, int32_t word)
{
    if (chan != -2)
        g_chan = chan;
    if (sub != -2)
        g_sub = sub;
    if (flag == 0 || flag == 1)
        g_flag = flag;
    if (mode != -2)
        g_mode = mode;
    if (word >= 0 && word <= 0x3FF)
        g_word = (uint32_t)word;
}

static void s189_presence_sub(Guest* g) { RETC((uint32_t)g_sub); }
static void s190_presence_chan(Guest* g) { RETC((uint32_t)g_chan); }
static void s204_presence_flag(Guest* g) { RETC((uint32_t)g_flag); }
static void s206_presence_mode(Guest* g) { RETC((uint32_t)g_mode); }
static void s207_presence_word(Guest* g) { RETC(g_word); }

/* --- records ---------------------------------------------------------------------------------------------- */
/* 167/169 (0x10023da0 / 0x10023f90): friend lists A (200) and B (100), empty */
static void s167_friend_a(Guest* g)
{
    if (ARG(0) >= 200)
        RETC(0xFFFFE3F9u);
    memset(ARGP(1), 0, 0xB0);
    wr32(ARG(1) + 8, (ARG(0) & 0xFF) << 20);
    RETC(0);
}

static void s169_friend_b(Guest* g)
{
    if (ARG(0) >= 100)
        RETC(0xFFFFE3F9u);
    memset(ARGP(1), 0, 0xB0);
    RETC(0);
}

/* PlayOnline's handle table and member id, when the host signed in there (gamecore_set_handles,
 * gamecore_set_member_id): slots 173 and 283 hand them out as retail's do. */
static uint8_t g_handles[64 * 0x28];
static int g_have_handles;
static uint64_t g_member_id = 0x5349474Eu; /* "SIGN": any non-zero id, without a PlayOnline sign-in */

void gamecore_set_handles(const uint8_t table[64 * 0x28])
{
    memcpy(g_handles, table, sizeof g_handles);
    g_have_handles = 1;
}

void gamecore_set_member_id(uint64_t id)
{
    if (id)
        g_member_id = id;
}

void gamecore_set_presence(int32_t handle, int32_t chr, int32_t mode, int32_t openstat, int32_t contents_class)
{
    /* the kept values as they are, -1 included (retail's 190 reports -1 with no handle) */
    g_chan = handle, g_sub = chr;
    if (mode == 0 || mode == 1)
        g_flag = mode;
    if (openstat >= 0 && openstat <= 4)
        g_mode = openstat;
    if (contents_class >= 0 && contents_class <= 0x3FF)
        g_word = (uint32_t)contents_class;
}

/* 173 (0x1001cc40): handle record a0 (0x28 bytes). PlayOnline's own when the host has them (the
 * game names the member's handle, and finds a free character slot in it at character select);
 * otherwise handle 0 is the one this session uses. */
static void s173_handle(Guest* g)
{
    if (ARG(0) >= 64)
        RETC(0xFFFFE3EAu);
    uint32_t r = ARG(1);
    if (g_have_handles)
    {
        memcpy(GUEST_PTR(r), g_handles + ARG(0) * 0x28, 0x28);
        RETC(0);
    }
    memset(GUEST_PTR(r), 0, 0x28);
    if (ARG(0) == 0)
    {
        wr32(r, 1); /* valid */
        strcpy((char*)GUEST_PTR(r + 8), "FFXI");
    }
    RETC(0);
}

/* 177 (0x100227d0): character record a0 (0x68 bytes), REQUIRED for world entry (spec 1a). PlayOnline's
 * own when the host has them; otherwise built from FFXiMain's character table: entry = [[FFXI_CHARS_PTR] + 0x13820 + a0 * 0x8c], contentId =
 * d[+4], charId = w[+8] | b[+0xf] << 16, world = w[+0xa]. Registered to handle 0 (q[0x10] bit 15). */
#define FFXI_MAX_CHARS 16

/* PlayOnline's content table, when the host signed in there (gamecore_set_contents): slot 177
 * hands its records out as retail's does, so the record the game picks at character select is
 * PlayOnline's, and so is the number slot 195 then names it by. */
static uint8_t g_contents[64 * 0x68];
static int g_have_contents;

void gamecore_set_contents(const uint8_t table[64 * 0x68])
{
    memcpy(g_contents, table, sizeof g_contents);
    g_have_contents = 1;
}

static void s177_character(Guest* g)
{
    uint32_t i = ARG(0), r = ARG(1);
    if (i >= 64)
        RETC(0xFFFFE3FDu);
    if (g_have_contents)
    {
        memcpy(GUEST_PTR(r), g_contents + i * 0x68, 0x68);
        RETC(0);
    }
    memset(GUEST_PTR(r), 0, 0x68);
    uint32_t base = rd32(FFXI_CHARS_PTR) /* FFXiMain sits at its preferred base */;
    if (!base || i >= FFXI_MAX_CHARS)
        RETC(0);
    uint32_t e = base + 0x13820 + i * 0x8C;
    uint32_t content = rd32(e + 4), char_id = rd16(e + 8) | (uint32_t)rd8(e + 0xF) << 16, world = rd16(e + 0xA);
    if (!content)
        RETC(0);
    wr8(r, 1);
    wr16(r + 2, 1); /* FFXI character */
    wr32(r + 4, (char_id & 0xFFFF) | (world & 0xFFFF) << 16 | ((char_id >> 16) & 0xFF) << 24);
    wr32(r + 8, content);
    wr32(r + 0x10, 0x8000); /* registered, handle 0 */
    RETC(0);
}

/* 283 (0x1001ac60): the signed-in member's id, a u64 in edx:eax, as polcore keeps it (0x10404a88);
 * non-zero */
static void s283_account_id(Guest* g)
{
    g->edx = (uint32_t)(g_member_id >> 32);
    RETC((uint32_t)g_member_id);
}

/* --- async requests (the spec's generic rule): start -> handle 0, poll -> done ------------------------------ */
static void s_start(Guest* g) { RETC(0); }
static void s_poll_done(Guest* g) { RETC(1); }
static void s_zero(Guest* g) { RETC(0); }

/* 208/209: look up a user; "found" is the id asked for */
static uint32_t g_lookup_lo, g_lookup_hi;
static void s208_lookup_begin(Guest* g)
{
    g_lookup_lo = ARG(1), g_lookup_hi = ARG(2);
    RETC(0);
}

static void s209_lookup_poll(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), g_lookup_lo), wr32(ARG(1) + 4, g_lookup_hi);
    if (ARG(2))
        wr32(ARG(2), 0);
    RETC(1);
}

/* --- 258 (0x10012420): gamecore's own sprintf (not libc's), exactly as the spec describes ---------------------- */
static void s258_sprintf(Guest* g)
{
    uint32_t dst = ARG(0), out = dst, limit = dst + (uint32_t)(int32_t)ARG(1) - 1, fmt = ARG(2), arg = 3;
    char pad = ' ';
    for (uint32_t f = fmt; rd8(f); )
    {
        if (out >= limit)
        {
            wr8(out, 0);
            RETC(0xFFFFFFFFu);
        }
        uint8_t c = rd8(f++);
        if (c != '%')
        {
            wr8(out++, c);
            continue;
        }
        int width = 0;
        uint8_t d = rd8(f);
        if (d == '0')
        {
            pad = '0';
            width = rd8(f + 1) - '0';
            f += 2;
        }
        else if (d >= '1' && d <= '9')
        {
            pad = ' ';
            width = d - '0';
            f++;
        }
        uint8_t t = rd8(f);
        if (!t)
            break;
        f++;
        if (t == 'd' || t == 'x' || t == 'X')
        {
            uint32_t v = ARG(arg++);
            char digits[16];
            int n = 0;
            if (t == 'd' && (int32_t)v < 0)
            {
                wr8(out++, '-'); /* without the limit check, as retail */
                v = (uint32_t)(-(int32_t)v);
            }
            const char* hex = t == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do
            {
                digits[n++] = t == 'd' ? (char)('0' + v % 10) : hex[v & 0xF];
                v = t == 'd' ? v / 10 : v >> 4;
            } while (v);
            for (int p = n; p < width; ++p)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, (uint8_t)pad);
            }
            while (n)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, (uint8_t)digits[--n]);
            }
        }
        else if (t == 's')
        {
            for (uint32_t s = ARG(arg++); rd8(s); ++s)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, rd8(s));
            }
        }
        else if (t == 'c')
            wr8(out++, (uint8_t)ARG(arg++)); /* without the limit check */
        else
            wr8(out++, t); /* literally, no argument */
    }
    wr8(out, 0);
    RETC(out - dst);
}

/* --- mail, paths, PIB, text -------------------------------------------------------------------------------- */
static void s270_mail_header(Guest* g)
{
    memset(ARGP(0), 0, 0x48);
    RETC(ARG(12));
}

static void s271_split(Guest* g)
{
    if (ARG(2))
        wr8(ARG(2), 0);
    RETC(0);
}

static void s272_concat(Guest* g)
{
    if (ARG(4))
        wr8(ARG(4), 0);
    RETC(0);
}

/* --- PlayOnline messages (the member's messages) ------------------------
 * With a PlayOnline sign-in the game's in-game message client runs as on retail (slot 836): it lists
 * the member's messages on the server (278/279), reads each new one (275/276), saves it under the
 * Viewer's pub\homeNN\msg folders through the file channels (921-931) by the name 284 makes, and
 * deletes it from the server (280/281). The network part goes to the host's request channel. */
static GamecoreRequestHost g_request_host;
static int g_have_request_host;
static uint64_t g_member_key;

void gamecore_set_request_host(const GamecoreRequestHost* host)
{
    g_have_request_host = host != NULL;
    if (host)
        g_request_host = *host;
}

const GamecoreRequestHost* gamecore_request_host(void)
{
    return g_have_request_host ? &g_request_host : NULL;
}

void gamecore_set_member_key(uint64_t key)
{
    g_member_key = key;
}

/* their names (0x100078a0 and its encoder) */
static const char B64[] = "";

static void b64_header(const uint8_t h[0x48], char out[97])
{
    for (int i = 0, o = 0; i < 0x48; i += 3)
    {
        uint32_t v = (uint32_t)h[i] << 16 | (uint32_t)h[i + 1] << 8 | h[i + 2];
        for (int k = 3; k >= 0; --k)
            out[o + 3 - k] = B64[(v >> (6 * k)) & 63];
        o += 4;
    }
    out[96] = 0;
}

static void unb64_header(const char* in, uint8_t h[0x48])
{
    for (int i = 0, o = 0; i < 96; i += 4, o += 3)
    {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char* p = in[i + k] ? strchr(B64, in[i + k]) : NULL;
            v = v << 6 | (uint32_t)(p ? p - B64 : 0);
        }
        h[o] = (uint8_t)(v >> 16), h[o + 1] = (uint8_t)(v >> 8), h[o + 2] = (uint8_t)v;
    }
}

/* A header's ids between the wire form (id ^ 0x0) and the form the game holds
 * (also ^ the member key): sender always, receiver unless the Type is 17 (polcore 0x1001b640,
 * 0x1001bd00). Its own inverse. */
static void rekey_header(uint8_t h[0x48])
{
    uint64_t k = 0x0ull ^ g_member_key;
    int type17 = ((h[0x3e] | h[0x3f] << 8) & 0xf80) == 0x880;
    for (int at = 0; at <= (type17 ? 0 : 8); at += 8)
        for (int b = 0; b < 8; ++b)
            h[at + b] ^= (uint8_t)(k >> (8 * b));
}

/* the requests 278/275/280 start, by the handle the game polls with (as polcore's 4 contexts) */
typedef struct MsgRequest
{
    int32_t id;   /* the host's; -1 free */
    int kind;     /* 278, 275, 280 */
    uint32_t dst; /* headers or the body */
    uint32_t cap; /* headers, or body bytes */
} MsgRequest;
static MsgRequest g_msg[4] = { { -1 }, { -1 }, { -1 }, { -1 } };

static int32_t msg_begin(int kind, const char* verb, const uint8_t* data, uint32_t n, uint32_t dst, uint32_t cap)
{
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; ++i)
        if (g_msg[i].id < 0)
            slot = i;
    if (slot < 0)
        return -256; /* no free context (0x1001eba0) */
    int32_t id = g_request_host.begin(g_request_host.ctx, verb, data, n);
    if (id < 0)
        return id;
    g_msg[slot] = (MsgRequest){ id, kind, dst, cap };
    return slot;
}

static int32_t msg_poll(int32_t h, int kind)
{
    if (h < 0 || h >= 4 || g_msg[h].id < 0 || g_msg[h].kind != kind)
        return -5136; /* not in use */
    MsgRequest* m = &g_msg[h];
    uint32_t room = kind == 278 ? m->cap * 0x48 : kind == 275 ? m->cap : kind == 175 ? sizeof g_contents : 0, got = 0;
    uint8_t* buf = room ? (uint8_t*)malloc(room) : NULL;
    int32_t r = g_request_host.poll(g_request_host.ctx, m->id, buf, room, &got);
    if (r != 0)
    {
        if (r > 0 && kind == 278)
        {
            /* the headers in the game's form, and bValid cleared in the rest (0x10027660) */
            uint32_t n = got / 0x48;
            for (uint32_t i = 0; i < n; ++i)
            {
                rekey_header(buf + 0x48 * i);
                memcpy(GUEST_PTR(m->dst + 0x48 * i), buf + 0x48 * i, 0x48);
            }
            for (uint32_t i = n; i < m->cap; ++i)
                wr8(m->dst + 0x48 * i + 0x3f, rd8(m->dst + 0x48 * i + 0x3f) & 0x7f);
        }
        else if (r > 0 && kind == 275 && got)
            memcpy(GUEST_PTR(m->dst), buf, got < m->cap ? got : m->cap);
        else if (r > 0 && kind == 175 && got == sizeof g_contents)
            gamecore_set_contents(buf); /* what 177 hands out from now on, as polcore's table */
        m->id = -1;
    }
    free(buf);
    return r;
}

/* 278 (0x1001bf30): list the member's messages , a0 = headers, a1 = how many */
static void s278_fetch_headers(Guest* g)
{
    if (!g_have_request_host)
    {
        memset(ARGP(0), 0, ARG(1) * 0x48);
        RETC(0);
    }
    uint8_t cap[2] = { (uint8_t)ARG(1), (uint8_t)(ARG(1) >> 8) };
    RETC((uint32_t)msg_begin(278, "msglist", cap, 2, ARG(0), ARG(1)));
}

static void s279_fetch_poll(Guest* g) { RETC(g_have_request_host ? (uint32_t)msg_poll((int32_t)ARG(0), 278) : 1u); }

/* 175 (0x1001e520 -> 0x1001d2f0): load the member's content ID list again (the request); the
 * request's handle. In the game, after the lobby's status, retail's core sends it on its own; its
 * reply refills the table 177 reads. 176 (0x1001e540 -> 0x1001d340) polls it: 0 busy, 1 done. */
static void s175_content_begin(Guest* g)
{
    if (!g_have_request_host)
        RETC(0);
    RETC((uint32_t)msg_begin(175, "contentlist", NULL, 0, 0, 0));
}

static void s176_content_poll(Guest* g) { RETC(g_have_request_host ? (uint32_t)msg_poll((int32_t)ARG(0), 175) : 1u); }

/* 275 (0x1001bfc0): read a message (the request), a0 = its header, a1 = the body (header +0x38 bytes) */
static void s275_read(Guest* g)
{
    if (!g_have_request_host)
        RETC(0);
    uint8_t h[0x48];
    memcpy(h, ARGP(0), 0x48);
    rekey_header(h);
    RETC((uint32_t)msg_begin(275, "msgread", h, 0x48, ARG(1), rd32(ARG(0) + 0x38)));
}

static void s276_read_poll(Guest* g) { RETC(g_have_request_host ? (uint32_t)msg_poll((int32_t)ARG(0), 275) : 1u); }

/* 280 (0x1001c070): delete a message on the server (the request), a0 = its header */
static void s280_delete(Guest* g)
{
    if (!g_have_request_host)
        RETC(0);
    uint8_t h[0x48];
    memcpy(h, ARGP(0), 0x48);
    rekey_header(h);
    RETC((uint32_t)msg_begin(280, "msgdelete", h, 0x48, 0, 0));
}

static void s281_delete_poll(Guest* g) { RETC(g_have_request_host ? (uint32_t)msg_poll((int32_t)ARG(0), 280) : 1u); }

/* 277: free a request's context */
static void s277_free(Guest* g)
{
    int32_t h = (int32_t)ARG(0);
    if (h >= 0 && h < 4)
        g_msg[h].id = -1;
    RETC(0);
}

static void s282_unread(Guest* g) { RETC(rd16(ARG(0) + 0x3E) >> 15); }

/* 284-287 (0x1001acb0..): a message's file: GetPath 7 (received, new), 8 (received, read), 5, 6
 * (sent), then the 96-character base64 of its header in the wire form; the header is left as it was */
static void s28x_path(Guest* g, uint32_t dir)
{
    char path[1024], name[97];
    uint8_t h[0x48];
    memcpy(h, ARGP(0), 0x48);
    rekey_header(h);
    b64_header(h, name);
    gamecore_path(dir, path, sizeof path);
    snprintf((char*)ARGP(1), 0x200, "%s%s", path, name);
    RETC(0);
}
static void s284_path(Guest* g) { s28x_path(g, 7); }
static void s285_path(Guest* g) { s28x_path(g, 8); }
static void s286_path(Guest* g) { s28x_path(g, 5); }
static void s287_path(Guest* g) { s28x_path(g, 6); }

/* 288: the notification callback. Stored; retail calls it for a message NOTICE only while the
 * Viewer has not switched it off for the title (slot 1032), so not during play. */
static uint32_t g_mail_callback;
static void s288_mail_callback(Guest* g)
{
    g_mail_callback = ARG(0);
    RETC(ARG(0));
}

/* 291 (0x1001b640): a file's name (96 characters at least) -> its header, in the game's form */
static void s291_decode_index(Guest* g)
{
    const char* name = (const char*)ARGP(0);
    if (strnlen(name, 0x60) < 0x60)
        RETC(0xFFFFEC00u);
    uint8_t h[0x48];
    unb64_header(name, h);
    rekey_header(h);
    memcpy(ARGP(1), h, 0x48);
    RETC(0);
}

/* 297 (0x10020720): query node op; 299: link n nodes */
static void s297_node_op(Guest* g)
{
    uint32_t n = ARG(0);
    wr32(n + 4, ARG(1));
    if (ARG(1) == 7)
    {
        wr8(n + 0xC, rd8(n + 0xC) | 1);
        if (ARG(2))
            wr8(n + 0xF, 1), wr32(n + 0x10, ARG(2));
    }
    RETC(0);
}

static void s299_link_nodes(Guest* g)
{
    int32_t n = (int32_t)ARG(0);
    uint32_t last = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        uint32_t node = ARG(1 + (uint32_t)i);
        if (!node)
            continue;
        wr32(node, last);
        last = node;
    }
    RETC(last);
}

/* 836 (0x100458c0): turn the in-game message client on. With a PlayOnline sign-in it answers 1 once,
 * as retail's did live (its flag is fetched and cleared); otherwise the client stays off. */
static int g_service_asked;

/* the message folders the client lists and writes (GetPath 5-8: pub\homeNN\msg\{s,r}\{b,a}), made
 * when missing, as an installed Viewer has them */
static void make_msg_folders(void)
{
    for (uint32_t id = 5; id <= 8; ++id)
    {
        char guest[1024], host[1024];
        gamecore_path(id, guest, sizeof guest);
        if (!vfs_host_path(guest, host, sizeof host))
            continue;
        for (char* p = host + 1; *p; ++p)
            if (*p == '/' || *p == '\\')
            {
                char c = *p;
                *p = 0;
                plat_mkdir(host);
                *p = c;
            }
        plat_mkdir(host);
    }
}

static void s836_online_client(Guest* g)
{
    if (!g_have_request_host || g_service_asked++)
        RETC(0);
    make_msg_folders();
    RETC(1);
}

/* 921..931: the file channels (0x100be0f8, 4 of them; retail runs them on a worker thread, here each
 * operation completes when started and its poll reports it) */
typedef struct FileChannel
{
    int used;
    PlatFile* f;
    int32_t bytes; /* the last read or write */
    int32_t count; /* the last listing */
} FileChannel;
static FileChannel g_files[4];

static int chan_new(void)
{
    for (int i = 0; i < 4; ++i)
        if (!g_files[i].used)
        {
            memset(&g_files[i], 0, sizeof g_files[i]);
            g_files[i].used = 1;
            return i;
        }
    return -1;
}

static FileChannel* chan_of(uint32_t c) { return c < 4 && g_files[c].used ? &g_files[c] : NULL; }

/* 921 (0x1002ec30): open a0, mode a1 (1 read; 0xe create, write, truncate) */
static void s921_open(Guest* g)
{
    char host[1024];
    if (!ARG(0) || !vfs_host_path((const char*)ARGP(0), host, sizeof host))
        RETC(0xFFFFD800u);
    vfs_match_case(host);
    int flags = ARG(1) & 0xe ? PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE : PLAT_READ;
    PlatFile* f = plat_file_open(host, flags);
    if (!f)
        RETC(0xFFFFD800u);
    int c = chan_new();
    if (c < 0)
    {
        plat_file_close(f);
        RETC(0xFFFFD800u);
    }
    g_files[c].f = f;
    RETC((uint32_t)c);
}

static void s922_open_poll(Guest* g) { RETC(chan_of(ARG(0)) ? 1u : 0xFFFFD800u); }

static void s923_close(Guest* g)
{
    FileChannel* c = chan_of(ARG(0));
    if (c)
    {
        if (c->f)
            plat_file_close(c->f);
        c->used = 0;
    }
    RETC(1);
}

static void s926_read(Guest* g)
{
    FileChannel* c = chan_of(ARG(0));
    if (!c || !c->f)
        RETC(0xFFFFD800u);
    int64_t n = plat_file_read(c->f, ARGP(1), ARG(2));
    c->bytes = n < 0 ? 0 : (int32_t)n;
    RETC(n < 0 ? 0xFFFFD800u : 0u);
}

static void s928_write(Guest* g)
{
    FileChannel* c = chan_of(ARG(0));
    if (!c || !c->f)
        RETC(0xFFFFD800u);
    int64_t n = plat_file_write(c->f, ARGP(1), ARG(2));
    c->bytes = n < 0 ? 0 : (int32_t)n;
    RETC(n < 0 ? 0xFFFFD800u : 0u);
}

/* 927 = 929: a read or write's bytes */
static void s927_rw_poll(Guest* g)
{
    FileChannel* c = chan_of(ARG(0));
    if (!c)
        RETC(0xFFFFD800u);
    if (ARG(1))
        wr32(ARG(1), (uint32_t)c->bytes);
    RETC(1);
}

/* 931 (0x1002f0d0): list a0\* into a1 (0x208-byte entries from a1 + 8: +0 1 | 2 if writable | 4 if
 * a directory, +4 size, +8 the name), skipping a2 entries, at most a3 - as _findfirst lists them,
 * "." and ".." first */
static void s931_list(Guest* g)
{
    char host[1024];
    if (!ARG(0) || !vfs_host_path((const char*)ARGP(0), host, sizeof host))
        RETC(0xFFFFD800u);
    vfs_match_case(host);
    PlatDir* d = plat_dir_open(host);
    if (!d)
        RETC(0xFFFFD800u);
    int c = chan_new();
    if (c < 0)
    {
        plat_dir_close(d);
        RETC(0xFFFFD800u);
    }
    uint32_t out = ARG(1) + 8, skip = ARG(2), max = ARG(3), index = 0;
    int32_t count = 0;
    for (int dots = 0;; ++dots)
    {
        const char* name = dots == 0 ? "." : dots == 1 ? ".." : plat_dir_next(d);
        if (!name)
            break;
        if (index++ < skip)
            continue;
        if ((uint32_t)count >= max)
            break;
        char full[1400];
        snprintf(full, sizeof full, "%s%s%s", host, plat_path_sep == '/' ? "/" : "\\", name);
        PlatStat st = { 0 };
        int is_dir = dots < 2;
        if (!is_dir && plat_stat(full, &st))
            is_dir = st.is_dir;
        uint32_t e = out + 0x208 * (uint32_t)count++;
        memset(GUEST_PTR(e), 0, 0x208);
        wr32(e, 1 | (st.readonly ? 0 : 2) | (is_dir ? 4 : 0));
        wr32(e + 4, (uint32_t)st.size);
        snprintf((char*)GUEST_PTR(e + 8), 0x104, "%s", name);
    }
    plat_dir_close(d);
    g_files[c].count = count;
    RETC((uint32_t)c);
}

/* 930 (0x1002f410): the listing's entries */
static void s930_list_poll(Guest* g)
{
    FileChannel* c = chan_of(ARG(0));
    if (!c)
        RETC(0xFFFFD800u);
    if (ARG(1))
        wr32(ARG(1), (uint32_t)c->count);
    RETC(1);
}

static void s_file_fail(Guest* g) { RETC(0xFFFFD800u); }

/* 1004/1005/1006: online error message text */
static void s1006_error_table(Guest* g) { RETC(ARG(0)); }

static void s1004_error_text(Guest* g)
{
    char msg[64];
    int n = snprintf(msg, sizeof msg, "Online error %d", (int32_t)ARG(1));
    uint32_t size = ARG(3);
    if (size)
    {
        uint32_t k = (uint32_t)n < size - 1 ? (uint32_t)n : size - 1;
        memcpy(GUEST_PTR(ARG(2)), msg, k);
        wr8(ARG(2) + k, 0);
    }
    RETC(0);
}

/* 1132 (0x1002e6f0): the text-input conversion every typed character goes through (the chat box
 * drops a character whose conversion is empty). The Latin path: ASCII is copied; a byte >= 0x80
 * becomes the pair 0x85, (b < 0xC0 ? b - 0x40 + (b == 0xBF) : b - 0x21). Returns the number of
 * converted characters. The Japanese path (half-width katakana tables in gamecore's data) is not
 * implemented: a US client sets LATIN. */
static void s1132_text_input(Guest* g)
{
    uint32_t src = ARG(0), dst = ARG(1), len = ARG(2), o = 0, count = 0;
    for (uint32_t i = 0; i < len;)
    {
        uint8_t b = rd8(src + i++);
        if (b < 0x80)
        {
            if (!b)
                break;
            wr8(dst + o++, b);
            continue;
        }
        count++;
        wr8(dst + o++, 0x85);
        wr8(dst + o++, (uint8_t)(b < 0xC0 ? b - 0x40 + (b == 0xBF) : b - 0x21));
    }
    wr8(dst + o, 0);
    RETC(count);
}

static void s1483_mail_text(Guest* g)
{
    if ((int32_t)ARG(2) > 0)
        wr8(ARG(1), 0);
    RETC(0);
}

static void s_settings_fail(Guest* g) { RETC(0xFFFFFF00u); } /* 1535/1536: no server-side settings */

#define SLOT(n, f) { 4 * (n), f }
static const GamecoreSlot PRESENCE[] = {
    SLOT(165, s_start), SLOT(166, s_poll_done), SLOT(167, s167_friend_a), SLOT(169, s169_friend_b),
    SLOT(173, s173_handle), SLOT(174, s_zero), SLOT(175, s175_content_begin), SLOT(176, s176_content_poll), SLOT(177, s177_character),
    SLOT(179, s_start), SLOT(180, s_poll_done), SLOT(181, s_start), SLOT(182, s_poll_done), SLOT(183, s_zero),
    SLOT(184, s_zero), SLOT(185, s_zero), SLOT(186, s_zero), SLOT(187, s_zero), SLOT(188, s_zero),
    SLOT(189, s189_presence_sub), SLOT(190, s190_presence_chan), SLOT(197, s_zero), SLOT(199, s_zero),
    SLOT(204, s204_presence_flag), SLOT(206, s206_presence_mode), SLOT(207, s207_presence_word),
    SLOT(208, s208_lookup_begin), SLOT(209, s209_lookup_poll), SLOT(258, s258_sprintf),
    SLOT(270, s270_mail_header), SLOT(271, s271_split), SLOT(272, s272_concat), SLOT(273, s_start),
    SLOT(274, s_poll_done), SLOT(275, s275_read), SLOT(276, s276_read_poll), SLOT(277, s277_free),
    SLOT(278, s278_fetch_headers), SLOT(279, s279_fetch_poll), SLOT(280, s280_delete), SLOT(281, s281_delete_poll),
    SLOT(282, s282_unread), SLOT(283, s283_account_id), SLOT(284, s284_path), SLOT(285, s285_path),
    SLOT(286, s286_path), SLOT(287, s287_path), SLOT(288, s288_mail_callback), SLOT(289, s_zero),
    SLOT(290, s_zero), SLOT(291, s291_decode_index), SLOT(292, s_zero), SLOT(293, s_zero), SLOT(294, s_zero),
    SLOT(295, s_zero), SLOT(296, s_zero), SLOT(297, s297_node_op), SLOT(298, s297_node_op),
    SLOT(299, s299_link_nodes), SLOT(300, s_zero), SLOT(301, s_start), SLOT(302, s_poll_done), SLOT(422, s_zero),
    SLOT(638, s_zero), SLOT(639, s_zero), SLOT(640, s_zero), SLOT(641, s_zero), SLOT(642, s_zero),
    SLOT(643, s_zero), SLOT(644, s_zero), SLOT(645, s_zero), SLOT(646, s_zero), SLOT(647, s_zero),
    SLOT(648, s_zero), SLOT(649, s_zero), SLOT(650, s_zero), SLOT(651, s_zero), SLOT(652, s_zero),
    SLOT(653, s_zero), SLOT(654, s_zero), SLOT(655, s_zero), SLOT(656, s_zero), SLOT(657, s_zero),
    SLOT(658, s_zero), SLOT(659, s_zero), SLOT(660, s_zero), SLOT(661, s_zero), SLOT(662, s_zero),
    SLOT(663, s_zero), SLOT(664, s_zero), SLOT(665, s_zero), SLOT(666, s_zero), SLOT(667, s_zero),
    SLOT(668, s_zero), SLOT(669, s_zero), SLOT(836, s836_online_client), SLOT(921, s921_open),
    SLOT(922, s922_open_poll), SLOT(923, s923_close), SLOT(924, s_file_fail), SLOT(925, s_file_fail),
    SLOT(926, s926_read), SLOT(927, s927_rw_poll), SLOT(928, s928_write), SLOT(929, s927_rw_poll),
    SLOT(930, s930_list_poll), SLOT(931, s931_list), SLOT(1004, s1004_error_text), SLOT(1005, s1004_error_text),
    SLOT(1006, s1006_error_table), SLOT(1132, s1132_text_input), SLOT(1152, s_zero), SLOT(1483, s1483_mail_text),
    SLOT(1535, s_settings_fail), SLOT(1536, s_settings_fail),
    { 0, NULL },
};

void gamecore_presence_init(void)
{
    gamecore_register(PRESENCE);
}
