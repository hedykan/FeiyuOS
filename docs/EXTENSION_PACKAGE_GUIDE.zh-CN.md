# 扩展包开发手册

本文是给设备新增能力的**唯一方式**。无论是本小组开发还是外部 PR，都必须遵守。

## 1. 不变的内核：七个工具

模型永远只看到七个原子工具：

```text
discover  read  write  exec  observe  input  session
```

新增能力**不允许**增加第八个工具，也不允许改动工具 schema（`src/agent_prompt.h` 里的 `kToolsJson`）。能力通过 **target 资源地址** 区分，由扩展包认领：

```text
ssh://开发机/etc/hostname      tui://current            tmux://开发机/work
device://local/display/text   device://local/wifi/scan settings://brightness
sensor://书桌/temperature     device://厨房ESP/gpio/2   device://local/ir/客厅电视/power
```

地址怎么写见第 5 节「资源地址」。只有七个动作确实表达不了的新语义，才考虑协议大版本升级。这需要单独提议，不能夹带在功能 PR 里。

## 2. 一次调用怎样到达扩展包

```text
语音/文字 → 模型（七工具 + 系统提示）
  → discover：注册表汇总所有包的 manifest / resources / 命令目录
  → read/write/exec/observe/input/session
      → Policy（allow / ask / deny；凭据路径硬拒绝）
      → registry::execute：按注册顺序询问 supports()，第一个认领的包执行
      → 没有包认领：返回“没有能力包可处理 …”，绝不猜测
```

`discover` 和执行共用同一份注册表，所以模型能发现的能力和设备真正能执行的能力始终一致。

## 3. 包接口

定义在 `src/registry.h`：

```cpp
struct Manifest {
    const char* id;                       // 稳定、唯一、小写
    const char* summary;                  // discover(capabilities) 展示的一句话
    const char* keywords;                 // 中英文检索词（中文会做双字切分匹配）
    std::vector<const char*> commands;    // 仅命令目录包：已审计的命令前缀
    const char* availability_program;     // 仅命令目录包：在 ssh:// 目标上探测的程序
};

class Package {
    virtual const Manifest& manifest() const = 0;
    virtual bool supports(const String& tool, const cJSON* arguments) const;   // 是否认领
    virtual String execute(const String& tool, const cJSON* arguments, bool approved);
    virtual String needs_secret(const String& tool, const cJSON* arguments);   // 需要密钥时返回提示语
    virtual String execute_secret(const String& tool, const cJSON* arguments, bool approved,
                                  const String& secret);                      // 密钥只从这里进来
    virtual void hints(std::vector<Hint>& out);            // 帮助页：名称、当前值、示例说法
    virtual void resources(cJSON* list, int& remaining);   // discover(resources)
    virtual bool available(const String& profile);         // 命令目录：远端可用性
    virtual const char* command_risk(const String& command) const;
    virtual const char* search_text(const String& command) const;
    // 以下可选（默认什么都不做），用于事件与触发，见下文“事件与触发”
    virtual void triggers(std::vector<TriggerSpec>& out) const;   // 声明能发出的事件
    virtual bool watch(const String& target, String& normalized, String& error);  // 登记需要定时的事件
    virtual void unwatch(const String& target);
    virtual String confirm_summary(const String& tool, const cJSON* arguments) const;  // 自定义确认文字
    virtual void loop();                                     // UI 任务上的周期工作，要轻
    virtual const char* usage() const;    // 用法；没有 index() 时每次都进系统提示
    virtual const char* index() const;    // 系统提示里的一行索引（地址和主要动作），用法改由 discover 给出
    virtual bool present() const;         // 可选硬件或服务在不在；不在时不进系统提示
};
```

包分三类：

| 类型 | 例子 | 要实现的方法 |
|:--|:--|:--|
| 执行包 | `local` `wifi` `ssh` `tmux` `agents` `policy` | `manifest` `supports` `execute`，通常还有 `resources` |
| 命令目录包 | `linux` `git` `docker` `systemd` | `manifest`（含 `commands`、`availability_program`）、`available`、`command_risk`；不执行，命令经 `ssh` 包的 `exec` 运行 |
| 传输/底层 | `ops_ssh` `ops_tui` `ops_local` | 不是包，是包调用的底层适配器 |

## 4. 新增一个包：最小例子

`src/packages/sht30_package.cpp`（Grove 口接的温湿度模块；读数按第 5 节放在 `sensor://`，模块没插时 `present()` 为 false）：

```cpp
#include "../common.h"
#include "../registry.h"
#include "../ops_grove.h"  // 示意：grove::found、grove::sht30_read 按实际的驱动来写

namespace vat::registry {
namespace {

constexpr char kUsage[] = "温湿度模块：read sensor://sht30/temperature 或 sensor://sht30/humidity。";

class Sht30Package : public Package {
public:
    const Manifest& manifest() const override {
        static const Manifest m{"sht30", "Grove 温湿度模块 SHT30", "温度 湿度 温湿度 sht30", {}};
        return m;
    }
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "read sensor://sht30/temperature|humidity"; }
    bool present() const override { return grove::found(0x44); }
    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "read" && json_str(arguments, "target").startsWith("sensor://sht30/");
    }
    String execute(const String&, const cJSON* arguments, bool) override {
        if (!present()) return "ERROR: 未接上 SHT30（Grove 口没有检测到 0x44）";
        return grove::sht30_read(json_str(arguments, "target").substring(strlen("sensor://sht30/")));
    }
};

}  // namespace

Package* make_sht30_package() { return new Sht30Package(); }

}  // namespace vat::registry
```

然后在 `src/packages/builtin.cpp` 里声明并注册一行：

```cpp
Package* make_sht30_package();
...
add(make_sht30_package());
```

注册是**显式**的：不做目录扫描、不动态加载，所有能力在 `builtin.cpp` 里一目了然，可审计。重复的包 ID 会被拒绝（运行日志记 `package_duplicate`）。

### 模型怎么知道用法 `usage()` `index()` `present()`

系统提示 = `agent_prompt.h` 的核心规矩 + 每个包的一段 + 收尾规矩，请求时逐段从 Flash 发出。每个包三选一：

| 包的声明 | 进系统提示的是 | 用法从哪来 | 适合 |
|:--|:--|:--|:--|
| 只有 `usage()` | 完整用法（常驻） | 系统提示 | 跨包都会用到、会改变做法的约定（SSH、终端、本机、调试、代码模式） |
| `usage()` + `index()` | 一行索引：包名、简介、地址和主要动作 | `discover`（capabilities，query 写包名） | 只在相关任务里用到的包（蓝牙、红外、程序、界面……） |
| `present()` 返回 false | 什么都不进 | `discover` 标明「未接上」 | 可选硬件没接、服务没配 Key |

```cpp
constexpr char kUsage[] = "……完整用法……";

class WifiPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
    const char* index() const override { return "扫描 read device://local/wifi/scan，连接 session(connect, device://local/wifi/网络名)……"; }
```

- 索引行要写全这个包的**主要动作**和内容格式：只写一半，模型会去猜（0.7.4 上机撞到过：只写了 install，模型去「安装」一个已在卡上的包）。
- 只放索引时模型很少主动去查：会改变做法的能力（如代码模式）必须常驻。
- 文本必须 JSON 安全：不含英文双引号、反斜杠和控制字符（引号用“”）；`tools/check_usage.py` 检查，不合格的在注册时被丢弃（运行日志 `package_usage_unsafe`）。
- 地址必须符合第 5 节，`tools/check_addresses.py` 检查。
- 改完先用[提示词实验台](E2E.zh-CN.md#提示词实验台)和 `tools/agent_lab.py` 跑（按同样规则从源码拼提示词，不用刷机），不退步再上实机。

### 帮助页条目 `hints()`

每个包都要提供帮助页条目：`{名称, 当前值, 示例说法}`。有当前值的条目出现在“设置”页，没有的出现在“能做的事”页。示例说法要写成用户真的会说的一句话，比如“搜索附近的 WiFi”。新包加上之后，帮助页会自动出现对应的条目。

### 事件与触发 `triggers()` / `watch()` / `unwatch()`

包注意到某件事时，用核心的事件总线发出一个 target：`events::emit("time://every/07:00")`。谁接收（目前是 `program` 包的触发表）包不需要知道，也不能引用它。

- `triggers()`：声明能发出哪些事件（target 写法 + 一句说明），出现在 `discover(resources)` 里并标“可作触发”。
- `watch()`：别人要这个事件时调用。需要定时或开销的事件（如 `time://`）在这里开始计时，并可把写法规范化（`time://in/10m` → 绝对时刻）；返回 true 表示是本包的事件，`error` 为空即接受。每次都会发出的事件（如 `device://boot`）直接接受。
- `unwatch()`：不再需要时调用。

登记只放内存；登记方会在开机后重新登记。事件处理函数在发出事件的任务上运行，只能排队，不能做耗时工作。

### 流 `streams()` / `open_stream()` / `close_stream()` / `sample()`

不断变化的量（姿态、加速度、位置）做成流：程序用 `{"stream": target, "as": "a"}` 取最新值，再用 `{a.x}`。**URI 就是资源本身的**，同一个资源 `read` 读一次、`stream` 取最新值，不另设前缀；能不能当流用，只看包有没有在 `streams()` 里声明。

- `streams()`：声明 `{target, fields, unit, period_ms, description}`。`fields` 是空格分隔的字段名（`"x y z"`），就是程序里的 `{a.x}`；`period_ms` 是多久取一次新值。出现在 `discover(resources)` 里并标「流」。
- `open_stream()`：第一次有人读时调用，打开数据源（传感器、串口）；返回空字符串表示可以，否则返回原因。
- `sample()`：按 `period_ms` 在 **UI 任务**上调用，把最新值按 `fields` 的顺序填进 `StreamValue::v`。**必须很快、不阻塞、不分配内存**；暂时没有值（如 GPS 未定位）返回 false。
- `close_stream()`：10 秒没人读时调用，关掉数据源。

注册中心负责打开、按周期采样、缓存最新值、空闲关闭；任何任务读到的都是缓存，不会和 UI 任务同时访问 I2C。现有的流：`sensor://imu/acceleration`、`sensor://imu/gyro`、`sensor://imu/orientation`（hardware 包），`sensor://gps/position`（gps 包，有 Cap 时）。

### 确认文字 `confirm_summary()`

默认的确认文字是“动作 + target + 内容前 60 字”。内容是 JSON 之类人读不懂的东西时（如保存程序），返回一句人话说明要确认什么。

### 周期工作 `loop()`

在 UI 任务每圈调用，只做很轻的事（检查队列、到点判断）；耗时工作交给 worker 的回调任务（`worker::Kind::Callback`）。

### 密钥 `needs_secret()` / `execute_secret()`

需要密码、口令或 token 的操作，由包在 `needs_secret()` 里**按确定性规则**判断并返回提示语（例如“开发机 · alice 的 sudo 密码”），不需要时返回空。判断依据只能是代码和状态：网络是否保存过、是否加密、`sudo -n` 是否成功、私钥是否被接受。不能由模型来判断。

设备随后暂停任务，弹出掩码输入框。用户输入后，值只通过 `execute_secret()` 的 `secret` 参数传进来，不会出现在参数 JSON、日志、任务记录或模型上下文中。取消输入时，这一步记为未执行，任务继续。

## 5. 资源地址

> 硬约束，和 `CLAUDE.md` 同级。`tools/check_addresses.py`（`make host-test` 运行）从本章的「协议头登记表」「动作词表」「已废弃的写法」三张表读规则，检查各包的 `kUsage`、`index()`、`supports()` 和 `prog://help` 里的地址；改规则就改这三张表。
> 来源：2026-11-13 盘点出 22 种协议头，动作的位置、列表、设置、事件、本机硬件的写法各不相同（同一个包里 `pkg://install/名字` 和 `pkg://名字/enable` 并存；LoRa 包专门接受模型猜的 `device://local/lora`）。

模型只有七个工具，能做的事全靠地址区分；程序里的 `vat.read(地址)` 也用同一套地址。地址就是 FeiyuOS 对模型和程序的接口，要像函数名一样有规则。

### 5.1 结构

```
协议头://资源路径[/动作][?参数]
```

| 部分 | 规则 | 例 |
|:--|:--|:--|
| 协议头 | 小写英文一个词，必须在 5.5 的登记表里 | `device` `prog` `ssh` |
| 资源路径 | 从大到小用 `/` 分层；名字照用户的叫法（可以是中文），编号用阿拉伯数字；不翻译、不改写 | `device://local/display/text`、`prog://工具箱`、`trigger://3` |
| 动作 | 只能是**最后一段**，必须在 5.2 的动作词表里；对整个集合的动作接在集合后，对某一个对象的动作接在对象后 | `trigger://new`、`trigger://3/delete`、`pkg://天气/install` |
| 参数 | `?键=值&…`，只用于读的筛选或不改变对象身份的选项；`url=` 放最后 | `memory://history?q=日志`、`prog://工具箱?lines=40-80` |

- 要写的值放在 `content`，不放进地址（反例：`device://local/ir/电视/on`，开关是动作 `power`）。
- 读用 `read`、改用 `write`、运行用 `exec`，地址里不出现 `get`、`set` 这类段。
- `read` 永远不改变任何状态（反例：`read pkg://catalog` 曾经会把当前目录换掉）。

### 5.2 动作词表

| 动作 | 工具 | 含义 |
|:--|:--|:--|
| `list` | read | 列出一个集合 |
| `status` | read | 当前状态 |
| `help` | read | 用法说明 |
| `new` | write | 新建（content 是新对象） |
| `delete` | write | 删除 |
| `clean` | write | 清理集合里失效的 |
| `enable` | write | 启用 |
| `disable` | write | 停用 |
| `pause` | write | 暂停 |
| `resume` | write | 继续 |
| `open` | write | 打开（界面、面板） |
| `close` | write | 关闭 |
| `install` | write | 安装 |
| `update` | write | 更新 |
| `rollback` | write | 退回上一版 |
| `scan` | read 或 write | 扫描：附近的网络和设备用 read；重新扫描卡用 write |
| `send` | write | 发出（content 是要发的） |
| `confirm` | write | 确认是否生效 |
| `try` | write | 换下一种试 |
| `patch` | write | 局部修改 |
| `clear` | write | 清空（屏幕） |
| `run` | write | 运行一段代码（代码模式） |

新增动作：先在本表加一行，写明配哪个工具。`help` 和 `run` 也是名词，可以在中间：`prog://help/examples` 是帮助的分页，`debug://run/编号` 是一次运行。

### 5.3 公用的名字

| 名字 | 含义 |
|:--|:--|
| `list` `status` `help` | 见动作词表，在任何协议头下都是同一个意思 |
| `event/名称` | 事件，只用作触发条件：`<来源>/event/<名称>`，事件的数据字段写在该包的用法里 |
| `local` | `device://local/` 下其余的（状态、电量、剪贴板）、其余 `memory://*` | 事件：`device://local/event/boot`（核心在启动就绪时发出）；注册在 lora、ble、wifi、ir、display、hardware 之后 |

### 5.4 按类别归属

| 类别 | 地址 | 例 |
|:--|:--|:--|
| **测量出来的读数**（本机模块、外接模块、别处的传感器都一样） | `sensor://名称/指标` | `sensor://imu/acceleration`、`sensor://gps/location`、`sensor://grove`、`sensor://书桌/temperature` |
| **本机自身的状态、部件和通道** | `device://local/…` | `device://local/battery`、`device://local/led`、`device://local/display/text`、`device://local/lora/send`、`device://local/ble/scan`、`device://local/wifi/scan`、`device://local/ir/电视/power` |
| **别处能控制的设备** | `device://位置/属性` | `device://客厅/light-1/power` |
| **设置页里能改的偏好** | `settings://…` | `settings://brightness`、`settings://volume`、`settings://lora/frequency`、`settings://ble/power` |
| **事件** | `<来源>/event/<名称>` | `device://local/event/boot`、`prog://event/failed`、`device://local/ble/event/connected`、`ui://event/panel/名字` |

时间计划（`time://every/07:00`）是触发条件，不是事件，用 `time://`。

**可选硬件**（Cap 扩展板上的 LoRa、GPS，要 Key 的高德……）由包实现 `present()` 说明在不在：不在时，提示词里不出现它的索引行和用法；`discover` 仍能查到并标明「未接上」；调用时返回明确的错误。提示词每次请求现拼，接上以后下一句话就生效。

### 5.5 协议头登记表

| 协议头 | 归属的包 | 管什么 |
|:--|:--|:--|
| `device` | local、display、hardware、lora、ble、wifi、ir | `device://local/…` 本机的状态、部件和通道；`device://位置/…` 别处的设备 |
| `sensor` | hardware、gps、grove | 读数：本机模块 `imu` `gps` `grove`，别处的 `sensor://位置/指标` |
| `settings` | `settings://`（read）；`settings://brightness`、`timeout`、`volume`、`context`、`animation`、`statusbar`、`font`、`lora/…`、`ble/power`、`open/<页>`（write） | 设置页里能改的偏好；`open/key/<提供方>` 弹出掩码输入框，Key 不经过模型；`open/portal`、`open/webtoken` 每次确认。全部在 UI 任务里执行 |
| `memory` | memory、local | 长上下文、历史、案卷、笔记、最近任务 |
| `file` | files | 卡上的文件 `file:///vat/…` |
| `prog` | program | 程序 |
| `trigger` | program | 触发 |
| `pkg` | pkg | 卡上程序和扩展包的管理、在线目录 |
| `ui` | ui | 面板、本地应用（保存的面板或画布） |
| `gfx` | gfx | 画图：画布 `gfx://canvas/名字`、程序逐帧画 `gfx://frame`（程序里 `-- @uses gfx` 后 `gfx.*`）、`gfx://screen`、`gfx://measure` |
| `debug` | debug | 三条全局流 |
| `lua` | lua | 代码模式 `lua://run` |
| `timer` | `timer://in/…` `timer://at/…` 的 write；`timer://list` 读；`timer://编号/delete`、`timer://all/delete` | 存 `/vat/things/timers/`；到点全屏提醒 + 提示音，不做别的事；关机期间错过的开机补发。触发的时间条件：`time://at/…` `in/…` `every/…` `weekday/…` `weekend/…` |
| `time` | timer | 触发的时间条件 |
| `ssh` | ssh | 远端主机、文件、命令 |
| `tmux` | tmux | 远端 tmux 会话 |
| `tui` | tmux | 当前终端 `tui://current` |
| `agent` | agents、tmux | 远程 Agent `agent://current` |
| `web` | web | 联网搜索、网页 |
| `http` | http | 接口 |
| `https` | http | 接口 |
| `place` | `place://` 的 here、nearby、search、weather、route（read） | 高德，`present()` 看有没有 Key；路线用 v5（步行、驾车、骑行、电动车、公交）；没有 GPS 时按 IP 估算并注明 |
| `ble` | `device://local/ble/…` 的 read/write；事件 `device://local/ble/event/…`；开关在 `settings://ble/power` | 扫描、广播、连接读写、订阅通知、当蓝牙键盘；注册在 `local` 之前 |
| `pkg` | `pkg://list` `pkg://catalog` 的 read；`pkg://scan`、`pkg://名字/enable|disable|delete|install|update|rollback`、`pkg://new?url=…` 的 write | 卡上程序与扩展包的管理、在线目录 |
| `lua` | `lua://run` 的 write | 代码模式：一段 Lua 一次跑完；要确认的调用在设备上逐个问用户 |
| `ask` | ask | 程序里问一句 |

新增协议头：先在本表登记，写明归属的包和管什么。

### 5.6 已废弃的写法

改地址时不留旧写法（不做「新旧都认」的兼容层）：调用旧写法不执行，直接失败并给出新写法（`address::retired()`，和开机改写同一张表），卡上已保存的程序、`@uses`、已确认的权限、触发、面板应用在开机时按本表一次性改写（记一条 `address_migrated` 事件，只做一次）。

| 旧写法 | 新写法 |
|:--|:--|
| `pkg://install/` | `pkg://名字/install` |
| `pkg://update/` | `pkg://名字/update` |
| `pkg://rollback/` | `pkg://名字/rollback` |
| `pkg://install?` | `pkg://new?` |
| `timer://cancel` | `timer://编号/delete`，全部 `timer://all/delete` |
| `ui://apps` | 列出用 `ui://apps/list` |
| `memory://cases` | 列出用 `memory://cases/list` |
| `memory://out` | 列出用 `memory://out/list` |
| `device://local/settings` | `settings://` |
| `device://local/display/brightness` | `settings://brightness` |
| `device://local/display/timeout` | `settings://timeout` |
| `device://local/audio/volume` | `settings://volume` |
| `device://local/context/length` | `settings://context` |
| `lora://settings/` | `settings://lora/` |
| `lora://` | `device://local/lora/` |
| `ble://power` | `settings://ble/power` |
| `ble://devices` | `device://local/ble/devices/list` |
| `ble://` | `device://local/ble/` |
| `wifi://` | `device://local/wifi/` |
| `ir://` | `device://local/ir/` |
| `device://boot` | `device://local/event/boot` |
| `device://lora` | `device://local/lora/` |
| `settings://open/` | `settings://页名/open`，Key 输入框 `settings://key/提供方/open` |
| `ui://canvas/` | `gfx://canvas/` |

第一列以 `/` 或 `?` 结尾的是前缀，其余的是整个地址（`ui://apps` 不拦 `ui://apps/名字`）。另有两条不在表里检查：面板选择的事件由 `ui://panel/名字` 改为 `ui://event/panel/名字`（`ui://panel/名字` 本身仍是面板）；LoRa 包原来额外接受的猜测写法（`device://lora/…`）去掉。

### 5.7 新增或修改地址的流程

1. 先改本章（登记表、动作词表、废弃表），说明理由；
2. 改包的 `supports()`、`kUsage`、`index()`；
3. `make host-test`（含 `check_addresses.py`）通过；
4. 提示词实验（`tools/prompt_lab.py --device`）和实验台（`tools/agent_lab.py`）不退步；
5. 改了已有地址：在废弃表里加一行，开机迁移照表改写卡上数据。

## 6. 必须遵守的规则

**认领**
- `supports()` 只按 target scheme 和 session action 精确认领，不能接管不属于自己的 target。不同包的认领范围不得重叠。
- 未知或不完整的 target 返回清楚的错误文本，不要猜一个默认值去执行。

**密钥**
- 任何密码、口令、token **只能**通过 `needs_secret()` 加设备上的输入框获取。**禁止从模型参数里读取密钥**：即使模型在 `content` 里给了密码也要忽略（参考 `wifi` 包）。
- 密钥只用于这一次操作：通过 stdin 传给远端，或者保存到它本来就该在的位置（如 `secrets.json` 里的 WiFi 密码）。不能写日志，不能出现在返回结果里，不能缓存。
- 用完立即清空持有密钥的变量。

**安全**
- 写入、删除、提权、重启、切换网络、任何物理动作都必须经过 Policy 确认。默认情况下 `write` 就是 ask；如果新包需要不同的规则，改 `src/policy.cpp` 并说明理由。
- 远端命令一律用 argv，经 `shell_join` 转义；禁止把模型给的字符串直接拼进 shell。
- manifest、resources、返回结果、日志里都不能出现密码、私钥、token、完整的主机配置。
- 凭据路径（`/.ssh/`、Key 文件、`secrets.json` 等）由 Policy 硬拒绝；新增敏感路径时加进 `ops_ssh.cpp` 的 `kSensitive`。

**资源与稳定性**：遵守 [固件开发约束](FIRMWARE_RULES.zh-CN.md)。重点：

- 每个 I/O 都要设超时；
- 返回结果要限长；
- 不在执行过程中长时间占用大块内存；
- 不在录音期间写板载 Flash。

**结果**
- `execute()` 返回给人看的文本或紧凑 JSON；失败也返回文本，不要抛出，也不要假装成功。
- **地址按第 5 节写**：先找已有的协议头和约定，再考虑新造（曾新造 `gps://`，模型按约定写成了 `sensor://GPS/location`，后来统一为 `sensor://gps/…`：模型按「是什么」而不是「在哪」猜地址）。
- **持久数据放 `/vat/things/<包名>/`**：设备档案、昵称、上次状态、验证记录等，一个对象一个 JSON 文件（或一个小的汇总文件），用 `storage::write_text` 原子写入；需要模型认得的名字（别名）通过上下文带入，不让模型猜。临时数据放 `/vat/cache/`。
- **返回清单时必须编号**：`1. 名称  关键属性`，一项一行，最多 12 项，整体控制在 600 字节以内（对话记忆中每条工具结果的上限）。用户之后说“第 N 个”时，模型就按这个编号找。参考 `wifi://scan`、tmux 会话列表、`discover`。
- Agent 页显示的是模型的一句话总结，原始结果进 RAW 详情层，所以结果要“机器可读、关键信息在前”。

## 7. 测试要求

每个新包至少在 `tools/vat_e2e.py` 中补充对应用例（现有包的用例在 S2–S6）：

1. `discover` 能找到它（`registry_discover_packages` 的期望列表加上新 ID）。
2. 典型语音/文字指令被路由到它，参数正确。
3. 读操作的结果和真实状态一致。
4. 写或物理动作先挂起（`awaiting_approval`）；拒绝后状态不变；批准后状态改变。
5. 未知或残缺的 target 不会被猜测执行。
6. 需要密钥的操作进入 `awaiting_secret`；通过 `/api/secret` 提交后成功；提交错误的密钥时返回明确错误；取消输入时不执行；在运行日志、对话、操作记录、时间线、任务记录中都找不到这个密钥（参考 S7 的 `secret_never_stored`）。
7. 帮助页能看到新包的条目。
8. 返回清单的能力：加一条“列出 → 说‘第 N 个’操作”的用例，验证选中的正是清单里第 N 项（参考 S6 的 `ordinal_*`）。
9. 跑完对应阶段后 `no_unexpected_reboot` 必须通过。

只有编译通过不算完成，必须在实物上跑通对应的 E2E 阶段。涉及按键、录音、屏幕或物理设备的，还要按 [人工验收](MANUAL_ACCEPTANCE.zh-CN.md) 补人工结果。

## 8. 现有包一览

| ID | 认领范围 | 说明 |
|:--|:--|:--|
| `memory` | `memory://out` `memory://history` `memory://cases` 的 read；`memory://cases/…` 的 write；session attach/disconnect `memory://cases/…` | 完整输出分页与 grep、历史检索、案卷；需要 TF 卡（注册在 `local` 之前） |
| `settings` | `settings://`（read）；`settings://brightness`、`timeout`、`volume`、`context`、`animation`、`statusbar`、`font`、`lora/…`、`ble/power`、`open/<页>`（write） | 设置页里能改的偏好；`open/key/<提供方>` 弹出掩码输入框，Key 不经过模型；`open/portal`、`open/webtoken` 每次确认。全部在 UI 任务里执行 |
| `local` | `device://local/` 下其余的（状态、电量、剪贴板）、其余 `memory://*` | 事件：`device://local/event/boot`（核心在启动就绪时发出）；注册在 lora、ble、wifi、ir、display、hardware 之后 |
| `ssh` | `ssh://…` 的 read/write/exec；session connect/disconnect/list（`ssh://` 或空） | libssh；输出在远端限长；connect 接入该主机的 `vat-shell` 终端；私钥未被接受时请求一次登录密码并装好设备公钥；需要密码的 sudo 请求 sudo 密码，经 stdin 传入 |
| `tmux` | observe/input；read `tui://` `agent://`；session create/attach、`tmux://` list/connect | 远端 tmux 观察与输入 |
| `agents` | session start_agent/resume_agent/interrupt/approve/reject | Codex / Claude / Pi 只读启动 |
| `policy` | session set_policy | 修改目标策略（本身需要确认） |
| `wifi` | `device://local/wifi/scan` `device://local/wifi/status` 读；session connect `device://local/wifi/网络名`（保存并切换） | 未保存的加密网络请求密码，模型给的密码一律忽略；注册在 `local` 之前 |
| `web` | `web://search?q=` `web://page?url=` 的 read | DeepSeek Search / Fetch；网页整页存成 out#N |
| `lora` | `device://local/lora/…` 的 read/write；设置在 `settings://lora/…` | Cap LoRa-1262，`present()` 看 Cap 在不在；默认关闭（芯片保持复位）；发射需频点 + 天线确认 + 每次确认；注册在 `local` 之前 |
| `gps` | `sensor://gps/…` 的 read | Cap 上的 GNSS，`present()` 看 Cap 在不在；只收不发，默认开启 |
| `display` | `device://local/display/{text,qr,image,banner,clear}` 的 write | 全屏浮层：大字、二维码、/vat 下的小图片、通知横幅；接受常见别名（注册在 `local` 之前） |
| `hardware` | `device://local/led`、`device://local/audio/beep` 的 write；`sensor://imu/…` 的 read | RGB 灯、提示音（录音中不响）、IMU 姿态；LED 与提示音免确认（注册在 `local` 之前） |
| `files` | `file:///vat/…` 的 read/write | 读目录即列出；写需确认；`secrets.json`、`/vat/ssh`、隐藏文件一律拒绝 |
| `timer` | `timer://in/…` `timer://at/…` 的 write；`timer://list` 读；`timer://编号/delete`、`timer://all/delete` | 存 `/vat/things/timers/`；到点全屏提醒 + 提示音，不做别的事；关机期间错过的开机补发。触发的时间条件：`time://at/…` `in/…` `every/…` `weekday/…` `weekend/…` |
| `grove` | `sensor://grove/…` 的 read | 扫描 Ex_I2C，按地址推测型号 |
| `ir` | `device://local/ir/<电器名>/<动作>` 的 write；`device://local/ir/list`、`device://local/ir/send` | 内置电视码组；发码后由用户确认是否生效（`confirm`），没反应换下一组（`try`）；档案存 `/vat/things/ir/`；注册在 `local` 之前 |
| `place` | `place://` 的 here、nearby、search、weather、route（read） | 高德，`present()` 看有没有 Key；路线用 v5（步行、驾车、骑行、电动车、公交）；没有 GPS 时按 IP 估算并注明 |
| `program` | `prog://…` 的 read/write/exec；`trigger://…` 的 read/write | Lua 程序与触发表；保存时一次确认；见 [程序与触发](PROGRAMS.zh-CN.md) |
| `ask` | `ask://` 的 write/read | 问大肥鱼一句，回复为结果；无人值守（需确认的操作自动拒绝）；已有对话时拒绝 |
| `linux` `git` `docker` `systemd` | 只参与 discover | 命令目录、风险等级、远端可用性、`--help` |

## 9. 方向

可以按本手册接入的能力和推荐顺序，见 [扩展方向](EXTENSIONS.zh-CN.md)。
