// 单元测试（不需要微信，任意平台）：
//   clang -std=c11 -DWXAR_TEST runtime/test_tip.c -o test_tip && ./test_tip
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tip.c"

static int fails, total;

// 模拟微信的 assign：超出容量时重新分配
void wx_realloc_assign(wstr *s, u64 n, u64 unused, const char *src) {
    (void)unused;
    if (n <= s->cap) { printf("FAIL 容量足够时不该走重新分配\n"); fails++; }
    char *p = malloc(n + 32);
    memcpy(p, src, n);
    p[n] = 0;
    if (s->cap >= 16) free(s->u.ptr);
    s->u.ptr = p;
    s->size = n;
    s->cap = n + 31;
}

static void eq(const char *name, const char *got, u64 n, const char *want) {
    total++;
    if (n != strlen(want) || memcmp(got, want, n)) {
        fails++;
        printf("FAIL %s\n  got : %.*s\n  want: %s\n", name, (int)n, got, want);
    }
}
static void eqs(const char *name, const char *got, const char *want) { eq(name, got, strlen(got), want); }
static void eqi(const char *name, u64 got, u64 want) {
    total++;
    if (got != want) { fails++; printf("FAIL %s: got %llu want %llu\n", name, got, want); }
}

static wstr str_n(const char *s, u64 n) {
    wstr w;
    memset(&w, 0, sizeof w);
    w.size = n;
    if (n < 16) { w.cap = 15; memcpy(w.u.buf, s, n); }
    else { w.cap = n; w.u.ptr = malloc(n + 1); memcpy(w.u.ptr, s, n); w.u.ptr[n] = 0; }
    return w;
}
static wstr str(const char *s) { return str_n(s, strlen(s)); }
static void drop(wstr *w) { if (w->cap >= 16) free(w->u.ptr); memset(w, 0, sizeof *w); w->cap = 15; }

static char g_out[OUT_MAX + 1];
static const char *tip_full(u32 type, u32 sub, const char *content, const char *sender, const char *orig, u32 t0, u32 t1) {
    entry e;
    memset(&e, 0, sizeof e);
    wstr c = str(content), s = str(sender);
    const char *r = "(不记录)";
    if (describe(type, sub, &c, &s, &e)) {
        e.time = t0;
        e.recalled = t1;
        u64 n = compose(orig, strlen(orig), &e, g_out);
        g_out[n] = 0;
        r = n ? g_out : "(不改)";
    }
    drop(&c);
    drop(&s);
    return r;
}
#define ZS "\"张三\" 撤回了"
static const char *tip(u32 type, const char *content) { return tip_full(type, 0, content, "", ZS "一条消息", 0, 0); }
static const char *tip2(u32 type, const char *content, const char *sender, const char *orig) {
    return tip_full(type, 0, content, sender, orig, 0, 0);
}
static char g_app[4096];
static const char *app(int t, const char *title) {
    snprintf(g_app, sizeof g_app, "<msg><appmsg appid=\"\" sdkver=\"0\"><title>%s</title><des></des><type>%d</type></appmsg></msg>", title, t);
    return g_app;
}
static const char *clean(const char *s, u64 n) {
    static char b[TEXT_MAX + 1];
    b[sanitize(s, n, b)] = 0;
    return b;
}
#define CLEAN(s) clean((s), sizeof(s) - 1)
static const char *dur(u32 s) {
    static char b[128];
    entry e;
    memset(&e, 0, sizeof e);
    e.time = 1700000000;
    e.recalled = 1700000000 + s;
    out_t o = {b, 0, sizeof b - 1, 0};
    put_elapsed(&o, &e);
    b[o.n] = 0;
    return b;
}
static u64 id_of(const char *xml) {
    wstr w = str(xml);
    u64 v = recalled_id(&w);
    drop(&w);
    return v;
}

// ------------------------------------------------------------ 挂钩层

static _Alignas(16) u8 g_msg[0x400], g_sys[0x400];

static void set_str(u8 *o, u64 off, const char *s) { drop(&FIELD(wstr, o, off)); FIELD(wstr, o, off) = str(s); }
static void message(u32 type, u64 id, u32 time, const char *content, const char *sender) {
    FIELD(u32, g_msg, MSG_TYPE) = type;
    FIELD(u32, g_msg, MSG_SUBTYPE) = 0;
    FIELD(u64, g_msg, MSG_ID) = id;
    FIELD(u32, g_msg, MSG_TIME) = time;
    FIELD(u64, g_msg, MSG_SORT) = (u64)time * 1000;
    set_str(g_msg, MSG_CONTENT, content);
    set_str(g_msg, MSG_SENDER, sender);
}
static void receive(u32 type, u64 id, u32 time, const char *content, const char *sender) {
    message(type, id, time, content, sender);
    on_finalized(g_msg);
}
// 与微信相同的顺序：先解析（on_parsed），再收尾（on_finalized）
static void parse(u64 id, const char *orig) {
    set_str(g_sys, SYS_TYPE, "revokemsg");
    set_str(g_sys, SYS_TIP, orig);
    FIELD(u64, g_sys, SYS_NEWMSGID) = id;
    on_parsed(g_sys, &FIELD(wstr, g_msg, MSG_CONTENT));
}
static void notice(u64 id, u32 time, const char *orig) {
    char x[512];
    snprintf(x, sizeof x, "<sysmsg type=\"revokemsg\"><revokemsg><session>a</session><msgid>1</msgid><newmsgid>%llu</newmsgid>"
             "<replacemsg><![CDATA[%s]]></replacemsg></revokemsg></sysmsg>", id, orig);
    message(10002, id + 1, time, x, "");
    parse(id, orig);
    on_finalized(g_msg);
}
static void tip_is(const char *name, const char *want) {
    const wstr *t = &FIELD(wstr, g_sys, SYS_TIP);
    eq(name, sdata(t), t->size, want);
    total++;
    if (t->size < t->cap && sdata(t)[t->size] != 0) { fails++; printf("FAIL %s：没有结尾 0\n", name); }
}

int main(void) {
    for (int i = 0; i < 2; i++) ((void)i);
    FIELD(wstr, g_msg, MSG_CONTENT).cap = FIELD(wstr, g_msg, MSG_SENDER).cap = 15;
    FIELD(wstr, g_sys, SYS_TYPE).cap = FIELD(wstr, g_sys, SYS_TIP).cap = 15;

    // 文案，与 macOS 版的用例一致
    eqs("文字", tip(1, "1"), ZS "一条消息：「1」");
    eqs("对方", tip2(1, "你好", "", "对方撤回了一条消息"), "对方撤回了一条消息：「你好」");
    eqs("群聊前缀", tip2(1, "wxid_abc:\n晚上开会", "wxid_abc", ZS "一条消息"), ZS "一条消息：「晚上开会」");
    eqs("私聊同形文字不误删", tip2(1, "abc:\nhello", "wxid_x", ZS "一条消息"), ZS "一条消息：「abc: hello」");
    eqs("换行合并", tip(1, "a\r\n\n  b\t c  "), ZS "一条消息：「a b c」");
    eqs("标记转全角", tip(1, "<a href=x>点</a>"), ZS "一条消息：「＜a href=x＞点＜/a＞」");
    eqs("实体转全角", tip(1, "&lt;b A&B"), ZS "一条消息：「＆lt;b A&B」");
    eqs("表情字符", tip(1, "[微笑]😀"), ZS "一条消息：「[微笑]😀」");
    eqs("空文字不记录", tip(1, " \n "), "(不记录)");
    eqs("图片", tip(3, "<msg/>"), ZS "一张图片");
    eqs("语音", tip(34, "<msg/>"), ZS "一条语音");
    eqs("名片", tip(42, "<msg/>"), ZS "一张名片");
    eqs("视频", tip(43, "<msg/>"), ZS "一个视频");
    eqs("表情包", tip(47, "<msg/>"), ZS "一个表情包");
    eqs("位置", tip(48, "<msg/>"), ZS "一个位置");
    eqs("文件", tip(49, app(6, "报价单 v2.xlsx")), ZS "一个文件：「报价单 v2.xlsx」");
    eqs("链接转义", tip(49, app(5, "A &amp; B &lt;新&gt;")), ZS "一个链接：「A & B ＜新＞」");
    eqs("CDATA", tip(49, app(5, "<![CDATA[标题]]>")), ZS "一个链接：「标题」");
    eqs("CDATA 含结束标签", tip(49, app(5, "<![CDATA[a</title>b]]>")), ZS "一个链接：「a＜/title＞b」");
    eqs("引用回复", tip(49, app(57, "好的")), ZS "一条消息：「好的」");
    eqs("聊天记录", tip(49, app(19, "群聊的聊天记录")), ZS "一份聊天记录");
    eqs("小程序", tip(49, app(33, "某小程序")), ZS "一个小程序：「某小程序」");
    eqs("音乐", tip(49, app(3, "某首歌")), ZS "一首音乐：「某首歌」");
    eqs("视频号", tip(49, app(51, "x")), ZS "一个视频号内容");
    eqs("动画表情", tip(1048625, app(8, "")), ZS "一个表情包");
    eqs("自闭合标题", tip(49, "<appmsg><title/><type>5</type><x><title>里层</title></x></appmsg>"), ZS "一个链接");
    eqs("空 appmsg 不记录", tip(49, ""), "(不记录)");
    eqs("子类型以结构体为准", tip_full(49, 5, "<appmsg><title><![CDATA[x<type>6</type>]]></title></appmsg>", "", ZS "一条消息", 0, 0), ZS "一个链接：「x＜type＞6＜/type＞」");
    eqs("系统消息", tip(10000, "x"), "(不记录)");
    eqs("撤回通知", tip(10002, "x"), "(不记录)");
    eqs("自己撤回", tip2(1, "1", "", "你撤回了一条消息"), "(不改)");
    eqs("英文自己撤回", tip2(1, "1", "", "You recalled a message"), "(不改)");
    eqs("不重复追加", tip2(1, "1", "", "对方撤回了一条消息：「1」"), "(不改)");
    eqs("昵称含同样的字", tip2(1, "1", "", "\"撤回了一条消息\" 撤回了一条消息"), "\"撤回了一条消息\" 撤回了一条消息：「1」");
    eqs("其他语言", tip2(1, "hi", "", "\"Tom\" recalled a message"), "\"Tom\" recalled a message：「hi」");
    eqs("其他语言图片", tip2(3, "", "", "\"Tom\" recalled a message"), "\"Tom\" recalled a message（一张图片）");
    eqs("文字带间隔", tip_full(1, 0, "2", "", "\"星\" 撤回了一条消息", 100, 105), "\"星\" 撤回了一条消息：「2」（发出 5 秒后）");
    eqs("图片带间隔", tip_full(3, 0, "", "", "对方撤回了一条消息", 100, 105), "对方撤回了一张图片（发出 5 秒后）");
    eqs("其他语言带间隔", tip_full(3, 0, "", "", "\"Tom\" recalled a message", 100, 105), "\"Tom\" recalled a message（一张图片）（发出 5 秒后）");

    char big[1200], want[700];
    u64 k = 0;
    for (int i = 0; i < 300; i++) { memcpy(big + k, "字", 3); k += 3; }
    k = 0;
    for (int i = 0; i < 100; i++) { memcpy(want + k, "字", 3); k += 3; }
    want[k] = 0;
    eqs("正好 100 字", clean(big, 300), want);
    strcat(want, "…");
    eqs("截断", clean(big, 900), want);
    k = 0;
    for (int i = 0; i < 300; i++) { memcpy(big + k, "😀", 4); k += 4; }
    eqi("四字节字符截断长度", strlen(clean(big, 1200)), 403);
    eqs("坏 UTF-8", CLEAN("a\xff\xe4\xbd" "b"), "ab");
    eqs("超长编码/代理项/越界", CLEAN("a\xE0\x80\xBC\xED\xA0\x80\xF4\x90\x80\x80" "b"), "ab");
    eqs("双向控制符", CLEAN("a\xE2\x80\xAE" "b"), "ab");
    eqs("零宽字符", CLEAN("a\xE2\x80\x8B\xE2\x80\x8F\xEF\xBB\xBF" "b"), "ab");
    eqs("末尾半个字符", CLEAN("a\xE5\xAD"), "a");

    char ub[128];
    out_t uo = {ub, 0, sizeof ub, 0};
    unescape(LIT("&quot;&apos;&#20320;&#x597D;&unknown;&amp"), &uo);
    eq("实体", ub, uo.n, "\"'你好&unknown;&amp");

    eqs("0 秒", dur(0), "（发出 1 秒内）");
    eqs("59 秒", dur(59), "（发出 59 秒后）");
    eqs("1 分钟", dur(60), "（发出 1 分钟后）");
    eqs("1 分 30 秒", dur(90), "（发出 1 分 30 秒后）");
    eqs("59 分 59 秒", dur(3599), "（发出 59 分 59 秒后）");
    eqs("1 小时", dur(3600), "（发出 1 小时后）");
    eqs("2 小时 5 分", dur(7500), "（发出 2 小时 5 分后）");
    eqs("1 天 3 小时", dur(97200), "（发出 1 天 3 小时后）");
    eqs("7 天", dur(7 * 86400), "（发出 7 天后）");
    eqs("超过 7 天", dur(7 * 86400 + 1), "");

    eqi("大 ID", id_of("<sysmsg><revokemsg><newmsgid>9223372036854775999</newmsgid></revokemsg></sysmsg>"), 9223372036854775999ull);
    eqi("负 ID", id_of("<revokemsg><newmsgid>-1</newmsgid></revokemsg>"), ~0ull);
    eqi("没有 revokemsg 标签", id_of("<sysmsg type=\"foo\"><newmsgid>5</newmsgid><![CDATA[revokemsg]]></sysmsg>"), 0);
    eqi("没有 newmsgid", id_of("<revokemsg><content>x</content></revokemsg>"), 0);
    eqi("没有结束标签", id_of("<revokemsg><newmsgid>5"), 0);

    // 挂钩层
    receive(1, 42, 1700000000, "wxid_a:\n晚上开会，这里要够长才会走堆分配", "wxid_a");
    notice(42, 1700000075, ZS "一条消息");
    tip_is("提示改写", ZS "一条消息：「晚上开会，这里要够长才会走堆分配」（发出 1 分 15 秒后）");
    eqi("newmsgid 清零", FIELD(u64, g_sys, SYS_NEWMSGID), 0);
    eqi("时间换成原消息的", FIELD(u32, g_msg, MSG_TIME), 1700000000);
    eqi("排序号", FIELD(u64, g_msg, MSG_SORT), 1700000000999ull);
    parse(42, ZS "一条消息");
    on_finalized(g_msg);
    tip_is("重复处理间隔不变", ZS "一条消息：「晚上开会，这里要够长才会走堆分配」（发出 1 分 15 秒后）");
    eqi("重复处理位置不变", FIELD(u64, g_msg, MSG_SORT), 1700000000999ull);

    receive(3, 44, 1700000000, "<msg/>", "");
    notice(44, 1700000003, "对方撤回了一条消息");
    tip_is("图片", "对方撤回了一张图片（发出 3 秒后）");

    notice(999999, 1700000003, ZS "一条消息");
    tip_is("没见过的消息不改文案", ZS "一条消息");
    eqi("没见过的消息也留住", FIELD(u64, g_sys, SYS_NEWMSGID), 0);
    eqi("没见过的消息不挪", FIELD(u32, g_msg, MSG_TIME), 1700000003);

    receive(1, 46, 1700000000, "我发的", "");
    notice(46, 1700000009, "你撤回了一条消息");
    tip_is("自己撤回不改文案", "你撤回了一条消息");
    eqi("自己撤回不拦", FIELD(u64, g_sys, SYS_NEWMSGID), 46);
    eqi("自己撤回不挪", FIELD(u32, g_msg, MSG_TIME), 1700000009);

    receive(1, 48, 0, "没有时间", "");
    notice(48, 1700000009, ZS "一条消息");
    tip_is("不知道原时间：有内容没间隔", ZS "一条消息：「没有时间」");
    eqi("不知道原时间不挪", FIELD(u32, g_msg, MSG_TIME), 1700000009);

    message(10002, 60, 1700000009, "<sysmsg type=\"foo\"><newmsgid>42</newmsgid></sysmsg>", "");
    on_finalized(g_msg);
    eqi("别的系统消息不挪", FIELD(u32, g_msg, MSG_TIME), 1700000009);
    set_str(g_sys, SYS_TYPE, "otherkind");
    set_str(g_sys, SYS_TIP, "x");
    FIELD(u64, g_sys, SYS_NEWMSGID) = 7;
    on_parsed(g_sys, &FIELD(wstr, g_msg, MSG_CONTENT));
    eqi("不是撤回对象不动", FIELD(u64, g_sys, SYS_NEWMSGID), 7);

    receive(1, 0, 1, "没有 ID", "");
    entry e;
    eqi("没有 ID 不缓存", (u64)recall(0, 0, &e), 0);
    receive(1, 70, 5, "真", "");
    receive(1, 70, 6, "假", "");
    recall(70, 0, &e);
    eq("先到先得", e.text, e.len, "真");
    receive(49, 71, 0, "<appmsg><type>57</type><title/></appmsg>", "");
    receive(1, 71, 9, "补上", "");
    recall(71, 0, &e);
    eq("空条目可补上", e.text, e.len, "补上");
    eqi("时间可补上", e.time, 9);

    for (u64 i = 0; i < N; i++) { message(1, 1000000 + i, 1, "x", ""); on_finalized(g_msg); }
    eqi("挤掉最旧的", (u64)recall(42, 0, &e), 0);
    u64 alive = 0;
    for (u64 i = 0; i < N; i++) alive += (u64)recall(1000000 + i, 0, &e);
    eqi("恰好保留上限", alive, N);
    for (u64 i = 0; i < 5 * N; i++) { message(1, 77 + i * 0x100000001ull, 1, "y", ""); on_finalized(g_msg); }
    alive = 0;
    for (u64 i = 0; i < 5 * N; i++) alive += (u64)recall(77 + i * 0x100000001ull, 0, &e);
    eqi("反复淘汰后链表完好", alive, N);

    // 异常的字符串对象：长度超过容量时不碰
    message(1, 80, 1, "x", "");
    FIELD(wstr, g_msg, MSG_CONTENT).size = 99;
    on_finalized(g_msg);
    eqi("坏字符串不缓存", (u64)recall(80, 0, &e), 0);
    FIELD(wstr, g_msg, MSG_CONTENT).size = 1;

    // 锁一直被占：放弃，不卡死，消息仍留住
    receive(1, 84, 1700000000, "x", "");
    g_lock = 1;
    receive(1, 86, 1700000000, "x", "");
    notice(84, 1700000009, ZS "一条消息");
    tip_is("拿不到锁不改文案", ZS "一条消息");
    eqi("拿不到锁也留住", FIELD(u64, g_sys, SYS_NEWMSGID), 0);
    g_lock = 0;
    eqi("拿不到锁不缓存", (u64)recall(86, 0, &e), 0);

    // 超长昵称：放不下就不改，但消息照样留住
    char longtip[900];
    memset(longtip, 'A', 600);
    strcpy(longtip + 600, "撤回了一条消息");
    receive(1, 82, 1700000000, "x", "");
    message(10002, 83, 1700000001, "<revokemsg><newmsgid>82</newmsgid></revokemsg>", "");
    parse(82, longtip);
    tip_is("超长提示不改", longtip);
    eqi("超长提示也留住", FIELD(u64, g_sys, SYS_NEWMSGID), 0);

    if (fails) printf("%d/%d 项失败\n", fails, total);
    else printf("全部通过（%d 项）\n", total);
    return fails != 0;
}
