// 单元测试（文案 + 挂钩层，不需要微信）：
//   clang++ -std=c++17 -DWXAR_NO_INIT runtime/test_tip.cpp -o /tmp/t && /tmp/t
#include "wxar_tip.cpp"

#include <cstdio>
#include <ctime>
#include <new>
#include <stdexcept>

using namespace wxar;

static int fails = 0;
static void eq(const char* name, const std::string& got, const std::string& want) {
    if (got != want) {
        fails++;
        printf("FAIL %s\n  got : %s\n  want: %s\n", name, got.c_str(), want.c_str());
    }
}

static std::string tip(uint32_t type, const std::string& content, const std::string& sender = "",
                       const std::string& orig = "\"张三\" 撤回了一条消息") {
    Entry e;
    if (!describe(type, 0, content, sender, e)) return "(不记录)";
    std::string out;
    return compose(orig, e, out) ? out : "(不改)";
}

static std::string app(int t, const std::string& title) {
    return "<msg><appmsg appid=\"\" sdkver=\"0\"><title>" + title + "</title><des></des><type>" +
           std::to_string(t) + "</type></appmsg></msg>";
}

// ------------------------------------------------------------ 挂钩层

alignas(16) static uint8_t g_msg[0x400], g_sys[0x400];
static std::string& str(uint8_t* b, size_t o) { return *reinterpret_cast<std::string*>(b + o); }

static uint64_t fake_fin(void* m, uint32_t) {
    static_cast<uint8_t*>(m)[kMsgFinalized] = 1;
    return 0xdead;
}
static uint64_t fake_parse(void* s, const std::string*, bool*) {
    str(static_cast<uint8_t*>(s), kSysType) = "revokemsg";
    str(static_cast<uint8_t*>(s), kSysReplace) = "\"张三\" 撤回了一条消息";
    return 0xffffffff00000001ull;
}
static uint64_t fake_parse_fail(void* s, const std::string* x, bool* f) {
    return fake_parse(s, x, f) & ~0xffffffffull;
}
static uint64_t throw_fin(void*, uint32_t) { throw std::runtime_error("f"); }
static uint64_t throw_parse(void*, const std::string*, bool*) { throw std::runtime_error("p"); }

static void hooks() {
    for (size_t o : {kMsgSender, kMsgContent}) new (&str(g_msg, o)) std::string;
    for (size_t o : {kSysType, kSysReplace}) new (&str(g_sys, o)) std::string;
    auto fin = [](FinalizeFn f, uint32_t force) {
        g_base = reinterpret_cast<uintptr_t>(f) - kFinalizeOrig;
        return hook_finalize(g_msg, force);
    };
    auto par = [](ParseFn f, const std::string& x) {
        g_base = reinterpret_cast<uintptr_t>(f) - kParseOrig;
        bool flag = false;
        return hook_parse(g_sys, &x, &flag);
    };
    auto set_id = [](uint64_t id) { memcpy(g_msg + kMsgServerId, &id, 8); };
    auto yes = [](bool b) { return std::string(b ? "是" : "否"); };

    uint32_t type = 1;
    memcpy(g_msg + kMsgType, &type, 4);
    str(g_msg, kMsgSender) = "wxid_a";
    str(g_msg, kMsgContent) = "wxid_a:\n晚上开会，这里要够长才会走堆分配";
    set_id(42);
    Entry e;
    eq("终结器返回值透传", std::to_string(fin(fake_fin, 1)), std::to_string(0xdead));
    eq("已缓存", yes(lookup(42, e)), "是");
    set_id(43);
    fin(fake_fin, 0);
    eq("已终结且非强制则跳过", yes(lookup(43, e)), "否");
    fin(fake_fin, 1);
    eq("强制则缓存", yes(lookup(43, e)), "是");

    const std::string x =
        "<sysmsg type=\"revokemsg\"><revokemsg><newmsgid>42</newmsgid></revokemsg></sysmsg>";
    eq("解析返回值透传", std::to_string(par(fake_parse, x)), std::to_string(0xffffffff00000001ull));
    eq("提示已改写", str(g_sys, kSysReplace),
       "\"张三\" 撤回了一条消息：「晚上开会，这里要够长才会走堆分配」");
    par(fake_parse_fail, x);
    eq("解析失败不改写", str(g_sys, kSysReplace), "\"张三\" 撤回了一条消息");
    // 提示跟在原消息下面
    auto set32 = [](size_t o, uint32_t v) { memcpy(g_msg + o, &v, 4); };
    auto set64 = [](size_t o, uint64_t v) { memcpy(g_msg + o, &v, 8); };
    auto place = [&](uint64_t orig, uint32_t sent, uint32_t recalled, uint32_t ty = 10002) {
        set32(kMsgType, 1);
        set_id(orig);
        set32(kMsgTime, sent);
        str(g_msg, kMsgContent) = "x";
        fin(fake_fin, 1);
        set32(kMsgType, ty);
        set_id(orig + 1);
        set32(kMsgTime, recalled);
        set64(kMsgSortSeq, (uint64_t)recalled * 1000);
        str(g_msg, kMsgContent) = "<sysmsg type=\"revokemsg\"><revokemsg><newmsgid>" +
                                  std::to_string(orig) + "</newmsgid></revokemsg></sysmsg>";
        fin(fake_fin, 1);
        return std::to_string(field<uint32_t>(g_msg, kMsgTime)) + "/" +
               std::to_string(field<uint64_t>(g_msg, kMsgSortSeq));
    };
    eq("挪到原消息那一秒末尾", place(800001, 1700000000, 1700000090), "1700000000/1700000000999");
    eq("重复终结结果不变", (fin(fake_fin, 1), std::to_string(field<uint64_t>(g_msg, kMsgSortSeq))),
       "1700000000999");
    eq("不知道原时间不挪", place(800003, 0, 1700000090), "1700000090/1700000090000");
    eq("时间倒挂不挪", place(800005, 1700000100, 1700000090), "1700000090/1700000090000");
    eq("隔太久不挪", place(800007, 1600000000, 1700000090), "1700000090/1700000090000");
    str(g_msg, kMsgContent) = "x";
    set32(kMsgType, 10002);
    set32(kMsgTime, 1700000090);
    str(g_msg, kMsgContent) = "<sysmsg type=\"foo\"><newmsgid>800001</newmsgid><![CDATA[revokemsg]]></sysmsg>";
    fin(fake_fin, 1);
    eq("别的系统消息不挪", std::to_string(field<uint32_t>(g_msg, kMsgTime)), "1700000090");
    eq("非撤回通知不挪", place(800009, 1700000000, 1700000090, 1), "1700000090/1700000090000");
    Entry late;
    late.time = 5;
    remember(800003, std::move(late));
    lookup(800003, e);
    eq("原先不知道的时间可补上", std::to_string(e.time) + e.text, "5x");

    int thrown = 0;
    try { fin(throw_fin, 1); } catch (const std::runtime_error&) { thrown++; }
    try { par(throw_parse, x); } catch (const std::runtime_error&) { thrown++; }
    eq("原函数异常照常穿透", std::to_string(thrown), "2");

    auto* m = static_cast<uint8_t*>(calloc(1, 0x9e28000));
    auto* mh = reinterpret_cast<mach_header_64*>(m);
    mh->magic = MH_MAGIC_64;
    mh->cputype = CPU_TYPE_ARM64;
    mh->ncmds = 1;
    auto* u = reinterpret_cast<uuid_command*>(mh + 1);
    u->cmd = LC_UUID;
    u->cmdsize = sizeof(*u);
    memcpy(u->uuid, kUUID, 16);
    auto load = [&] { g_base = 0; on_image(reinterpret_cast<mach_header*>(m), 0); return g_base != 0; };
    eq("未打补丁不启用", yes(load()), "否");
    uint32_t c1 = enc_branch(0x94000000, kParseCall, kStubParse);
    uint32_t c2 = enc_branch(0x14000000, kFinalize, kStubFinalize);
    memcpy(m + kParseCall, &c1, 4);
    memcpy(m + kFinalize, &c2, 4);
    u->uuid[0] ^= 1;
    eq("UUID 不符不启用", yes(load()), "否");
    u->uuid[0] ^= 1;
    eq("已打补丁则启用", yes(load()), "是");
    free(m);
}

int main() {
    const std::string p = "\"张三\" 撤回了";
    eq("文字", tip(1, "1"), p + "一条消息：「1」");
    eq("对方", tip(1, "你好", "", "对方撤回了一条消息"), "对方撤回了一条消息：「你好」");
    eq("群聊前缀", tip(1, "wxid_abc:\n晚上开会", "wxid_abc"), p + "一条消息：「晚上开会」");
    eq("私聊同形文字不误删", tip(1, "abc:\nhello", "wxid_x"), p + "一条消息：「abc: hello」");
    eq("换行合并", tip(1, "a\r\n\n  b\t c  "), p + "一条消息：「a b c」");
    eq("标记转全角", tip(1, "<a href=x>点</a>"), p + "一条消息：「＜a href=x＞点＜/a＞」");
    eq("实体转全角", tip(1, "&lt;b A&B"), p + "一条消息：「＆lt;b A&B」");
    eq("表情字符", tip(1, "[微笑]😀"), p + "一条消息：「[微笑]😀」");
    eq("空文字不记录", tip(1, " \n "), "(不记录)");
    eq("图片", tip(3, "<msg><img/></msg>"), p + "一张图片");
    eq("语音", tip(34, "<msg/>"), p + "一条语音");
    eq("视频", tip(43, "<msg/>"), p + "一个视频");
    eq("表情包", tip(47, "<msg><emoji/></msg>"), p + "一个表情包");
    eq("文件", tip(49, app(6, "报价单 v2.xlsx")), p + "一个文件：「报价单 v2.xlsx」");
    eq("链接转义", tip(49, app(5, "A &amp; B &lt;新&gt;")), p + "一个链接：「A & B ＜新＞」");
    eq("CDATA", tip(49, app(5, "<![CDATA[标题]]>")), p + "一个链接：「标题」");
    eq("引用回复", tip(49, app(57, "好的")), p + "一条消息：「好的」");
    eq("聊天记录", tip(49, app(19, "群聊的聊天记录")), p + "一份聊天记录");
    eq("动画表情", tip(1048625, app(8, "")), p + "一个表情包");
    eq("系统消息", tip(10000, "x"), "(不记录)");
    eq("撤回通知", tip(10002, "<sysmsg/>"), "(不记录)");
    eq("自己撤回", tip(1, "1", "", "你撤回了一条消息"), "(不改)");
    eq("不重复追加", tip(1, "1", "", "对方撤回了一条消息：「1」"), "(不改)");
    eq("原文同字也不重复", tip(1, "撤回了一条消息", "", "对方撤回了一条消息：「撤回了一条消息」"), "(不改)");
    eq("其他语言", tip(1, "hi", "", "\"Tom\" recalled a message"), "\"Tom\" recalled a message：「hi」");
    eq("其他语言图片", tip(3, "", "", "\"Tom\" recalled a message"), "\"Tom\" recalled a message（一张图片）");

    std::string big;
    for (int i = 0; i < 300; i++) big += "字";
    std::string cut;
    for (int i = 0; i < 100; i++) cut += "字";
    eq("截断", tip(1, big), p + "一条消息：「" + cut + "…」");
    eq("正好 100 字不加省略号", sanitize(cut), cut);
    eq("坏 UTF-8", sanitize(std::string("a\xff\xe4\xbd") + "b"), "ab");
    eq("双向控制符", sanitize("a\xE2\x80\xAE" "b"), "ab");

    std::string v;
    xml_text("<sysmsg type=\"revokemsg\"><revokemsg><msgid>1</msgid><newmsgid>9223372036854775999"
             "</newmsgid></revokemsg></sysmsg>", "newmsgid", 0, v);
    eq("大 ID", std::to_string(parse_id(v)), "9223372036854775999");
    eq("负 ID", std::to_string(parse_id("-1")), "18446744073709551615");

    Entry a, b, c;
    a.text = "真";
    b.text = "假";
    remember(7, std::move(a));
    remember(7, std::move(b));
    lookup(7, c);
    eq("先到先得", c.text, "真");
    for (uint64_t i = 100; i < 100 + kMaxEntries; i++) remember(i, Entry{});
    eq("淘汰最旧", lookup(7, c) ? "在" : "不在", "不在");

    eq("恰好保留上限", std::to_string(table().size()), std::to_string(kMaxEntries));

    // 审查中发现的边界
    Entry e;
    eq("空 appmsg 不占位", describe(49, 0, "", "", e) ? "记" : "不记", "不记");
    e = Entry{};
    describe(49, 5, "<msg><appmsg><title><![CDATA[x<type>6</type>]]></title><type>5</type></appmsg></msg>", "", e);
    eq("子类型以结构体为准", e.noun, "一个链接");
    xml_text("<a><title/><b><title>里层</title></b></a>", "title", 0, v);
    eq("自闭合标题", v, "");
    xml_text("<title><![CDATA[a</title>b]]></title>", "title", 0, v);
    eq("CDATA 含结束标签", v, "a</title>b");
    eq("实体", xml_unescape("&quot;&apos;&#20320;&#x597D;&unknown;&amp"), "\"'你好&unknown;&amp");
    eq("昵称含同样的字", tip(1, "1", "", "\"撤回了一条消息\" 撤回了一条消息"),
       "\"撤回了一条消息\" 撤回了一条消息：「1」");
    eq("英文自己撤回", tip(1, "1", "", "You recalled a message"), "(不改)");
    eq("超长编码/代理项/越界", sanitize("a\xE0\x80\xBC\xED\xA0\x80\xF4\x90\x80\x80" "b"), "ab");
    eq("零宽字符", sanitize("a\xE2\x80\x8B\xE2\x80\x8F\xEF\xBB\xBF" "b"), "ab");
    Entry first, second, got;
    second.noun = "一个文件";
    second.text = "报价单.xlsx";
    remember(900001, std::move(first));
    remember(900001, std::move(second));
    lookup(900001, got);
    eq("空条目可补上", got.text, "报价单.xlsx");
    clock_t t0 = clock();
    describe(49, 5, "<appmsg><title>" + std::string(2 << 20, '&') + "</title></appmsg>", "", e);
    eq("恶意标题不卡", (clock() - t0) * 10 < CLOCKS_PER_SEC ? "快" : "慢", "快");

    hooks();

    printf(fails ? "%d 项失败\n" : "全部通过\n", fails);
    return fails != 0;
}
