# 微信 macOS 防撤回 · wechat-antirecall

别人撤回的消息，原样留在你的聊天里，并且在它下面告诉你撤回的是什么。私聊、群聊都有效。

| 对方撤回了 | 你看到的提示（消息本身也还在） |
|---|---|
| 文字 | "张三" 撤回了一条消息：「晚上八点开会」（发出 1 分 15 秒后） |
| 表情包 | "张三" 撤回了一个表情包（发出 10 秒后） |
| 图片、语音、视频 | "张三" 撤回了一张图片、一条语音、一个视频（发出 … 后） |
| 文件、链接 | "张三" 撤回了一个文件：「报价单.xlsx」（发出 2 分钟后） |

**Windows 用户看这里：[windows/README.md](windows/README.md)。** 除了「一键安装」，下面都是 macOS 版的说明。

只在你自己的电脑上生效，对方不会知道。不改网络协议、不自动化、不上传任何数据。

> **使用前请先看 [风险说明](#风险说明)。** 这是修改微信客户端的非官方工具，有被限制账号的可能，重要账号慎用。

## 一键安装

先**完全退出微信**（macOS 按 ⌘Q；Windows 在任务栏托盘图标上点右键退出），再运行对应的一行。装不了会直接说明原因；中途失败会自动放回原来的文件。

**macOS**，在「终端」里：

```bash
git clone https://github.com/stargif/wechat-antirecall.git ~/wechat-antirecall && ~/wechat-antirecall/install.sh
```

**Windows**（微信 4.2.0.53），在 PowerShell 里：

```powershell
irm https://raw.githubusercontent.com/stargif/wechat-antirecall/main/windows/get.ps1 | iex
```

macOS 提示目录已存在（`already exists`，以前下载过），就改成 `git -C ~/wechat-antirecall pull && ~/wechat-antirecall/install.sh`。

还原：macOS 运行 `~/wechat-antirecall/uninstall.sh`；Windows 按 Win + R，运行 `%LOCALAPPDATA%\WeChatAntiRecall\windows\uninstall.cmd`。

## 能不能用

| 要求 | 说明 |
|---|---|
| 电脑 | Apple Silicon（M 系列）的 Mac，macOS 13 或更高。Intel 不支持 |
| 微信 | 4.x，已收录 43 个构建号（268575 → 270416） |
| 命令行工具 | 没装过就先运行 `xcode-select --install` |

三种效果，安装时会自动选你的微信版本支持的最好的一种：

| 效果 | 消息留住 | 撤回提示 | 支持的版本 |
|---|---|---|---|
| `keeptip-content` | ✓ | 显示撤回的内容 | 仅 **App Store 版 4.2.0（构建号 270416）** |
| `keeptip` | ✓ | 普通的「撤回了一条消息」 | 已收录的版本（268880 除外） |
| `silent` | ✓ | 不提示 | 已收录的版本（270416 除外） |

「构建号」和「版本号」不是一回事：同样显示 4.1.13，官网版和 App Store 版的构建号不同，不能通用。
不用自己查，下面的检查命令会告诉你能不能装。

## 分步安装

**1. 下载**

```bash
git clone https://github.com/stargif/wechat-antirecall.git
cd wechat-antirecall
```

不会用 git 也可以点页面上的 **Code → Download ZIP**，解压后在终端里 `cd` 进去，后面的命令前面加 `bash`，
例如 `bash install.sh`。

**2. 检查能不能装**（不会改动任何东西）

```bash
./install.sh --check
```

**3. 完全退出微信**

切到微信按 **⌘Q**。只关窗口不算退出，微信会留在后台。

**4. 安装**

```bash
./install.sh
```

中途会问你要开机密码（不要自己加 `sudo`）。看到 `✓ 安装完成` 就可以打开微信了。

**5. 验证**

让别人**私聊**发一条消息再撤回，看看提示。

脚本会先在副本上打补丁、重签名、验签，全部通过后才替换正式的微信；任何一步失败都会自动还原。
原版微信完整保存在 `~/Library/Application Support/WeChatAntiRecall/Originals/`。

## 还原

退出微信，然后：

```bash
./uninstall.sh
```

会放回安装前的官方原版。如果找不到备份，直接删掉微信、从 App Store 或官网重装即可。聊天记录存在别处，替换微信程序不会删它。

## 装到别的位置的微信

电脑上有多个微信，或者微信不叫 `WeChat.app`，用 `--app` 指定完整路径：

```bash
./install.sh --app "/Applications/微信2.app"
./uninstall.sh --app "/Applications/微信2.app"
```

## 指定效果

```bash
./install.sh --variant silent
```

`keeptip` 可以直接升级成 `keeptip-content`。其他方向的切换，先 `./uninstall.sh` 再重新安装。

## 常见问题

**提示「还不支持构建号 xxx」**
补丁是按构建号收录的，地址不能互套。`/usr/bin/python3 wechat_antirecall.py list` 可以看支持哪些。

**提示「系统不允许移动微信」或 `Operation not permitted`**
macOS 的「App 管理」保护。到 **系统设置 → 隐私与安全性 → App 管理**，打开「终端」的开关
（列表里没有就点左下角 `+` 添加），**完全退出终端再重开**，重新安装。

**装完打开微信，弹窗要求访问数据 / 看不到聊天记录**
先点「允许」。还不行就到 **系统设置 → 隐私与安全性 → 完全磁盘访问权限** 里添加微信，重新打开。

**要我重新登录**
重新签名后微信可能读不到之前的登录信息，用手机扫码即可，聊天记录还在。

**微信更新之后失效了**
正常现象，更新会整个替换微信。先用 `./install.sh --check` 看新版本是否支持。建议关掉微信的自动更新。

**群里撤回没有提示**
270416 上群聊和私聊都会显示提示。更早的版本在群聊里可能不弹提示，但消息还在。

**有时提示里没有内容，只有「撤回了一条消息」**
`keeptip-content` 只认得**本次启动微信之后**收到或加载过的消息，更早的查不到。

**系统「分享」菜单里的微信不能用了**
重签名后不再是腾讯的签名，系统会拒绝它访问腾讯的共享容器，依赖它的系统集成可能失效。

**提示 detritus / FinderInfo**
多半是给微信加过 Finder 颜色标签或自定义图标，去掉后再试。

## 风险说明

**有被限制账号的可能，没有人能保证不会。请自行判断，风险由使用者承担。**

- 腾讯的态度很明确：非官方、未经授权的微信插件都属于违规，会视情节限制功能或封禁
  （[报道](https://thirdpage.thepaper.cn/h5/jrtt/31627957)）。
- 防撤回是「本地不删那条消息」，服务端看不到你删没删。但微信会在运行时检查自己的签名，
  重签名后不再有腾讯的证书，这是最现实的检测途径。
- `keeptip` 和 `silent` 只改微信里的几个字节。**`keeptip-content` 会让微信多加载一个模块，属于代码注入，
  更容易被检测到。** 在意风险就用 `./install.sh --variant keeptip`。
- 建议先用小号观察一段时间；不要和自动回复、群发、多开挂机之类的工具混用。
- 一旦收到「检测到非官方插件」之类的提醒，立刻 `./uninstall.sh` 并停用。

请尊重他人的隐私和意愿，不要用撤回的内容去伤害别人。

## 原理

微信 4.x 的消息逻辑在 `Contents/Resources/wechat.dylib`。对方撤回时，服务器推送 `revokemsg`，
客户端把它解析成「删哪条（`newmsgid`）」和「提示文案（`replacemsg`）」，再据此删消息、插提示。

- **`silent`**：让解析直接返回空，什么都不发生。
- **`keeptip`**：解析照跑，但把 `newmsgid` 清零。找不到要删的消息，于是消息留住、提示照常。
- **`keeptip-content`**：在 `keeptip` 的基础上加载 `libwxar_tip.dylib`。收到消息时在内存里记下摘要，
  撤回时按 ID 查出来写进提示，并让提示排在原消息下面。模块没加载成功时自动退回 `keeptip` 的效果。
  模块自己不写盘、不联网；改写后的提示由微信决定是否存进本地聊天库。

安全设计：每个补丁点都先核对原始字节，对不上就整体中止；写入是事务式的，失败自动回滚；
重签名保留 entitlements 并在签完后复核。

更多细节见 [docs/270416-runtime-tip.md](docs/270416-runtime-tip.md)。

## 给开发者

```
install.sh / uninstall.sh      一键安装、还原
wechat_antirecall.py           补丁引擎：info / apply / restore / verify / list
patches.json                   按构建号收录的补丁点
runtime/wxar_tip.cpp           keeptip-content 的模块源码
runtime/libwxar_tip.dylib      预编译的模块（SHA-256 登记在 patches.json）
runtime/test_tip.cpp           单元测试（不需要微信）
runtime/e2e.cpp                端到端测试（加载打过补丁的 wechat.dylib，不登录、不联网）
runtime/gen_patch.py           生成 270416 的补丁字节
runtime/build.sh               跑测试、重新编译模块并更新登记的 SHA-256
```

不放心预编译的模块，自己编译一份再装：

```bash
./runtime/build.sh
```

直接用补丁引擎（高级用法，会就地修改，自带备份）：

```bash
/usr/bin/python3 wechat_antirecall.py info --app /Applications/WeChat.app
/usr/bin/python3 wechat_antirecall.py apply --app /path/to/WeChat.app --variant keeptip --dry-run
```

新增版本：往 `patches.json` 的 `builds` 里加一条，`expected` **必须**填原始字节，再用 `info` 和 `--dry-run` 确认全部命中。
定位方法可参考上游 [WeChatTweak](https://github.com/zengtianli/WeChatTweak)。

## 许可证与致谢

[AGPL-3.0](LICENSE)。补丁点数据库与重签名流程派生自以下项目，详见 [NOTICE.md](NOTICE.md)：

- [zengtianli/WeChatTweak](https://github.com/zengtianli/WeChatTweak)（AGPL-3.0）：4.x 补丁点数据库、重签名流程
- [fzlzjerry/wechat-antirecall](https://github.com/fzlzjerry/wechat-antirecall)（MIT）：`keeptip` 与运行时提示的思路、部分补丁点
- [sunnyyoung/WeChatTweak](https://github.com/sunnyyoung/WeChatTweak)（AGPL-3.0）：最早的 macOS 微信防撤回实现

「微信」「WeChat」是腾讯公司的商标，本项目与腾讯没有任何关联。仅供个人学习与研究使用。
