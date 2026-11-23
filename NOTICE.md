# NOTICE · 来源、派生关系与致谢

本项目的补丁点数据库与重签名流程**并非从零原创**，而是派生自以下公开项目。按 AGPL-3.0
的要求，在此明确说明派生关系与来源。

---

## 1. 补丁点数据库 `patches.json`

派生自两个项目的公开补丁库：

| 来源 | 许可证 | 贡献内容 |
|---|---|---|
| [zengtianli/WeChatTweak](https://github.com/zengtianli/WeChatTweak) 的 `config.json` | **AGPL-3.0** | 绝大部分构建号的补丁点、地址、`expected` 原始字节、变体定义（`revoke` / `revoke-keeptip` / `multiInstance` / `update`） |
| [fzlzjerry/wechat-antirecall](https://github.com/fzlzjerry/wechat-antirecall) 的 `patches.json` | **MIT** | 部分构建号的补丁点（经 WeChatTweak 汇集） |

269602 的补丁点另参考了 [EchoXml/WeChatTweak](https://github.com/EchoXml/WeChatTweak) 的 `feature/wechat-269602` 分支。

其中构建号 **269602** 在官方 arm64 切片上**逐字节独立复核**，
并在 `patches.json` 的 `verified` 字段中记录了复核方式与切片 SHA-256
（`413c124643903640946836d631fb4185133a8ffccbf2863e77b9a4b70568abd6`）。

本项目的工作是：剔除了无原始字节校验、且面向 3.x 的条目，重新组织数据结构，
补充来源与复核元数据。

## 2. 重签名流程 `Resigner` / `sign_code_object` / `check_drift`

**逐条移植**自 [zengtianli/WeChatTweak](https://github.com/zengtianli/WeChatTweak)
的 `Sources/WeChatTweak/Resigner.swift`（**AGPL-3.0**），包括：

- 快照所有 code object 的 entitlements，注入 `disable-library-validation` 与
  `allow-unsigned-executable-memory`，剥离受限的 `com.apple.developer.*`
- 使用 `--preserve-metadata=identifier,flags,runtime`（而**非** `requirements`）分层签名
- 签名后复核漂移，按「最深层优先」显式补签，仍漂移则失败
- macOS 15+ 的 `--force-library-entitlements`
- 只清 `com.apple.quarantine`、绝不用 `xattr -c`
- `.cstemp` 残留清理

该流程本身又源自 [fzlzjerry/wechat-antirecall](https://github.com/fzlzjerry/wechat-antirecall)（MIT）
的实践经验。

## 3. `keeptip` 变体思路

「保留 `cbz`、把写入 `newmsgid` 的 `str x0` 改成 `str xzr`」这一做法，来自
[fzlzjerry/wechat-antirecall](https://github.com/fzlzjerry/wechat-antirecall)（**MIT**）的
`revoke-tip` 模式，经 WeChatTweak 分析并收录。

## 3.1 `keeptip-content` 变体

运行时改写提示（挂钩解析函数与 Message 终结器、缓存正文）的思路与结构体布局线索，参考了
[fzlzjerry/wechat-antirecall](https://github.com/fzlzjerry/wechat-antirecall)（**MIT**）的 `--runtime-tip` 文档；
`runtime/` 下的代码与 270416 的补丁点为本项目独立编写和定位。

## 4. 历史工作

[sunnyyoung/WeChatTweak-macOS](https://github.com/sunnyyoung/WeChatTweak-macOS) ——
最早的 macOS 微信防撤回实现（2017 起），本项目的存在建立在其长期的社区验证之上。

## 5. 其他参考

[EEEEhex/RevokeHook](https://github.com/EEEEhex/RevokeHook) —— Windows 侧微信 4.0 防撤回实现
与字符串解密思路，用于交叉印证 `revokemsg` 的处理链路。

---

## fzlzjerry/wechat-antirecall 的 MIT 许可声明

```
MIT License

Copyright (c) 2026 Morax

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 许可证

由于派生自 AGPL-3.0 的 WeChatTweak（补丁数据与重签名流程），**本项目整体以 AGPL-3.0 发布**，
完整条款见 `LICENSE`。

上游 WeChatTweak 亦沿用其上游的 AGPL-3.0。
fzlzjerry/wechat-antirecall 以 MIT 发布，其被派生部分在本项目中间接受 AGPL-3.0 约束。

## 商标与免责

「微信」「WeChat」为腾讯公司的商标。本项目与腾讯公司**没有任何关联**，
未获其授权或认可。本项目仅供个人学习与研究使用，修改客户端可能违反微信的服务条款，
并存在账号被限制的风险，**风险由使用者自行承担**。
