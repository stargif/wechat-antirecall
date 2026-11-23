// wxar_tip —— 让微信的撤回提示显示具体内容（微信 4.2.0 / build 270416 / arm64）
//
// 由 wechat.dylib 通过 LC_LOAD_WEAK_DYLIB 加载。wechat.dylib 里有两处静态跳转，
// 经由 __TEXT 头部空白区的两段小桩，间接跳到本模块写入的两个函数指针：
//
//   SLOT_FINALIZE  Message 终结器：新消息成型后，记下「服务器 ID → 类型 + 摘要」
//   SLOT_PARSE     撤回解析函数：提示文案填好后，按 newmsgid 查表并改写提示
//
// 指针为 0（本模块没加载 / 构建号不符）时，小桩直接走原逻辑，等价于 keeptip。
//
// 隐私：缓存只在进程内存里，不写盘、不联网；日志只有计数和类型，没有聊天内容。

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/lock.h>
#include <os/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

namespace wxar {

// ------------------------------------------------------------ 构建号 270416

constexpr uint8_t kUUID[16] = {0xEB, 0x2B, 0xC5, 0x53, 0x17, 0xD1, 0x37, 0xDD,
                               0x97, 0x7E, 0x25, 0x6D, 0xCB, 0xDE, 0xFE, 0xFE};

constexpr uintptr_t kParseOrig = 0x4666c28;    // 撤回 XML 解析函数
constexpr uintptr_t kParseCall = 0x4666ba4;    // 它唯一的调用点（已改为 bl 桩 1）
constexpr uintptr_t kFinalize = 0x4612e90;     // Message 终结器入口（已改为 b 桩 2）
constexpr uintptr_t kStubParse = 0x16e00;      // 桩 1
constexpr uintptr_t kStubFinalize = 0x16e20;   // 桩 2
constexpr uintptr_t kFinalizeOrig = 0x16e30;   // 桩 2 里的「原始入口」
constexpr uintptr_t kSlotParse = 0x9e27f00;    // __DATA 末尾零填充空洞
constexpr uintptr_t kSlotFinalize = 0x9e27f08;

// Message 布局
constexpr size_t kMsgType = 0x0C;
constexpr size_t kMsgSubType = 0x10;
constexpr size_t kMsgSender = 0x48;    // std::string
constexpr size_t kMsgServerId = 0xF8;  // uint64
constexpr size_t kMsgContent = 0x130;  // std::string
constexpr size_t kMsgSortSeq = 0x100;  // uint64，聊天里的排序号 = 时间 * 1000 + 序号
constexpr size_t kMsgTime = 0x114;     // uint32，秒
constexpr size_t kMsgFinalized = 0x258;

// 撤回（sysmsg）对象布局
constexpr size_t kSysType = 0x1A8;     // std::string
constexpr size_t kSysReplace = 0x1D0;  // std::string

constexpr size_t kMaxEntries = 10000;
constexpr size_t kMaxChars = 100;
constexpr size_t kMaxContent = 4u << 20;
constexpr uint32_t kMaxRecallAge = 7 * 24 * 3600;  // 时间差超过它就不挪提示

// ------------------------------------------------------------ 文本处理

// 严格解码一个 UTF-8 字符；非法（含超长编码、代理项、越界）返回 0
static size_t decode(const std::string& s, size_t i, uint32_t& cp) {
    unsigned char c = (unsigned char)s[i];
    size_t n = c < 0x80 ? 1 : c < 0xC2 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF5 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    cp = n == 1 ? c : c & (0x7Fu >> n);
    for (size_t k = 1; k < n; k++) {
        unsigned char x = (unsigned char)s[i + k];
        if ((x & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (x & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

// 控制符、双向控制符、零宽字符（保留 U+200D，组合表情要用）
static bool invisible(uint32_t cp) {
    return (cp >= 0x7F && cp <= 0x9F) || cp == 0x061C || cp == 0x200B || cp == 0x200C ||
           cp == 0x200E || cp == 0x200F || (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2069) || cp == 0xFEFF;
}

static bool looks_like_entity(const std::string& s, size_t i) {
    size_t j = i + 1;
    while (j < s.size() && j - i <= 10 && (isalnum((unsigned char)s[j]) || s[j] == '#')) j++;
    return j > i + 1 && j < s.size() && s[j] == ';';
}

// 单行化、去控制字符、把会被当成标记的字符换成全角，并截断
std::string sanitize(const std::string& s, size_t max_chars = kMaxChars) {
    std::string out;
    size_t chars = 0;
    bool space = false;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = 0;
        size_t n = decode(s, i, c);
        if (n == 0) { i++; continue; }
        if (c <= 0x20) { space = !out.empty(); i++; continue; }
        if (invisible(c)) { i += n; continue; }
        if (chars >= max_chars) { out += "…"; return out; }
        if (space) {
            out += ' ';
            space = false;
            if (++chars >= max_chars) { out += "…"; return out; }
        }
        if (c == '<') out += "＜";
        else if (c == '>') out += "＞";
        else if (c == '&' && looks_like_entity(s, i)) out += "＆";
        else out.append(s, i, n);
        chars++;
        i += n;
    }
    return out;
}

static void append_cp(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 63));
        out += (char)(0x80 | (cp & 63));
    } else {
        out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 63));
        out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63));
    }
}

std::string xml_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t e = i + 1;
            while (e < s.size() && e - i <= 10 && s[e] != ';') e++;
            if (e < s.size() && s[e] == ';') {
                std::string n = s.substr(i + 1, e - i - 1);
                bool done = true;
                if (n == "amp") out += '&';
                else if (n == "lt") out += '<';
                else if (n == "gt") out += '>';
                else if (n == "quot") out += '"';
                else if (n == "apos") out += '\'';
                else if (n.size() > 1 && n[0] == '#')
                    append_cp(out, (n[1] == 'x' || n[1] == 'X')
                                       ? (uint32_t)strtoul(n.c_str() + 2, nullptr, 16)
                                       : (uint32_t)strtoul(n.c_str() + 1, nullptr, 10));
                else done = false;
                if (done) { i = e + 1; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// 取 <tag>…</tag> 的文本（支持 CDATA 与自闭合），从 from 开始找第一个，最多取 4 KB
bool xml_text(const std::string& xml, const char* tag, size_t from, std::string& out) {
    constexpr size_t kMaxRaw = 4096;
    const std::string open = std::string("<") + tag + ">";
    size_t a = xml.find(open, from);
    size_t empty = xml.find(std::string("<") + tag + "/>", from);
    if (empty != std::string::npos && (a == std::string::npos || empty < a)) {
        out.clear();
        return true;
    }
    if (a == std::string::npos) return false;
    a += open.size();
    size_t s = xml.find_first_not_of(" \t\r\n", a);
    bool cdata = s != std::string::npos && xml.compare(s, 9, "<![CDATA[") == 0;
    if (cdata) a = s + 9;
    size_t b = xml.find(cdata ? std::string("]]>") : std::string("</") + tag + ">", a);
    if (b == std::string::npos) return false;
    std::string v = xml.substr(a, std::min(b - a, kMaxRaw));
    out = cdata ? v : xml_unescape(v);
    return true;
}

uint64_t parse_id(const std::string& s) {
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == '-') return (uint64_t)strtoll(p, nullptr, 10);
    return strtoull(p, nullptr, 10);
}

// ------------------------------------------------------------ 消息摘要

struct Entry {
    const char* noun = "一条消息";  // 静态字符串
    std::string text;               // 有则显示为：「text」
    uint32_t time = 0;              // 原消息的时间，0 表示不知道
};

// 群聊正文形如 "发送者:\n正文"；只在前缀与消息自身的发送者一致时才去掉
std::string strip_sender(const std::string& content, const std::string& sender) {
    if (!sender.empty() && content.size() >= sender.size() + 2 &&
        content.compare(0, sender.size(), sender) == 0 &&
        content.compare(sender.size(), 2, ":\n") == 0)
        return content.substr(sender.size() + 2);
    return content;
}

// 返回 false 表示这类消息不需要记（系统消息、撤回通知等）
bool describe(uint32_t type, uint32_t subtype, const std::string& content,
              const std::string& sender, Entry& e) {
    switch (type) {
        case 1: {
            e.noun = "一条消息";
            e.text = sanitize(strip_sender(content, sender));
            return !e.text.empty();
        }
        case 3: e.noun = "一张图片"; return true;
        case 34: e.noun = "一条语音"; return true;
        case 42:
        case 66: e.noun = "一张名片"; return true;
        case 43:
        case 62: e.noun = "一个视频"; return true;
        case 47: e.noun = "一个表情包"; return true;
        case 48: e.noun = "一个位置"; return true;
        case 0:
        case 10000:
        case 10002: return false;
        default: break;
    }
    if ((type & 0xFFFF) != 49) return false;

    size_t at = content.find("<appmsg");
    size_t from = at == std::string::npos ? 0 : at;
    std::string v, title;
    uint32_t app = subtype;
    // 微信终结消息时已把正确的子类型写进结构体，只有没写时才从正文里找
    if (app == 0 && xml_text(content, "type", from, v)) app = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    if (xml_text(content, "title", from, v)) title = sanitize(v);
    if (app == 0 && title.empty()) return false;

    switch (app) {
        case 3:
        case 76:
        case 92: e.noun = "一首音乐"; e.text = title; return true;
        case 6:
        case 74: e.noun = "一个文件"; e.text = title; return true;
        case 8: e.noun = "一个表情包"; return true;
        case 17: e.noun = "一个实时位置"; return true;
        case 19:
        case 40: e.noun = "一份聊天记录"; return true;
        case 33:
        case 36: e.noun = "一个小程序"; e.text = title; return true;
        case 51:
        case 63: e.noun = "一个视频号内容"; return true;
        case 4:
        case 5: e.noun = "一个链接"; e.text = title; return true;
        case 57: e.noun = "一条消息"; e.text = title; return true;  // 引用回复，标题就是回复正文
        default: e.noun = "一条消息"; e.text = title; return true;
    }
}

// 把原提示改写成带内容的提示；不需要改时返回 false
bool compose(const std::string& orig, const Entry& e, std::string& out) {
    if (orig.empty()) return false;
    for (const char* own : {"你撤回", "您撤回", "你收回", "You recalled"})  // 自己撤回的不改
        if (orig.compare(0, strlen(own), own) == 0) return false;
    std::string tail = e.text.empty() ? "" : "：「" + e.text + "」";
    const std::string kZh = "撤回了一条消息";
    if (orig.find(kZh + "：「") != std::string::npos) return false;  // 已经带内容
    size_t p = orig.rfind(kZh);  // 昵称里可能也有这几个字，取最后一处
    if (p != std::string::npos) {
        std::string rest = orig.substr(p + kZh.size());
        out = orig.substr(0, p) + "撤回了" + e.noun + tail + rest;
        return true;
    }
    if (orig.find("：「") != std::string::npos) return false;
    out = orig + (e.text.empty() ? std::string("（") + e.noun + "）" : tail);
    return true;
}

// ------------------------------------------------------------ 缓存

static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;

static std::unordered_map<uint64_t, Entry>& table() {
    static auto* t = new std::unordered_map<uint64_t, Entry>();
    return *t;
}
static std::deque<uint64_t>& order() {
    static auto* d = new std::deque<uint64_t>();
    return *d;
}

void remember(uint64_t id, Entry&& e) {
    os_unfair_lock_lock(&g_lock);
    try {
        // 先到先得：引用消息里会再次出现同一个 ID，不让它覆盖真正收到的那条；
        // 只有先到的那条什么内容都没有时才允许补上
        auto it = table().find(id);
        if (it != table().end()) {
            uint32_t time = it->second.time ? it->second.time : e.time;
            if (it->second.text.empty() && strcmp(it->second.noun, "一条消息") == 0)
                it->second = std::move(e);
            it->second.time = time;
        } else {
            order().push_back(id);
            table().emplace(id, std::move(e));
            if (order().size() > kMaxEntries) {
                table().erase(order().front());
                order().pop_front();
            }
        }
    } catch (...) {
    }
    os_unfair_lock_unlock(&g_lock);
}

bool lookup(uint64_t id, Entry& e) {
    bool hit = false;
    os_unfair_lock_lock(&g_lock);
    try {
        auto it = table().find(id);
        if (it != table().end()) { e = it->second; hit = true; }
    } catch (...) {
    }
    os_unfair_lock_unlock(&g_lock);
    return hit;
}

// ------------------------------------------------------------ 挂钩

static uintptr_t g_base = 0;
static uintptr_t base() { return __atomic_load_n(&g_base, __ATOMIC_ACQUIRE); }

static os_log_t logger() {
    static os_log_t l = os_log_create("wxar.tip", "tip");
    return l;
}

template <typename T>
static const T& field(const void* obj, size_t off) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(obj) + off);
}

static void capture(const void* msg) noexcept {
    try {
        uint64_t id = field<uint64_t>(msg, kMsgServerId);
        if (id == 0) return;
        const std::string& content = field<std::string>(msg, kMsgContent);
        if (content.size() > kMaxContent) return;
        Entry e;
        if (describe(field<uint32_t>(msg, kMsgType), field<uint32_t>(msg, kMsgSubType), content,
                     field<std::string>(msg, kMsgSender), e)) {
            e.time = field<uint32_t>(msg, kMsgTime);
            remember(id, std::move(e));
        }
    } catch (...) {
    }
}

// 撤回通知里的 newmsgid；不是撤回通知返回 0
static uint64_t recalled_id(const std::string& xml) {
    std::string v;
    if (xml.size() > kMaxContent) return 0;
    size_t p = xml.find("<revokemsg>");
    if (p == std::string::npos) return 0;
    return xml_text(xml, "newmsgid", p, v) ? parse_id(v) : 0;
}

static void rewrite(void* self, const std::string* xml) noexcept {
    try {
        if (!xml || xml->size() > kMaxContent) return;
        if (field<std::string>(self, kSysType) != "revokemsg") return;
        uint64_t id = recalled_id(*xml);
        if (id == 0) return;
        Entry e;
        bool hit = lookup(id, e);
        bool changed = false;
        if (hit) {
            auto& tip = *reinterpret_cast<std::string*>(static_cast<uint8_t*>(self) + kSysReplace);
            std::string out;
            if (compose(tip, e, out)) {
                tip.assign(out.data(), out.size());
                changed = true;
            }
        }
        os_log(logger(), "revoke hit=%d changed=%d", (int)hit, (int)changed);
    } catch (...) {
    }
}

// 微信把撤回通知自己的时间和排序号抄给提示，于是提示落在撤回那一刻。
// 这里换成原消息那一秒的末尾，提示就会紧跟在原消息下面。
static void reposition(void* msg) noexcept {
    try {
        if (field<uint32_t>(msg, kMsgType) != 10002) return;
        uint64_t id = recalled_id(field<std::string>(msg, kMsgContent));
        Entry e;
        if (id == 0 || !lookup(id, e) || e.time == 0) return;
        uint32_t now = field<uint32_t>(msg, kMsgTime);
        if (now < e.time || now - e.time > kMaxRecallAge) return;
        uint64_t seq = (uint64_t)e.time * 1000 + 999;
        auto* p = static_cast<uint8_t*>(msg);
        *reinterpret_cast<uint32_t*>(p + kMsgTime) = e.time;
        *reinterpret_cast<uint64_t*>(p + kMsgSortSeq) = seq;
        os_log(logger(), "moved back %u s", now - e.time);
    } catch (...) {
    }
}

using ParseFn = uint64_t (*)(void*, const std::string*, bool*);
using FinalizeFn = uint64_t (*)(void*, uint32_t);

static uint64_t hook_parse(void* self, const std::string* xml, bool* flag) {
    uint64_t r = reinterpret_cast<ParseFn>(base() + kParseOrig)(self, xml, flag);
    if ((uint32_t)r != 0) rewrite(self, xml);
    return r;
}

static uint64_t hook_finalize(void* msg, uint32_t force) {
    bool skip = field<uint8_t>(msg, kMsgFinalized) == 1 && force == 0;
    uint64_t r = reinterpret_cast<FinalizeFn>(base() + kFinalizeOrig)(msg, force);
    if (!skip) {
        capture(msg);
        reposition(msg);
    }
    return r;
}

// ------------------------------------------------------------ 装载

static bool uuid_matches(const mach_header_64* mh) {
    auto p = reinterpret_cast<const uint8_t*>(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<const load_command*>(p);
        if (lc->cmd == LC_UUID)
            return memcmp(reinterpret_cast<const uuid_command*>(lc)->uuid, kUUID, 16) == 0;
        p += lc->cmdsize;
    }
    return false;
}

static uint32_t enc_branch(uint32_t op, uintptr_t from, uintptr_t to) {
    return op | (uint32_t)(((intptr_t)(to - from) >> 2) & 0x3FFFFFF);
}

static void on_image(const mach_header* mh32, intptr_t) {
    auto mh = reinterpret_cast<const mach_header_64*>(mh32);
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64 || base()) return;
    if (!uuid_matches(mh)) return;
    uintptr_t addr = reinterpret_cast<uintptr_t>(mh);
    // 两处跳转必须正好是我们打的补丁，否则什么都不做
    if (*reinterpret_cast<const uint32_t*>(addr + kParseCall) !=
            enc_branch(0x94000000, kParseCall, kStubParse) ||
        *reinterpret_cast<const uint32_t*>(addr + kFinalize) !=
            enc_branch(0x14000000, kFinalize, kStubFinalize)) {
        os_log(logger(), "patch bytes mismatch, inactive");
        return;
    }
    __atomic_store_n(&g_base, addr, __ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<uintptr_t*>(addr + kSlotFinalize),
                     reinterpret_cast<uintptr_t>(&hook_finalize), __ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<uintptr_t*>(addr + kSlotParse),
                     reinterpret_cast<uintptr_t>(&hook_parse), __ATOMIC_RELEASE);
    os_log(logger(), "active");
}

#ifndef WXAR_NO_INIT
__attribute__((constructor)) static void init() { _dyld_register_func_for_add_image(on_image); }
#endif

}  // namespace wxar
