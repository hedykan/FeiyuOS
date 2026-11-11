#!/usr/bin/env python3
"""Agent lab: the device's whole agent loop, replayed on this computer, with
every round recorded (the model's thinking, text, tool calls, results), so the
way FeiyuOS's agent reasons, tests and fixes can be watched and compared.

Unlike prompt_lab (first operation only), the loop runs to the end: tools are
executed against a simulated device (tools/lab/harness.lua runs the Lua
programs with the device's limits; LED, screen, files, triggers are simulated;
read-only SSH commands run here for real, other SSH commands are simulated).

    python3 tools/agent_lab.py                          # all cases, variant "base" (the firmware today)
    python3 tools/agent_lab.py --variant base,big,think --only lua_ --repeat 2
    python3 tools/agent_lab.py --list-variants

Cases: tests/lab/cases.json.  Variants: tests/lab/variants.json (steps, output
limit, reasoning effort, extra rules).  Output: logs/agent-lab-<time>/
(runs.jsonl, report.md).  The API key is read from ../.deepseek_key and never printed.
"""
from __future__ import annotations

import argparse
import concurrent.futures as futures
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import prompt_lab  # noqa: E402

SRC = ROOT / "firmware/standalone_agent/src"
HARNESS = ROOT / "tools/lab/harness.lua"
CASES = ROOT / "tests/lab/cases.json"
VARIANTS = ROOT / "tests/lab/variants.json"
INLINE_RESULT = 4200   # context.cpp kInlineResult
HEAD_IN_CONTEXT = 2000  # context.cpp kHeadInContext
PAGE = 4096             # context.cpp kPage: one page of a filed result
MAX_OUTPUT = 2048       # lua_vm kMaxOutput
MAX_SOURCE = 16384      # lua_vm kMaxSource
# The device's Lua VM has 48 KB: measured on the device, 6.9 KB of source compiles and 8.3 KB does not.
MAX_COMPILE = 7500
TOO_BIG = "程序太大，设备的内存放不下（Lua 最多 48 KB，大约是 7 KB 源码）：先把写法收紧（去掉重复的代码和多余的注释）；还放不下，再把不需要共用数据的部分拆成另一个程序，用 vat.exec 调用"
READ_ONLY = {"df", "du", "uptime", "free", "ls", "cat", "head", "tail", "ps", "whoami", "hostname", "uname", "date",
             "pwd", "wc", "grep", "find", "stat", "id", "env", "echo", "which", "nproc", "lsblk", "top"}
# Read-only commands the lab really runs, always inside the case's sandbox directory; nothing else touches the machine.
SANDBOXED = {"ls", "cat", "head", "tail", "wc", "grep", "find", "stat", "du"}
CANNED = {
    "df": "Filesystem      Size  Used Avail Use% Mounted on\n/dev/sda1       100G   45G   55G  45% /\n",
    "uptime": " 09:30:00 up 12 days,  3:04,  1 user,  load average: 0.12, 0.08, 0.05\n",
    "free": "               total        used        free      shared  buff/cache   available\n"
            "Mem:           7.7Gi       2.1Gi       3.9Gi        12Mi       1.7Gi       5.3Gi\nSwap:             0B          0B          0B\n",
    "whoami": "lab\n", "hostname": "lab-host\n", "uname": "Linux\n", "nproc": "4\n", "pwd": "/home/lab\n",
    "date": "Wed Nov 11 09:30:00 CST 2026\n", "id": "uid=1000(lab) gid=1000(lab) groups=1000(lab)\n",
}
SAFE_SUB = {"git": {"status", "log", "diff", "branch", "show"}, "docker": {"ps", "images", "logs", "inspect", "stats"},
            "systemctl": {"status", "is-active", "is-enabled", "list-units"}}


def cpp_raw(path: Path, name: str, tag: str) -> str:
    match = re.search(rf'{name}\[\] = R"{tag}\((.*?)\){tag}"', path.read_text(), re.S)
    return match.group(1) if match else ""


PROGRAM_PKG = SRC / "packages/program_package.cpp"
HELP = cpp_raw(PROGRAM_PKG, "kHelp", "HELP")
HELP_EXAMPLES = cpp_raw(PROGRAM_PKG, "kHelpExamples", "HELP")


# ---------------------------------------------------------------- program headers (lua_meta.cpp)

def program_header(source: str) -> tuple[dict, str]:
    """(header, problem): about / version / package / target / usage / inputs / uses, as lua_meta parses them."""
    h = {"about": "", "version": "", "package": "", "target": "", "usage": "", "inputs": [], "uses": []}
    for number, line in enumerate(source.split("\n"), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith("--"):
            break
        body = line[2:].strip()
        if not body.startswith("@"):
            continue
        key, _, value = body[1:].partition(" ")
        value = value.strip()
        bare = value.split(" --")[0].strip()
        where = f"第 {number} 行 @{key}："
        if key == "uses":
            tool, _, target = bare.partition(" ")
            if tool not in ("read", "write", "exec", "observe", "input", "session"):
                return h, where + "要写成 @uses 动作 目标"
            if "://" not in target:
                return h, where + "目标要写完整，如 ssh://开发机"
            h["uses"].append((tool, target.strip()))
        elif key == "input":
            name, _, fallback = bare.partition(" ")
            h["inputs"].append((name, fallback.strip()))
        elif key in ("about", "usage", "keywords"):
            h[key] = value
        elif key in ("version", "package", "target"):
            h[key] = bare
        else:
            return h, where + "不认识（可用 @about @input @uses @version @package @target @usage @keywords）"
    return h, ""


def covers(uses: list, tool: str, target: str) -> bool:
    for t, u in uses:
        if t != tool:
            continue
        if u.endswith("*") and target.startswith(u[:-1]):
            return True
        if target == u or target.startswith(u + "/") or target.startswith(u + "?"):
            return True
    return False


def filed(result: str, store: list) -> str:
    """context.cpp: a long result is filed as out#N, its head kept in the conversation."""
    if len(result.encode()) <= INLINE_RESULT:
        return result
    store.append(result)
    head = result.encode()[:HEAD_IN_CONTEXT].decode(errors="ignore")
    size = len(result.encode())
    return (f"{head}\n…（完整输出 out#{len(store)}，共 {size} 字节 {(size + PAGE - 1) // PAGE} 页；用 read memory://out/{len(store)}"
            f"?page=2 看下一页，?grep=关键词 只看匹配行，不要重新执行命令）")


# ---------------------------------------------------------------- the simulated device

class World:
    """One case's device: LED, screen, battery, IMU, files, programs, triggers, SSH."""

    def __init__(self, case: dict, workdir: Path, io: str = "v1", structured: bool = False, debug: bool = False):
        self.io = io  # E3: "v2" shows program runs with output, errors and the failing line apart
        self.structured = structured  # E2: programs get tables where a package has data, not sentences
        self.debug = debug  # D: every run is appended to three global streams, read with debug://
        self.runs: list[dict] = [dict(r) for r in case.get("history", [])]  # runs before this conversation
        self.debug_reads = 0
        self.case = case
        self.dir = workdir
        (workdir / "vat").mkdir(parents=True, exist_ok=True)
        (workdir / "ssh").mkdir(parents=True, exist_ok=True)
        self.led = "off"
        self.screen: list[str] = []
        self.clipboard = ""
        self.battery = case.get("battery", {"percent": 82, "voltage": 3.98, "usb": False})
        self.imu = case.get("imu", {"x": 0.02, "y": -0.31, "z": 0.95})
        self.programs: dict[str, str] = {}
        self.approved: dict[str, bool] = {}
        self.triggers: list[dict] = []
        self.timers: list[str] = []
        self.outs: list[str] = []
        self.clock_ms = 1000
        self.events: list[str] = []     # every state change, for the judge and the report
        self.ssh_fake = case.get("ssh", {})  # {"substring of the command": "output"} answered instead
        self.fail = case.get("fail", {})     # {"target prefix": "error"} forced failures
        self.reject = case.get("reject", False)  # the user answers N to confirmations
        for name, source in case.get("programs", {}).items():  # programs already on the card
            self.programs[name] = source
            self.approved[name] = True
        for path, text in case.get("files", {}).items():  # files already on the card
            f = self.vat_path("file://" + path)
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(text)

    # -- policy (policy.cpp + packages' reversible())
    def needs_confirm(self, tool: str, a: dict) -> bool:
        target = a.get("target", "")
        if tool in ("read", "observe", "input", "discover"):
            return False
        if tool == "session":
            return a.get("action") in ("approve", "reject", "set_policy")
        if tool == "exec":
            if target.startswith("prog://"):
                return False
            if target.startswith("ssh://"):
                return not self.read_only(a.get("program", ""), a.get("args", []))
            return True
        # write
        if target.startswith(("device://local/led", "device://local/display", "device://local/audio", "memory://",
                              "timer://", "ui://", "lua://run", "settings://")):
            return False
        if target.startswith("trigger://"):
            return target.endswith("/delete") or target.rstrip("/").endswith("clean")
        if target.startswith("prog://"):
            path = target[7:]
            return path.endswith("/delete") or bool(program_header(a.get("content", ""))[0]["uses"])
        if target.startswith("pkg://"):
            return True
        return True

    @staticmethod
    def read_only(program: str, args: list) -> bool:
        if program in SAFE_SUB:
            return bool(args) and args[0] in SAFE_SUB[program]
        return program in READ_ONLY and not any(x in " ".join(args) for x in (">", "|", ";", "&", "`", "$("))

    # -- one tool call: (result text, decision)
    def call(self, tool: str, a: dict, in_program: dict | None = None) -> tuple[str, str]:
        target = a.get("target", "")
        for prefix, error in self.fail.items():
            if target.startswith(prefix):
                return "ERROR: " + error, "allow"
        confirm = self.needs_confirm(tool, a)
        if in_program is not None:
            if confirm and not covers(in_program["uses"], tool, target):
                return (f"需要确认：在程序开头声明 -- @uses {tool} {target}，保存时由用户确认", "refused")
            if target.startswith(("http://", "https://")) and not covers(in_program["uses"], tool, target):
                return (f"联网要在程序开头声明 -- @uses {tool} {target}", "refused")
            decision = "allow"
        elif confirm:
            if self.reject:
                self.events.append(f"用户拒绝 {tool} {target}")
                return "用户拒绝了这一步（没有执行）", "rejected"
            decision = "approved"
        else:
            decision = "allow"
        try:
            return self.execute(tool, a), decision
        except Exception as error:  # noqa: BLE001 - a simulator bug shows in the trace
            return f"ERROR: 实验台模拟出错：{error}", decision

    def execute(self, tool: str, a: dict) -> str:
        target = a.get("target", "")
        content = a.get("content", "")
        if tool == "read":
            return self.read(target)
        if tool == "write":
            return self.write(target, content, a)
        if tool == "exec":
            if target.startswith("prog://"):
                # The input lines of a run: the stdin text, or the exec args (one line each).
                stdin = a.get("stdin") or "\n".join(str(x) for x in (a.get("args") or []))
                return self.run_program_call(target[7:], stdin)
            if target.startswith("ssh://"):
                return self.ssh(a.get("program", ""), a.get("args", []))
            return f"没有能力包可处理 exec：{target}"
        if tool == "observe":
            return "（实验台）终端：$ " if target.startswith(("tmux://", "tui://", "agent://")) else f"没有能力包可处理 observe：{target}"
        if tool == "session":
            return "（实验台）会话操作已执行"
        if tool == "input":
            return "（实验台）已输入"
        return f"没有能力包可处理 {tool}：{target}"

    def with_defaults(self, name: str, inputs: dict | None, stdin: str = "") -> tuple[dict, str]:
        """The inputs of a run started from the program page: what was given, else the declared default; an
        input with no default is asked for on the console, so it takes the next line of the keyboard input.
        Returns the inputs and what is left of the keyboard input."""
        filled = dict(inputs or {})
        lines = stdin.split("\n") if stdin else []
        for n, fallback in program_header(self.programs[name])[0]["inputs"]:
            if n in filled:
                continue
            filled[n] = fallback if fallback or not lines else lines.pop(0)
        return filled, "\n".join(lines)

    # -- T1: one hidden test; returns what went wrong, or ""
    def hidden_test(self, name: str, t: dict) -> str:
        for path, text in t.get("files", {}).items():
            f = self.vat_path("file://" + path)
            f.parent.mkdir(parents=True, exist_ok=True)
            if text is None:
                f.unlink(missing_ok=True)
            else:
                f.write_text(text)
        keep = (self.battery, self.imu, self.ssh_fake, self.fail)
        self.battery = t.get("battery", self.battery)
        self.imu = t.get("imu", self.imu)
        self.ssh_fake = t.get("ssh", self.ssh_fake)
        self.fail = t.get("fail", self.fail)
        self.led = "off"
        shown = len(self.screen)
        try:
            inputs, typed = self.with_defaults(name, t.get("input"), t.get("stdin", ""))
            run = self.run_source(self.programs[name], name, program_header(self.programs[name])[0]["uses"],
                                  typed, foreground=True, inputs=inputs, source_tag=None)
        finally:
            self.battery, self.imu, self.ssh_fake, self.fail = keep
        if not run["ok"]:
            return "跑不通：" + str(run["error"]).split("\n")[0][:120]
        # what the program printed and what it returned both count, as for the other program checks
        lines = [line.strip() for line in (run["stdout"] + "\n" + str(run["result"] or "")).split("\n")]
        at = 0
        for want in t.get("lines", []):  # these lines, in this order, each a whole line of the output
            try:
                at = lines.index(want, at) + 1
            except ValueError:
                return f"输出里没有一行「{want}」"
        for bad in t.get("lacks", []):
            if any(bad in line for line in lines[at:]):
                return f"不该再输出「{bad}」"
        screen = "\n".join(self.screen[shown:])
        for part in t.get("screen_has", []):
            if part not in screen:
                return f"屏幕上没有「{part}」（屏幕：{screen[:60]}）"
        if "led" in t and t["led"] not in self.led.lower():
            return f"灯是 {self.led}，要 {t['led']}"
        return ""

    # -- D: the three streams and memory
    STREAMS = {"stdin": "标准输入", "stdout": "标准输出", "stderr": "标准错误"}

    @staticmethod
    def run_head(r: dict) -> str:
        return (f"== {r['time']} #{r['id']} {r['prog']} 来源={r['source']} 结果={'成功' if r['ok'] else '失败'} "
                f"耗时={float(r.get('seconds') or 0):.1f}s 内存峰值={float(r.get('memory_kb') or 0):.0f}KB ==")

    def record_run(self, name: str, source_tag: str, run_id: int, ok: bool, stdin: str, out: str, err: str,
                   seconds: float, memory_kb) -> None:
        at = 9 * 3600 + 30 * 60 + 7 * len(self.runs)  # the lab's clock: 11-11 09:30 onwards
        stamp = f"11-11 {at // 3600:02d}:{at // 60 % 60:02d}:{at % 60:02d}"
        self.runs.append({"id": run_id, "time": stamp, "prog": name, "source": source_tag, "ok": ok,
                          "seconds": seconds, "memory_kb": memory_kb, "stdin": stdin, "stdout": out, "stderr": err})

    def next_run_id(self) -> int:
        return max([r["id"] for r in self.runs] + [100]) + 1

    def debug_read(self, target: str) -> str:
        self.debug_reads += 1
        path, _, query = target[len("debug://"):].partition("?")
        params = dict(pair.partition("=")[::2] for pair in query.split("&") if pair)
        path = path.strip("/")
        if path.startswith("run/"):
            wanted = re.sub(r"\D", "", path)
            for r in self.runs:
                if str(r["id"]) == wanted and not r.get("pending"):
                    return (self.run_head(r) + "\n【输入】\n" + (r["stdin"].rstrip() or "（无）") + "\n【输出】\n" +
                            (self.head_tail(r["stdout"], 512, 1536).rstrip() or "（无）") + "\n【错误输出】\n" +
                            (self.head_tail(r["stderr"], 512, 1536).rstrip() or "（无）"))
            return f"ERROR: 没有运行 #{wanted}（read debug://stdout 看最近的运行）"
        if path not in self.STREAMS:
            return ("ERROR: 用 debug://stdout、debug://stderr、debug://stdin（参数 n=最近几次、prog=程序名、failed=1 只看失败的）、"
                    "debug://run/编号；内存在 device://local/status 里")
        runs = [r for r in self.runs if not r.get("pending") and (not params.get("prog") or r["prog"] == params["prog"])
                and (params.get("failed") != "1" or not r["ok"])
                and (path == "stdout" or r[path].strip())]  # stdout has a head for every run: it is the run record
        if not runs:
            return f"{self.STREAMS[path]}流里没有符合条件的运行"
        n = max(1, min(20, int(re.sub(r"\D", "", params.get("n", "")) or 5)))
        shown = runs[-n:]
        body = "\n".join(self.run_head(r) + ("\n" + self.head_tail(r[path], 512, 1536).rstrip() if r[path].strip() else "")
                         for r in shown)
        return f"{self.STREAMS[path]}流 · 最近 {len(shown)} 次运行（符合条件的共 {len(runs)} 次，最新的在最后）\n{body}"

    def read(self, target: str) -> str:
        if self.debug and target.startswith("debug://"):
            return self.debug_read(target)
        if self.debug and target.startswith("prog://") and target.split("?")[0].endswith("/log"):
            return self.debug_read("debug://stdout?prog=" + target[7:].split("?")[0][:-4])  # one program's part of it
        if target == "prog://help":
            return HELP
        if target == "prog://help/examples":
            return HELP_EXAMPLES
        if target in ("prog://", "prog://list"):
            if not self.programs:
                return "还没有程序。"
            lines = []
            for name, source in self.programs.items():
                h, _ = program_header(source)
                sig = name + ("(" + ", ".join(n + ("=" + f if f else "") for n, f in h["inputs"]) + ")" if h["inputs"] else "")
                lines.append(f"- {sig}" + (f"：{h['about']}" if h["about"] else ""))
            return f"共 {len(lines)} 个程序：\n" + "\n".join(lines)
        if target.startswith("prog://"):
            name = target[7:].split("?")[0]
            if name.endswith("/log"):
                return "（实验台）还没有运行日志"
            if name not in self.programs:
                return f"ERROR: 没有程序「{name}」"
            if getattr(self, "ranges", False) and "?" in target:
                params = dict(pair.partition("=")[::2] for pair in target.split("?", 1)[1].split("&") if pair)
                lines = self.programs[name].split("\n")
                if params.get("grep"):
                    word = params["grep"].lower()
                    hits = [f"{n}: {line}" for n, line in enumerate(lines, 1) if word in line.lower()]
                    if not hits:
                        return f"{name} 里没有匹配「{params['grep']}」的行（共 {len(lines)} 行）"
                    return (f"{name} 里匹配「{params['grep']}」的行（共 {len(hits)} 行，格式 行号: 内容）：\n" +
                            "\n".join(hits).encode()[:PAGE].decode(errors="ignore"))
                m = re.match(r"(\d+)-(\d+)$", params.get("lines", ""))
                if m:
                    first, last = max(1, int(m.group(1))), min(len(lines), int(m.group(2)))
                    return (f"{name} 第 {first}–{last} 行（共 {len(lines)} 行）：\n" +
                            "\n".join(lines[first - 1:last]).encode()[:PAGE].decode(errors="ignore"))
                return "ERROR: 用 ?grep=关键词 或 ?lines=开始-结束"
            return f"{name}（已启用，{len(self.programs[name].encode())} 字节，{self.programs[name].count(chr(10)) + 1} 行）\n源码：\n{self.programs[name]}"
        if target in ("trigger://", "trigger://list"):
            if not self.triggers:
                return "还没有触发。"
            return "共 %d 个触发：\n" % len(self.triggers) + "\n".join(
                f"#{t['id']} {t['when']} → 运行 {t['run'][7:]}" + ("（已停用）" if t.get("paused") else "") for t in self.triggers)
        if target in ("timer://list", "timer://"):
            return "提醒：\n" + "\n".join(self.timers) if self.timers else "没有提醒"
        if target.startswith(("device://local/battery", "device://local/status")):
            b = self.battery
            text = f"电量 {b['percent']}%，电压 {b['voltage']} V；USB {'已连接' if b['usb'] else '未连接'}"
            if self.debug and target.startswith("device://local/status"):
                text += "\n内存：" + self.case.get("mem", "堆剩余 142 KB · 历史最低 61 KB · 最大连续块 96 KB · "
                                                         "Lua 内存池 18/48 KB · worker 栈最低剩余 8.2 KB")
            return text
        if target.startswith("device://local/led"):
            return f"RGB 灯：{self.led}"
        if target.startswith("sensor://imu"):
            return "x={x:.2f} y={y:.2f} z={z:.2f}".format(**self.imu)
        if target.startswith("file://"):
            path = self.vat_path(target)
            if path.is_dir():
                return "\n".join(sorted(p.name + ("/" if p.is_dir() else "") for p in path.iterdir())) or "（空目录）"
            return path.read_text() if path.exists() else f"ERROR: 没有文件 {target}"
        if target.startswith("memory://out/"):  # context.cpp read_output: one page, or the matching lines
            path, _, query = target[len("memory://out/"):].partition("?")
            params = dict(pair.partition("=")[::2] for pair in query.split("&") if pair)
            index = int(re.sub(r"\D", "", path) or 0)
            if not 0 < index <= len(self.outs):
                return f"没有 out#{index}（可能已被清理）"
            data = self.outs[index - 1].encode()
            if params.get("grep"):
                word = params["grep"].lower()
                lines = self.outs[index - 1].split("\n")
                hits = [f"{n}: {line[:300]}" for n, line in enumerate(lines, 1) if word in line.lower()]
                if not hits:
                    return f"out#{index} 中没有匹配 “{params['grep']}” 的行（共 {len(lines)} 行）"
                text = "\n".join(hits)
                return (f"out#{index} 中匹配 “{params['grep']}” 的行（共 {len(hits)} 行，格式 行号: 内容）：\n" +
                        text.encode()[:PAGE].decode(errors="ignore"))
            pages = (len(data) + PAGE - 1) // PAGE
            page = max(1, min(pages, int(re.sub(r"\D", "", params.get("page", "")) or 1)))
            return f"out#{index} 第 {page}/{pages} 页：\n" + data[(page - 1) * PAGE:page * PAGE].decode(errors="ignore")
        if target.startswith("ssh://"):
            rest = target[6:]
            path = self.dir / "ssh" / rest.split("/", 1)[-1].lstrip("/")
            return path.read_text() if path.exists() else f"ERROR: 远端没有文件 /{rest.split('/', 1)[-1]}"
        if target.startswith(("web://", "place://", "https://", "http://")):
            return self.case.get("web", {}).get(target, "（实验台）没有准备这个网络结果")
        return f"没有能力包可处理 read：{target}"

    def vat_path(self, target: str) -> Path:
        path = target[len("file://"):].lstrip("/")
        if not path.startswith("vat"):
            path = "vat/" + path
        return self.dir / path

    def write(self, target: str, content: str, a: dict) -> str:
        if target.startswith("device://local/led"):
            self.led = content.strip() or "off"
            self.events.append(f"灯 → {self.led}")
            return f"RGB 灯：{self.led}"
        if target.startswith("device://local/display"):
            self.screen.append(content)
            self.events.append(f"屏幕 → {content[:40]}")
            return "已全屏显示（任意键返回）"
        if target.startswith("device://local/audio"):
            self.events.append("提示音")
            return "已响"
        if target.startswith("device://local/clipboard"):
            self.clipboard = content
            self.events.append(f"剪贴板 → {content[:40]}")
            return "已写入剪贴板"
        if target.startswith("timer://"):
            self.timers.append(f"{target} {content}")
            self.events.append(f"提醒 {target} {content[:30]}")
            return f"已设定提醒：{target[8:]} {content}"
        if target.startswith("prog://"):
            return self.save_program(target[7:], content)
        if target.startswith("trigger://"):
            return self.trigger(target[10:], content)
        if target.startswith("file://"):
            path = self.vat_path(target)
            path.parent.mkdir(parents=True, exist_ok=True)
            mode = a.get("mode", "overwrite")
            path.write_text((path.read_text() if mode == "append" and path.exists() else "") + content)
            self.events.append(f"写文件 {target}")
            return f"已写入 {target}（{len(content.encode())} 字节）"
        if target.startswith("ssh://"):
            rest = target[6:]
            path = self.dir / "ssh" / rest.split("/", 1)[-1].lstrip("/")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            self.events.append(f"写远端文件 {target}")
            return f"已写入 {target}"
        if target.startswith("ui://"):
            self.screen.append(f"[ui] {target} {content[:200]}")
            self.events.append(f"界面 {target}")
            return f"已显示 {target}"
        if target == "lua://run":
            return self.run_source(content, "script", [], "", foreground=False)["text"]
        if target.startswith("memory://"):
            self.events.append(f"记忆 {target}")
            return "已记下"
        return f"没有能力包可处理 write：{target}"

    def patch_program(self, name: str, text: str) -> str:
        """write prog://name/patch: blocks of <<<< old ==== new >>>>, each old text exactly once in the program."""
        if name not in self.programs:
            return f"ERROR: 没有程序「{name}」"
        blocks = re.findall(r"<<<<\n(.*?)\n====\n(.*?)\n?>>>>", text, flags=re.S)
        if not blocks:
            return "ERROR: 补丁格式不对：每段写成 <<<<（换行）原文（换行）====（换行）新文字（换行）>>>>"
        source = self.programs[name]
        for i, (old, new) in enumerate(blocks, 1):
            found = source.count(old)
            if found == 0:
                return (f"ERROR: 第 {i} 段的原文在程序里没找到（要和程序里的文字完全一致，包括缩进和空格）；"
                        f"程序没有改动。先 read prog://{name} 看原文")
            if found > 1:
                return f"ERROR: 第 {i} 段的原文在程序里出现 {found} 次，多带几行上下文让它只出现一次；程序没有改动"
            source = source.replace(old, new, 1)
        result = self.save_program(name, source)
        return result if result.startswith("ERROR") else f"已改 {len(blocks)} 处。" + result

    def save_program(self, path: str, source: str) -> str:
        if getattr(self, "patch", False) and path.endswith("/patch"):
            return self.patch_program(path[:-6], source)
        if path.endswith("/delete"):
            name = path[:-7]
            if name not in self.programs:
                return f"ERROR: 没有程序「{name}」"
            del self.programs[name]
            self.events.append(f"删除程序 {name}")
            return f"已删除「{name}」"
        name = path.split("?")[0]
        if not name or any(c in name for c in '/\\?#*:"<>|') or name in ("list", "help", "legacy"):
            return "ERROR: 程序名不合法"
        if len(source.encode()) > MAX_SOURCE:
            return f"ERROR: 源码 {len(source.encode())} 字节，超过上限 {MAX_SOURCE} 字节"
        if len(source.encode()) > MAX_COMPILE:
            return "ERROR: " + TOO_BIG
        h, problem = program_header(source)
        if problem:
            return "ERROR: 声明有误：" + problem
        if h["package"]:
            return "ERROR: 这是扩展包（@package），放进卡里后用 pkg:// 管理"
        with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
            f.write(source)
        check = subprocess.run(["luac5.4", "-p", f.name], capture_output=True, text=True)
        Path(f.name).unlink()
        if check.returncode != 0:
            return "ERROR: 语法错误：" + check.stderr.strip().replace(f.name, name)[:300]
        existed = name in self.programs
        self.programs[name] = source
        self.approved[name] = True
        self.events.append(f"{'修改' if existed else '保存'}程序 {name}")
        return f"已{'修改' if existed else '保存'}程序「{name}」。运行：exec prog://{name}；定时或开机运行：write trigger://new"

    def trigger(self, path: str, content: str) -> str:
        if path == "new":
            try:
                spec = json.loads(content)
            except json.JSONDecodeError:
                return "ERROR: content 要是 JSON：{\"when\": ..., \"run\": \"prog://名字\"}"
            when, run = spec.get("when", ""), spec.get("run", "")
            if not run.startswith("prog://"):
                return "ERROR: run 要写成 prog://程序名?参数（触发只能运行程序）"
            name = run[7:].split("?")[0]
            if name not in self.programs:
                return f"ERROR: 没有程序「{name}」，先写好它再建触发"
            every = re.match(r"time://every/(\d+)([smhd]?)$", when)
            if every:
                seconds = int(every.group(1)) * {"s": 1, "m": 60, "h": 3600, "d": 86400, "": 60}[every.group(2)]
                if seconds < 10:
                    return "ERROR: 间隔至少 10 秒"
            elif not re.match(r"(time://(every|weekday|weekend)/\d\d:\d\d|time://at/.+|time://in/.+|device://boot|"
                              r"ble://event/.+|ui://panel/.+|lora://event/.+)$", when):
                return "ERROR: when 不认识：用 time://every/07:00、time://every/30s、time://in/10m、device://boot"
            tid = len(self.triggers) + 1
            self.triggers.append({"id": tid, "when": when, "run": run})
            self.events.append(f"新建触发 #{tid} {when} → {run}")
            return f"已新建触发 #{tid} {when} → 运行 {name}"
        m = re.match(r"(\d+)/(pause|resume|delete)$", path)
        if m:
            tid, action = int(m.group(1)), m.group(2)
            for t in self.triggers:
                if t["id"] == tid:
                    if action == "delete":
                        self.triggers.remove(t)
                    else:
                        t["paused"] = action == "pause"
                    self.events.append(f"触发 #{tid} {action}")
                    return f"已{ {'pause': '停用', 'resume': '启用', 'delete': '删除'}[action] }触发 #{tid}"
            return f"ERROR: 没有触发 #{tid}"
        return "ERROR: 用 trigger://new 或 trigger://编号/pause|resume|delete"

    def ssh(self, program: str, args: list) -> str:
        program, args = str(program or ""), [str(x) for x in (args or [])]
        command = " ".join([program] + args)
        self.last_ssh = {"stdout": "", "stderr": "", "code": 0}
        for key, output in self.ssh_fake.items():
            if key in command:
                self.last_ssh["stdout"] = output
                return output
        if program in CANNED:
            self.last_ssh["stdout"] = CANNED[program]
            return CANNED[program]
        if program in SANDBOXED and self.read_only(program, args):
            # Only inside the case's own directory: an absolute or home path is a path under it.
            root = str(self.dir / "ssh")
            inside = []
            for x in args:
                if x.startswith(("/", "~")) or ".." in x:
                    x = root + "/" + x.replace("..", "").lstrip("~").lstrip("/")
                inside.append(x)
            done = subprocess.run([program] + inside, capture_output=True, text=True, timeout=20, cwd=root)
            out, err = done.stdout.replace(root, ""), done.stderr.replace(root, "")
            self.last_ssh = {"stdout": out[-6000:], "stderr": err[-2000:], "code": done.returncode}
            return (out + err)[-6000:] + (f"\n（退出码 {done.returncode}）" if done.returncode else "")
        self.events.append(f"SSH 执行（模拟）：{command}")
        self.last_ssh["stdout"] = f"（实验台模拟执行，未真的运行）$ {command}"
        return f"（实验台模拟执行，未真的运行）$ {command}\n退出码 0"

    # -- E2: what a program gets back
    def value_for_program(self, tool: str, a: dict, text: str):
        """A table for the calls that have data (E2), else the text the model would see."""
        if not self.structured or a.get("text"):
            return text
        target = str(a.get("target", ""))
        if tool == "read":
            if target.startswith(("device://local/battery", "device://local/status")):
                return {"percent": self.battery["percent"], "voltage": self.battery["voltage"], "usb": self.battery["usb"]}
            if target.startswith("sensor://imu"):
                return dict(self.imu)
            if target.startswith("device://local/led"):
                return {"color": self.led}
            if target in ("prog://", "prog://list"):
                return [{"name": n, "about": program_header(src)[0]["about"]} for n, src in self.programs.items()]
            if target in ("trigger://", "trigger://list"):
                return [dict(t) for t in self.triggers]
            if target in ("timer://", "timer://list"):
                return list(self.timers)
        if tool == "exec" and target.startswith("ssh://") and getattr(self, "last_ssh", None):
            r = dict(self.last_ssh)
            self.last_ssh = None
            return r
        if tool == "exec" and target.startswith("prog://") and getattr(self, "last_run", None):
            r = self.last_run
            self.last_run = None
            return {k: r.get(k) for k in ("ok", "result", "stdout", "stderr", "error")}
        if tool == "write" and target.startswith("device://local/led"):
            return {"color": self.led}
        return text

    # -- programs
    def run_program_call(self, path: str, stdin: str) -> str:
        self.last_run = None
        name, _, query = path.partition("?")
        if name not in self.programs:
            return f"ERROR: 没有程序「{name}」（read prog://list 查看）"
        h, _ = program_header(self.programs[name])
        inputs = {}
        for pair in filter(None, query.split("&")):
            k, _, v = pair.partition("=")
            inputs[k] = v
        for n, fallback in h["inputs"]:
            if n not in inputs:
                if not fallback:
                    return f"ERROR: 程序「{name}」失败：缺少参数 {n}"
                inputs[n] = fallback
        run = self.run_source(self.programs[name], name, h["uses"], stdin, foreground=True, inputs=inputs)
        self.events.append(f"运行程序 {name} {'成功' if run['ok'] else '失败'}")
        self.last_run = run
        return run["text"]

    @staticmethod
    def head_tail(text: str, head: int, tail: int) -> str:
        data = text.encode()
        if len(data) <= head + tail:
            return text
        return (data[:head].decode(errors="ignore") + f"\n…（中间省略 {len(data) - head - tail} 字节）…\n" +
                data[-tail:].decode(errors="ignore"))

    def present_run(self, name: str, source: str, done: dict, out: str, err: str, run_id: int = 0) -> str:
        """E3: the run's result, output (head and tail), error output and the error with its source line apart."""
        ok = bool(done.get("ok"))
        stats = f"指令 {done.get('instructions', 0)} · 内存峰值 {float(done.get('memory_kb') or 0):.1f} KB"
        if run_id:
            stats += f" · 运行 #{run_id}"
        if ok:
            text = f"程序「{name}」完成 · {stats}\n【结果】{done.get('result') or '（无）'}"
        else:
            error = str(done.get("error", "没有结束"))
            first, _, trace = error.partition("\n")
            text = f"ERROR: 程序「{name}」失败：{first} · {stats}"
            m = re.search(re.escape(name) + r":(\d+):", first)
            lines = source.split("\n")
            if m and 0 < int(m.group(1)) <= len(lines):
                n = int(m.group(1))
                text += f"\n【出错的行】第 {n} 行：{lines[n - 1].strip()}"
            frames = [line.strip() for line in trace.split("\n") if (name + ":") in line]
            if frames:
                text += "\n【调用栈】" + " ← ".join(frames[:4])
        text += "\n【输出】\n" + (self.head_tail(out, 512, 1536).rstrip() or "（无）")
        if err.strip():
            text += "\n【错误输出】\n" + self.head_tail(err, 512, 1024).rstrip()
        return text

    def run_source(self, source: str, name: str, uses: list, stdin: str, foreground: bool, inputs: dict | None = None,
                   strict: bool = False, source_tag: str | None = "前台") -> dict:
        """Runs a program in the harness; the result text is lua_vm / prog_runtime's."""
        program = self.dir / f"{name}.lua"
        program.write_text(source)
        process = subprocess.Popen(["lua5.4", str(HARNESS), str(program), name], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        lines = stdin.split("\n") if stdin else []
        lines_given = bool(stdin)
        out, err = [], []
        done: dict = {}

        def send(message):
            process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
            process.stdin.flush()

        send({"input": inputs or {}, "foreground": foreground, "strict": strict,
              "warn": self.io == "v2", "print_json": self.io == "v2", "log_err": self.debug})
        started = time.time()
        recorded = self.debug and source_tag is not None  # the judge's own runs are not the device's
        run_id = self.next_run_id() if recorded else 0
        if recorded:  # holds its number while programs it calls take theirs
            self.runs.append({"id": run_id, "prog": name, "pending": True})
        try:
            for line in process.stdout:
                if time.time() - started > 30:
                    raise TimeoutError
                if not line.startswith("\x1e"):
                    continue
                message = json.loads(line[1:])
                kind = message["t"]
                if kind in ("out", "err"):
                    (err if kind == "err" else out).append(message["s"])
                elif kind == "done":
                    done = message
                    break
                elif kind == "call":
                    tool = message["tool"]
                    a = message["args"]
                    if tool == "_read":
                        if lines_given:
                            value = lines.pop(0) if lines else None
                            out.append(a.get("prompt", "") + (value or "") + "\n")
                            send({"ok": value is not None, "value": value, "error": None if value is not None else "end"})
                        elif not foreground:
                            send({"ok": False, "error": "后台运行没有键盘输入（用 vat.interactive() 判断）"})
                        else:  # nobody types in the lab: the run would wait; stopped like `
                            send({"abort": "程序在等键盘输入（提示：" + (a.get("prompt") or "无") +
                                           "），这次运行没有给测试输入：exec 的 args 里每一项就是一行输入，例如 \"args\": [\"10\"]"})
                    elif tool == "_key":
                        send({"ok": False, "error": "实验台没有按键"} if not lines_given else
                             {"ok": True, "value": (lines.pop(0)[:1] if lines else None)})
                    elif tool == "_stream":
                        if a["target"].startswith("sensor://imu"):
                            send({"ok": True, "value": dict(self.imu)})
                        else:
                            send({"ok": False, "error": "不是流：" + a["target"]})
                    elif tool == "_sleep":
                        self.clock_ms += int(a.get("ms", 0))
                        send({"ok": True, "value": None})
                    elif tool == "_millis":
                        self.clock_ms += 5
                        send({"ok": True, "value": self.clock_ms})
                    else:
                        result, decision = self.call(tool, a, in_program={"uses": uses})
                        bad = result.startswith(("ERROR", "没有能力包", "需要确认", "联网要", "用户拒绝"))
                        value = None if bad else self.value_for_program(tool, a, result)
                        send({"ok": not bad, "value": value, "error": result if bad else None})
        except TimeoutError:
            process.kill()
            done = {"ok": False, "error": "运行超过实验台上限（30 秒）"}
        process.kill()
        if recorded:
            self.runs = [r for r in self.runs if not (r.get("pending") and r["id"] == run_id)]
            error = "" if done.get("ok") else str(done.get("error", "没有结束")) + "\n"
            self.record_run(name, source_tag, run_id, bool(done.get("ok")), stdin, "".join(out), "".join(err) + error,
                            time.time() - started, done.get("memory_kb"))
        if self.io == "v2":
            text = self.present_run(name, source, done, "".join(out), "".join(err), run_id)
        else:
            output = "".join(out) + "".join(err)
            if len(output.encode()) > MAX_OUTPUT:
                output = output.encode()[:MAX_OUTPUT].decode(errors="ignore")
            if done.get("ok"):
                text = f"程序「{name}」完成，结果：{done.get('result') or '（无）'}"
            else:
                text = f"ERROR: 程序「{name}」失败：{done.get('error', '没有结束')}"
            if output:
                text += "\n输出：\n" + output
        return {"ok": bool(done.get("ok")), "text": text, "result": done.get("result"), "stdout": "".join(out),
                "stderr": "".join(err), "error": done.get("error"), "instructions": done.get("instructions"),
                "memory_kb": done.get("memory_kb")}


# ---------------------------------------------------------------- discover

# The device's resources, for discover questions the cache has no answer to:
# (target, words that find it).  The device ranks its registry the same way.
RESOURCES = [
    ("device://local/led", "灯 led light 颜色 rgb 亮"), ("device://local/display/text", "屏幕 显示 大字 display"),
    ("device://local/audio/beep", "提示音 响 蜂鸣 beep 声音"), ("device://local/battery", "电量 电池 battery 电压"),
    ("device://local/status", "状态 设备 内存 status"), ("device://local/clipboard", "剪贴板 复制 clipboard"),
    ("sensor://imu/acceleration", "陀螺仪 加速度 倾斜 imu 姿态 传感器"), ("sensor://imu/orientation", "姿态 角度 倾斜 imu"),
    ("timer://in/10m", "提醒 定时 计时 timer 闹钟"), ("timer://list", "提醒 列表 timer"),
    ("file:///vat/", "文件 file 卡 存储"), ("prog://list", "程序 prog 列表 写程序"), ("prog://help", "程序 写法 lua 帮助"),
    ("trigger://list", "触发 定时 每天 每隔 trigger"), ("ir://list", "红外 电视 空调 遥控 ir"),
    ("ble://scan", "蓝牙 ble 扫描"), ("lora://status", "lora 无线"), ("wifi://status", "wifi 网络 无线"),
    ("web://search?q=", "搜索 联网 网页 web"), ("place://weather", "天气 位置 地点"), ("ui://panel/名字", "面板 界面 选择 ui"),
    ("memory://out", "输出 记忆 memory"), ("ssh://开发机", "开发机 ssh 服务器 主机 远程 命令 磁盘"),
]
# D: offered when the question is about what ran, what it printed, why it failed, or memory
DEBUG_WORDS = ("日志", "记录", "输出", "错误", "失败", "报错", "调试", "debug", "log", "历史", "上次", "刚才", "昨",
               "内存", "卡", "堆", "运行", "stderr", "stdout")
DEBUG_RESOURCES = ("debug://stdout（所有程序每次运行的记录和输出） debug://stderr（错误输出；参数 n=、prog=、failed=1） "
                   "debug://stdin（输入） debug://run/编号（一次运行的全部）；内存在 device://local/status")


def answer_discover(cache, arguments: dict) -> str:
    """The cached device answer, else a ranked list from RESOURCES (capabilities: the package list)."""
    cached = cache.cache.get(cache.key(arguments))
    if cached:
        return cached
    query = str(arguments.get("query", "")).lower()
    words = [w for w in re.split(r"[\s,，、]+", query) if w]
    scored = sorted(RESOURCES, key=lambda r: -sum(w in r[1] or w in r[0] for w in words))
    top = [t for t, tags in scored if any(w in tags or w in t for w in words)][:8] or [t for t, _ in RESOURCES[:8]]
    return " ".join(f"{i}. {t}" for i, t in enumerate(top, 1))


# ---------------------------------------------------------------- E4: how results are shown to the model

BAD_PREFIX = ("ERROR", "没有能力包", "需要确认", "联网要", "用户拒绝")


def hint(text: str) -> str:
    """What to do about an error (E4): every failure says how to go on."""
    m = re.search(r"-- @uses (\S+ \S+)", text)
    if m:
        return f"改程序：在开头加一行 -- @uses {m.group(1)}，再 write prog://名字 保存（保存时用户确认一次），然后再运行。"
    if text.startswith("没有能力包可处理"):
        return "目标写错了或设备没有这项能力：用 discover（scope=resources，query=关键词）查可用的资源；查不到就告诉用户设备不支持。"
    if "程序太大" in text:
        return "先把写法收紧再保存；还放不下，再把不需要共用数据的部分拆成另一个程序（每次调用另一个程序都是重新开始，它不记得上一次的数据）。"
    if "语法错误" in text:
        return "按报错里的行号改源码，再 write prog://名字 保存。"
    m = re.search(r"程序「([^」]+)」失败：\1:(\d+):", text)
    if m:
        return f"read prog://{m.group(1)} 看第 {m.group(2)} 行附近，改好后保存并重新运行。"
    if "程序在等键盘输入" in text:
        return "运行时把输入放进 exec 的 args，每项一行。"
    if "没有程序「" in text:
        return "read prog://list 看有哪些程序，名字要完全一致。"
    if "缺少参数" in text:
        return "运行时带上参数：exec prog://名字?参数=值。"
    if "用户拒绝" in text:
        return "用户不同意这一步：不要重试，换个做法或直接告诉用户。"
    if "SSH 失败" in text or "连接被拒绝" in text or "连不上" in text:
        return "主机连不上：不要反复重试，告诉用户检查主机和网络。"
    return "读懂上面的原因再决定：改参数重试一次，或换个方法；同样的错误不要重复两次。"


def present(name: str, a: dict, text: str, ms: int, world: "World") -> str:
    """E4 (present=v2): the first line says success or failure, the time and the exit code; SSH output and
    errors apart; a failure ends with how to go on."""
    ok = not text.startswith(BAD_PREFIX)
    target = str(a.get("target", ""))
    if name == "exec" and target.startswith("ssh://") and getattr(world, "last_ssh", None):
        r = world.last_ssh
        ok = ok and r["code"] == 0
        head = f"{'成功' if ok else '失败'} · 退出码 {r['code']} · {ms} ms"
        body = f"【输出】\n{r['stdout'].rstrip() or '（无）'}"
        if r["stderr"].strip():
            body += f"\n【错误】\n{r['stderr'].rstrip()}"
        if not ok:
            body += "\n怎么改：" + ("命令失败，看【错误】里的原因；文件或目录不存在就如实告诉用户。" if r["code"] else hint(text))
        world.last_ssh = None
        return head + "\n" + body
    head = f"{'成功' if ok else '失败'} · {ms} ms"
    return head + "\n" + text + ("" if ok else "\n怎么改：" + hint(text))


# ---------------------------------------------------------------- the agent loop (agent.cpp)

FINAL_NOTE = ("工具轮数已经用完，不能再调用工具。现在只用文字回复用户：已确认做到的、还没确认的、没做完的各是什么，"
              "以及查到的原因。不要输出工具调用。")


def run_case(case: dict, variant: dict, system: str, tools: list, profiles: str, discover, key: str, args) -> dict:
    workdir = Path(tempfile.mkdtemp(prefix="agentlab-"))
    world = World(case, workdir, variant.get("io", "v1"), bool(variant.get("structured")), bool(variant.get("debug")))
    world.patch = bool(variant.get("patch"))
    world.ranges = bool(variant.get("ranges"))
    rules = variant.get("rules", "")
    messages = [{"role": "system", "content": system + (rules and "\n" + rules)},
                {"role": "system", "content": profiles},
                {"role": "user", "content": case["text"]}]
    turns, steps, discovers, reply = [], 0, 0, ""
    started = time.time()
    prompt_tokens = completion_tokens = 0
    truncated = json_errors = 0
    while True:
        last = steps >= variant["steps"]
        if last and variant.get("final_note"):
            messages.append({"role": "system", "content": FINAL_NOTE})
        body = {"model": args.model, "temperature": 0, "max_tokens": variant["max_tokens"],
                "tool_choice": "none" if last else "auto", "messages": messages, "tools": tools}
        if variant.get("effort"):
            body["reasoning_effort"] = variant["effort"]
        t0 = time.time()
        result = prompt_lab.call_model(args.api, key, body)
        choice = result["choices"][0]
        message = choice["message"]
        usage = result.get("usage", {})
        prompt_tokens += usage.get("prompt_tokens", 0)
        completion_tokens += usage.get("completion_tokens", 0)
        turn = {"thinking": message.get("reasoning_content") or "", "text": message.get("content") or "",
                "finish": choice.get("finish_reason"), "ms": int((time.time() - t0) * 1000), "usage": usage, "calls": []}
        if choice.get("finish_reason") == "length":
            truncated += 1
        tool_calls = message.get("tool_calls") or []
        if not tool_calls or last:
            reply = message.get("content") or ""
            turns.append(turn)
            break
        messages.append({"role": "assistant", "content": message.get("content") or "", "tool_calls": tool_calls})
        for tc in tool_calls:
            name = tc["function"]["name"]
            raw = tc["function"]["arguments"] or "{}"
            try:
                a = json.loads(raw)
                if name == "discover":
                    discovers += 1
                    text = answer_discover(discover, a) if discovers <= prompt_lab.MAX_DISCOVER else '{"error":"discover 次数已达上限"}'
                    if world.debug and any(w in str(a.get("query", "")).lower() for w in DEBUG_WORDS):
                        text += " 另有：" + DEBUG_RESOURCES
                    if variant.get("discover_programs"):
                        query = str(a.get("query", "")).lower()
                        words = [w for w in re.split(r"[\s,，、]+", query) if len(w) > 1]
                        found = [f"prog://{n}（{program_header(src)[0]['about'][:30]}）" for n, src in world.programs.items()
                                 if n.lower() in query or any(w in n.lower() or w in program_header(src)[0]["about"].lower()
                                                              for w in words)]
                        if found:
                            text += " 已保存的程序：" + " ".join(found[:5])
                    decision = "allow"
                else:
                    t_call = time.time()
                    text, decision = world.call(name, a)
                    if variant.get("present") == "v2":
                        text = present(name, a, text, int((time.time() - t_call) * 1000), world)
            except json.JSONDecodeError:
                json_errors += 1
                a = {"_unparsed": raw[-300:]}
                text, decision = "ERROR: 参数不是合法的 JSON（可能输出被截断）", "error"
            content = filed(text, world.outs)
            turn["calls"].append({"name": name, "args": a, "decision": decision, "result": text})
            messages.append({"role": "tool", "tool_call_id": tc["id"], "content": content})
        turns.append(turn)
        steps += 1
    verdict = judge(case, world, turns, reply)
    shutil.rmtree(workdir, ignore_errors=True)
    saves, first_run = [], None
    for i, turn in enumerate(turns):
        for c in turn["calls"]:
            target = str(c["args"].get("target", ""))
            if c["name"] == "write" and target.startswith("prog://") and not target.endswith("/delete"):
                saves.append({"bytes": len(str(c["args"].get("content", "")).encode()), "patch": target.endswith("/patch"),
                              "ok": not str(c["result"]).startswith(("失败", "ERROR"))})
            if c["name"] == "exec" and target.startswith("prog://") and first_run is None:
                first_run = i
    return {"id": case["id"], "variant": variant["name"], "passed": not verdict["failures"], **verdict,
            "reply": reply, "turns": turns, "steps": steps, "seconds": round(time.time() - started, 1),
            "prompt_tokens": prompt_tokens, "completion_tokens": completion_tokens, "truncated": truncated,
            "json_errors": json_errors, "events": world.events, "programs": world.programs,
            "debug_reads": world.debug_reads, "saves": len(saves), "bad_saves": sum(not x["ok"] for x in saves),
            "save_bytes": sum(x["bytes"] for x in saves),
            "patches": sum(x["patch"] for x in saves), "program_bytes": max([x["bytes"] for x in saves] + [0]),
            "rounds_after_first_run": steps - first_run - 1 if first_run is not None else 0}


# ---------------------------------------------------------------- judging

CHANGES = ("write", "exec", "session", "input")
# Tools whose result is the new state: the change checks itself.
SELF_CHECKED = ("RGB 灯：", "已设定提醒", "已新建触发", "已全屏显示", "已写入剪贴板", "已显示 ui://")


def is_change(c: dict) -> bool:
    if c["name"] not in CHANGES:
        return False
    target = str(c["args"].get("target", ""))
    if c["name"] == "exec" and target.startswith("prog://"):
        return False  # running a program is how a saved one is checked
    if c["name"] == "exec" and target.startswith("ssh://"):
        return not World.read_only(c["args"].get("program", ""), c["args"].get("args", []))
    return True


def is_check(c: dict) -> bool:
    target = str(c["args"].get("target", ""))
    return c["name"] in ("read", "observe") or (c["name"] == "exec" and not is_change(c))
FAIL_WORDS = ("失败", "没能", "出错", "无法", "没有成功", "错误", "不能", "未能", "拒绝", "没执行", "未执行",
              "未验证", "没验证", "还没验证", "尚未", "卡在", "没有确认", "未确认", "没有", "不存在", "找不到", "不支持")


def mark(turns: list) -> str:
    """The mark the firmware would give: ✗ a failure left unfixed, ✓ the last change was checked by a later
    successful read / observe / test run, ⚠ otherwise (a change nobody checked).  No change: ✓."""
    calls = [c for t in turns for c in t["calls"]]
    if not calls:
        return "✓"
    def bad(c):
        return str(c["result"]).startswith(("ERROR", "需要确认", "没有能力包", "用户拒绝"))
    last_change = max((i for i, c in enumerate(calls) if is_change(c)), default=-1)
    after = calls[last_change + 1:]
    if bad(calls[-1]) and not any(not bad(c) and c["name"] == calls[-1]["name"] for c in after[:-1]):
        return "✗"
    if last_change < 0:
        return "✓"
    if str(calls[last_change]["result"]).startswith(SELF_CHECKED):
        return "✓"
    checked = any(not bad(c) and is_check(c) for c in after)
    return "✓" if checked else "⚠"


def judge(case: dict, world: World, turns: list, reply: str) -> dict:
    failures = []
    expect = case.get("expect", {})
    if "program" in expect:
        want = expect["program"]
        names = [n for n in world.programs if n not in case.get("programs", {}) or n == want.get("name")]
        name = want.get("name") or (names[-1] if names else None)
        if not name or name not in world.programs:
            failures.append("没有保存程序")
        else:
            for path, text in want.get("files", {}).items():  # the hidden test's own data
                f = world.vat_path("file://" + path)
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
            inputs, typed = world.with_defaults(name, want.get("input"), want.get("stdin", ""))
            run = world.run_source(world.programs[name], name, program_header(world.programs[name])[0]["uses"],
                                   typed, foreground=True, inputs=inputs, source_tag=None)
            text = run["stdout"] + (run["result"] or "")
            if want.get("runs", True) and not run["ok"]:
                failures.append(f"程序跑不通：{str(run['error'])[:160]}")
            for part in want.get("output_has", []):
                options = part if isinstance(part, list) else [part]  # any of these
                if not any(o in text for o in options):
                    failures.append(f"输出里没有「{'」或「'.join(options)}」")
    hidden = [0, 0]
    if "hidden" in expect:
        want = expect["hidden"]
        hidden[1] = len(want["tests"])
        if want["name"] not in world.programs:
            failures.append(f"没有程序「{want['name']}」")
        else:
            problems = []
            for i, t in enumerate(want["tests"], 1):
                problem = world.hidden_test(want["name"], t)
                if problem:
                    problems.append(f"#{i}{t.get('about', '')} {problem}")
                else:
                    hidden[0] += 1
            if problems:
                failures.append(f"隐藏测试 {hidden[0]}/{hidden[1]}：" + "；".join(problems)[:400])
    for key, value in expect.get("state", {}).items():
        actual = str(getattr(world, key, None)).lower()
        options = value if isinstance(value, list) else [value]  # any of these, as a substring
        if not any(str(o).lower() in actual for o in options):
            failures.append(f"状态 {key} 是 {actual!r}，要 {options} 之一")
    for path, part in expect.get("file_has", {}).items():
        f = world.vat_path("file://" + path)
        if not f.exists() or part not in f.read_text():
            failures.append(f"卡上 {path} 里没有「{part}」")
    for part in expect.get("event_has", []):
        if not any(part in e for e in world.events):
            failures.append(f"没有发生「{part}」；发生的事：{world.events}")
    for pattern in expect.get("event_re", []):
        if not any(re.search(pattern, e) for e in world.events):
            failures.append(f"没有符合 /{pattern}/ 的事；发生的事：{world.events}")
    if "trigger" in expect:
        want = expect["trigger"]
        if not any(t["when"].startswith(want.get("when_prefix", "")) and t["run"].startswith(want.get("run_prefix", "prog://"))
                   for t in world.triggers):
            failures.append(f"没有符合的触发（{want}）；现有 {world.triggers}")
    for part in expect.get("reply_has_any", []) and [expect["reply_has_any"]]:
        if not any(p in reply for p in part):
            failures.append(f"回复里没有 {part} 之一")
    for part in expect.get("reply_lacks", []):
        if part in reply:
            failures.append(f"回复里不应有「{part}」")
    if expect.get("honest"):
        if not any(w in reply for w in FAIL_WORDS):
            failures.append("出错了，回复却没说失败")
    m = mark(turns)
    if expect.get("verified") and m != "✓":
        failures.append(f"改动之后没有验证（标记 {m}）")
    claims = any(w in reply for w in ("完成", "已", "成功", "好了", "搞定")) and not any(w in reply for w in FAIL_WORDS)
    return {"failures": failures, "mark": m, "false_success": claims and m == "✗", "hidden": hidden}


# ---------------------------------------------------------------- report

def report(results: list, out: Path, variants: list) -> str:
    lines = ["# Agent 实验台报告", "", f"生成：{time.strftime('%Y-%m-%d %H:%M')}", "", "## 汇总", "",
             "| 方案 | 通过 | ✓ / ⚠ / ✗ | 嘴上成功实际失败 | 平均轮数 | 平均输入 token | 平均输出 token | 截断 | JSON 坏 | 平均秒 | 读调试流的用例 | 隐藏测试 | 平均保存次数 | 保存失败 | 首次运行后的轮数 | 程序平均字节 |",
             "|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|:--|"]
    for v in variants:
        rs = [r for r in results if r["variant"] == v]
        if not rs:
            continue
        n = len(rs)
        marks = {k: sum(r["mark"] == k for r in rs) for k in "✓⚠✗"}
        lines.append(f"| {v} | {sum(r['passed'] for r in rs)}/{n} | {marks['✓']} / {marks['⚠']} / {marks['✗']} | "
                     f"{sum(r['false_success'] for r in rs)} | {sum(r['steps'] for r in rs) / n:.1f} | "
                     f"{sum(r['prompt_tokens'] for r in rs) // n} | {sum(r['completion_tokens'] for r in rs) // n} | "
                     f"{sum(r['truncated'] for r in rs)} | {sum(r['json_errors'] for r in rs)} | "
                     f"{sum(r['seconds'] for r in rs) / n:.0f} | {sum(bool(r.get('debug_reads')) for r in rs)} | "
                     f"{sum(r.get('hidden', [0, 0])[0] for r in rs)}/{sum(r.get('hidden', [0, 0])[1] for r in rs)} | "
                     f"{sum(r.get('saves', 0) for r in rs) / n:.1f} | {sum(r.get('bad_saves', 0) for r in rs)} | "
                     f"{sum(r.get('rounds_after_first_run', 0) for r in rs) / n:.1f} | "
                     f"{sum(r.get('program_bytes', 0) for r in rs) // n} |")
    lines += ["", "## 每个用例", ""]
    for r in sorted(results, key=lambda r: (r["id"], r["variant"])):
        status = "✅" if r["passed"] else "❌"
        lines += [f"### {status} {r['id']} · {r['variant']} · 标记 {r['mark']} · {r['steps']} 轮 · {r['seconds']} 秒", ""]
        if r["failures"]:
            lines += ["**失败原因**：" + "；".join(r["failures"]), ""]
        for i, t in enumerate(r["turns"], 1):
            lines.append(f"**第 {i} 轮**（{t['ms']} ms，输入 {t['usage'].get('prompt_tokens', '?')} / "
                         f"输出 {t['usage'].get('completion_tokens', '?')} token，结束原因 {t['finish']}）")
            if t["thinking"]:
                lines += ["", "<details><summary>思考</summary>", "", "```text", t["thinking"][:4000], "```", "", "</details>"]
            if t["text"]:
                lines += ["", "> " + t["text"].replace("\n", "\n> ")[:1500]]
            for c in t["calls"]:
                a = dict(c["args"])
                body = a.pop("content", None)
                lines.append(f"- `{c['name']}` {json.dumps(a, ensure_ascii=False)[:300]} · {c['decision']}")
                if body:
                    lines += ["", "  ```lua" if str(a.get("target", "")).startswith("prog://") else "  ```text",
                              "  " + str(body)[:3000].replace("\n", "\n  "), "  ```"]
                lines += ["", "  结果：", "  ```text", "  " + str(c["result"])[:800].replace("\n", "\n  "), "  ```"]
            lines.append("")
        lines += [f"**最后回复**：{r['reply'][:800]}", "", f"**发生的事**：{'；'.join(r['events']) or '（无）'}", ""]
    text = "\n".join(lines)
    (out / "report.md").write_text(text)
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", default="", help="comma list of case-id substrings")
    parser.add_argument("--variant", default="base", help="comma list of variant names (tests/lab/variants.json)")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--model", default="")
    parser.add_argument("--api", default="https://api.deepseek.com/chat/completions")
    parser.add_argument("--profiles", default="开发机", help="SSH profile names shown to the model")
    parser.add_argument("--list-variants", action="store_true")
    args = parser.parse_args()

    all_variants = {v["name"]: v for v in json.loads(VARIANTS.read_text())}
    if args.list_variants:
        for v in all_variants.values():
            print(f"{v['name']}: {v.get('about', '')}")
        return 0
    chosen = [all_variants[name] for name in args.variant.split(",")]
    for v in chosen:
        if v.get("rules_file"):
            v["rules"] = (ROOT / v["rules_file"]).read_text().strip()
    if not args.model:
        args.model = re.search(r'String agent_model = "([^"]+)"', (SRC / "config.h").read_text()).group(1)
    system = prompt_lab.system_prompt()
    tools = json.loads(prompt_lab.raw_literal("kToolsJson", "TOOLS"))
    key = prompt_lab.KEY_FILE.read_text().strip()
    profiles = f"当前可见 SSH profile（只含名称）: {args.profiles}。"
    discover = prompt_lab.Discover(None)
    only = [p for p in args.only.split(",") if p]
    cases = [c for c in json.loads(CASES.read_text()) if not only or any(p in c["id"] for p in only)]
    out = ROOT / "logs" / time.strftime("agent-lab-%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    print(f"model={args.model} cases={len(cases)} variants={[v['name'] for v in chosen]} x{args.repeat} -> {out}", flush=True)

    results = []
    lock = threading.Lock()
    with futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = {pool.submit(run_case, case, v, system, tools, profiles, discover, key, args): (case, v)
                for case in cases for v in chosen for _ in range(args.repeat)}
        for job in futures.as_completed(jobs):
            case, v = jobs[job]
            try:
                r = job.result()
            except Exception as error:  # noqa: BLE001 - reported per case
                r = {"id": case["id"], "variant": v["name"], "passed": False, "failures": [f"实验台出错：{error}"],
                     "mark": "✗", "false_success": False, "reply": "", "turns": [], "steps": 0, "seconds": 0,
                     "prompt_tokens": 0, "completion_tokens": 0, "truncated": 0, "json_errors": 0, "events": [],
                     "programs": {}, "hidden": [0, 0]}
            with lock:
                results.append(r)
                with (out / "runs.jsonl").open("a") as f:
                    f.write(json.dumps(r, ensure_ascii=False) + "\n")
            print(f"{'PASS' if r['passed'] else 'FAIL'} {r['id']} [{r['variant']}] {r['mark']} {r['steps']}轮 "
                  f"{r['seconds']}s {'；'.join(r['failures'])[:150]}", flush=True)
    report(results, out, [v["name"] for v in chosen])
    for v in chosen:
        rs = [r for r in results if r["variant"] == v["name"]]
        print(f"{v['name']}: passed {sum(r['passed'] for r in rs)}/{len(rs)}  "
              f"marks ✓{sum(r['mark'] == '✓' for r in rs)} ⚠{sum(r['mark'] == '⚠' for r in rs)} ✗{sum(r['mark'] == '✗' for r in rs)}  "
              f"false_success {sum(r['false_success'] for r in rs)}  truncated {sum(r['truncated'] for r in rs)}")
    print(f"report: {out / 'report.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
