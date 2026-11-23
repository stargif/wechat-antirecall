// 端到端测试（开发用，在 Apple Silicon 的 Mac 上借助 Rosetta 跑）：
// 把打过补丁的 Weixin.dll 映射进内存，从两处补丁点开始直接执行它的机器码，
// 一路走到新增代码、微信自己的 std::string 赋值，再回到原函数的收尾。不运行微信，不联网。
//
//   clang -arch x86_64 -O1 tools/e2e.c -o /tmp/e2e && /tmp/e2e /path/to/patched/Weixin.dll
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define RVA_NEW 0x776931c
#define RVA_DELETE 0x7769360
#define RVA_FIN 0xa8b286     // 终结器补丁点
#define RVA_PARSE 0x22e38b9  // 解析补丁点
#define RVA_OTHER 0x22e38c1  // flag 不为 0 时走的另一条路

typedef struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } wstr;
static uint8_t *B;
static _Atomic int bad, news, dels;

__attribute__((ms_abi)) static void *host_new(uint64_t n) { news++; return malloc(n); }
__attribute__((ms_abi)) static void host_delete(void *p, uint64_t n) { (void)n; dels++; free(p); }

// 搭出与原函数序言之后一样的栈，再跳到补丁点；原函数的收尾会弹栈并返回到标号 1
__asm__(
    ".globl _call_fin\n_call_fin:\n"
    " push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n sub $8,%rsp\n"
    " lea 1f(%rip),%rax\n push %rax\n"
    " push %rsi\n push %rdi\n push %rbx\n sub $0x20,%rsp\n"
    " mov %rsi,%rax\n mov %rdi,%rsi\n jmp *%rax\n"
    "1: add $8,%rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n"
    ".globl _call_parse\n_call_parse:\n"
    " push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n sub $8,%rsp\n"
    " lea 1f(%rip),%rax\n push %rax\n"
    " push %rbp\n push %r15\n push %r14\n push %r13\n push %r12\n push %rsi\n push %rdi\n push %rbx\n sub $0x48,%rsp\n"
    " lea 0x40(%rsp),%rbp\n mov %cl,-0x20(%rbp)\n"
    " mov %rdx,%rax\n mov %rdi,%r10\n mov %rsi,%rdi\n mov %r10,%rsi\n lea 0x250(%rsi),%r14\n jmp *%rax\n"
    "1: add $8,%rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");
extern void call_fin(void *msg, void *target);
extern uint64_t call_parse(void *self, void *xml, void *target, int flag);

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) exit(2);
    fclose(f);
    uint32_t pe = *(uint32_t *)(d + 0x3c);
    uint16_t ns = *(uint16_t *)(d + pe + 6), os = *(uint16_t *)(d + pe + 20);
    uint8_t *opt = d + pe + 24;
    uint64_t base = *(uint64_t *)(opt + 24);
    uint32_t size = *(uint32_t *)(opt + 56);
    B = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (B == MAP_FAILED) { perror("mmap"); exit(2); }
    for (int i = 0; i < ns; i++) {
        uint8_t *s = opt + os + 40 * i;
        uint32_t vs = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rs = *(uint32_t *)(s + 16), ro = *(uint32_t *)(s + 20);
        if (va + (vs > rs ? vs : rs) > ((size + 0xFFF) & ~0xFFFu) || (rs && ro + rs > n)) { printf("节 %d 越界\n", i); exit(2); }
        memcpy(B + va, d + ro, rs < vs ? rs : vs);
    }
    int64_t delta = (int64_t)((uint64_t)B - base);
    uint32_t r = *(uint32_t *)(opt + 112 + 40), rn = *(uint32_t *)(opt + 112 + 44);
    for (uint32_t o = 0; o < rn;) {
        uint32_t pg = *(uint32_t *)(B + r + o), sz = *(uint32_t *)(B + r + o + 4);
        if (!sz) break;
        for (uint32_t k = 0; k < (sz - 8) / 2; k++) {
            uint16_t e = *(uint16_t *)(B + r + o + 8 + 2 * k);
            if (e >> 12 == 10) *(int64_t *)(B + pg + (e & 0xFFF)) += delta;
        }
        o += sz;
    }
    struct { uint32_t rva; void *fn; } h[] = {{RVA_NEW, host_new}, {RVA_DELETE, host_delete}};
    for (int i = 0; i < 2; i++) {
        uint8_t *p = B + h[i].rva;
        p[0] = 0x48; p[1] = 0xB8;
        memcpy(p + 2, &h[i].fn, 8);
        p[10] = 0xFF; p[11] = 0xE0;
    }
    memcpy(B + RVA_OTHER, "\xb0\x02\xeb\x08", 4);  // mov al,2; jmp 收尾
    free(d);
}

static wstr mk(const char *s) {
    wstr w = {.size = strlen(s)};
    if (w.size < 16) { w.cap = 15; memcpy(w.u.buf, s, w.size); }
    else { w.cap = w.size; w.u.ptr = malloc(w.size + 1); strcpy(w.u.ptr, s); }
    return w;
}
static const char *sd(const wstr *s) { return s->cap >= 16 ? s->u.ptr : s->u.buf; }
static void rel(wstr *s) { if (s->cap >= 16) free(s->u.ptr); }
#define S(p, o) (*(wstr *)((p) + (o)))
#define U32(p, o) (*(uint32_t *)((p) + (o)))
#define U64(p, o) (*(uint64_t *)((p) + (o)))

typedef struct { _Alignas(16) uint8_t m[0x300], s[0x300]; } ctx;
static void msg(ctx *c, uint32_t type, uint64_t id, uint32_t t, const char *content, const char *sender) {
    memset(c->m, 0, sizeof c->m);
    U32(c->m, 0x0c) = type;
    U64(c->m, 0x148) = id;
    U32(c->m, 0x164) = t;
    U64(c->m, 0x150) = (uint64_t)t * 1000;
    S(c->m, 0x180) = mk(content);
    S(c->m, 0x58) = mk(sender);
}
static void end(ctx *c) { rel(&S(c->m, 0x180)); rel(&S(c->m, 0x58)); }
static void recv_(ctx *c, uint32_t type, uint64_t id, uint32_t t, const char *content, const char *sender) {
    msg(c, type, id, t, content, sender);
    call_fin(c->m, B + RVA_FIN);
    if (c->m[0x2c0] != 1) { bad++; printf("FAIL 终结标志没有置位\n"); }
    end(c);
}
// 返回提示；顺序与微信一致：先解析，再收尾
static const char *revoke(ctx *c, uint64_t id, uint32_t t, const char *tip, uint64_t *left, uint32_t *time, uint64_t *seq) {
    char x[600];
    snprintf(x, sizeof x, "<sysmsg type=\"revokemsg\"><revokemsg><newmsgid>%llu</newmsgid><replacemsg><![CDATA[%s]]></replacemsg></revokemsg></sysmsg>", id, tip);
    msg(c, 10002, id + 1, t, x, "");
    rel(&S(c->s, 0x220));
    rel(&S(c->s, 0x250));
    S(c->s, 0x220) = mk("revokemsg");
    S(c->s, 0x250) = mk(tip);
    U64(c->s, 0x248) = id;
    uint64_t r = call_parse(c->s, c->m + 0x180, B + RVA_PARSE, 0);
    if ((r & 0xff) != 1) { bad++; printf("FAIL 返回值 %llx\n", r); }
    call_fin(c->m, B + RVA_FIN);
    if (left) *left = U64(c->s, 0x248);
    if (time) *time = U32(c->m, 0x164);
    if (seq) *seq = U64(c->m, 0x150);
    end(c);
    const wstr *w = &S(c->s, 0x250);
    if (sd(w)[w->size] != 0 || w->size > w->cap) { bad++; printf("FAIL 字符串损坏\n"); }
    return sd(w);
}
static void check(const char *n, const char *got, const char *want) {
    int ok = !strcmp(got, want);
    if (!ok) bad++;
    printf("%s %s: %s\n", ok ? "ok  " : "FAIL", n, got);
}
static void num(const char *n, uint64_t got, uint64_t want) {
    if (got != want) { bad++; printf("FAIL %s: %llu != %llu\n", n, got, want); } else printf("ok   %s\n", n);
}

static void *worker(void *arg) {
    uint64_t k = (uint64_t)arg;
    ctx *c = calloc(1, sizeof *c);
    S(c->s, 0x220).cap = S(c->s, 0x250).cap = 15;
    for (uint64_t i = 0; i < 20000; i++) {
        uint64_t id = (k + 1) * 10000000 + i * 2;
        char a[64], w[160];
        snprintf(a, sizeof a, "第 %llu 条消息", id);
        recv_(c, 1, id, 1700000000, a, "");
        if (i % 3 == 0) {
            snprintf(w, sizeof w, "对方撤回了一条消息：「%s」（发出 7 秒后）", a);
            if (strcmp(revoke(c, id, 1700000007, "对方撤回了一条消息", 0, 0, 0), w)) bad++;
        }
    }
    rel(&S(c->s, 0x220));
    rel(&S(c->s, 0x250));
    free(c);
    return 0;
}

int main(int n, char **v) {
    if (n < 2) return 2;
    load(v[1]);
    ctx *c = calloc(1, sizeof *c);
    S(c->s, 0x220).cap = S(c->s, 0x250).cap = 15;
    uint64_t left = 1, seq = 0;
    uint32_t t = 0;
#define Z "\"张三\" 撤回了"
#define GO(name, type, id, content, sender, want) do { recv_(c, type, id, 1700000000, content, sender); \
    check(name, revoke(c, id, 1700000075, Z "一条消息", &left, &t, &seq), Z want "（发出 1 分 15 秒后）"); \
    if (left != 0) { bad++; printf("FAIL newmsgid 未清零\n"); } } while (0)
    GO("文字", 1, 7000000000000000001ull, "1", "", "一条消息：「1」");
    num("时间", t, 1700000000);
    num("排序号", seq, 1700000000999ull);
    GO("长 ID", 1, 9223372036854775999ull, "晚上八点开会", "", "一条消息：「晚上八点开会」");
    GO("群聊", 1, 7000000000000000021ull, "wxid_abc:\n你好", "wxid_abc", "一条消息：「你好」");
    GO("表情包", 47, 7000000000000000003ull, "<msg/>", "", "一个表情包");
    GO("图片", 3, 7000000000000000005ull, "<msg/>", "", "一张图片");
    GO("文件", 49, 7000000000000000011ull, "<msg><appmsg><title>报价单.xlsx</title><type>6</type></appmsg></msg>", "", "一个文件：「报价单.xlsx」");
    check("没见过", revoke(c, 7000000000000000099ull, 1700000075, Z "一条消息", &left, &t, 0), Z "一条消息");
    num("没见过也留住", left, 0);
    num("没见过不挪", t, 1700000075);
    check("自己撤回", revoke(c, 7000000000000000001ull, 1700000075, "你撤回了一条消息", &left, &t, 0), "你撤回了一条消息");
    num("自己撤回不拦", left, 7000000000000000001ull);
    // flag 不为 0：应走另一条路（被改成返回 2）
    msg(c, 10002, 5, 1, "x", "");
    num("flag 不为 0 走原来的另一条路", call_parse(c->s, c->m + 0x180, B + RVA_PARSE, 1) & 0xff, 2);
    end(c);
    pthread_t th[8];
    for (uint64_t i = 0; i < 8; i++) pthread_create(&th[i], 0, worker, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(th[i], 0);
    printf("并发 8x20000 完成，微信的 new %d 次、delete %d 次\n", news, dels);
    if (bad) printf("%d 项失败\n", bad);
    else printf("全部通过\n");
    return bad != 0;
}
