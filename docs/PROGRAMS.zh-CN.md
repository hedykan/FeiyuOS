# 程序与触发（Lua）

> 0.7 起，程序用 Lua 5.4 写，存成 TF 卡上的 `.lua` 文件；原来的 JSON 步骤语言（`prog_engine`）已删除。
> 给模型的完整写法在 `read prog://help`、`read prog://help/examples`（`packages/program_package.cpp`）。

## 1. 分层

| 层 | 文件 | 做什么 |
|:--|:--|:--|
| 开头声明 | `lua_meta.cpp`（主机测试） | 解析 `-- @about @input @uses @version @package @target @usage @keywords`；权限匹配；权限摘要 |
| 虚拟机 | `lua_vm.cpp`、`lua_all.cpp`、`third_party/lua` | 每次运行新建、结束即关；私有堆；`vat.*` 绑定；内存、步数、时间上限 |
| 程序与触发 | `prog_runtime.cpp` | 卡上文件、索引、确认记录、运行、触发器、排队 |
| 扩展包 | `packages/program_package.cpp`、`packages/pkg_package.cpp` | `prog://`、`trigger://`、`pkg://` |

## 2. 程序

一个程序就是 `/vat/things/lua/名字.lua`（`/vat/things/` 下的文件会备份到板载存储）。

```lua
-- @about 早上查天气，下雨就提醒带伞
-- @input city 深圳                      -- 参数和默认值；不写默认值就是必填
-- @uses write ssh://开发机/home/me/log/*  -- 需要用户确认一次的能力
local w = vat.read("place://weather?city=" .. vat.urlencode(input.city))
if w and w:find("雨") then vat.write("device://local/display/text", "带伞") end
return w
```

- `write prog://名字`（content 为源码）保存：先解析声明、编译检查语法，声明了 `@uses` 的必须由用户确认。
- `exec prog://名字?参数=值` 运行：结果是 `return` 的值，`print` 进运行输出。
- 名字最长 48 字节；不能含 `/ \ ? # * : " < > |`；`list help legacy` 保留。

### 2.1 vat

| 函数 | 说明 |
|:--|:--|
| `vat.read / write / exec / observe / input / session` | 与模型的七个工具相同，经 Policy 与注册中心交给扩展包；失败返回 `nil, 原因`，并在标准错误里记一行带行号的警告。有数据的调用返回表，见下 |
| `vat.stream(target)` | 流的最新值（表，如 `a.x`） |
| `vat.after("25m", "prog://名字?参数")` | 过一段时间运行某个程序一次（建一个一次性触发） |
| `vat.sleep(ms)` | 原地等待，单次最长 60 秒 |
| `vat.json / tojson / urlencode / millis` | 工具函数 |
| `vat.log(...)` | 写到标准错误，前面带程序开始以来的时间 `[+1.23s]`；不占标准输出的 2 KB |
| `vat.now()` | `{ms, time, date}`；没对时的时候只有 `ms` |
| `print(...)`、`io.write(...)`、`io.stderr:write(...)` | 标准输出（`io.write` 不换行，`\r` 回到行首覆盖）；标准错误红色 |
| `io.read()`、`io.read("n")`、`vat.readline("提示")` | 标准输入：读一行 / 一个数；用户在程序页输入，` 中断；`exec` 带 `stdin` 时从它读，读完返回 `nil` |
| `vat.key(超时毫秒)` | 读一个按键：`"a"`、`"enter"`、`"del"`、`"tab"`、`"space"`（`; . , /` 约定为上下左右），超时 `nil` |
| `vat.clear()`、`vat.interactive()` | 清空程序页；是否前台运行（有人在看） |
| `vat.exec("prog://名字?x=1")` | 调用另一个程序，返回它 `return` 的值；最多嵌套 3 层 |

可用的库：`string table math utf8 coroutine`，以及只有 `read / write / stderr` 的 `io`（控制台，不能打开文件）。没有 `os package debug`，也没有 `load / loadfile / dofile`（只收文本源码）。

### 2.0 前台与后台

| 怎么运行 | 前台 / 后台 | 输出 | 输入 | 上限 |
|:--|:--|:--|:--|:--|
| 用户对肥鱼说「运行…」（`exec prog://`），或在程序页（Fn+A）选中回车 | 前台：自动切到程序页 | 实时显示；同时作为结果的「输出：」给肥鱼（2 KB） | 键盘；`exec` 带 `stdin` 时先从它读 | 30 分钟、2 亿条指令；` 中断 |
| 触发器（定时、事件、`vat.after`） | 后台：不切页 | 只记在「最近」和结果里 | 没有：`io.read` 返回 `nil, 原因` | 150 秒、2000 万条指令 |

程序调用的程序跟着最外层的运行走。每次运行（前台、后台、被别的程序调用）结束后，它的输入、输出、错误追加到三条全局的流里，见 2.4。程序画的画面（`device://local/display/*`、`ui://`）在图形页显示；程序读一行输入时自动切回程序页，输完再回到图形页；读按键（`vat.key`）时画面不动。

### 2.1.1 返回值

有数据的调用返回表，程序直接取字段：

| 调用 | 返回 |
|:--|:--|
| `vat.read("device://local/battery")` | `{percent, voltage, usb}` |
| `vat.read("sensor://imu/acceleration")`、`gyro` | `{x, y, z}`；`orientation` 是 `{pitch, roll}`（凡是流，读一次都得到它的字段） |
| 读写 `device://local/led` | `{color}` |
| `vat.exec("ssh://主机", 命令, {参数})` | `{stdout, stderr, code}` |
| `vat.exec("prog://名字", "run", {输入})` | `{ok, result, stdout, stderr}`；对方出错时和别的调用一样返回 `nil, 原因` |

这些表当文字用时就是原来那句话：`tostring(r)`、`r .. ""`、`r:match(...)` 等字符串方法都作用在文字上（程序的调用是对方 `return` 的结果），所以按文字结果写的旧程序照常运行。加选项 `{text=true}` 得到纯文字。其他调用返回文字。扩展包用 `registry::offer_data()` 交出数据。

`print` 一个表显示成 JSON。输出超过 2 KB 时保留开头 512 字节和结尾。

### 2.1.2 改程序的一部分

- `write prog://名字/patch`：content 是若干段 `<<<<`（换行）原文（换行）`====`（换行）新文字（换行）`>>>>`。原文必须和程序里的文字完全一致并且只出现一次；找不到或出现多次时报错，程序不改动。改完照常做语法检查和权限确认。
- `read prog://名字?grep=关键词`：匹配的行，带行号。`read prog://名字?lines=120-160`：这一段原文。

### 2.1.3 画图（gfx）

程序开头写 `-- @uses gfx`（`@uses` 后面只写包名，表示用这个扩展包的 Lua 写法，不需要确认），就能逐帧画：

```lua
-- @uses gfx
local x, vx = 12, 6
for i = 1, 300 do
  gfx.fillScreen("#101820")
  gfx.fillCircle(x, 70, 10, "#E8A33D")
  gfx.drawString("帧 " .. i, 4, 4, {size = 1, color = "white"})
  gfx.present()          -- 这一帧一次上屏；不调用，屏幕不变
  x = x + vx
  if x > 228 or x < 12 then vx = -vx end
  vat.sleep(33)
end
```

- `gfx.<函数名>(参数…)` 与 M5GFX 同名同参，76 个：形状（`fillRect`、`fillSmoothCircle`、`drawWideLine`、`fillGradientRect`、`fillArc`……）、文字（`drawString`、`drawCentreString`、`print`、`println`、`printf`、`setCursor`、`setTextColor`、`setFont`……）、图片与位图（`drawPng/Jpg/Bmp` 读 `/vat/` 下文件、`drawBitmap`、`pushImage`、`qrcode`）、状态与裁剪（`setColor`、`setClipRect`……）、查询（`width`、`textWidth`、`fontHeight`、`getCursorX`、`color565`……立即返回），颜色常量 `gfx.TFT_RED` 这类。全部函数和例子：`read gfx://help`。另有 FeiyuOS 的 `progressBar`、`drawQr`、`rotate`。颜色省略时用 `setColor` 的颜色，画图时给了颜色也会改掉当前颜色（同 M5GFX）；状态在下一帧继续有效。参数不对时报错并指出是哪个参数。一帧最多 64 条，位图、像素和图片合计 32 KB。
- 不提供：`readPixel`、`floodFill`、`copyRect`、`scroll`、sprite（要整屏缓冲，留给 0.8 独占态），以及 `startWrite`、DMA 这类底层调用。
- `gfx.present()`：和 `write gfx://frame` 一样过权限判断（屏幕属于本机可撤销，直接放行），把这一帧交给界面；上一帧还没被界面取走时最多等 100 ms，所以画得再快也不会白算。每次运行的第一帧会切到图形页。
- `gfx.size()` 返回 240、135；`gfx.measure(文字, size)` 返回文字宽度；`gfx.close()` 收起画面。
- 画面由合成器上屏（`docs/V075_PLAN.zh-CN.md`）：一帧要么全部出现，要么不出现，不会画到一半被看见；没变的条带不推送。实测每帧 `vat.sleep(30)` 约 29 帧/秒，不停顿约 47 帧/秒。

### 2.4 运行记录：三条全局的流

- `/vat/io/stdin.log`、`stdout.log`、`stderr.log`，所有程序共用。每次运行有一个编号，在流里以一行头开始：`== 11-11 02:00:03 #128 每日备份 来源=触发:每天 结果=失败 耗时=1.2s 内存峰值=18KB ==`。标准输出流每次运行都写头行（它同时是运行记录），另外两条只在有内容时写。
- 每条流 64 KB（没有 TF 卡时 16 KB），满了变成 `.1`，留两份。
- 读：`read debug://stdout | stderr | stdin`（`?n=最近几次&prog=名字&failed=1`），`read debug://run/编号`。工具返回的运行结果里带着「运行 #编号」。
- 运行结果分段给模型：【结果】【输出】【错误输出】，失败时还有【出错的行】和【调用栈】。

### 2.2 权限（D9 的新形式）

- 每次 `vat.*` 调用单独过 Policy：允许的直接执行；拒绝的（凭据路径等）一律拒绝；**要确认的，只有被已确认的 `@uses` 覆盖才执行**，否则返回「需要确认：在开头声明 -- @uses …」。
- `@uses 动作 目标`：目标精确匹配，或匹配 `目标/…`、`目标?…`；以 `*` 结尾表示前缀。
- **联网**（`http://`、`https://`，http 包）：程序访问的每个网站都要有 `@uses`，**读也一样**（肥鱼自己 GET 是直接放行的，但程序可能把数据拼进网址带走）。例如 `-- @uses read https://api.github.com/*`、`-- @uses write https://hooks.example.com/notify`。`vat.read(网址, {headers = {Accept = "application/json"}, save = "file:///vat/x.json", max_bytes = 4096})`；`vat.write(网址, 表或文本, {method = "PUT"})`（默认 POST，表按 JSON 发）。2xx 返回响应体，其他状态码返回 `nil, "ERROR: HTTP 404：…"`。请求头里的 `{secret:名字}` 必须事先由肥鱼调用一次、在设备上输入，程序里不能输入。
- 确认记录在 `/vat/things/lua/approved.json`：按程序名记权限摘要（只由 `@uses` 和包的 `@target` 决定，改代码不变）。文件在电脑上被改过、权限变了，状态就变成「待确认」，需要重新保存或 `write pkg://名字/enable`。
- 读取类、本机可撤销的操作（屏幕、LED、声音、剪贴板、`timer://`、`ui://`）不用声明。需要输密码的操作不能放进程序。

### 2.3 上限

| 项 | 上限 |
|:--|:--|
| 单次运行 | 150 秒（含 sleep；worker 任务超过 180 秒会重启设备） |
| `vat.sleep` | 单次 60 秒；更长用 `vat.after` |
| 内存 | 48 KB（先取 24 KB 私有堆，不够再追加 12 KB 一块） |
| 指令 | 2000 万条（约 6 秒的纯计算） |
| 嵌套调用 | 3 层 |
| 源码 | 16 KB；程序和扩展包合计 64 个；触发 32 个 |

## 3. 触发

`write trigger://new`，content：`{"when": "time://every/07:00", "run": "prog://名字?参数"}`。

- 只能运行程序；单个动作也写成一两行的程序。触发本身不需要确认（它只能运行已确认的程序），删除需要确认。
- `when`：`time://at/18:30`（一次）、`time://in/10m`（一次）、`time://every/07:00`、`time://weekday/09:00`、`time://weekend/10:00`、`device://local/event/boot`，以及扩展包声明的事件（如 `ui://panel/名字`）。事件数据在程序里是 `input.event`。
- 一次性触发执行后删除；关机期间错过的不补跑，记一条「错过」。
- 触发的运行排队，只在 worker 空闲且没有对话时执行，结果写进时间线。
- 存在 `/vat/things/lua/triggers.json`；v0.7 之前的触发首次启动时迁移（运行程序的转成新格式，其余记一条「未迁移」，原文件改名为 `.old`）。

- **每隔多久**：`time://every/30s`、`every/5m`、`every/2h`（至少 10 秒；`every/07:00` 这种带冒号的仍是每天某时刻）。到点时上一轮还没跑完、或设备正忙（在对话、在跑别的），这一轮跳过，不会堆积。
- **状态**：`read trigger://list` 每一项显示开机以来运行几次、上次时间和结果、连续失败几次、忙时跳过几次。
- **连续失败 5 次自动停用**，并在屏幕底部提示；`write trigger://编号/resume` 重新启用（失败计数清零）。
- **清理**：`write trigger://clean`（需确认）删除指向已删除程序的触发，以及失败后自动停用的触发。

## 4. pkg：卡上文件的管理

| 调用 | 作用 |
|:--|:--|
| `read pkg://list` | 全部程序和扩展包、状态（已启用 / 待确认 / 已停用 / 声明有误）、权限 |
| `write pkg://scan` | 重新扫描 `/vat/things/lua/*.lua`，重建索引（电脑上改过卡之后） |
| `write pkg://名字/enable` | 启用；声明了权限的程序、以及所有扩展包，需要用户确认 |
| `write pkg://名字/disable` · `/delete` | 停用；删除（需确认，被触发使用时拒绝） |
| `read pkg://catalog` | 在线目录：可安装的程序和扩展包、版本、权限、已装 / 可更新（`?url=` 换目录地址） |
| `write pkg://名字/install` · `pkg://名字/update` | 从目录安装 / 更新（需确认，确认框显示版本、来源、权限；更新显示新增的权限） |
| `write pkg://名字/rollback` | 退回更新前保留的上一版（需确认） |
| `write pkg://new?sha256=…&url=https://…/名字.lua` | 从任意地址下载一个 .lua（`url` 放最后；需确认），装好后是「待确认」，看过权限再 enable |

`pkg` 是内置扩展包：它负责确认卡上的代码，所以自己不能是卡上的文件。

### 4.1 在线目录与安装

- 目录是一个 JSON（默认 FeiyuOS 仓库的 `packages/index.json`，用 `tools/pkg_index.py` 生成，见 `packages/README.md`），每项记着名字、版本、说明、声明的权限、文件地址、**SHA-256 和大小**。
- 安装 / 更新时：确认框里的内容来自上一次 `read pkg://catalog` 存在卡上的目录（`/vat/cache/pkg/`）；下载先放到 `/vat/cache/pkg/download.lua`，大小、SHA-256 都对、声明能解析、代码能编译，并且声明的权限、scheme、说明**和目录写的完全一样**，才放进 `/vat/things/lua/`，并直接按确认过的权限启用。任何一项不对：不装，卡上原来的版本不动。
- 更新时旧版本留在 `/vat/things/lua/old/名字.lua`，`write pkg://名字/rollback` 换回来（再执行一次就又换回新版）；退回后权限和当前确认的不同时，状态变「待确认」。
- 目录和下载都走 http 包的网络通道，不需要在 `@uses` 里声明；`http://` 地址会在确认框里标出「未加密」，靠 SHA-256 保证文件没被改。

### 4.2 卡上的扩展包

开头声明 `@package 名字` 和 `@target scheme://` 的 .lua 是扩展包：它负责 `@target` 下的调用，肥鱼和其他程序像用内置包一样用它。

```lua
-- @about 查天气（和风）
-- @package 天气
-- @target weather://
-- @usage 查天气用 read weather://now?city=城市名
-- @keywords 天气 气温 下雨
-- @uses read https://devapi.qweather.com/*
function read(target, args)
  local city = target:match("city=([^&]+)") or "深圳"
  local body, err = vat.read("https://devapi.qweather.com/v7/weather/now?location=" .. vat.urlencode(city),
                             {headers = {["X-QW-Api-Key"] = "{secret:qweather}"}})
  if not body then error(err) end
  local now = vat.json(body).now
  return {city = city, temp = now.temp, text = now.text}
end
```

- 例子里的 `{secret:qweather}` 是[和风天气](https://console.qweather.com/)控制台「项目管理」里创建的 API KEY。扩展包和程序里不能弹出输入框，所以先录入一次：对肥鱼说「用请求头 X-QW-Api-Key: {secret:qweather} 调一下和风天气 devapi.qweather.com 的实时天气」，设备弹出输入框，输入后这个令牌只发给 `devapi.qweather.com`，之后包里就能用了。
- 每次调用运行一遍文件，再调用与工具同名的**全局函数**：`read(target, args)`、`write(target, args)`（内容在 `args.content`）、`exec(target, args)`。返回文本即结果，返回表按 JSON 给出，`error("…")` 是失败。
- 扩展包**都要用户启用一次**，没有 `@uses` 也一样：`@usage` 会写进肥鱼的系统提示词，确认时会显示出来；`@usage`、`@target`、`@uses` 任何一个变了都回到「待确认」。
- 包里的 `vat.*` 调用按它自己确认过的 `@uses` 放行，和程序一样；读调用默认放行，写和执行默认要确认（Policy 照常）。
- 内置包已经负责的 scheme（`ssh://`、`http://`、`prog://`…）不能被卡上的包接管；两个卡上的包也不能负责同一个 scheme，后扫到的跳过。
- 系统提示词里每个包的 `@usage` 最多 240 字节，所有卡上包合计约 1200 字节。

## 5. 旧程序

v0.7 之前的 JSON 程序（`/vat/things/prog/programs/`）不再运行，`read prog://list` 会列出它们。翻译：`read prog://legacy/名字` 看原文，让肥鱼写成 Lua 后 `write prog://名字` 保存。

## 6. 测试

- 主机：`tests/host/lua_meta_test.cpp`（声明解析、权限匹配、摘要、名字）。
- E2E S10：`tests/programs/*.lua` 与 `cases.json`，逐个保存、运行，检查结果；扩展包 `pkg_echo.lua`（echo://）：扫描后待确认、经确认启用、读、返回表、出错、停用；另有触发（一次性、每天、暂停恢复、删除需确认）和意图（「运行某程序」「以后每天…」）。
