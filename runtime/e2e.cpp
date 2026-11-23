// 端到端测试：加载打过补丁的 wechat.dylib，用微信自己的代码构造消息与撤回通知。
// 不登录、不联网；建议在断网禁写沙箱里跑：
//   APP=/path/to/打过补丁的/WeChat.app
//   clang++ -arch arm64 -std=c++17 runtime/e2e.cpp -o /tmp/e2e \
//     -Wl,-rpath,"$APP/Contents/Frameworks" -Wl,-rpath,"$APP/Contents/Frameworks/ld"
//   sandbox-exec -p '(version 1)(allow default)(deny network*)(deny file-write*)' \
//     /tmp/e2e "$APP/Contents/Resources/wechat.dylib"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static uintptr_t B;
static std::atomic<int> bad{0};

struct Msg {
    uint8_t* p = (uint8_t*)calloc(1, 0x400);
    Msg() { ((void* (*)(void*))(B + 0xa78fc0))(p); }
    std::string& s(size_t o) { return *(std::string*)(p + o); }
    void make(uint32_t type, uint64_t id, const std::string& content, const std::string& sender = "") {
        s(0x48) = sender;
        s(0x130) = content;
        *(uint64_t*)(p + 0xF8) = id;
        ((void (*)(void*, uint32_t))(B + 0x4612884))(p, type);
        ((uint64_t(*)(void*, uint32_t))(B + 0x4612e90))(p, 1);
    }
};

static std::string recall(uint64_t id, const std::string& tip) {
    Msg m;
    m.make(10002, id + 1, "<sysmsg type=\"revokemsg\"><revokemsg><session>x</session><msgid>1</msgid><newmsgid>" +
                              std::to_string(id) + "</newmsgid><replacemsg><![CDATA[" + tip +
                              "]]></replacemsg></revokemsg></sysmsg>");
    uint8_t* c = *(uint8_t**)(m.p + 0x220);
    if (!c) return "(无内容对象)";
    if (*(uint64_t*)(c + 0x1c8) != 0) { bad++; printf("FAIL newmsgid 未清零\n"); }
    return *(std::string*)(c + 0x1d0);
}

static void check(const char* name, const std::string& got, const std::string& want) {
    if (got != want) bad++;
    printf("%s %s: %s\n", got == want ? "ok  " : "FAIL", name, got.c_str());
}

int main(int, char** v) {
    bool off = getenv("EXPECT_OFF") != nullptr;  // 模块缺失时应退回普通提示
    if (!dlopen(v[1], RTLD_NOW | RTLD_LOCAL)) { printf("dlopen: %.200s\n", dlerror()); return 2; }
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        std::string n = _dyld_get_image_name(i);
        // Frameworks 里还有个同名小桩，要认 Resources 里的
        if (n.size() > 23 && n.compare(n.size() - 23, 23, "/Resources/wechat.dylib") == 0)
            B = (uintptr_t)_dyld_get_image_header(i);
    }
    bool on = *(uintptr_t*)(B + 0x9e27f00) && *(uintptr_t*)(B + 0x9e27f08);
    printf("挂钩%s\n", on ? "已启用" : "未启用");
    if (on == off) bad++;

    const std::string t = "\"张三\" 撤回了一条消息", p = "\"张三\" 撤回了";
    auto go = [&](const char* n, uint32_t type, uint64_t id, const std::string& c, const std::string& want,
                  const std::string& sender = "") {
        { Msg m; m.make(type, id, c, sender); }
        check(n, recall(id, t), off ? t : p + want);
    };
    auto app = [](int ty, const std::string& title) {
        return "<msg><appmsg appid=\"\" sdkver=\"0\"><title>" + title + "</title><type>" + std::to_string(ty) +
               "</type></appmsg></msg>";
    };
    go("文字", 1, 7000000000000000001ull, "1", "一条消息：「1」");
    go("长 ID", 1, 9223372036854775999ull, "晚上八点开会", "一条消息：「晚上八点开会」");
    go("群聊", 1, 7000000000000000021ull, "wxid_abc:\n你好", "一条消息：「你好」", "wxid_abc");
    go("表情包", 47, 7000000000000000003ull, "<msg><emoji md5=\"a\"></emoji></msg>", "一个表情包");
    go("图片", 3, 7000000000000000005ull, "<msg><img/></msg>", "一张图片");
    go("语音", 34, 7000000000000000007ull, "<msg><voicemsg/></msg>", "一条语音");
    go("视频", 43, 7000000000000000009ull, "<msg><videomsg/></msg>", "一个视频");
    go("文件", 49, 7000000000000000011ull, app(6, "报价单.xlsx"), "一个文件：「报价单.xlsx」");
    go("链接", 49, 7000000000000000013ull, app(5, "<![CDATA[x<type>6</type>]]>"), "一个链接：「x＜type＞6＜/type＞」");
    check("没见过的消息", recall(7000000000000000099ull, t), t);
    check("自己撤回", recall(7000000000000000001ull, "你撤回了一条消息"), "你撤回了一条消息");

    // 位置：撤回通知的时间与排序号应换成原消息那一秒的末尾
    {
        uint64_t id = 7000000000000000031ull;
        Msg o;
        *(uint32_t*)(o.p + 0x114) = 1700000000;
        *(uint64_t*)(o.p + 0x100) = 1700000000000ull;
        o.make(1, id, "位置");
        Msg r;
        *(uint32_t*)(r.p + 0x114) = 1700000090;
        *(uint64_t*)(r.p + 0x100) = 1700000090000ull;
        r.make(10002, id + 1, "<sysmsg type=\"revokemsg\"><revokemsg><session>x</session><msgid>1</msgid><newmsgid>" +
                                  std::to_string(id) + "</newmsgid><replacemsg><![CDATA[" + t +
                                  "]]></replacemsg></revokemsg></sysmsg>");
        uint8_t* c = *(uint8_t**)(r.p + 0x220);
        check("间隔", c ? *(std::string*)(c + 0x1d0) : "(无内容对象)", off ? t : t + "：「位置」（发出 1 分 30 秒后）");
        ((uint64_t(*)(void*, uint32_t))(B + 0x4612e90))(r.p, 1);  // 微信会重复终结，结果应不变
        check("间隔（重复终结）", c ? *(std::string*)(c + 0x1d0) : "(无内容对象)", off ? t : t + "：「位置」（发出 1 分 30 秒后）");
        check("位置", std::to_string(*(uint32_t*)(r.p + 0x114)) + "/" + std::to_string(*(uint64_t*)(r.p + 0x100)),
              off ? "1700000090/1700000090000" : "1700000000/1700000000999");
    }

    std::vector<std::thread> th;
    for (int k = 0; k < 8; k++)
        th.emplace_back([=] {
            for (int i = 0; i < 3000; i++) {
                uint64_t id = (uint64_t)(k + 1) * 1000000 + i * 2;
                std::string x = "x" + std::to_string(id);
                { Msg m; m.make(1, id, x); }
                std::string base = "对方撤回了一条消息";
                if (i % 3 == 0 && recall(id, base) != (off ? base : base + "：「" + x + "」")) bad++;
            }
        });
    for (auto& x : th) x.join();
    printf("并发 8x3000 完成\n");
    printf(bad ? "%d 项失败\n" : "全部通过\n", bad.load());
    fflush(stdout);
    _exit(bad != 0);
}
