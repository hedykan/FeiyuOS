# 扩展包开发手册

本文是给设备新增能力的**唯一方式**。无论是本小组开发还是外部 PR，都必须遵守。

## 1. 不变的内核：七个工具

模型永远只看到七个原子工具：

```text
discover  read  write  exec  observe  input  session
```

新增能力**不允许**增加第八个工具，也不允许改动工具 schema（`src/agent_prompt.h` 里的 `kToolsJson`）。能力通过 **target 资源地址** 区分，由扩展包认领：

```text
ssh://开发机/etc/hostname      tui://current        tmux://开发机/work
device://local/display/brightness   memory://notes   wifi://scan
sensor://desk/temperature      gpio://esp-kitchen/2  ir://living-room/tv
```

只有七个动作确实表达不了的新语义，才考虑协议大版本升级。这需要单独提议，不能夹带在功能 PR 里。

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
};
```

包分三类：

| 类型 | 例子 | 要实现的方法 |
|:--|:--|:--|
| 执行包 | `local` `wifi` `ssh` `tmux` `agents` `policy` | `manifest` `supports` `execute`，通常还有 `resources` |
| 命令目录包 | `linux` `git` `docker` `systemd` | `manifest`（含 `commands`、`availability_program`）、`available`、`command_risk`；不执行，命令经 `ssh` 包的 `exec` 运行 |
| 传输/底层 | `ops_ssh` `ops_tui` `ops_local` | 不是包，是包调用的底层适配器 |

## 4. 新增一个包：最小例子

`src/packages/battery_package.cpp`：

```cpp
#include <M5Cardputer.h>

#include "../common.h"
#include "../registry.h"

namespace vat::registry {
namespace {

class BatteryPackage : public Package {
public:
    const Manifest& manifest() const override {
        static const Manifest m{"battery", "电池电量与充电状态", "电池 电量 充电 battery", {}};
        return m;
    }
    bool supports(const String& tool, const cJSON* arguments) const override {
        return tool == "read" && json_str(arguments, "target").startsWith("battery://");
    }
    String execute(const String&, const cJSON*, bool) override {
        return "电量 " + String(M5Cardputer.Power.getBatteryLevel()) + "%";
    }
    void resources(cJSON* list, int& remaining) override {
        if (remaining-- <= 0) return;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "target", "battery://local");
        cJSON_AddStringToObject(item, "kind", "battery");
        cJSON_AddItemToArray(list, item);
    }
};

}  // namespace

Package* make_battery_package() { return new BatteryPackage(); }

}  // namespace vat::registry
```

然后在 `src/packages/builtin.cpp` 里声明并注册一行：

```cpp
Package* make_battery_package();
...
add(make_battery_package());
```

注册是**显式**的：不做目录扫描、不动态加载，所有能力在 `builtin.cpp` 里一目了然，可审计。重复的包 ID 会被拒绝（运行日志记 `package_duplicate`）。

### 模型怎么知道用法 `usage()`

包在自己的文件里声明一段 `kUsage`，由 `usage()` 返回；系统提示 = `agent_prompt.h` 的核心规则 + 所有包的 `usage()`（按 `builtin.cpp` 注册顺序），请求时逐段从 Flash 发出，不在内存里拼接：

```cpp
// Always in the system prompt (keep it short; details belong in discover).
constexpr char kUsage[] = "WiFi 规范：搜索、扫描附近 WiFi 用 read target=wifi://scan；……";

class WifiPackage : public Package {
public:
    const char* usage() const override { return kUsage; }
```

- 只写模型从 discover 学不到、又必须在第一次调用前就知道的东西：target 写法、内容格式、容易猜错的地方。能等 discover 再说的细节放进 `resources()` 或能力说明。
- 文本必须 JSON 安全：不含英文双引号、反斜杠和控制字符（引号用“”）；不合格的 `usage()` 在注册时被丢弃，运行日志记 `package_usage_unsafe`。
- 改完先用[提示词实验台](E2E.zh-CN.md#提示词实验台)跑意图用例（实验台按同样规则从源码拼出提示词，不用刷机），不退步再上实机。

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

## 5. 必须遵守的规则

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
- **target 先找已有约定再新造 scheme**：传感器一律 `sensor://名称/指标`（GPS 是 `sensor://gps/location`，曾新造 `gps://` 而模型按约定写了 `sensor://GPS/location`）；设备状态 `device://`；本机资源 `device://local/`。
- **持久数据放 `/vat/things/<包名>/`**：设备档案、昵称、上次状态、验证记录等，一个对象一个 JSON 文件（或一个小的汇总文件），用 `storage::write_text` 原子写入；需要模型认得的名字（别名）通过上下文带入，不让模型猜。临时数据放 `/vat/cache/`。
- **返回清单时必须编号**：`1. 名称  关键属性`，一项一行，最多 12 项，整体控制在 600 字节以内（对话记忆中每条工具结果的上限）。用户之后说“第 N 个”时，模型就按这个编号找。参考 `wifi://scan`、tmux 会话列表、`discover`。
- Agent 页显示的是模型的一句话总结，原始结果进 RAW 详情层，所以结果要“机器可读、关键信息在前”。

## 6. 测试要求

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

## 7. 现有包一览

| ID | 认领范围 | 说明 |
|:--|:--|:--|
| `memory` | `memory://out` `memory://history` `memory://cases` 的 read；`memory://cases/…` 的 write；session attach/disconnect `memory://cases/…` | 完整输出分页与 grep、历史检索、案卷；需要 TF 卡（注册在 `local` 之前） |
| `local` | `device://local/*`、其余 `memory://*` | 状态、亮度、息屏、音量、剪贴板、记忆笔记、最近任务。事件：`device://boot`（核心在启动就绪时发出） |
| `ssh` | `ssh://…` 的 read/write/exec；session connect/disconnect/list（`ssh://` 或空） | libssh；输出在远端限长；connect 接入该主机的 `vat-shell` 终端；私钥未被接受时请求一次登录密码并装好设备公钥；需要密码的 sudo 请求 sudo 密码，经 stdin 传入 |
| `tmux` | observe/input；read `tui://` `agent://`；session create/attach、`tmux://` list/connect | 远端 tmux 观察与输入 |
| `agents` | session start_agent/resume_agent/interrupt/approve/reject | Codex / Claude / Pi 只读启动 |
| `policy` | session set_policy | 修改目标策略（本身需要确认） |
| `wifi` | `wifi://scan` `wifi://status` 读；`wifi://SSID` 写或 session connect `wifi://名称`（保存并切换） | 未保存的加密网络请求密码，模型给的密码一律忽略 |
| `web` | `web://search?q=` `web://page?url=` 的 read | DeepSeek Search / Fetch；网页整页存成 out#N |
| `lora` | `lora://` 的 read/write；session connect/disconnect `lora://` | Cap LoRa-1262；默认关闭（芯片保持复位）；发射需频点 + 天线确认 + 每次确认 |
| `gps` | `sensor://gps/…` 的 read；session connect/disconnect `sensor://gps` | Cap 上的 GNSS，只收不发，默认开启 |
| `display` | `device://local/display/{text,qr,image,banner,clear}` 的 write | 全屏浮层：大字、二维码、/vat 下的小图片、通知横幅；接受常见别名（注册在 `local` 之前） |
| `hardware` | `device://local/led`、`device://local/audio/beep` 的 write；`sensor://imu/…` 的 read | RGB 灯、提示音（录音中不响）、IMU 姿态；LED 与提示音免确认（注册在 `local` 之前） |
| `files` | `file:///vat/…` 的 read/write | 读目录即列出；写需确认；`secrets.json`、`/vat/ssh`、隐藏文件一律拒绝 |
| `timer` | `timer://in/…` `timer://at/…` 的 write；`timer://list` 读；`timer://cancel/…` | 存 `/vat/things/timers/`；到点全屏提醒 + 提示音；关机期间错过的开机补发。事件：`time://at/…` `in/…` `every/…` `weekday/…` `weekend/…` |
| `grove` | `sensor://grove/…` 的 read | 扫描 Ex_I2C，按地址推测型号 |
| `ir` | `ir://<电器名>/<动作>` 的 write | 内置电视码组；发码后由用户确认是否生效，没反应换下一组；档案存 `/vat/things/ir/` |
| `place` | `place://` 的 here、nearby、search、weather、route（read） | 高德；路线用 v5（步行、驾车、骑行、电动车、公交）；没有 GPS 时按 IP 估算并注明 |
| `program` | `prog://…` 的 read/write/exec；`trigger://…` 的 read/write | 程序（JSON 步骤，可互相调用）与触发表；保存时一次确认，运行在 worker 回调任务里；见 [程序与触发](PROGRAMS.zh-CN.md) |
| `ask` | `ask://` 的 write/read | 问大肥鱼一句，回复为结果；无人值守（需确认的操作自动拒绝）；已有对话时拒绝 |
| `linux` `git` `docker` `systemd` | 只参与 discover | 命令目录、风险等级、远端可用性、`--help` |

## 8. 方向

可以按本手册接入的能力和推荐顺序，见 [扩展方向](EXTENSIONS.zh-CN.md)。
