// 让 Windows 微信的撤回提示显示具体内容、隔了多久，并排在原消息下面（微信 4.2.0.53 / x64）
//
// 编译成一段不依赖任何库、没有重定位的代码，由 tools/build.py 放进 Weixin.dll 新增的节里。
// Weixin.dll 里只改两处指令，分别 call 到这里的两个桩：
//
//   thunk_fin    消息终结器收尾处：记下「服务器 ID → 类型 + 摘要」；是撤回通知就调整位置
//   thunk_parse  撤回解析成功处：按 newmsgid 查到原消息，改写提示，并把 newmsgid 清零让消息留住
//
// 与 macOS 版（runtime/wxar_tip.cpp）是同一套规则。不分配内存、不抛异常、不写盘、不联网、没有日志。
// 定义 WXAR_TEST 时可在任意平台编译，供单元测试使用。

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

// ------------------------------------------------------------ 4.2.0.53 的布局

enum {
    MSG_TYPE = 0x0C,      // u32
    MSG_SUBTYPE = 0x10,   // u32
    MSG_SENDER = 0x58,    // std::string
    MSG_ID = 0x148,       // u64，服务器 ID
    MSG_SORT = 0x150,     // u64，排序号 = 时间 * 1000 + 同一秒内的序号
    MSG_TIME = 0x164,     // u32，秒
    MSG_CONTENT = 0x180,  // std::string
    SYS_TYPE = 0x220,     // std::string，"revokemsg"
    SYS_NEWMSGID = 0x248, // u64，要删的消息
    SYS_TIP = 0x250,      // std::string，提示文案
};

// MSVC x64 release 的 std::string
typedef struct {
    union { char buf[16]; char *ptr; } u;
    u64 size;
    u64 cap;
} wstr;

// 微信自己的 std::string::assign（仅用于超出现有容量时；用它的分配器，不混用堆）
extern void wx_realloc_assign(wstr *s, u64 n, u64 unused, const char *src);

enum {
    N = 4096,          // 记住最近多少条
    TEXT_MAX = 404,    // 100 个字 * 4 字节 + 省略号，再留一点
    MAX_CHARS = 100,
    RAW_MAX = 2048,    // 从 XML 里最多取多少字节
    TIP_MAX = 512,     // 原提示超过这么长就不改
    OUT_MAX = 1400,
    MAX_CONTENT = 4 << 20,
    MAX_AGE = 7 * 24 * 3600,
};

#define FIELD(type, p, off) (*(type *)((u8 *)(p) + (off)))
#define NOINLINE __attribute__((noinline))

// ------------------------------------------------------------ 基础

// 自带 mem 函数，编译器可能会生成对它们的调用
#ifndef WXAR_TEST
NOINLINE void *memcpy(void *d, const void *s, u64 n) {
    u8 *a = d; const u8 *b = s;
    while (n--) { *a++ = *b++; __asm__ volatile("" ::: "memory"); }
    return d;
}
NOINLINE void *memset(void *d, int c, u64 n) {
    u8 *a = d;
    while (n--) { *a++ = (u8)c; __asm__ volatile("" ::: "memory"); }
    return d;
}
#endif

static void copy(char *d, const char *s, u64 n) {
    for (u64 i = 0; i < n; i++) { d[i] = s[i]; __asm__ volatile("" ::: "memory"); }
}
static int same(const char *a, const char *b, u64 n) {
    for (u64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
#define LIT(s) (s), (sizeof(s) - 1)

static const char *sdata(const wstr *s) { return s->cap >= 16 ? s->u.ptr : s->u.buf; }
static int sane(const wstr *s) { return s->size <= s->cap && s->size <= MAX_CONTENT; }

#define NPOS (~0ull)
static u64 find(const char *h, u64 hn, u64 from, const char *n, u64 nn) {
    for (u64 i = from; nn <= hn && i <= hn - nn; i++)
        if (h[i] == n[0] && same(h + i, n, nn)) return i;
    return NPOS;
}
static u64 rfind(const char *h, u64 hn, const char *n, u64 nn) {
    if (nn > hn) return NPOS;
    for (u64 i = hn - nn + 1; i-- > 0;)
        if (h[i] == n[0] && same(h + i, n, nn)) return i;
    return NPOS;
}

// 往定长缓冲区里追加；放不下就置 bad
typedef struct { char *p; u64 n, cap; int bad; } out_t;
static void put(out_t *o, const char *s, u64 n) {
    if (o->bad || n > o->cap - o->n) { o->bad = 1; return; }
    copy(o->p + o->n, s, n);
    o->n += n;
}
static void put_u32(out_t *o, u32 v) {
    char b[10];
    int i = 10;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(o, b + i, (u64)(10 - i));
}

// ------------------------------------------------------------ 文本处理

// 严格解码一个 UTF-8 字符；非法（含超长编码、代理项、越界）返回 0
static u32 decode(const char *s, u64 n, u64 i, u32 *cp) {
    u8 c = (u8)s[i];
    u32 k = c < 0x80 ? 1 : c < 0xC2 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF5 ? 4 : 0;
    if (k == 0 || i + k > n) return 0;
    u32 v = k == 1 ? c : (u32)c & (0x7Fu >> k);
    for (u32 j = 1; j < k; j++) {
        u8 x = (u8)s[i + j];
        if ((x & 0xC0) != 0x80) return 0;
        v = v << 6 | (x & 0x3Fu);
    }
    if ((k == 3 && v < 0x800) || (k == 4 && v < 0x10000) || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return k;
}

// 控制符、双向控制符、零宽字符（保留 U+200D，组合表情要用）
static int invisible(u32 c) {
    return (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x200B || c == 0x200C || c == 0x200E || c == 0x200F ||
           (c >= 0x2028 && c <= 0x202E) || (c >= 0x2060 && c <= 0x2069) || c == 0xFEFF;
}

static int alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int entity_at(const char *s, u64 n, u64 i) {
    u64 j = i + 1;
    while (j < n && j - i <= 10 && (alnum(s[j]) || s[j] == '#')) j++;
    return j > i + 1 && j < n && s[j] == ';';
}

// 单行化、去不可见字符、把会被当成标记的字符换成全角、截断。out 至少 TEXT_MAX 字节，返回长度
static u32 sanitize(const char *s, u64 n, char *out) {
    out_t o = {out, 0, TEXT_MAX, 0};
    u32 chars = 0, space = 0;
    for (u64 i = 0; i < n;) {
        u32 c = 0, k = decode(s, n, i, &c);
        if (k == 0) { i++; continue; }
        if (c <= 0x20) { space = o.n != 0; i++; continue; }
        if (invisible(c)) { i += k; continue; }
        if (chars >= MAX_CHARS) { put(&o, LIT("…")); break; }
        if (space) {
            put(&o, LIT(" "));
            space = 0;
            if (++chars >= MAX_CHARS) { put(&o, LIT("…")); break; }
        }
        if (c == '<') put(&o, LIT("＜"));
        else if (c == '>') put(&o, LIT("＞"));
        else if (c == '&' && entity_at(s, n, i)) put(&o, LIT("＆"));
        else put(&o, s + i, k);
        chars++;
        i += k;
    }
    return (u32)o.n;
}

static void put_cp(out_t *o, u32 c) {
    char b[4];
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return;
    if (c < 0x80) { b[0] = (char)c; put(o, b, 1); }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); put(o, b, 2); }
    else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63));
        put(o, b, 3);
    } else {
        b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63));
        b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63));
        put(o, b, 4);
    }
}

static u32 parse_num(const char *s, u64 n, u32 base) {
    u32 v = 0;
    for (u64 i = 0; i < n; i++) {
        char c = s[i];
        u32 d = c >= '0' && c <= '9' ? (u32)(c - '0') : c >= 'a' && c <= 'f' ? (u32)(c - 'a' + 10)
              : c >= 'A' && c <= 'F' ? (u32)(c - 'A' + 10) : 99;
        if (d >= base) break;
        if (v > 0x0FFFFFFF / base) return 0xFFFFFFFF;
        v = v * base + d;
    }
    return v;
}

// 反转义 s[0,n) 到 o
static void unescape(const char *s, u64 n, out_t *o) {
    for (u64 i = 0; i < n;) {
        if (s[i] == '&') {
            u64 e = i + 1;
            while (e < n && e - i <= 10 && s[e] != ';') e++;
            if (e < n && s[e] == ';') {
                const char *t = s + i + 1;
                u64 k = e - i - 1;
                int done = 1;
                if (k == 3 && same(t, "amp", 3)) put(o, "&", 1);
                else if (k == 2 && same(t, "lt", 2)) put(o, "<", 1);
                else if (k == 2 && same(t, "gt", 2)) put(o, ">", 1);
                else if (k == 4 && same(t, "quot", 4)) put(o, "\"", 1);
                else if (k == 4 && same(t, "apos", 4)) put(o, "'", 1);
                else if (k > 1 && t[0] == '#')
                    put_cp(o, t[1] == 'x' || t[1] == 'X' ? parse_num(t + 2, k - 2, 16) : parse_num(t + 1, k - 1, 10));
                else done = 0;
                if (done) { i = e + 1; continue; }
            }
        }
        put(o, s + i, 1);
        i++;
    }
}

// 找 <tag>…</tag> 的文本（支持 CDATA 与自闭合），从 from 开始找第一个。tag 不含尖括号，最长 16
typedef struct { const char *p; u64 n; int cdata; } slice;
static int xml_find(const char *x, u64 n, const char *tag, u64 tn, u64 from, slice *r) {
    char open[24], empty[24], close[24];
    copy(open + 1, tag, tn); open[0] = '<'; open[tn + 1] = '>';
    copy(empty, open, tn + 1); empty[tn + 1] = '/'; empty[tn + 2] = '>';
    close[0] = '<'; close[1] = '/'; copy(close + 2, tag, tn); close[tn + 2] = '>';
    u64 a = find(x, n, from, open, tn + 2), e = find(x, n, from, empty, tn + 3);
    r->cdata = 0;
    if (e != NPOS && (a == NPOS || e < a)) { r->p = x + e; r->n = 0; return 1; }
    if (a == NPOS) return 0;
    a += tn + 2;
    u64 s = a;
    while (s < n && (x[s] == ' ' || x[s] == '\t' || x[s] == '\r' || x[s] == '\n')) s++;
    u64 b;
    if (s + 9 <= n && same(x + s, "<![CDATA[", 9)) {
        r->cdata = 1;
        a = s + 9;
        b = find(x, n, a, LIT("]]>"));
    } else {
        b = find(x, n, a, close, tn + 3);
    }
    if (b == NPOS) return 0;
    r->p = x + a;
    r->n = b - a < RAW_MAX ? b - a : RAW_MAX;
    return 1;
}

static u64 parse_id(const char *s, u64 n) {
    u64 i = 0, v = 0;
    int neg = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (u64)(s[i] - '0');
    return neg ? 0 - v : v;
}

// 撤回通知里的 newmsgid；不是撤回通知返回 0
static u64 recalled_id(const wstr *xml) {
    if (!sane(xml)) return 0;
    const char *x = sdata(xml);
    u64 p = find(x, xml->size, 0, LIT("<revokemsg>"));
    slice r;
    if (p == NPOS || !xml_find(x, xml->size, LIT("newmsgid"), p, &r)) return 0;
    return parse_id(r.p, r.n);
}

// ------------------------------------------------------------ 消息摘要

// 定长二维数组：指针表会产生重定位，不能用
enum { N_MSG, N_IMG, N_VOICE, N_CARD, N_VIDEO, N_EMOJI, N_LOC, N_MUSIC, N_FILE, N_LIVE, N_RECORD, N_MINI, N_FINDER, N_LINK };
static const char NOUN[][32] = {
    "一条消息", "一张图片", "一条语音", "一张名片", "一个视频", "一个表情包", "一个位置", "一首音乐",
    "一个文件", "一个实时位置", "一份聊天记录", "一个小程序", "一个视频号内容", "一个链接",
};

typedef struct {
    u64 id;
    u32 time;      // 原消息的时间，0 表示不知道
    u32 recalled;  // 撤回的时间，0 表示还没撤回
    u16 next;
    u16 len;
    u8 noun;
    u8 used;
    char text[TEXT_MAX + 2];
} entry;

static NOINLINE void describe_title(const char *x, u64 n, u64 from, entry *e) {
    slice r;
    char raw[RAW_MAX];
    if (!xml_find(x, n, LIT("title"), from, &r)) return;
    if (r.cdata) { e->len = (u16)sanitize(r.p, r.n, e->text); return; }
    out_t o = {raw, 0, RAW_MAX, 0};
    unescape(r.p, r.n, &o);
    e->len = (u16)sanitize(raw, o.n, e->text);
}

// 填 noun / text / len；返回 0 表示这类消息不用记
static int describe(u32 type, u32 sub, const wstr *content, const wstr *sender, entry *e) {
    const char *x = sdata(content);
    u64 n = content->size;
    e->noun = N_MSG;
    e->len = 0;
    if (type == 1) {
        u64 k = sender->size;
        // 群聊正文形如 "发送者:\n正文"；只在前缀与消息自身的发送者一致时才去掉
        if (sane(sender) && k && n >= k + 2 && same(x, sdata(sender), k) && x[k] == ':' && x[k + 1] == '\n') {
            x += k + 2;
            n -= k + 2;
        }
        e->len = (u16)sanitize(x, n, e->text);
        return e->len != 0;
    }
    if (type == 3) { e->noun = N_IMG; return 1; }
    if (type == 34) { e->noun = N_VOICE; return 1; }
    if (type == 42 || type == 66) { e->noun = N_CARD; return 1; }
    if (type == 43 || type == 62) { e->noun = N_VIDEO; return 1; }
    if (type == 47) { e->noun = N_EMOJI; return 1; }
    if (type == 48) { e->noun = N_LOC; return 1; }
    if (type == 0 || type == 10000 || type == 10002 || (type & 0xFFFF) != 49) return 0;

    u64 from = find(x, n, 0, LIT("<appmsg"));
    if (from == NPOS) from = 0;
    u32 app = sub;
    slice r;
    // 微信终结消息时已把正确的子类型写进结构体，只有没写时才从正文里找
    if (app == 0 && xml_find(x, n, LIT("type"), from, &r)) app = parse_num(r.p, r.n, 10);
    describe_title(x, n, from, e);
    if (app == 0 && e->len == 0) return 0;

    int keep = 0;  // 是否保留标题
    if (app == 3 || app == 76 || app == 92) { e->noun = N_MUSIC; keep = 1; }
    else if (app == 6 || app == 74) { e->noun = N_FILE; keep = 1; }
    else if (app == 8) e->noun = N_EMOJI;
    else if (app == 17) e->noun = N_LIVE;
    else if (app == 19 || app == 40) e->noun = N_RECORD;
    else if (app == 33 || app == 36) { e->noun = N_MINI; keep = 1; }
    else if (app == 51 || app == 63) e->noun = N_FINDER;
    else if (app == 4 || app == 5) { e->noun = N_LINK; keep = 1; }
    else keep = 1;  // 57 引用回复的标题就是回复正文；其余未知类型也显示标题
    if (!keep) e->len = 0;
    return 1;
}

// 隔了多久撤回；不知道或不合理时什么都不写
static void put_elapsed(out_t *o, const entry *e) {
    if (e->time == 0 || e->recalled < e->time || e->recalled - e->time > MAX_AGE) return;
    u32 s = e->recalled - e->time;
    if (s == 0) { put(o, LIT("（发出 1 秒内）")); return; }
    put(o, LIT("（发出 "));
    if (s < 60) { put_u32(o, s); put(o, LIT(" 秒")); }
    else if (s < 3600) {
        put_u32(o, s / 60);
        if (s % 60) { put(o, LIT(" 分 ")); put_u32(o, s % 60); put(o, LIT(" 秒")); }
        else put(o, LIT(" 分钟"));
    } else if (s < 86400) {
        put_u32(o, s / 3600);
        put(o, LIT(" 小时"));
        if (s % 3600 / 60) { put(o, LIT(" ")); put_u32(o, s % 3600 / 60); put(o, LIT(" 分")); }
    } else {
        put_u32(o, s / 86400);
        put(o, LIT(" 天"));
        if (s % 86400 / 3600) { put(o, LIT(" ")); put_u32(o, s % 86400 / 3600); put(o, LIT(" 小时")); }
    }
    put(o, LIT("后）"));
}

static int starts(const char *s, u64 n, const char *p, u64 pn) { return n >= pn && same(s, p, pn); }

// 自己撤回的：不改文案，也不拦，让微信照常删除
static int own_recall(const char *s, u64 n) {
    return starts(s, n, LIT("你撤回")) || starts(s, n, LIT("您撤回")) || starts(s, n, LIT("你收回")) ||
           starts(s, n, LIT("You recalled"));
}

static u64 noun_len(u8 k) {
    u64 n = 0;
    while (n < sizeof NOUN[0] && NOUN[k][n]) n++;
    return n;
}

// 改写提示；不需要改或放不下时返回 0，否则返回新长度
static u64 compose(const char *t, u64 n, const entry *e, char *buf) {
    static const char ZH[] = "撤回了一条消息";
    out_t o = {buf, 0, OUT_MAX, 0};
    if (n == 0 || n > TIP_MAX || own_recall(t, n)) return 0;
    if (find(t, n, 0, LIT("撤回了一条消息：「")) != NPOS) return 0;  // 已经带内容
    u64 p = rfind(t, n, LIT(ZH));                                    // 昵称里可能也有这几个字，取最后一处
    u64 rest = 0;
    if (p != NPOS) {
        put(&o, t, p);
        put(&o, LIT("撤回了"));
        put(&o, NOUN[e->noun], noun_len(e->noun));
        rest = p + sizeof ZH - 1;
    } else {
        if (find(t, n, 0, LIT("：「")) != NPOS) return 0;
        put(&o, t, n);
        rest = n;
        if (e->len == 0) { put(&o, LIT("（")); put(&o, NOUN[e->noun], noun_len(e->noun)); put(&o, LIT("）")); }
    }
    if (e->len) { put(&o, LIT("：「")); put(&o, e->text, e->len); put(&o, LIT("」")); }
    put_elapsed(&o, e);
    put(&o, t + rest, n - rest);
    return o.bad ? 0 : o.n;
}

// ------------------------------------------------------------ 缓存

static entry g_ring[N];
static u16 g_head[N];  // 下标 + 1，0 表示空
static u32 g_pos;
static volatile int g_lock;

static void lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&g_lock, __ATOMIC_RELAXED)) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
}
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static u32 slot_of(u64 id) { return (u32)((id * 0x9E3779B97F4A7C15ull) >> 52); }  // N = 2^12

static entry *locate(u64 id) {  // 需持锁
    for (u32 i = g_head[slot_of(id)]; i; i = g_ring[i - 1].next)
        if (g_ring[i - 1].id == id) return &g_ring[i - 1];
    return 0;
}

static void fill(entry *d, const entry *s) {
    d->noun = s->noun;
    d->len = s->len;
    copy(d->text, s->text, s->len);
}

static void remember(const entry *s) {
    lock();
    entry *d = locate(s->id);
    if (d) {
        // 先到先得：引用消息里会再次出现同一个 ID；只有先到的那条什么内容都没有时才允许补上
        if (d->len == 0 && d->noun == N_MSG) fill(d, s);
        if (d->time == 0) d->time = s->time;
    } else {
        u32 k = g_pos;
        g_pos = (k + 1) % N;
        d = &g_ring[k];
        if (d->used) {  // 挤掉最旧的
            u16 *link = &g_head[slot_of(d->id)];
            while (*link && *link != k + 1) link = &g_ring[*link - 1].next;
            if (*link) *link = d->next;
        }
        d->id = s->id;
        d->time = s->time;
        d->recalled = 0;
        d->used = 1;
        fill(d, s);
        u16 *head = &g_head[slot_of(s->id)];
        d->next = *head;
        *head = (u16)(k + 1);
    }
    unlock();
}

// 取出条目；now 非 0 时记下撤回时间（只记第一次：通知的时间之后会被改成原消息的时间）
static int recall(u64 id, u32 now, entry *e) {
    lock();
    entry *d = locate(id);
    if (d) {
        if (d->recalled == 0) d->recalled = now;
        e->id = d->id;
        e->time = d->time;
        e->recalled = d->recalled;
        fill(e, d);
    }
    unlock();
    return d != 0;
}

// ------------------------------------------------------------ 两个挂钩

static NOINLINE void capture(u8 *msg) {
    entry e;
    const wstr *content = &FIELD(wstr, msg, MSG_CONTENT);
    e.id = FIELD(u64, msg, MSG_ID);
    if (e.id == 0 || !sane(content)) return;
    if (!describe(FIELD(u32, msg, MSG_TYPE), FIELD(u32, msg, MSG_SUBTYPE), content, &FIELD(wstr, msg, MSG_SENDER), &e)) return;
    e.time = FIELD(u32, msg, MSG_TIME);
    remember(&e);
}

// 微信会把撤回通知自己的时间和排序号抄给提示，于是提示落在撤回那一刻。
// 换成原消息那一秒的末尾，提示就紧跟在原消息下面。
static NOINLINE void reposition(u8 *msg) {
    entry e;
    u64 id = recalled_id(&FIELD(wstr, msg, MSG_CONTENT));
    // recalled 只有 on_parsed 改写过提示才会有值，自己撤回的不会
    if (id == 0 || !recall(id, 0, &e) || e.recalled == 0 || e.time == 0) return;
    u32 now = FIELD(u32, msg, MSG_TIME);
    if (now < e.time || now - e.time > MAX_AGE) return;
    FIELD(u32, msg, MSG_TIME) = e.time;
    FIELD(u64, msg, MSG_SORT) = (u64)e.time * 1000 + 999;
}

void on_finalized(u8 *msg) {
    if (FIELD(u32, msg, MSG_TYPE) == 10002) reposition(msg);
    else capture(msg);
}

static NOINLINE void rewrite(u8 *self, const u8 *msg, u64 id) {
    entry e;
    char buf[OUT_MAX];
    wstr *tip = &FIELD(wstr, self, SYS_TIP);
    if (!recall(id, FIELD(u32, msg, MSG_TIME), &e)) return;
    u64 n = compose(sdata(tip), tip->size, &e, buf);
    if (n == 0) return;
    if (n <= tip->cap) {
        char *p = tip->cap >= 16 ? tip->u.ptr : tip->u.buf;
        copy(p, buf, n);
        p[n] = 0;
        tip->size = n;
    } else {
        wx_realloc_assign(tip, n, 0, buf);
    }
}

// self：撤回对象；xml：Message 的正文字段（调用点固定传 msg + MSG_CONTENT）
void on_parsed(u8 *self, wstr *xml) {
    const wstr *kind = &FIELD(wstr, self, SYS_TYPE), *tip = &FIELD(wstr, self, SYS_TIP);
    if (kind->size != 9 || !same(sdata(kind), "revokemsg", 9) || !sane(tip)) return;
    if (own_recall(sdata(tip), tip->size)) return;
    u64 id = recalled_id(xml);
    if (id) rewrite(self, (const u8 *)xml - MSG_CONTENT, id);
    FIELD(u64, self, SYS_NEWMSGID) = 0;  // 找不到要删的消息，消息就留住了
}

// ------------------------------------------------------------ 桩

#ifndef WXAR_TEST
__asm__(
    ".text\n"
    ".globl thunk_fin\n"
    "thunk_fin:\n"                       // rsi = msg；被替换的指令 + 尾调用
    "  movb $1, 0x2c0(%rsi)\n"
    "  movq %rsi, %rcx\n"
    "  jmp on_finalized\n"
    ".globl thunk_parse\n"
    "thunk_parse:\n"                     // rsi = 撤回对象，rdi = msg + 0x180
    "  subq $0x28, %rsp\n"
    "  movq %rsi, %rcx\n"
    "  movq %rdi, %rdx\n"
    "  call on_parsed\n"
    "  addq $0x28, %rsp\n"
    "  movb $1, %al\n"
    "  cmpb $0, -0x20(%rbp)\n"           // 调用处接着用这个比较结果跳转
    "  ret\n");
#endif
