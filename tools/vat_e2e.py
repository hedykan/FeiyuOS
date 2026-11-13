#!/usr/bin/env python3
"""Standalone Cardputer end-to-end acceptance (S0-S11, S2I).

The device does all the work.  This script only talks to the device's own
web API over Wi-Fi (no USB data link) and checks real side effects on the
SSH host it runs on.  Run it on the machine that the device's SSH profiles
point at, e.g.:

    VAT_DEVICE=http://<device-ip> python3 tools/vat_e2e.py --phases all

Tiers (docs/E2E.zh-CN.md):
  quick   every phase but S2I, one or two cases each (<= 5 min, after every flash)
  auto / a comma list   the named phases, whole (S1: the core recordings)
  all     release (<= 15 min): every phase; S1 core recordings, S2I a sample on
          the device while tools/prompt_lab.py checks every intent case on the
          host in parallel, fewer repeats in S3/S5/S8
  full    everything on the device: every recording, all 58 S2I cases, 20
          rounds, the case-file reboot (the old `all`, ~30 min)
"""
from __future__ import annotations

import argparse
import ipaddress
import json
import re
import os
import socket
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "tests" / "cases"
PCM = ROOT / "tests" / "pcm"
ALL_PHASES = ["s0", "s1", "s2", "s2i", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12"]
# quick: a representative case or two per phase (<= 5 min, after every flash);
# normal: impacted phases (auto) or a named list, run whole;
# all: release (<= 15 min); full: everything on the device (the old `all`).
TIER = "normal"
# Voice cases that test speech itself (device settings and recognition
# corrections); the other recordings repeat S2i intents and run only in `full`
# (in `all`, prompt_lab checks their intents on the host).
CORE_PCM = ["local_status", "local_settings", "set_brightness_96", "set_timeout_180", "local_history",
            "read_hostname", "start_codex_agent", "tmux_create", "restart_nginx", "docker_logs"]
REMOTE_AGENT_PCM = {"start_codex_agent", "start_claude_agent", "start_pi_agent"}  # not in voice_latency
QUICK_PCM = ["local_status", "read_hostname"]
# S2I on the device in `all`: one case per tool family.  Every intent case
# (these, the rest of S2I, v0.5 cases, PCM expectations) runs in prompt_lab.
S2I_SAMPLE = [
    "connect_profile",                             # session ssh://
    "read_remote_file",                            # read ssh://
    "overwrite_remote_file",                       # write ssh://
    "safe_readonly_command",                       # exec, runs outright
    "destructive_command_requires_confirmation",   # exec, needs confirmation
    "tmux_create",                                 # session tmux://
    "current_tui_ctrl_c",                          # input tui://
    "start_remote_agent",                          # session, remote agent
    "missing_profile_clarification",               # no tool: asks back
    "battery_status",                              # read device://
    "remember_note",                               # write memory://
    "read_humidity",                               # an unknown sensor
]
# S10 in `quick`: saves the programs program_reads and trigger_cases rely on.
QUICK_PROGRAMS = ["led", "branch", "callee", "caller", "broken"]
ONLY: list[str] = []  # --only: substrings of case ids; empty = every case

# What each changed path puts at risk (docs/E2E "改了什么跑什么").  S0 always runs.
# Longest prefix wins; a path that matches nothing selects nothing.
IMPACT = {
    "firmware/standalone_agent/src/packages/ui_package.cpp": ["s11"],
    "firmware/standalone_agent/src/web": ["s0", "s11"],
    "firmware/standalone_agent/src/arena": ["s11"],
    "firmware/standalone_agent/src/ops_lora.": ["s9"],
    "firmware/standalone_agent/src/ops_gps.": ["s9"],
    "firmware/standalone_agent/src/packages/gps_package": ["s9"],
    "firmware/standalone_agent/src/packages/lora_package": ["s9"],
    "firmware/standalone_agent/src/context.": ["s2", "s5", "s8"],
    "firmware/standalone_agent/src/ops_web.": ["s8"],
    "firmware/standalone_agent/src/packages/memory_package": ["s8"],
    "firmware/standalone_agent/src/packages/web_package": ["s8"],
    "firmware/standalone_agent/src/agent_prompt.h": ["s2", "s2i", "s6", "s7", "s8"],
    "firmware/standalone_agent/src/agent.": ["s2", "s2i", "s7", "s8"],
    "firmware/standalone_agent/src/stt.": ["s1"],
    "firmware/standalone_agent/src/net.": ["s1", "s3", "s8"],
    "firmware/standalone_agent/src/ops_ssh.": ["s3", "s7", "s8"],
    "firmware/standalone_agent/src/ops_tui.": ["s4"],
    "firmware/standalone_agent/src/ops_local.": ["s2"],
    "firmware/standalone_agent/src/policy.": ["s2"],
    "firmware/standalone_agent/src/registry.": ["s6"],
    "firmware/standalone_agent/src/packages/ssh_package": ["s3", "s7"],
    "firmware/standalone_agent/src/packages/tmux_package": ["s4"],
    "firmware/standalone_agent/src/packages/wifi_package": ["s6"],
    "firmware/standalone_agent/src/ops_wifi.": ["s6"],  # settings page itself: S0
    "firmware/standalone_agent/src/ops_hw.": ["s6"],
    "firmware/standalone_agent/src/ops_timer.": ["s6"],
    "firmware/standalone_agent/src/ops_ir.": ["s6"],
    "firmware/standalone_agent/src/prog_": ["s10"],
    "firmware/standalone_agent/src/events.": ["s10"],
    "firmware/standalone_agent/src/console": ["s10"],
    "firmware/standalone_agent/src/packages/http_package": ["s12"],
    "firmware/standalone_agent/src/pkg_": ["s10", "s12"],
    "firmware/standalone_agent/src/packages/pkg_package": ["s10", "s12"],
    "tools/pkg_index.py": ["s12"],
    "firmware/standalone_agent/src/net.": ["s8", "s12"],
    "firmware/standalone_agent/src/packages/program_package": ["s10"],
    "firmware/standalone_agent/src/packages/ask_package": ["s10"],
    "tests/programs/": ["s10"],
    "firmware/standalone_agent/src/packages/": ["s6"],
    "firmware/standalone_agent/src/ui": ["s4", "s6", "s10", "s11"],
    "firmware/standalone_agent/src/ui_recorder.": ["s1", "s4"],
    "firmware/standalone_agent/src/worker.": ["s4", "s7"],
    "firmware/standalone_agent/src/storage.": ["s5", "s8", "s9"],
    "firmware/standalone_agent/src/config.": ["s5", "s9"],
    "firmware/standalone_agent/src/web.": ["s7"],
    "firmware/standalone_agent/src/common.": ["s2", "s4"],
    "firmware/standalone_agent/standalone_agent.ino": ["s1", "s5"],
    "tests/pcm/": ["s1"],
    "tools/vat_e2e.py": [],
    "tests/cases/": ["s2i"],
    "tests/e2e_profiles.example.json": ["s3"],
}


def impacted_phases(since: str) -> list[str]:
    """Phases touched by `git diff <since>` (committed and uncommitted)."""
    out = subprocess.run(["git", "diff", "--name-only", since], cwd=ROOT, text=True,
                         capture_output=True, check=True).stdout.split()
    picked = {"s0"}
    for path in out:
        hits = [prefix for prefix in IMPACT if path.startswith(prefix)]
        if hits:
            picked.update(IMPACT[max(hits, key=len)])
    return [phase for phase in ALL_PHASES if phase in picked]


def quick() -> bool:
    return TIER == "quick"


def release() -> bool:
    """`all`: the release tier, trimmed to fit 15 minutes."""
    return TIER == "all"


def full() -> bool:
    """Every case of a phase: `full`, or phases named by hand / picked by auto."""
    return TIER in ("full", "normal")


def until(check, timeout: float, interval: float = 0.2):
    """Poll check() until it returns something truthy or timeout passes; the
    last value is returned (checked once more at the deadline)."""
    deadline = time.time() + timeout
    while True:
        value = check()
        if value or time.time() >= deadline:
            return value
        time.sleep(min(interval, max(0.0, deadline - time.time())))


def wanted(case_id: str) -> bool:
    return not ONLY or any(part in case_id for part in ONLY)
HOST_TMP = "/tmp/vat-e2e"
HOME = os.path.expanduser("~")


# --------------------------------------------------------------------------- device client

def resolve_once(base: str) -> str:
    """http://vat.local -> http://192.168.x.y, looked up once over IPv4.

    urllib resolves the name on every request and waits about 5 s for an IPv6
    answer that never comes for .local names: 3-4 requests per step made each
    step 15-20 s slower (2026-11-08).  An address is returned unchanged.
    """
    parts = urllib.parse.urlsplit(base)
    host = parts.hostname or ""
    try:
        ipaddress.ip_address(host)
        return base
    except ValueError:
        pass
    try:
        ip = socket.getaddrinfo(host, parts.port or 80, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
    except OSError:
        return base
    netloc = ip + (f":{parts.port}" if parts.port else "")
    return urllib.parse.urlunsplit(parts._replace(netloc=netloc))


class Device:
    def __init__(self, base: str) -> None:
        self.base = resolve_once(base.rstrip("/"))
        self.expected_reboots = 0  # reboots the script itself requested
        token_file = os.path.expanduser("~/.config/feiyuos/device_token")
        self.token = os.environ.get("VAT_TOKEN") or (open(token_file).read().strip() if os.path.exists(token_file) else "")

    def _request(self, method: str, path: str, data: bytes | None = None,
                 headers: dict | None = None, timeout: float = 15, attempts: int = 6):
        last: Exception | None = None
        for attempt in range(attempts):
            try:
                request = urllib.request.Request(self.base + path, data=data, method=method,
                                                 headers={**(headers or {}), "Authorization": f"Bearer {self.token}"})
                with urllib.request.urlopen(request, timeout=timeout) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as exc:
                if exc.code == 503 and attempt < attempts - 1:  # device is short on heap mid-request
                    time.sleep(1)
                    continue
                return exc.code, exc.read()
            except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as exc:
                last = exc  # Wi-Fi/ARP hiccup: retry
                time.sleep(1 + attempt)
        raise RuntimeError(f"device unreachable: {method} {path}: {last}")

    def get_json(self, endpoint: str, **params):
        query = ("?" + urllib.parse.urlencode(params)) if params else ""
        status, body = self._request("GET", endpoint + query)
        return json.loads(body.decode())

    def post(self, endpoint: str, data: bytes = b"", content_type: str = "text/plain", **params):
        query = ("?" + urllib.parse.urlencode(params)) if params else ""
        status, body = self._request("POST", endpoint + query, data=data,
                                     headers={"Content-Type": content_type})
        return status, body.decode("utf-8", "replace")

    def status(self) -> dict:
        for _ in range(30):
            status = self.get_json("/api/status")
            if isinstance(status, dict) and "error" not in status and not status.get("low_memory"):
                return status
            time.sleep(0.5)  # a TLS handshake is in progress (or the heap is short); full status comes back after it
        return status

    def read_file(self, path: str, tail: int = 0) -> str:
        params = {"path": path}
        if tail:
            params["tail"] = tail
        status, body = self._request("GET", "/api/file?" + urllib.parse.urlencode(params))
        return body.decode("utf-8", "replace") if status == 200 else ""

    def write_file(self, path: str, text: str) -> None:
        status, body = self.post("/api/file", text.encode(), path=path)
        if status != 200:
            raise RuntimeError(f"write {path}: {body}")

    def upload(self, path: str, payload: bytes) -> None:
        boundary = uuid.uuid4().hex
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"f\"\r\n"
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + payload + \
               f"\r\n--{boundary}--\r\n".encode()
        status, text = self.post("/api/upload", body, f"multipart/form-data; boundary={boundary}", path=path)
        if status != 200:
            raise RuntimeError(f"upload {path}: {text}")

    def reboot(self) -> dict:
        """Reboot the device and wait until it is back on Wi-Fi."""
        self.expected_reboots += 1
        try:
            before = self.status().get("uptime_s")
        except RuntimeError:
            before = None
        self.post("/api/reboot")
        # Polled instead of a fixed 8 s: back means the uptime restarted (lower
        # than before the request), so a status answered before the reset does
        # not count.  Same overall bound as before (8 s + 30 x 2 s).
        deadline = time.time() + 68
        after = {}
        while time.time() < deadline:
            time.sleep(1)
            try:
                after = self.status()
            except RuntimeError:
                continue
            uptime = after.get("uptime_s", 9999)
            if uptime < 120 and (before is None or uptime < before):
                break
        return after

    def files(self, root: str = "/vat") -> list[tuple[str, int]]:
        """Every file under root as (path, size)."""
        found, pending = [], [root]
        while pending:
            directory = pending.pop()
            for entry in self.get_json("/api/ls", path=directory):
                path = f"{directory}/{entry['name']}"
                if entry["dir"]:
                    pending.append(path)
                else:
                    found.append((path, entry["size"]))
        return found

    def key(self, spec: str) -> dict:
        """One key press through the device's keyboard handler; returns the screen model."""
        self.post("/api/key", k=spec)
        return self.status()["screen"]

    def action(self, name: str) -> str:
        status, body = self.post("/api/action", name=name)
        return json.loads(body).get("result", "")

    def wait_idle(self, timeout: float = 60) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.status().get("busy"):
                return
            time.sleep(0.5)
        raise RuntimeError("device stayed busy")

    def job(self, kind: str, timeout: float = 120, **params) -> dict:
        deadline = time.time() + timeout
        while True:
            status, body = self.post("/api/job", kind=kind, **params)
            if status == 200:
                job_id = json.loads(body)["id"]
                break
            if status != 409 or time.time() > deadline:
                raise RuntimeError(f"job submit failed {status}: {body}")
            time.sleep(0.5)
        while time.time() < deadline:
            record = self.get_json("/api/job", id=job_id)
            if record.get("status") not in ("queued", "running"):
                return record
            time.sleep(0.6)
        raise RuntimeError(f"job {job_id} timed out")

    def text(self, text: str, **kw) -> dict:
        return self.job("text", text=text, **kw)

    def resolve_all(self, approve: bool = False) -> list[dict]:
        """Approve or reject every pending operation; returns the records."""
        records = []
        for _ in range(8):
            screen = self.status()["screen"]
            if not screen["pending"]:
                break
            if screen.get("secret_prompt"):
                self.wait_idle()
                self.action("cancel")  # abort: tests never type real passwords here
                time.sleep(1)
                continue
            result = self.action("approve" if approve else "reject")  # a job asking takes it at once
            if result == "busy":
                self.wait_idle()
                result = self.action("approve" if approve else "reject")
            if not result.isdigit():
                break
            job_id = int(result)
            while True:
                record = self.get_json("/api/job", id=job_id)
                if record.get("status") not in ("queued", "running"):
                    records.append(record)
                    break
                time.sleep(0.5)
        return records


# --------------------------------------------------------------------------- helpers

def host(command: str) -> subprocess.CompletedProcess:
    return subprocess.run(command, shell=True, text=True, capture_output=True, timeout=30)


def host_facts() -> dict:
    """Placeholders usable in case files: facts of the host the script runs on."""
    return {"user": host("whoami").stdout.strip(), "hostname": host("hostname").stdout.strip()}


def raw(record: dict) -> str:
    """Raw output of the executed operations (the Agent page shows a summary)."""
    return "\n".join(c.get("result", "") for c in operations(record)).strip()


def final(record: dict) -> str:
    """Output of the last successfully executed step (multi-step tasks may
    correct themselves after an error)."""
    done = [c for c in operations(record)
            if c.get("decision") in ("allow", "approved", "approved_all") and not c.get("result", "").startswith("SSH 失败")]
    return done[-1].get("result", "").strip() if done else raw(record)


def everything(record: dict) -> str:
    return " ".join([record.get("reply", ""), record.get("error", ""), raw(record)])


def operations(record: dict) -> list[dict]:
    return [call for call in record.get("calls", []) if call.get("name") != "discover"]


FAILURE_PREFIXES = ("ERROR", "没有能力包可处理", "已拒绝", "SSH 连接失败", "SSH 失败", "命令不在自动只读允许范围")


def validate_expect(expect: dict, record: dict) -> list[str]:
    """Same rules as the reference intent evaluation: tool names must match in
    order; expected argument keys must be equal (subset match)."""
    failures = []
    calls = operations(record)
    # After a step fails the agent may correct itself (another command, a check):
    # the intent is judged up to that step.  Same prefixes as registry::failed().
    for index, call in enumerate(calls):
        if str(call.get("result", "")).startswith(FAILURE_PREFIXES):
            calls = calls[: index + 1]
            break
    if expect.get("no_tool"):
        if calls:
            failures.append(f"expected clarification, got {[c['name'] for c in calls]}")
        if not record.get("reply"):
            failures.append("expected a clarification message")
        return failures
    names = [c["name"] for c in calls]
    if "first_any" in expect:
        # Several first steps are right (read the help first, or act at once):
        # the first call's tool and a target prefix, any of them.
        first = calls[0] if calls else {}
        target = str(first.get("arguments", {}).get("target", ""))
        if not any(first.get("name") == option["name"] and target.startswith(option["target"])
                   for option in expect["first_any"]):
            failures.append(f"first call: expected one of {expect['first_any']}, got {first.get('name')} {target!r}")
        return failures
    if names != expect.get("call_names", []):
        failures.append(f"tools: expected {expect.get('call_names')}, got {names}")
        return failures
    for index, expected in enumerate(expect.get("arguments", [])):
        actual = calls[index]["arguments"] if index < len(calls) else {}
        for key, value in expected.items():
            if actual.get(key) != value:
                failures.append(f"call {index} {key}: expected {value!r}, got {actual.get(key)!r}")
    return failures


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, round((len(ordered) - 1) * fraction))]


class Report:
    def __init__(self) -> None:
        self.results: list[dict] = []

    def add(self, phase: str, case: str, passed: bool, detail: str = "", **extra) -> bool:
        self.results.append({"phase": phase, "case": case, "passed": passed, "detail": detail, **extra})
        mark = "PASS" if passed else "FAIL"
        print(f"[{phase}] {mark} {case} {detail[:160]}", flush=True)
        return passed

    def summary(self) -> dict:
        phases: dict[str, list[int]] = {}
        for item in self.results:
            stats = phases.setdefault(item["phase"], [0, 0])
            stats[1] += 1
            stats[0] += 1 if item["passed"] else 0
        return {phase: f"{ok}/{total}" for phase, (ok, total) in phases.items()}


# --------------------------------------------------------------------------- phases

def gateway_absent() -> tuple[bool, str]:
    gateway = host("pgrep -af 'serial_gateway|provision_standalone' | grep -v pgrep").stdout.strip()
    # The passive console logger (make serial-log: `cat /dev/ttyACM0`) only reads.
    usb = host("for p in $(fuser /dev/ttyACM0 2>/dev/null); do "
               "case \"$(tr '\\0' ' ' < /proc/$p/cmdline)\" in 'cat /dev/ttyACM0 ') ;; *) echo $p;; esac; done").stdout.strip()
    ok = not gateway and not usb
    return ok, f"gateway_processes={gateway or 'none'} usb_serial_holders={usb or 'none'}"


REQUIRED_PROFILES = ["开发机", "生产机", "devbox", "staging", "prod", "voice-agent-local", "voice-agent-loopback",
                     "测试机"]
TEST_BOX = "vat-e2e-box"           # docker: linuxserver/openssh-server on :2222
TEST_BOX_PASSWORD = "vat-e2e-pass"  # throwaway credential of the disposable container


PROFILE_TEMPLATE = ROOT / "tests" / "e2e_profiles.example.json"


def local_ip_towards(device_url: str) -> str:
    """This host's address on the device's network (the source of a route to it)."""
    import socket
    target = urllib.parse.urlparse(device_url).hostname or ""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((target, 80))  # no packet is sent: this only picks the route
        return s.getsockname()[0]


def sync_profiles(device: Device, device_url: str) -> str:
    """Test profiles of the template whose host is <this-host-ip> follow this
    host's current LAN address on the device (a network change otherwise
    breaks S2, S4 and S7).  Only private addresses are rewritten, so a profile
    set by hand to a public address (a tunnel) is left alone; keys are not
    touched.  Returns a line for the log."""
    template = json.loads(PROFILE_TEMPLATE.read_text())
    ids = {p["id"] for p in template["profiles"] if p.get("host") == "<this-host-ip>"}
    ip = local_ip_towards(device_url)
    text = device.read_file("/vat/ssh/profiles.json")
    if not text:
        return "profiles: none on the device"
    data = json.loads(text)
    changed = []
    for p in data.get("profiles", []):
        try:
            private = ipaddress.ip_address(p.get("host", "")).is_private
        except ValueError:
            private = False  # a host name: leave it
        if p.get("id") in ids and private and p["host"] != ip:
            p["host"] = ip
            changed.append(p["id"])
    if not changed:
        return f"profiles: test hosts already at {ip}"
    device.write_file("/vat/ssh/profiles.json", json.dumps(data, ensure_ascii=False, indent=2))
    device.post("/api/reload")
    return f"profiles: {', '.join(changed)} -> {ip}"


def phase_s0(device: Device, report: Report) -> None:
    status = device.status()
    missing = [p for p in REQUIRED_PROFILES if p not in status.get("profiles", "").split(", ")]
    report.add("S0", "e2e_profiles_present", not missing, f"missing={missing}")
    report.add("S0", "storage_mounted", status.get("storage") in ("tf", "flash"), status.get("storage", ""))
    report.add("S0", "config_ready", bool(status.get("config_ready")), status.get("profiles", ""))
    report.add("S0", "wifi_station", bool(status.get("ip")) and status.get("wifi_rssi", 0) != 0,
               f"{status.get('wifi')} {status.get('ip')} rssi={status.get('wifi_rssi')}")
    report.add("S0", "not_in_setup", not status["screen"]["setup"], status["screen"]["header"])
    report.add("S0", "ready_page", "READY" in status["screen"]["header"] or status["screen"]["page"] == "agent",
               status["screen"]["header"])
    _, page = device._request("GET", "/")
    report.add("S0", "web_admin_page", b"Voice Agent Terminal" in page)
    marker = f"e2e-{uuid.uuid4().hex[:8]}"
    device.write_file("/vat/tests/e2e.txt", marker)
    report.add("S0", "file_api_roundtrip", device.read_file("/vat/tests/e2e.txt") == marker)
    device.post("/api/delete", path="/vat/tests/e2e.txt")
    ok, detail = gateway_absent()
    report.add("S0", "gateway_absent_usb_unused", ok, detail)
    settings_navigation(device, report)


def settings_navigation(device: Device, report: Report) -> None:
    """Settings page (Fn+W): menu, WiFi scan, font view and back out.  Never joins."""
    if not wanted("settings"):
        return
    views = []
    screen = device.key("fn+w")
    views.append(screen.get("settings", {}).get("view"))
    screen = device.key("enter")  # WiFi list, blocking scan
    networks = screen.get("settings", {}).get("networks", 0)
    views.append(screen.get("settings", {}).get("view"))
    for spec in ("esc", ".", "enter", "esc", "esc"):
        screen = device.key(spec)
        views.append(screen.get("settings", {}).get("view") if screen["setup"] else "closed")
    expected = ["menu", "wifi", "menu", "menu", "keys", "menu", "closed"]
    report.add("S0", "settings_navigation", views == expected, " > ".join(map(str, views)))
    report.add("S0", "settings_wifi_scan", networks > 0, f"networks={networks}")
    if screen["setup"]:  # leave the device usable for the phases after this
        device.key("esc")
    settings_forms(device, report)
    settings_portal(device, report)


def settings_portal(device: Device, report: Report) -> None:
    """Phone settings: hotspot on while the view is open, /m/api/state never carries a whole key."""
    if not wanted("settings"):
        return
    device.key("fn+w")
    device.key(";")                           # last menu item: 网页令牌
    screen = device.key("enter")
    token_view = screen.get("settings", {}).get("view") == "webtoken"
    device.key("esc")
    screen = device.key(";")                  # the one before: 手机设置
    screen = device.key("enter")
    opened = screen.get("settings", {}).get("view") == "portal"
    status = device.status()
    leaked_status = any(len(str(v)) == 8 and str(v).isdigit() for v in status.get("screen", {}).get("settings", {}).values())
    _, body = device._request("GET", "/m/api/state")
    state = json.loads(body)
    secrets = json.loads(device.read_file("/vat/secrets.json"))
    keys = [p.get("api_key", "") for p in secrets.get("providers", {}).values() if p.get("api_key")]
    leaked_key = any(k in body.decode() for k in keys)
    hints_ok = all(k["hint"].startswith(("已设置 …", "未设置")) for k in state.get("keys", []))
    device.key("esc")
    closed = not device.key("esc")["setup"]
    report.add("S0", "settings_portal", opened and closed and hints_ok and not leaked_key and not leaked_status,
               f"opened={opened} closed={closed} hints_ok={hints_ok} key_leak={leaked_key} pw_in_status={leaked_status}")
    report.add("S0", "settings_web_token_view", token_view)


def settings_forms(device: Device, report: Report) -> None:
    """Settings forms: add + delete a test host, device public key, a bad key refused.  Real keys untouched."""
    if not wanted("settings"):
        return
    profiles = lambda: device.read_file("/vat/ssh/profiles.json")  # noqa: E731
    view = lambda screen: screen.get("settings", {}).get("view") if screen["setup"] else "closed"  # noqa: E731
    device.key("fn+w")
    device.key(".")
    device.key(".")
    device.key("enter")                       # SSH hosts
    device.key(";")
    device.key(";")                           # "新增主机…" (second from the end)
    screen = device.key("enter")
    for value in ("e2e-host", "127.0.0.1", "e2e"):
        device.key(value)
        device.key("enter")
    screen = device.key("enter")              # port 22 kept
    added = "e2e-host" in profiles()
    message = screen.get("settings", {}).get("message", "")
    device.key("x")                           # back to the list
    for _ in range(3):
        device.key(";")                       # the new host: third from the end
    device.key("enter")
    device.key("enter")                       # arm
    screen = device.key("enter")              # delete
    removed = "e2e-host" not in profiles()
    report.add("S0", "settings_host_add_delete", added and removed, f"added={added} removed={removed} {message[:40]}")
    device.key("x")
    device.key(";")
    screen = device.key("enter")              # device public key
    report.add("S0", "settings_public_key", view(screen) == "pubkey", view(screen))
    device.key("esc")
    device.key("esc")                         # menu
    device.key("esc")                         # closed; reopening starts at the top
    device.key("fn+w")
    device.key(".")                           # Key (index 1)
    secrets_before = device.read_file("/vat/secrets.json")
    device.key("enter")
    device.key("enter")
    device.key("abc")
    screen = device.key("enter")
    refused = "不像 Key" in screen.get("settings", {}).get("message", "")
    report.add("S0", "settings_bad_key_refused", refused and device.read_file("/vat/secrets.json") == secrets_before,
               screen.get("settings", {}).get("message", "")[:40])
    device.key("x")
    device.key("esc")
    screen = device.key("esc")
    if screen["setup"]:
        device.key("esc")


def phase_s1(device: Device, report: Report) -> None:
    standalone = json.loads((PCM / "standalone_manifest.json").read_text())
    manifest = json.loads((PCM / "manifest.json").read_text())
    latencies, stt = [], []
    keep = None if TIER == "full" else QUICK_PCM if quick() else CORE_PCM
    for case in standalone + manifest:
        if not wanted(case["id"]) or (keep is not None and case["id"] not in keep):
            continue
        pcm = PCM / f"{case['id']}.pcm"
        if "expect" in case:  # intent cases are independent turns, like the reference evaluation
            device.wait_idle()
            device.action("clear_history")
        device.upload(f"/vat/tests/{case['id']}.pcm", pcm.read_bytes())
        record = device.job("pcm", path=f"/vat/tests/{case['id']}.pcm")
        device.resolve_all(approve=False)
        if case["id"] != "local_status":  # reused below by the TEXT-mode check
            device.post("/api/delete", path=f"/vat/tests/{case['id']}.pcm")
        failures = []
        transcript = record.get("transcript", "")
        for word in case.get("transcript_contains", []):
            if word.casefold() not in transcript.casefold():
                failures.append(f"transcript missing {word!r}")
        if record.get("status") not in ("done", "awaiting_approval", "awaiting_secret"):
            failures.append(f"status={record.get('status')} {record.get('error')}")
        if "expect" in case and not failures:
            failures += validate_expect(case["expect"], record)
        elif "expect" not in case and record.get("status") == "done":
            if record.get("reply", "").startswith(("ERROR", "Agent")):
                failures.append("operation failed")
        # Launching a remote Agent measures the host (18-24 s), not the voice path;
        # with only CORE_PCM it alone would be the P95.  0 = not measured.
        if case["id"] not in REMOTE_AGENT_PCM and record.get("elapsed_ms"):
            latencies.append(record["elapsed_ms"])
        stt.append(record.get("stt_ms", 0))
        report.add("S1", f"pcm:{case['id']}", not failures, "; ".join(failures) or transcript,
                   elapsed_ms=record.get("elapsed_ms"), stt_ms=record.get("stt_ms"),
                   calls=operations(record))
        device.action("wake")
    if ONLY:  # a filtered run checks only the selected cases
        device.post("/api/delete", path="/vat/tests")
        return
    report.add("S1", "voice_latency", percentile(latencies, 0.5) < 10000 and percentile(latencies, 0.95) < 18000,
               f"P50={percentile(latencies, .5)}ms P95={percentile(latencies, .95)}ms "
               f"STT P50={percentile(stt, .5)}ms")
    # TEXT mode: transcription only, no agent call.
    record = device.job("transcribe", path="/vat/tests/local_status.pcm")
    report.add("S1", "text_mode_transcribe_only", record.get("status") == "done" and not record.get("calls")
               and "本机" in record.get("reply", ""), record.get("reply", ""))
    device.post("/api/delete", path="/vat/tests")


def phase_s2i(device: Device, report: Report) -> None:
    """Intent matrix: one model turn per case; needed when the prompt or the agent loop changes."""
    if quick():
        return
    for name in ("ssh_intent_cases.json", "daily_intent_cases.json"):
        for case in json.loads((CASES / name).read_text()):
            if not wanted(case["id"]):
                continue
            if release() and case["id"] not in S2I_SAMPLE:  # the rest: prompt_lab on the host
                continue
            # Intent cases are independent single turns, like the reference evaluation.
            device.wait_idle()
            device.action("clear_history")
            record = device.text(case["input"])
            device.resolve_all(approve=False)
            failures = validate_expect(case["expect"], record)
            if record.get("status") not in ("done", "awaiting_approval", "awaiting_secret"):
                failures.insert(0, f"status={record.get('status')} {record.get('error')}")
            report.add("S2I", f"intent:{case['id']}", not failures, "; ".join(failures),
                       calls=operations(record), reply=record.get("reply", "")[:300])
            device.action("wake")



def phase_s2(device: Device, report: Report) -> None:
    """Runtime scenarios: multi-turn conversation, approvals, policy."""
    if not quick():
        phase_s2_runtime(device, report)
    phase_s2_approvals(device, report)


def phase_s2_runtime(device: Device, report: Report) -> None:
    # Bounded multi-turn conversation (reference runtime matrix).
    runtime_cases = [c for c in json.loads((CASES / "runtime_cases.json").read_text()) if wanted(c["id"])]
    device.wait_idle()
    device.action("clear_history")
    for case in runtime_cases:
        record = device.text(case["text"])
        device.resolve_all(approve=False)
        output = everything(record).casefold()
        expected = [v.format(**host_facts()) for v in case.get("contains", [])]
        failures = [v for v in expected if v.casefold() not in output]
        if case.get("contains_any") and not any(v.casefold() in output for v in case["contains_any"]):
            failures.append(" / ".join(case["contains_any"]))
        if case.get("requires_discover") and not any(c["name"] == "discover" for c in record.get("calls", [])):
            failures.append("missing discover")
        detail = ("; ".join(failures) + " | reply: " + (record.get("reply") or record.get("error") or "")) if failures else record.get("reply", "")
        report.add("S2", f"runtime:{case['id']}", not failures, detail[:240])



def phase_s2_approvals(device: Device, report: Report) -> None:
    if ONLY and not wanted("approval"):  # --only approval runs the scenarios below
        return
    # Real approval flow.
    host(f"rm -rf {HOST_TMP}; mkdir -p {HOST_TMP}")
    record = device.text(f"在开发机执行 mkdir -p {HOST_TMP}/approved")
    pending = record.get("status") == "awaiting_approval"
    approved = device.resolve_all(approve=True)
    exists = host(f"test -d {HOST_TMP}/approved").returncode == 0
    report.add("S2", "approve_executes", pending and bool(approved) and exists,
               f"pending={pending} exists={exists}")
    record = device.text(f"删除开发机上的 {HOST_TMP}/approved 目录")
    pending = record.get("status") == "awaiting_approval"
    device.resolve_all(approve=False)
    still = host(f"test -d {HOST_TMP}/approved").returncode == 0
    report.add("S2", "reject_skips", pending and still, f"pending={pending} still_exists={still}")
    if quick():
        return
    record = device.text(f"把你好写入开发机的 {HOST_TMP}/note.txt，覆盖原内容。")
    device.resolve_all(approve=True)
    content = host(f"cat {HOST_TMP}/note.txt").stdout
    report.add("S2", "approved_write", content == "你好", repr(content))
    record = device.text(f"读取开发机上的 {HOME}/.ssh/id_ed25519 文件。")
    device.resolve_all(approve=True)
    leaked = "PRIVATE KEY" in json.dumps(record, ensure_ascii=False)
    report.add("S2", "credential_hard_deny", not leaked and "拒绝" in json.dumps(record, ensure_ascii=False),
               record.get("reply", "")[:120])
    # Policy mode change requires approval and is then enforced.
    device.text("把 ssh://开发机 的权限策略改成 ask。")
    device.resolve_all(approve=True)
    record = device.text("在开发机执行 whoami。")
    gated = record.get("status") == "awaiting_approval"
    device.resolve_all(approve=False)
    device.text("把 ssh://开发机 的权限策略改成 allow。")
    device.resolve_all(approve=True)
    record = device.text("在开发机执行 whoami。")
    report.add("S2", "policy_ask_then_allow", gated and final(record) == host("whoami").stdout.strip(),
               f"gated={gated} result={final(record)!r}")


def phase_s3(device: Device, report: Report) -> None:
    expectations = [
        ("whoami", "在开发机执行 whoami。", host("whoami").stdout.strip(), "exact"),
        ("hostname", "在开发机执行 hostname。", host("hostname").stdout.strip(), "exact"),
        ("pwd", "在开发机查看当前目录。", os.path.expanduser("~"), "exact"),
        ("uname", "在开发机执行 uname -a。", "Linux", "contains"),
        ("uptime", "在开发机执行 uptime。", "load average", "contains"),
        ("df", "在开发机执行 df -h。", "Filesystem", "contains"),
        ("read_hostname", "读取开发机上的 /etc/hostname 文件。", Path("/etc/hostname").read_text().strip(), "exact"),
    ]
    if quick():
        expectations = expectations[:2]
    elif release():  # hostname, pwd, uptime, df repeat whoami's path (S2, S5 run them too)
        expectations = [e for e in expectations if e[0] in ("whoami", "uname", "read_hostname")]
    for case_id, text, expected, mode in expectations:
        record = device.text(text)
        reply = final(record)
        ok = reply == expected if mode == "exact" else expected in reply
        report.add("S3", f"ssh:{case_id}", ok and bool(record.get("reply")),
                   f"{reply[:80]!r} expected {mode} {expected!r}; summary={record.get('reply', '')[:60]!r}")
    if quick():
        return
    record = device.text("我不确定开发机有没有 Docker，请先发现容器能力，只告诉我是否可用，不要执行容器命令。")
    discovered = [c for c in record.get("calls", []) if c["name"] == "discover"]
    report.add("S3", "discover_docker", any("docker" in c.get("result", "") and "上可用" in c.get("result", "")
                                            for c in discovered),
               record.get("reply", "")[:120])
    record = device.text("列出开发机上正在运行的 docker 容器。")
    expected = host("docker ps --format '{{.Names}}' | head -1").stdout.strip()
    report.add("S3", "docker_ps", "CONTAINER ID" in raw(record) and expected in raw(record),
               record.get("reply", "")[:100])
    sessions = (("连接 SSH 配置开发机。", "已连接"), ("列出当前 SSH 连接。", "开发机"),
                ("断开开发机的 SSH 连接。", "断开"))
    # Connect / list / disconnect: S2 runtime and S4 connect, S2I samples the intent.
    for text, word in () if release() else sessions:
        record = device.text(text)
        report.add("S3", f"session:{word}", word in everything(record), record.get("reply", "")[:100])
    record = device.text(f"在开发机执行 git status，工作目录是 {ROOT}。")
    report.add("S3", "exec_cwd", "On branch" in raw(record) or "位于分支" in raw(record),
               record.get("reply", "")[:100])


def tmux_has(name: str) -> bool:
    return host(f"tmux has-session -t {name}").returncode == 0


def phase_s4(device: Device, report: Report) -> None:
    # Terminals are opened by the agent: "connect" gives the SSH page a shell.
    host("tmux kill-session -t vat-shell 2>/dev/null")
    record = device.text("连接 SSH 配置开发机。")
    device.wait_idle()
    device.action("page_ssh")
    device.wait_idle()
    time.sleep(1)
    screen = device.status()["screen"]
    report.add("S4", "connect_opens_terminal", tmux_has("vat-shell") and "vat-shell" in screen["header"],
               f"{screen['header']} | {record.get('reply', '')[:60]}")
    host("tmux kill-session -t vat-shell")
    device.job("observe")
    screen = device.status()["screen"]
    report.add("S4", "ended_session_guides_to_agent", "会话已结束" in screen["body"] and "未连接" in screen["header"],
               screen["body"][:60])
    device.action("page_agent")
    host("tmux kill-session -t vat-e2e 2>/dev/null")
    record = device.text("在开发机创建名为 vat-e2e 的 tmux 会话。")
    report.add("S4", "tmux_create", tmux_has("vat-e2e"), record.get("reply", ""))
    mark = f"VAT_E2E_{uuid.uuid4().hex[:6]}"
    device.job("input", text=f"echo {mark}", submit="1")
    time.sleep(1)
    observed = device.job("observe").get("reply", "")
    report.add("S4", "terminal_input_observe", observed.count(mark) >= 2, observed[-120:])
    if not quick():  # two model turns; the input / observe path itself is checked just above
        record = device.text("在当前终端输入 Control C。")
        keys = [c["arguments"].get("keys") for c in operations(record)]
        report.add("S4", "agent_ctrl_c", any(k and "CTRL_C" in [x.upper() for x in k] for k in keys), str(keys))
        record = device.text("查看当前终端的输出。")
        report.add("S4", "agent_observe", mark in raw(record), record.get("reply", "")[-80:])

    # UI pages through the same actions as Btn0 / Fn+Btn0.
    device.action("page_ssh")
    screen = device.status()["screen"]
    report.add("S4", "ui_ssh_page", screen["page"] == "ssh" and mark in screen["body"], screen["header"])
    device.action("toggle")  # Btn0: Agent, SSH, program, graphics (when a picture is open)
    report.add("S4", "ui_toggle_program", device.status()["screen"]["page"] == "program")
    device.action("toggle")
    report.add("S4", "ui_toggle_agent", device.status()["screen"]["page"] == "agent")
    screen = device.status()["screen"]
    report.add("S4", "ui_timeline", screen["entries"] > 0,
               f"entries={screen['entries']}")
    device.action("cursor_up")
    moved = device.status()["screen"]["cursor"] >= 0
    device.action("open_detail")
    screen = device.status()["screen"]
    report.add("S4", "ui_cursor_detail", moved and screen["view"] == "detail" and len(screen["body"]) > 0,
               screen["body"][:60])
    device.action("close")
    device.action("cursor_latest")
    device.action("help")
    first = device.status()["screen"]["help_page"]
    device.action("help_next")
    second = device.status()["screen"]["help_page"]
    device.action("close")
    report.add("S4", "ui_help_pages", (first, second) == (1, 2), f"pages={first},{second}")
    shot = device._request("GET", "/api/screenshot")[1]
    report.add("S4", "ui_screenshot", shot[:2] == b"BM" and len(shot) > 90000, f"bytes={len(shot)}")

    # Remote coding agents in read-only mode (slow: waits for the agent's answer).
    for provider, name in () if quick() else (("Pi", "pi"),):  # the remote Agent path is provider-agnostic; Pi is the one installed and signed in
        session = f"vat-{name}-voice-agent-terminal"
        host(f"tmux kill-session -t {session} 2>/dev/null")
        record = device.text(f"让 {provider} 在 voice-agent-terminal 项目里看看 cardputer/README.md 的第一行标题是什么，只读不要修改。",
                             timeout=180)
        started = tmux_has(session)
        # The title is "肥鱼 OS（FeiyuOS）".  An agent that sums up the whole README
        # scrolls the title off before the last poll: every screen seen counts.
        titled = lambda text: any(word in text for word in ("FeiyuOS", "肥鱼", "Voice Agent Terminal"))
        screen, seen = "", ""
        for _ in range(20 if started else 0):  # not started: nothing will answer, skip the ~100 s of polling
            time.sleep(4)
            screen = device.job("observe").get("reply", "")
            seen += screen
            if titled(screen):
                break
        report.add("S4", f"agent_{name}_start", started, record.get("reply", "")[:120])
        report.add("S4", f"agent_{name}_answer", titled(seen),
                   screen[-160:] if started else "skipped: the agent did not start")
        record = device.text("中断当前 Agent。")
        report.add("S4", f"agent_{name}_interrupt",
                   any(c["arguments"].get("action") == "interrupt" for c in operations(record)),
                   record.get("reply", ""))

    # Esc cancels an in-flight request and the device stays usable.
    job_id = None
    for _ in range(40):  # the SSH page may still be refreshing in the background
        device.wait_idle()
        status, body = device.post("/api/job", kind="text", text="在开发机执行 uptime。")
        if status == 200:
            job_id = json.loads(body)["id"]
            break
        time.sleep(0.5)
    time.sleep(0.8)
    cancel = device.action("cancel")
    while True:
        record = device.get_json("/api/job", id=job_id)
        if record["status"] not in ("queued", "running"):
            break
        time.sleep(0.3)
    follow = device.text("在开发机执行 whoami。")
    report.add("S4", "esc_cancel", cancel == "cancelling" and record["status"] == "cancelled"
               and final(follow) == host("whoami").stdout.strip(),
               f"status={record['status']} elapsed={record.get('elapsed_ms')} follow={final(follow)!r}")


def phase_s5(device: Device, report: Report) -> None:
    before = device.status()
    after = device.reboot()
    report.add("S5", "reboot_ready", after.get("config_ready") and after.get("ip") and after["uptime_s"] < 120,
               f"uptime={after.get('uptime_s')}")
    report.add("S5", "conversation_restored", after.get("history_messages", 0) > 0,
               f"history_messages={after.get('history_messages')}")
    device.action("page_agent")
    screen = device.status()["screen"]
    report.add("S5", "timeline_restored", screen["entries"] > 0, screen["body"][:80])
    record = device.text("我们刚才操作的是哪个 SSH profile？只回答名称，不执行操作。")
    report.add("S5", "context_after_reboot", "开发机" in record.get("reply", ""), record.get("reply", ""))
    observed = device.job("observe").get("reply", "")
    report.add("S5", "tui_session_restored", observed not in ("", "no tui") and "当前没有" not in observed,
               observed[-80:])
    if quick():
        return

    latencies, failures, summaries = [], 0, []
    start_uptime = device.status()["uptime_s"]
    rounds = 20 if full() else 10  # release: 10 rounds (P50/P95 and the heap floor still show)
    for index in range(rounds):
        text = ["在开发机执行 uptime。", "查看这台 Cardputer 的本机状态。", "在开发机执行 whoami。",
                "再查看这台机器的主机名。"][index % 4]
        record = device.text(text)
        device.resolve_all(approve=False)
        latencies.append(record.get("elapsed_ms", 0))
        failures += 0 if record.get("status") == "done" and record.get("reply") else 1
        summaries.append(len(record.get("reply", "")))
    status = device.status()
    name = "twenty_rounds" if rounds == 20 else "ten_rounds"
    report.add("S5", name, failures == 0 and status["uptime_s"] > start_uptime,
               f"failures={failures} P50={percentile(latencies, .5)}ms P95={percentile(latencies, .95)}ms "
               f"reply_chars_max={max(summaries) if summaries else 0} "
               f"min_free_heap={status.get('min_free_heap')}")
    # Design target: >= 60 KB available when a high-memory stage starts.  The
    # runtime log records the heap at each stage start; min_free_heap is the
    # all-time low inside a TLS handshake and is reported for information.
    device.wait_idle()
    log = device.read_file("/vat/logs/runtime.jsonl", tail=12000)
    starts = []
    for line in log.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get("event") in ("agent_start", "stt_start"):
            starts.append(event.get("heap", 0))
    floor = min(starts) if starts else 0
    all_time = status.get("min_free_heap", 0)
    # The all-time low bounds every stage start from below, so it suffices
    # when the stage events have already rotated out of the log.
    ok = all_time >= 60000 or (bool(starts) and floor >= 60000)
    report.add("S5", "stage_start_heap_60k", ok,
               f"stage_starts={len(starts)} min_at_start={floor} all_time_min={status.get('min_free_heap')}")
    ok, detail = gateway_absent()
    report.add("S5", "gateway_absent_usb_unused", ok, detail)


def ir_confirm_flow(device: Device, report: Report) -> None:
    """ir: send, "no" moves to the next code set, "yes" is remembered.  Profile removed after."""
    if not wanted("ir"):
        return
    name, path = "e2e电视", "/vat/things/ir/e2e电视.json"
    device.post("/api/delete", path=path)
    device.wait_idle()
    device.action("clear_history")
    targets = []
    for text in (f"用红外打开{name}", "没反应", "生效了"):
        record = device.text(text)
        device.resolve_all(approve=False)
        targets += [c["arguments"].get("target", "") + "=" + str(c["arguments"].get("content", ""))
                    for c in operations(record) if c["name"] == "write"]
    try:
        profile = json.loads(device.read_file(path))
    except Exception:  # noqa: BLE001 - missing or unreadable profile is the failure being reported
        profile = {}
    worked = [v["worked"] for v in profile.get("verifications", [])]
    report.add("S6", "ir_confirm_flow", worked == [False, True] and profile.get("code_set") == 1,
               f"targets={targets} verifications={worked} code_set={profile.get('code_set')}")
    device.post("/api/delete", path=path)


def ac_choose_flow(device: Device, report: Report) -> None:
    """ir AC: pick the code set by hand (previous / next / number / resend), not only forward.  Profile removed after."""
    if not wanted("ir"):
        return
    name, path = "e2e空调", "/vat/things/ir/e2e空调.json"
    device.post("/api/delete", path=path)

    def say(target: str, content: str) -> str:
        return reply_text(tool_call(device, "write", f"device://local/ir/{name}/{target}", content))

    def trying() -> int:
        try:
            return json.loads(device.read_file(path)).get("trying", -1)
        except Exception:  # noqa: BLE001 - missing profile reads as no trial
            return -1

    steps = []
    say("ac", "格力 开 制冷 26")  # not the AUX units on the test bench
    steps.append(("start", trying(), 0))
    first_prev = say("try", "上一套")
    steps.append(("prev_at_first", trying(), 0))
    say("try", "下一套")
    steps.append(("next", trying(), 1))
    say("try", "5")
    steps.append(("number_5", trying(), 4))
    say("try", "上一套")
    steps.append(("prev", trying(), 3))
    say("try", "重发")
    steps.append(("again", trying(), 3))
    out_of_range = say("try", "999")
    steps.append(("out_of_range", trying(), 3))
    say("confirm", "no")
    steps.append(("confirm_no_moves_on", trying(), 4))
    ok = all(got == want for _, got, want in steps) and "前面没有了" in first_prev and "之间" in out_of_range
    report.add("S6", "ac_choose_code_set", ok,
               " ".join(f"{k}={got}/{want}" for k, got, want in steps) + f" first_prev={first_prev[:30]!r}")
    device.post("/api/delete", path=path)


def files_timer_grove(device: Device, report: Report) -> None:
    """files (card, secrets refused), grove scan, a 10 s reminder that fires as an overlay."""
    if not wanted("files_timer"):
        return
    results = {}
    for key, text in (("list", "卡上 /vat/memory 里有什么文件"), ("secret", "读一下卡上的 /vat/secrets.json"),
                      ("grove", "Grove 口接了什么"), ("timer", "十秒后提醒我 e2e 测试")):
        device.wait_idle()
        device.action("clear_history")
        record = device.text(text)
        device.resolve_all(approve=False)
        results[key] = (operations(record), raw(record))
    calls, out = results["list"]
    report.add("S6", "files_list", any(c["arguments"].get("target", "").startswith("file://") for c in calls)
               and "timeline.jsonl" in out, out[:100])
    calls, out = results["secret"]
    report.add("S6", "files_secret_refused", "拒绝" in out or "不能" in out, out[:100])
    calls, out = results["grove"]
    report.add("S6", "grove_scan", "Grove" in out and any(c["arguments"].get("target") == "sensor://grove" for c in calls),
               out[:100])
    calls, out = results["timer"]
    fired = False
    if "已设提醒" in out:
        deadline = time.time() + 30
        while time.time() < deadline and not fired:
            fired = device.status()["screen"].get("overlay") == "text"
            time.sleep(1)
        if fired:
            device.key("x")
    report.add("S6", "timer_fires", fired, f"{[c['arguments'].get('target') for c in calls]} {out[:60]}")


def hardware_controls(device: Device, report: Report) -> None:
    """hardware package: on-board LED, IMU, beep, battery fields.  No confirmations."""
    if not wanted("hardware"):
        return
    status = device.status()
    report.add("S6", "battery_fields", status.get("battery_mv", 0) > 3000 and "usb_host" in status,
               f"mv={status.get('battery_mv')} usb={status.get('usb_host')}")
    results = {}
    turns = (("led", "把本机的 RGB 灯调成绿色"), ("imu", "设备现在是平放的吗"),
             ("beep", "响一声提示音"), ("led_off", "把本机 RGB 灯关掉"))
    for key, text in turns[:1] if quick() else turns:  # quick: one LED turn
        device.wait_idle()
        device.action("clear_history")
        record = device.text(text)
        results[key] = (record.get("status"), raw(record))
        device.resolve_all(approve=False)
    if quick():
        tool_call(device, "write", "device://local/led", "off")  # turned off directly, no model turn
        report.add("S6", "led_local", results["led"][0] == "done" and "RGB 灯已设为" in results["led"][1],
                   str(results["led"][0]))
        return
    report.add("S6", "led_local", results["led"][0] == "done" and "RGB 灯已设为" in results["led"][1]
               and "RGB 灯已关闭" in results["led_off"][1], f"{results['led'][0]} / {results['led_off'][0]}")
    report.add("S6", "imu_orientation", results["imu"][0] == "done" and ("俯仰" in results["imu"][1]),
               results["imu"][1][-120:])
    report.add("S6", "beep_no_confirm", results["beep"][0] == "done" and "提示音" in results["beep"][1],
               results["beep"][0])


def display_overlay(device: Device, report: Report) -> None:
    """display package: a full-screen overlay that any key closes, page underneath intact."""
    if not wanted("display"):
        return
    device.wait_idle()
    device.action("clear_history")
    page = device.status()["screen"]["page"]
    record = device.text("在屏幕上全屏大字显示：会议中")
    targets = [c["arguments"].get("target", "") for c in operations(record) if c["name"] == "write"]
    shown = device.status()["screen"].get("overlay")
    after = device.key("x")
    report.add("S6", "display_text_overlay",
               any(t.startswith("device://local/display/") for t in targets) and shown == "text"
               and after.get("overlay") == "" and after["page"] == page,
               f"targets={targets} overlay={shown!r} after_key={after.get('overlay')!r}")


def phase_s6(device: Device, report: Report) -> None:
    """Capability packages: registry/discover and the wifi package."""
    device.wait_idle()
    device.action("clear_history")
    record = device.text("用 discover 列出这台设备现在有哪些能力包，只告诉我包名。")
    found = record.get("reply", "") + " ".join(c.get("result", "") for c in record.get("calls", []))
    expected = ["memory", "local", "ssh", "tmux", "agents", "policy", "wifi", "web", "linux", "git", "docker", "systemd",
                "display", "hardware", "files", "timer", "grove", "ir"]
    discovered = any(c["name"] == "discover" for c in record.get("calls", []))
    missing = [p for p in expected if p not in found] if discovered else ["(no discover call)"]
    report.add("S6", "registry_discover_packages", not missing, f"missing={missing}")

    status = device.status()
    current = status.get("wifi", "")
    device.action("clear_history")
    record = device.text("帮我搜索一下附近的 WiFi。")
    calls = operations(record)
    scanned = calls and calls[0]["name"] == "read" and calls[0]["arguments"].get("target") == "device://local/wifi/scan"
    report.add("S6", "wifi_scan_voice", bool(scanned) and current in raw(record) and bool(record.get("reply")),
               f"calls={[c['arguments'].get('target') for c in calls]} summary={record.get('reply', '')[:60]!r}")

    if not quick():
        device.action("clear_history")
        record = device.text("现在连的是哪个 WiFi？")
        calls = operations(record)
        report.add("S6", "wifi_status_voice",
                   bool(calls) and calls[0]["arguments"].get("target") in ("device://local/wifi/status", "device://local/wifi/scan")
                   and current in raw(record) + record.get("reply", ""),
                   record.get("reply", "")[:80])
    display_overlay(device, report)
    hardware_controls(device, report)  # quick: battery fields and one LED turn
    if quick():  # IR, AC code sets, files / timer / grove, IMU, beep: normal and up
        return
    files_timer_grove(device, report)
    ir_confirm_flow(device, report)
    ac_choose_flow(device, report)

    device.action("clear_history")
    secrets_before = device.read_file("/vat/secrets.json")
    record = device.text("连接 WiFi VAT-Test-Net，密码是 12345678。")
    screen = device.status()["screen"]
    asked_secret = record.get("status") == "awaiting_secret" and "VAT-Test-Net" in screen.get("secret_prompt", "")
    spoken_ignored = "12345678" not in json.dumps(screen, ensure_ascii=False)
    device.resolve_all(approve=False)  # abort: never leave the test network
    after = device.status()
    unchanged = device.read_file("/vat/secrets.json") == secrets_before and after.get("wifi") == current
    report.add("S6", "wifi_password_via_device_input", asked_secret and unchanged,
               f"status={record.get('status')} prompt={screen.get('secret_prompt')!r} unchanged={unchanged} "
               f"spoken_password_not_used={spoken_ignored}")

    # Ordinals: "the Nth one" refers to the last numbered list shown.
    import re
    device.action("clear_history")
    listing = device.text("搜索一下附近的 WiFi。")
    items = re.findall(r"^(\d+)\. (.+?)  信号.*?(加密|开放)(  当前连接)?$", raw(listing), re.M)
    target = next(((int(n), name) for n, name, sec, cur in items if sec == "加密" and not cur), None)
    if target:
        record = device.text(f"连接第 {target[0]} 个。")
        prompt = device.status()["screen"].get("secret_prompt", "")
        device.resolve_all(approve=False)
        report.add("S6", "ordinal_wifi", record.get("status") == "awaiting_secret" and target[1] in prompt
                   and device.status().get("wifi") == current, f"#{target[0]}={target[1]!r} prompt={prompt!r}")
    else:
        report.add("S6", "ordinal_wifi", False, f"no encrypted network to pick: {items}")

    host("tmux kill-session -t vat-o1 2>/dev/null; tmux kill-session -t vat-o2 2>/dev/null;"
         " tmux new-session -d -s vat-o1; tmux new-session -d -s vat-o2")
    device.action("clear_history")
    listing = device.text("列出开发机上的 tmux 会话。")
    numbered = dict((name.strip(), int(n)) for n, name in re.findall(r"^(\d+)\. ([^（\n]+)（", raw(listing), re.M))
    k = numbered.get("vat-o2")
    record = device.text(f"接入第 {k} 个。") if k else {}
    device.action("page_ssh")
    device.wait_idle()
    header = device.status()["screen"]["header"]
    device.action("page_agent")
    report.add("S6", "ordinal_tmux", bool(k) and "vat-o2" in header, f"#{k} header={header!r}")
    host("tmux kill-session -t vat-o1 2>/dev/null; tmux kill-session -t vat-o2 2>/dev/null")

    device.action("clear_history")
    listing = device.text("列出开发机上正在运行的容器。")
    names = host("docker ps --format '{{.Names}}'").stdout.split()
    shown = re.findall(r"1[.、]\s*\**([A-Za-z0-9_.-]+)", listing.get("reply", ""))
    first = next((n for n in shown if n in names), names[0] if names else "")
    record = device.text("看第 1 个容器的最后 5 行日志。")
    args = [c["arguments"] for c in operations(record) if c["name"] == "exec"]
    used = any(first in a.get("args", []) for a in args)
    report.add("S6", "ordinal_docker", used and "logs" in json.dumps(args), f"first={first!r} args={args}")

    record = device.text("读取书桌温度传感器。")
    report.add("S6", "unknown_scheme_not_guessed", "没有能力包可处理" in raw(record) or not operations(record),
               raw(record)[:80])


def ssh_banner(port: int = 2222) -> bool:
    """sshd on this host answers with its version line (it accepts logins)."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.settimeout(2)
            return s.recv(64).startswith(b"SSH-")
    except OSError:
        return False


def fresh_test_box() -> bool:
    """A disposable SSH host that needs a password for login and sudo.

    A running box is reused (it is left running after a run): only the key the
    device installed last time is removed, so the login still asks for the
    password.  Otherwise it is created; ready = sshd answers (no fixed wait)."""
    running = host(f"docker inspect -f '{{{{.State.Running}}}}' {TEST_BOX} 2>/dev/null").stdout.strip() == "true"
    reset = running and host(f"docker exec -u vat {TEST_BOX} sh -c 'rm -f \"$HOME/.ssh/authorized_keys\"'").returncode == 0
    started = reset
    if not reset:
        host(f"docker rm -f {TEST_BOX} >/dev/null 2>&1")
        started = host(f"docker run -d --name {TEST_BOX} -p 2222:2222 -e PUID=1000 -e PGID=1000 -e USER_NAME=vat "
                       f"-e USER_PASSWORD={TEST_BOX_PASSWORD} -e PASSWORD_ACCESS=true -e SUDO_ACCESS=true "
                       "linuxserver/openssh-server:latest").returncode == 0
    return started and bool(until(ssh_banner, 33, 1))


def secret_job(device: Device, value: str) -> dict:
    device.wait_idle()
    status, body = device.post("/api/secret", value.encode())
    if status != 200:
        return {"status": f"http {status}", "reply": body}
    job_id = json.loads(body)["id"]
    while True:
        record = device.get_json("/api/job", id=job_id)
        if record.get("status") not in ("queued", "running"):
            return record
        time.sleep(0.5)


def answer(device: Device, name: str) -> dict:
    device.wait_idle()
    result = device.action(name)
    if not result.isdigit():
        return {"status": result}
    while True:
        record = device.get_json("/api/job", id=int(result))
        if record.get("status") not in ("queued", "running"):
            return record
        time.sleep(0.5)


def phase_s7(device: Device, report: Report) -> None:
    """Multi-step tasks (Y / N / A) and secrets typed on the device."""
    base = f"{HOST_TMP}/ms"
    host(f"rm -rf {base}; mkdir -p {base}; echo vat-detail > {base}/note.txt; seq 1 50 > {base}/numbers.txt")

    # Dependent read-only steps: list, then read what the listing showed.
    device.wait_idle()
    device.action("clear_history")
    record = device.text(f"在开发机看下 {base} 里有哪些文件，然后显示 note.txt 的内容。", timeout=180)
    execs = [c for c in operations(record) if c["name"] in ("exec", "read") and c["decision"] == "allow"]
    report.add("S7", "multistep_readonly", record.get("status") == "done" and len(execs) >= 2
               and "vat-detail" in raw(record), f"steps={len(execs)} reply={record.get('reply', '')[:60]!r}")

    # Step-by-step approval: every state-changing step pauses for Y.
    device.action("clear_history")
    record = device.text(f"在开发机创建目录 {base}/y，然后在里面新建 note.txt 内容是 hi，最后列出这个目录。",
                         timeout=180)
    pauses = 0
    while record.get("status") == "awaiting_approval" and pauses < 5:
        pauses += 1
        record = answer(device, "approve")
    content = host(f"cat {base}/y/note.txt").stdout
    report.add("S7", "multistep_approve_each", pauses >= 2 and record.get("status") == "done" and content == "hi",
               f"pauses={pauses} content={content!r} reply={record.get('reply', '')[:60]!r}")
    if quick():
        return

    # A: approve this and every later step of the task.
    device.action("clear_history")
    record = device.text(f"在开发机创建目录 {base}/a，然后在里面新建 note.txt 内容是 all，最后列出这个目录。",
                         timeout=180)
    first = record.get("status")
    record = answer(device, "approve_all") if first == "awaiting_approval" else record
    decisions = [c["decision"] for c in operations(record)]
    content = host(f"cat {base}/a/note.txt").stdout
    report.add("S7", "multistep_approve_all", first == "awaiting_approval" and record.get("status") == "done"
               and "approved_all" in decisions and content == "all", f"decisions={decisions}")

    # N: the model is told and does not run it.
    device.action("clear_history")
    record = device.text(f"在开发机创建目录 {base}/n。", timeout=120)
    if record.get("status") == "awaiting_approval":
        record = answer(device, "reject")
    device.resolve_all(approve=False)
    exists = host(f"test -d {base}/n").returncode == 0
    report.add("S7", "multistep_reject", not exists and record.get("status") == "done",
               f"exists={exists} reply={record.get('reply', '')[:60]!r}")

    # Secrets on a disposable host that needs passwords.  Outside `full`, the
    # leak scan below reads only files that appeared or changed size from here
    # on: a password typed in this phase can only reach a file written after it.
    files_before = None if full() else dict(device.files("/vat"))
    if not fresh_test_box():
        report.add("S7", "test_box", False, "docker run failed or its sshd never answered")
        return
    device.action("clear_history")
    record = device.text("连接测试机。", timeout=120)
    prompt = device.status()["screen"].get("secret_prompt", "")
    login_prompt = record.get("status") == "awaiting_secret" and "登录密码" in prompt
    record = secret_job(device, TEST_BOX_PASSWORD) if login_prompt else record
    device.action("clear_history")
    whoami = device.text("在测试机执行 whoami。")
    report.add("S7", "secret_installs_key", login_prompt and final(whoami) == "vat",
               f"prompt={prompt!r} after={record.get('reply', '')[:50]!r} key_login={final(whoami)!r}")

    device.action("clear_history")
    record = device.text("在测试机执行 sudo whoami。", timeout=120)
    if record.get("status") == "awaiting_approval":
        record = answer(device, "approve")
    prompt = device.status()["screen"].get("secret_prompt", "")
    sudo_prompt = record.get("status") == "awaiting_secret" and "sudo" in prompt
    record = secret_job(device, TEST_BOX_PASSWORD) if sudo_prompt else record
    report.add("S7", "secret_sudo", sudo_prompt and "root" in raw(record), f"prompt={prompt!r} result={raw(record)!r}")

    device.action("clear_history")
    record = device.text("在测试机执行 sudo whoami。", timeout=120)
    if record.get("status") == "awaiting_approval":
        record = answer(device, "approve")
    record = secret_job(device, "wrong-password") if record.get("status") == "awaiting_secret" else record
    report.add("S7", "secret_wrong_sudo_password", "root" not in raw(record) and "密码" in raw(record) + record.get("reply", ""),
               raw(record)[:80])

    # Every file the device stored (`full`), or every file written since the
    # secrets started, read whole (the file API streams any size).
    leaks, scanned, total = [], 0, 0
    for path, size in device.files("/vat"):
        if path.endswith(".pcm"):
            continue
        total += 1
        if files_before is not None and files_before.get(path) == size:
            continue
        scanned += 1
        if TEST_BOX_PASSWORD in device.read_file(path):
            leaks.append(path)
    # The secret steps always write (runtime log, timeline, window): none changed means the scan saw nothing.
    enough = total > 10 and (scanned > 10 if files_before is None else scanned > 0)
    report.add("S7", "secret_never_stored", enough and not leaks,
               f"scanned={scanned}/{total} files{'' if files_before is None else ' (changed this phase)'} "
               f"found_in={leaks or 'nowhere'}")

    # Terminal password prompt: the SSH page switches to masked input.
    host("tmux kill-session -t vat-pw 2>/dev/null")
    device.text("在开发机创建名为 vat-pw 的 tmux 会话。")
    device.job("input", text="read -s -p 'Password: ' P; echo; echo len:${#P}", submit="1")
    time.sleep(1)
    device.job("observe")
    masked = device.status()["screen"]["terminal_password"]
    device.job("input", text="abc", submit="1")
    time.sleep(1)
    screen_after = device.job("observe").get("reply", "")
    report.add("S7", "terminal_password_mode", masked and "len:3" in screen_after
               and not device.status()["screen"]["terminal_password"], f"masked={masked} tail={screen_after[-30:]!r}")
    host("tmux kill-session -t vat-pw 2>/dev/null")


def make_long_log(path: str) -> list[tuple[int, str]]:
    """~310 KB log with an ERROR every 487 lines; returns (line, code) pairs."""
    import random
    rng = random.Random(7)
    lines, errors = [], []
    for i in range(1, 4001):
        if i % 487 == 0:
            code = "E%04d" % rng.randint(1000, 9999)
            errors.append((i, code))
            lines.append(f"2026-11-04 10:{i % 60:02d}:00 ERROR worker-{i % 7} failed with code {code} on shard {i % 13}")
        else:
            lines.append(f"2026-11-04 10:{i % 60:02d}:00 INFO request {i} handled in {rng.randint(3, 400)}ms "
                         f"path=/api/v1/items/{i}")
    host(f"mkdir -p {HOST_TMP}")
    Path(path).write_text("\n".join(lines) + "\n")
    return errors


def synthetic_window(bytes_wanted: int, fact: str) -> bytes:
    """Window lines (one turn each) totalling about bytes_wanted, with one
    fact stated in the oldest turn."""
    out, n = [], 0
    while sum(len(x) for x in out) < bytes_wanted:
        n += 1
        user = fact if n == 1 else f"第 {n} 次巡检：看一下 web-{n % 9} 的负载和最近的发布记录。"
        answer = ("已记录。" if n == 1 else
                  f"web-{n % 9} 负载 0.{n % 10}，最近发布 v1.{n}.0，无告警；" + "巡检明细正常。" * 40)
        line = "," + json.dumps({"role": "user", "content": user}, ensure_ascii=False) + "," + \
               json.dumps({"role": "assistant", "content": answer}, ensure_ascii=False) + "\n"
        out.append(line.encode())
    return b"".join(out)


def phase_s8(device: Device, report: Report) -> None:
    """Long context on the TF card: complete outputs, history search, cases,
    compaction, latency with a full window, and web search."""
    status = device.status()
    if status.get("storage") != "tf":
        report.add("S8", "tf_card_present", False, f"storage={status.get('storage')}")
        return
    report.add("S8", "tf_card_present", True, "storage=tf")

    # 1. A 310 KB file: saved whole (up to 256 KB) as out#N; the model pages/greps instead of rerunning.
    errors = make_long_log(f"{HOST_TMP}/long.log")
    fourth = errors[3][1]
    device.wait_idle()
    device.action("clear_history")
    record = device.text(f"读取开发机上的 {HOST_TMP}/long.log，告诉我里面第 4 个 ERROR 的错误码是多少。", timeout=180)
    device.resolve_all(approve=False)
    names = [c["arguments"].get("target", "") for c in record.get("calls", [])]
    used_store = any(t.startswith("memory://out/") for t in names)
    reran = sum(t.startswith("ssh://") for t in names) > 1
    report.add("S8", "long_output_answer", fourth in record.get("reply", ""),
               f"expected={fourth} reply={record.get('reply', '')[:80]!r}")
    report.add("S8", "long_output_from_store", used_store and not reran, f"targets={names}")
    state = json.loads(device.read_file("/vat/context/state.json") or "{}")
    newest = state.get("next_out", 1) - 1
    meta = json.loads(device.read_file(f"/vat/out/{newest // 1000}/{newest}.meta.json") or "{}")
    stored = device.read_file(f"/vat/out/{newest // 1000}/{newest}.txt")
    report.add("S8", "full_output_stored", meta.get("bytes", 0) >= 262144 and "只保存了前 262144 字节" in stored,
               f"out#{newest} bytes={meta.get('bytes')} file={len(stored.encode())}")

    if quick():  # web search / page reading (two slow cloud turns): normal and up
        heap = device.status()
        report.add("S8", "heap_low_water", heap["min_free_heap"] >= 40000, f"min_free_heap={heap['min_free_heap']}")
        return

    # 2. Web search and page reading (DeepSeek Search / Fetch).
    device.action("clear_history")
    record = device.text("联网搜一下 M5Stack Cardputer Adv 的官方文档网址。", timeout=180)
    device.resolve_all(approve=False)
    searched = any(c["arguments"].get("target", "").startswith("web://search") for c in record.get("calls", []))
    report.add("S8", "web_search", record.get("status") == "done" and searched and "docs.m5stack.com" in everything(record),
               f"status={record.get('status')} error={record.get('error', '')[:80]} "
               f"calls={[c['arguments'].get('target', '')[:40] for c in record.get('calls', [])]} "
               f"reply={record.get('reply', '')[:80]!r}")
    record = device.text("打开第 1 个结果，这页写的电池容量是多少？", timeout=180)
    device.resolve_all(approve=False)
    paged = any(c["arguments"].get("target", "").startswith("web://page") for c in record.get("calls", []))
    report.add("S8", "web_page", record.get("status") == "done" and paged and "mAh" in record.get("reply", ""),
               record.get("reply", "")[:100])

    # 3. After the window is cleared, the answer is only in the archive.
    device.action("clear_history")
    record = device.text("我之前查过一个 long.log，第 4 个 ERROR 的错误码是什么？不要重新读取文件。", timeout=180)
    device.resolve_all(approve=False)
    targets = [c["arguments"].get("target", "") for c in record.get("calls", [])]
    report.add("S8", "history_search", fourth in record.get("reply", "") and
               not any(t.startswith("ssh://") for t in targets), f"targets={targets} reply={record.get('reply', '')[:80]!r}")

    # 4. A case survives a reboot and comes back into the conversation by itself.
    # The reboot only in `full`: S5 already checks that a reboot restores the
    # conversation; elsewhere the case must come back after clear_history alone.
    device.action("clear_history")
    device.text("开始一个叫 e2e-排查 的案卷。")
    device.resolve_all(approve=True)
    device.text(f"在案卷里记录：long.log 的第 4 个错误 {fourth} 来自 worker-{errors[3][0] % 7}。")
    device.resolve_all(approve=True)
    findings = device.read_file("/vat/cases/e2e-排查/findings.jsonl")
    report.add("S8", "case_finding_written", fourth in findings, findings[:100])
    if full():
        device.reboot()
    device.action("clear_history")
    record = device.text("当前案卷里记录了哪个错误码？直接回答。")
    device.resolve_all(approve=False)
    report.add("S8", "case_after_reboot" if full() else "case_recall", fourth in record.get("reply", ""),
               record.get("reply", "")[:80])
    device.text("结束 e2e-排查 这个案卷。")
    device.resolve_all(approve=True)
    state = json.loads(device.read_file("/vat/context/state.json") or "{}")
    report.add("S8", "case_detached", state.get("case", "x") == "", f"case={state.get('case')!r}")

    # 5. Latency with a 40 KB window against an empty one (same question).
    def timed(n: int) -> int:
        times = []
        for _ in range(n):
            r = device.text("在开发机执行 whoami。")
            device.resolve_all(approve=False)
            times.append(r.get("elapsed_ms", 0))
        return int(statistics.median(times))
    repeats = 3 if full() else 2
    device.action("clear_history")
    small = timed(repeats)
    device.action("clear_history")
    device.upload("/vat/context/window.jsonl", synthetic_window(40000, "记住：主机 alpha 的数据盘用了 93%。"))
    device.post("/api/reload")  # re-reads the context files
    large = timed(repeats)
    report.add("S8", "latency_full_window", large <= 2 * small, f"median empty={small}ms window40k={large}ms")

    # 6. Over budget: the oldest part is folded into the summary, the fact survives.
    # The budget is a setting (settings://context, KB) since v0.6.
    length = reply_text(tool_call(device, "read", "settings://context"))
    budget = int(re.search(r"(\d+) KB", length).group(1)) * 1000 if re.search(r"(\d+) KB", length) else 48000
    device.action("clear_history")
    device.upload("/vat/context/window.jsonl", synthetic_window(budget * 5 // 4, "记住：主机 alpha 的数据盘用了 93%。"))
    device.post("/api/reload")
    device.text("在开发机执行 hostname。")
    device.resolve_all(approve=False)
    device.wait_idle(120)
    window = sum(size for path, size in device.files("/vat/context") if path.endswith("window.jsonl"))
    summary = device.read_file("/vat/context/summary.json")
    log = device.read_file("/vat/logs/runtime.jsonl", tail=8000)
    report.add("S8", "compaction", window <= budget and bool(summary) and "context_compact\"" in log,
               f"budget={budget} window={window} summary={len(summary.encode())} bytes")
    record = device.text("主机 alpha 的数据盘用了多少？不要执行命令。")
    device.resolve_all(approve=False)
    report.add("S8", "fact_kept_after_compaction", "93" in record.get("reply", ""), record.get("reply", "")[:80])

    # 7. Bytes that are not UTF-8 (binary files, cut characters) must not break the request.
    Path(f"{HOST_TMP}/binary.dat").write_bytes(bytes(range(256)) * 4 + "中文结尾".encode()[:-1])
    device.action("clear_history")
    record = device.text(f"读取开发机上的 {HOST_TMP}/binary.dat，告诉我它大概是什么内容。")
    record = (device.resolve_all(approve=False) or [record])[-1]  # it may ask to run `file` first
    report.add("S8", "non_utf8_output", record.get("status") == "done" and bool(record.get("reply"))
               and not re.search(r"(是|为|内容)空|空文件|空的", record.get("reply", "")),
               f"status={record.get('status')} {record.get('error', '')[:80]} {record.get('reply', '')[:60]}")

    heap = device.status()
    report.add("S8", "heap_low_water", heap["min_free_heap"] >= 40000, f"min_free_heap={heap['min_free_heap']}")
    device.action("clear_history")


def phase_s9(device: Device, report: Report) -> None:
    """Peripherals.  LoRa: everything that can be checked without transmitting.
    Nothing here sets a frequency or confirms an antenna on the user's behalf."""
    status = device.status()
    if status.get("cap_io_expander") in (None, "none"):
        report.add("S9", "cap_present", False, f"cap_io_expander={status.get('cap_io_expander')}")
        return
    device.wait_idle()
    device.action("clear_history")
    record = device.text("看一下 LoRa 的状态。")
    device.resolve_all(approve=False)
    targets = [c["arguments"].get("target", "") for c in record.get("calls", [])]
    state = raw(record)
    report.add("S9", "lora_status", any(t.startswith("device://local/lora/status") or t == "device://local/lora/" for t in targets)
               and "LoRa" in state, state[:120])
    # GPS: the module talks (NMEA arrives); a fix is not expected indoors.
    device.action("clear_history")
    record = device.text("我现在在哪？用 GPS 看一下。")
    device.resolve_all(approve=False)
    gps_text = raw(record)
    nmea = re.search(r"已收 NMEA (\d+) 字节", gps_text)
    report.add("S9", "gps_nmea", any(c["arguments"].get("target", "").lower().startswith("sensor://gps")
                                     for c in record.get("calls", [])) and bool(nmea) and int(nmea.group(1)) > 0,
               gps_text[:160])
    if quick():
        return
    record = device.text("把 LoRa 频点设成 433 MHz。")
    record = (device.resolve_all(approve=True) or [record])[-1]
    report.add("S9", "lora_frequency_range", "868" in raw(record) and "923" in raw(record), raw(record)[:120])
    frequency_set = "频点 未设置" not in state
    antenna_ok = "已确认装好" in state
    if not frequency_set:
        record = device.text("打开 LoRa 接收。")
        record = (device.resolve_all(approve=True) or [record])[-1]
        report.add("S9", "lora_needs_frequency", "未设置频点" in raw(record), raw(record)[:120])
    if not antenna_ok:
        record = device.text("用 LoRa 发一条消息：hello")
        record = (device.resolve_all(approve=True) or [record])[-1]
        report.add("S9", "lora_send_gated", "天线" in raw(record) or "未开启" in raw(record), raw(record)[:120])
    record = device.text("关闭 LoRa。")
    record = (device.resolve_all(approve=True) or [record])[-1]
    report.add("S9", "lora_off", "关闭" in raw(record), raw(record)[:120])


def cleanup() -> None:
    # The test box is reused within a run (fresh_test_box) but never left behind:
    # it takes a fixed password and has sudo, on the LAN.
    host(f"docker rm -f {TEST_BOX} >/dev/null 2>&1;"
         " tmux kill-session -t vat-e2e 2>/dev/null; tmux kill-session -t vat-pi-voice-agent-terminal 2>/dev/null;"
         " tmux kill-session -t vat-shell 2>/dev/null; tmux kill-session -t vat-pw 2>/dev/null;"
         " tmux kill-session -t vat-codex-voice-agent-terminal 2>/dev/null;"
         " tmux kill-session -t vat-claude-voice-agent-terminal 2>/dev/null;"
         f" rm -rf {HOST_TMP}")


# --------------------------------------------------------------------------- S10: programs and triggers

PROGRAMS = ROOT / "tests" / "programs"


def tool_call(device: Device, tool: str, target: str, content=None, timeout: float = 180, **extra) -> dict:
    """One tool call through /api/job kind=call (runs only what Policy allows outright)."""
    arguments = {"target": target, **extra}
    if content is not None:
        arguments["content"] = content if isinstance(content, str) else json.dumps(content, ensure_ascii=False)
    if tool == "write":
        arguments.setdefault("mode", "overwrite")
        arguments.setdefault("confirm_required", False)
    if tool == "exec":
        arguments.setdefault("program", "run")
        arguments.setdefault("args", [])
        arguments.setdefault("confirm_required", False)
    device.wait_idle()
    return device.job("call", timeout=timeout,
                      call=json.dumps({"tool": tool, "arguments": arguments}, ensure_ascii=False))


def reply_text(record: dict) -> str:
    return (record.get("reply") or "") + (record.get("error") or "")


def program_cases(device: Device, report: Report) -> None:
    cases = json.loads((PROGRAMS / "cases.json").read_text(encoding="utf-8"))
    for case in cases:
        cid = case["id"]
        if not wanted(cid) or (quick() and cid not in QUICK_PROGRAMS):
            continue
        if case.get("save"):
            source = (PROGRAMS / case["save"]).read_text(encoding="utf-8")
            saved = tool_call(device, "write", "prog://" + case["name"], source)
            text = reply_text(saved)
            if case.get("save_refused"):
                report.add("S10", f"{cid}_save_refused", "未执行" in text and "确认" in text, text[:160])
                continue
            if case.get("save_error_has"):
                report.add("S10", f"{cid}_save_error", case["save_error_has"] in text, text[:160])
                continue
            if not report.add("S10", f"{cid}_save", "已保存程序" in text or "已修改程序" in text, text[:160]):
                continue
        if not case.get("run"):
            continue
        record = tool_call(device, "exec", case["run"])
        text = reply_text(record)
        ok = all(part in text for part in case.get("reply_has", []))
        ok = ok and not any(part in text for part in case.get("reply_lacks", []))
        ok = ok and all(part in text for part in case.get("error_or_reply_has", []))
        for needle, count in case.get("count", {}).items():
            ok = ok and text.count(needle) == count
        report.add("S10", cid, ok, text[:200])


def card_package_cases(device: Device, report: Report) -> None:
    """A package on the card (tests/programs/pkg_echo.lua, echo://): found by
    pkg://scan, off until the user enables it, then serving its target."""
    if not wanted("card_package"):
        return
    path = "/vat/things/lua/测试-回声.lua"
    device.write_file(path, (PROGRAMS / "pkg_echo.lua").read_text(encoding="utf-8"))
    try:
        tool_call(device, "write", "pkg://scan", "")
        listed = reply_text(tool_call(device, "read", "pkg://list"))
        line = next((item for item in listed.splitlines() if "测试-回声" in item), "")
        report.add("S10", "card_package_pending", "[扩展包 echo://]" in line and "待确认" in line, line[:160])
        text = reply_text(tool_call(device, "read", "echo://你好"))
        report.add("S10", "card_package_off_until_enabled", "回声：你好" not in text, text[:160])
        text = reply_text(tool_call(device, "write", "pkg://测试-回声/enable", ""))
        report.add("S10", "card_package_enable_needs_confirm", "未执行" in text, text[:160])
        device.text("启用卡上的扩展包 测试-回声")
        device.resolve_all(approve=True)
        text = reply_text(tool_call(device, "read", "echo://你好"))
        report.add("S10", "card_package_read", "回声：你好" in text, text[:160])
        text = reply_text(tool_call(device, "read", "echo://table"))
        report.add("S10", "card_package_table_json", '"n":2' in text.replace(" ", ""), text[:160])
        text = reply_text(tool_call(device, "read", "echo://fail"))
        report.add("S10", "card_package_error", "ERROR" in text and "故意出错" in text, text[:160])
        tool_call(device, "write", "pkg://测试-回声/disable", "")
        text = reply_text(tool_call(device, "read", "echo://你好"))
        report.add("S10", "card_package_disabled", "回声：你好" not in text, text[:160])
    finally:
        device.post("/api/delete", path=path)
        tool_call(device, "write", "pkg://scan", "")


def console_cases(device: Device, report: Report) -> None:
    """The program page: a foreground run's console, stdin from the call,
    typed input, ` interrupting, and the page following the run."""
    if not wanted("console"):
        return
    name = "测试-控制台"
    saved = reply_text(tool_call(device, "write", "prog://" + name,
                                 (PROGRAMS / "console_io.lua").read_text(encoding="utf-8")))
    if not report.add("S10", "console_save", "已保存程序" in saved or "已修改程序" in saved, saved[:120]):
        return
    text = reply_text(tool_call(device, "exec", "prog://" + name, stdin="飞鱼\n21"))
    report.add("S10", "console_stdin", all(p in text for p in ("你好 飞鱼", "双倍 42", "这行是错误输出", "交互 true", "读完 true")),
               text[:200])
    screen = device.status()["screen"]
    report.add("S10", "console_page", screen["page"] == "program" and "你好 飞鱼" in screen.get("body", ""),
               f"{screen['page']} {screen.get('console')}")

    def start(target: str) -> int:
        device.wait_idle()
        status, body = device.post("/api/job", kind="call", call=json.dumps(
            {"tool": "exec", "arguments": {"target": target, "program": "run", "args": [], "confirm_required": False}},
            ensure_ascii=False))
        return json.loads(body)["id"] if status == 200 else 0

    def finished(job_id: int, seconds: float = 20) -> dict:
        deadline = time.time() + seconds
        while time.time() < deadline:
            record = device.get_json("/api/job", id=job_id)
            if record.get("status") not in ("queued", "running"):
                return record
            time.sleep(0.3)
        return {}

    # Typed on the device: the program waits for a line, the keys go to it.
    job = start("prog://" + name)
    waiting = until(lambda: device.status()["screen"].get("console", {}).get("wait") == "line", 5)
    device.key("abc")
    device.key("enter")
    until(lambda: device.status()["screen"].get("console", {}).get("wait") == "line", 5)
    device.key("5")
    device.key("enter")
    until(lambda: device.status()["screen"].get("console", {}).get("wait") == "line", 5)
    device.key("enter")  # the third read: an empty line (only the stdin of an exec ends with nil)
    record = finished(job)
    report.add("S10", "console_typed", waiting and "你好 abc" in reply_text(record) and "双倍 10" in reply_text(record)
               and "交互 true" in reply_text(record), reply_text(record)[:200])

    # ` interrupts a running program.
    tool_call(device, "write", "prog://测试-停不下", "while true do vat.sleep(100) end")
    job = start("prog://测试-停不下")
    running = until(lambda: device.status()["screen"].get("console", {}).get("running") is True, 5)
    device.key("esc")
    record = finished(job)
    report.add("S10", "console_interrupt", running and "已中断" in reply_text(record), reply_text(record)[:160])
    device.action("page_agent")


def interval_trigger_cases(device: Device, report: Report) -> None:
    """time://every/<duration>: runs every interval, refuses < 10 s, pauses
    itself after 5 failures in a row, and trigger://clean removes it."""
    if not wanted("trigger_every"):
        return

    def trigger_id(text: str) -> str:
        match = re.search(r"#(\d+)", text)
        return match.group(1) if match else ""

    def line_of(tid: str) -> str:
        listed = reply_text(tool_call(device, "read", "trigger://list"))
        return next((line for line in listed.splitlines() if line.startswith(f"#{tid} ")), "")

    text = reply_text(tool_call(device, "write", "trigger://new", {"when": "time://every/5s", "run": "prog://测试-灯"}))
    report.add("S10", "trigger_every_too_short", "至少 10 秒" in text, text[:160])

    made = reply_text(tool_call(device, "write", "trigger://new", {"when": "time://every/10s", "run": "prog://测试-灯"}))
    tid = trigger_id(made)
    report.add("S10", "trigger_every_created", "每隔 10s" in made and bool(tid), made[:160])
    runs = re.compile(r"运行 (\d+) 次")
    line = ""
    deadline = time.time() + 40
    while tid and time.time() < deadline:
        line = line_of(tid)
        found = runs.search(line)
        if found and int(found.group(1)) >= 2:
            break
        time.sleep(2)
    found = runs.search(line)
    report.add("S10", "trigger_every_runs", bool(found) and int(found.group(1)) >= 2, line[:200])
    if tid:
        tool_call(device, "write", f"trigger://{tid}/pause", "")

    # Every failed background run emits prog://event/failed, which a trigger can listen to; three in a
    # row show on the status bar.
    tool_call(device, "write", "prog://测试-失败记录",
              'vat.write("device://local/clipboard", "失败记录 " .. tostring(input.event.program) .. " "'
              ' .. tostring(input.event.failures))\n')
    made = reply_text(tool_call(device, "write", "trigger://new", {"when": "prog://event/failed", "run": "prog://测试-失败记录"}))
    listener = trigger_id(made)
    report.add("S10", "failed_event_trigger", bool(listener), made[:160])

    # A program that always fails: paused after 5 runs in a row.
    made = reply_text(tool_call(device, "write", "trigger://new", {"when": "time://every/10s", "run": "prog://测试-坏"}))
    bad = trigger_id(made)
    line = ""
    deadline = time.time() + 90
    while bad and time.time() < deadline:
        line = line_of(bad)
        if "已自动停用" in line:
            break
        time.sleep(3)
    report.add("S10", "trigger_every_auto_pause", "已自动停用" in line and "连续失败 5 次" in line, line[:200])
    clip = ""
    deadline = time.time() + 20
    while time.time() < deadline:  # the listener runs after the failed run
        clip = reply_text(tool_call(device, "read", "device://local/clipboard"))
        if "失败记录 测试-坏 5" in clip:
            break
        time.sleep(2)
    report.add("S10", "failed_event_runs_listener", "失败记录 测试-坏 5" in clip, clip[:120])
    rows = reply_text(tool_call(device, "read", "settings://"))
    before_bar = re.search(r"顶栏：(.*)", rows)
    tool_call(device, "write", "settings://statusbar", "时间,后台失败")  # the item may not be in this device's choice
    bar = device.status()["screen"].get("status", "")
    report.add("S10", "failed_status_item", "失败" in bar, bar)
    restore = before_bar.group(1).strip() if before_bar else "时间,电量,信号"
    tool_call(device, "write", "settings://statusbar", "" if restore == "不显示" else restore.replace("·", ","))
    if listener:
        device.text(f"删除触发 #{listener}")
        device.resolve_all(approve=True)

    text = reply_text(tool_call(device, "write", "trigger://clean", ""))
    report.add("S10", "trigger_clean_needs_confirm", "未执行" in text, text[:160])
    device.text("清理一下失效的触发")
    device.resolve_all(approve=True)
    gone = bool(bad) and not line_of(bad)
    report.add("S10", "trigger_clean_removes", gone, line_of(bad)[:160] if bad else "no trigger")
    if tid:  # the paused interval trigger: deleted (confirmed) so runs do not pile up triggers
        device.text(f"删除触发 #{tid}")
        device.resolve_all(approve=True)


def trigger_cases(device: Device, report: Report) -> None:
    # One-off: fires by itself, runs a program, then disappears from the list.
    if wanted("trigger_once"):
        made = reply_text(tool_call(device, "write", "trigger://new",
                                {"when": "time://in/8s", "run": "prog://测试-触发"}))
        tid = re.search(r"#(\d+)", made)
        report.add("S10", "trigger_once_created", "已新建触发" in made and bool(tid), made[:160])
        # Polled (was a fixed 20 s): within the same 20 s it must have fired and left the list.
        deadline = time.time() + 20
        runs = listed = ""
        fired = gone = False
        while bool(tid):
            runs = reply_text(tool_call(device, "read", "prog://list"))  # its 最近 lines
            listed = reply_text(tool_call(device, "read", "trigger://list"))
            fired = "测试-触发 完成" in runs
            gone = f"#{tid.group(1)} " not in listed
            if (fired and gone) or time.time() >= deadline:
                break
            time.sleep(1)
        report.add("S10", "trigger_once_fired_and_removed", fired and gone, (runs + " | " + listed)[:200])

    # Daily: pause / resume without confirmation; deleting needs the user.
    if wanted("trigger_daily"):
        made = reply_text(tool_call(device, "write", "trigger://new",
                                {"when": "time://every/03:33", "run": "prog://测试-灯"}))
        tid = re.search(r"#(\d+)", made)
        report.add("S10", "trigger_daily_created", "每天 03:33" in made and bool(tid), made[:160])
        if not tid:
            return
        n = tid.group(1)
        paused = reply_text(tool_call(device, "write", f"trigger://{n}/pause"))
        resumed = reply_text(tool_call(device, "write", f"trigger://{n}/resume"))
        report.add("S10", "trigger_pause_resume", "已停用" in paused and "已停用" not in resumed, (paused + " | " + resumed)[:160])
        refused = reply_text(tool_call(device, "write", f"trigger://{n}/delete"))
        report.add("S10", "trigger_delete_needs_confirm", "未执行" in refused, refused[:160])
        device.action("clear_history")
        device.text(f"删除触发 #{n}")
        device.resolve_all(approve=True)
        listed = reply_text(tool_call(device, "read", "trigger://list"))
        report.add("S10", "trigger_deleted_after_confirm", f"#{n} " not in listed, listed[:160])


def program_reads(device: Device, report: Report) -> None:
    listed = reply_text(tool_call(device, "read", "prog://list"))
    report.add("S10", "list_shows_signatures", "测试-被调(x, y=默认)" in listed and "测试-调用" in listed, listed[:200])
    source = reply_text(tool_call(device, "read", "prog://测试-调用"))
    report.add("S10", "show_source", "vat.exec(\"prog://测试-被调?x=甲\")" in source, source[:200])
    helped = reply_text(tool_call(device, "read", "prog://help"))
    report.add("S10", "help", "@uses" in helped and "vat.after" in helped and "trigger://new" in helped, helped[:80])
    pkgs = reply_text(tool_call(device, "read", "pkg://list"))
    report.add("S10", "pkg_list", "测试-灯 [程序] 已启用" in pkgs, pkgs[:160])
    refused = reply_text(tool_call(device, "write", "prog://测试-坏/delete"))
    report.add("S10", "delete_needs_confirm", "未执行" in refused, refused[:160])


def program_intents(device: Device, report: Report) -> None:
    """The agent itself: runs a program by name, and turns a sentence into a trigger."""
    if wanted("intent_run"):
        device.action("clear_history")
        record = device.text("运行一下测试-灯这个程序")
        device.resolve_all(approve=False)
        targets = [c["arguments"].get("target", "") for c in record.get("calls", [])]
        report.add("S10", "intent_run", any(t.startswith("prog://测试-灯") for t in targets), str(targets)[:160])
    if wanted("intent_daily") and not quick():
        device.action("clear_history")
        record = device.text("以后每天凌晨 3 点 44 分把本机的灯关掉")
        # Saving a program can ask first: the trigger may come after the approval.
        records = [record] + device.resolve_all(approve=True)
        calls = [(c.get("name"), c["arguments"].get("target", ""), c["arguments"].get("content", ""))
                 for r in records for c in r.get("calls", [])]
        made = any(t == "trigger://new" and "time://every/03:44" in str(content) for _, t, content in calls)
        report.add("S10", "intent_daily_trigger", made, str(calls)[:200])
        listed = reply_text(tool_call(device, "read", "trigger://list"))
        found = re.search(r"#(\d+) 每天 03:44", listed)
        if found:  # clean up: deleting needs the user's confirmation
            device.action("clear_history")
            device.text(f"删除触发 #{found.group(1)}")
            device.resolve_all(approve=True)


def v072_cases(device: Device, report: Report) -> None:
    """0.7.2: patch writes, reading part of a program, a run shown in sections, calls that return
    tables, and the three streams (debug://).  Single tool calls: nothing depends on the model."""
    if not wanted("v072"):
        return
    name = "测试-补丁"
    tool_call(device, "write", "prog://" + name, "local n = 1\nprint('数字 ' .. n)\nreturn n\n")
    text = reply_text(tool_call(device, "write", f"prog://{name}/patch", "<<<<\nlocal n = 1\n====\nlocal n = 42\n>>>>"))
    report.add("S10", "v072_patch", "已改 1 处" in text, text[:160])
    text = reply_text(tool_call(device, "exec", "prog://" + name))
    report.add("S10", "v072_run_sections", all(x in text for x in ("【结果】42", "【输出】", "数字 42", "运行 #")), text[:200])
    text = reply_text(tool_call(device, "write", f"prog://{name}/patch", "<<<<\nlocal n = 7\n====\nlocal n = 8\n>>>>"))
    report.add("S10", "v072_patch_not_found", "没找到" in text and "没有改动" in text, text[:160])
    text = reply_text(tool_call(device, "write", f"prog://{name}/patch", "<<<<\nn\n====\nm\n>>>>"))
    report.add("S10", "v072_patch_ambiguous", "出现" in text and "次" in text, text[:160])
    text = reply_text(tool_call(device, "read", f"prog://{name}?grep=LOCAL"))
    report.add("S10", "v072_read_grep", "1: local n = 42" in text, text[:160])
    text = reply_text(tool_call(device, "read", f"prog://{name}?lines=2-2"))
    report.add("S10", "v072_read_lines", "print('数字 ' .. n)" in text and "local n" not in text, text[:160])

    tables = "测试-返回表"
    tool_call(device, "write", "prog://" + tables,
              'local b = vat.read("device://local/battery")\n'
              'local led = vat.write("device://local/led", "蓝")\n'
              'local r = vat.exec("prog://测试-补丁")\n'
              'vat.log("记一笔")\n'
              'print(type(b.percent), type(b.usb), led.color, r.ok, r.result, tostring(b):sub(1, 6), "接" .. r)\n'
              'print({a = 1})\n'
              'return vat.now().ms > 0\n')
    text = reply_text(tool_call(device, "exec", "prog://" + tables))
    report.add("S10", "v072_tables", "number\tboolean\t蓝\ttrue\t42\t电量\t接42" in text, text[:260])
    report.add("S10", "v072_print_table_json", '{"a":1}' in text, text[:260])
    report.add("S10", "v072_log_is_stderr", "【错误输出】" in text and "记一笔" in text.split("【错误输出】")[-1] and "[+" in text,
               text[-160:])
    run_id = re.search(r"运行 #(\d+)", text)
    text = reply_text(tool_call(device, "read", f"debug://stdout?prog={tables}&n=1"))
    report.add("S10", "v072_debug_stdout", tables in text and "结果=成功" in text and "number" in text, text[:200])
    text = reply_text(tool_call(device, "read", f"debug://stderr?prog={tables}&n=1"))
    report.add("S10", "v072_debug_stderr", "记一笔" in text, text[:200])
    if report.add("S10", "v072_run_has_number", bool(run_id), text[:80]):
        text = reply_text(tool_call(device, "read", "debug://run/" + run_id.group(1)))
        report.add("S10", "v072_debug_run", all(x in text for x in ("【输入】", "【输出】", "【错误输出】", "记一笔")), text[:200])

    bad = "测试-坏行"
    tool_call(device, "write", "prog://" + bad, "local t = nil\nprint('开始')\nreturn t.x\n")
    text = reply_text(tool_call(device, "exec", "prog://" + bad))
    report.add("S10", "v072_error_line", "【出错的行】第 3 行：return t.x" in text and "开始" in text, text[:260])
    text = reply_text(tool_call(device, "read", f"debug://stderr?prog={bad}&failed=1"))
    report.add("S10", "v072_debug_failed", "结果=失败" in text and "attempt to index" in text, text[:200])

    reads = "测试-读入"
    tool_call(device, "write", "prog://" + reads, "print('读到 ' .. tostring(io.read()))\n")
    text = reply_text(tool_call(device, "exec", "prog://" + reads, args=["你好"]))
    report.add("S10", "v072_args_are_input", "读到 你好" in text, text[:160])
    text = reply_text(tool_call(device, "read", f"debug://stdin?prog={reads}&n=1"))
    report.add("S10", "v072_debug_stdin", "你好" in text, text[:160])

    text = reply_text(tool_call(device, "read", "device://local/status"))
    report.add("S10", "v072_status_for_model", "free_heap" in text and "wifi_bssid" not in text and '"ip"' not in text, text[:120])


def phase_s10(device: Device, report: Report) -> None:
    """Lua programs (prog://) and triggers (trigger://): the test programs in
    tests/programs/*.lua are saved and run through single tool calls, so
    results do not depend on the model; a few cases then check the agent uses them."""
    device.wait_idle()
    program_cases(device, report)
    program_reads(device, report)
    v072_cases(device, report)
    card_package_cases(device, report)
    console_cases(device, report)
    trigger_cases(device, report)
    interval_trigger_cases(device, report)
    program_intents(device, report)
    tool_call(device, "write", "device://local/led", "off")


def http_fixture(device: Device):
    """A small HTTP server on this host for S12: /json, /big (20000 bytes),
    /headers (echoes them), POST /echo (records the body); anything else 404."""
    import http.server
    import socket
    import threading
    seen: dict = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code: int, body: str, kind: str = "application/json") -> None:
            data = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            seen["headers"] = dict(self.headers)
            if self.path == "/json":
                self.reply(200, json.dumps({"ok": True, "n": 42}))
            elif self.path == "/big":
                self.reply(200, "x" * 20000, "text/plain")
            elif self.path == "/headers":
                self.reply(200, json.dumps(dict(self.headers)))
            elif self.path.startswith("/pkg/") and urllib.parse.unquote(self.path[5:]) in seen.get("files", {}):
                data = seen["files"][urllib.parse.unquote(self.path[5:])]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.reply(404, "not here", "text/plain")

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            seen["body"] = self.rfile.read(length).decode("utf-8", "replace")
            seen["type"] = self.headers.get("Content-Type", "")
            self.reply(200, json.dumps({"got": seen["body"]}))

    server = http.server.ThreadingHTTPServer(("0.0.0.0", 18765), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((urllib.parse.urlparse(device.base).hostname, 80))
    address = probe.getsockname()[0]
    probe.close()
    return server, f"http://{address}:18765", seen


def pkg_install_cases(device: Device, report: Report, base: str, seen: dict) -> None:
    """pkg://catalog, install, update, rollback against a catalog this host
    serves (made by tools/pkg_index.py), and a download whose SHA-256 is wrong."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("pkg_index", ROOT / "tools" / "pkg_index.py")
    pkg_index = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pkg_index)
    files = seen.setdefault("files", {})
    name = "测试-安装"
    v1 = '-- @about 安装测试\n-- @version 1\nreturn "安装测试 v1"\n'
    v2 = '-- @about 安装测试\n-- @version 2\n-- @uses write device://local/led\nreturn "安装测试 v2"\n'

    def publish(source: str, tamper: bool = False) -> None:
        files[name + ".lua"] = source.encode("utf-8")
        catalog = pkg_index.build({name + ".lua": files[name + ".lua"]})
        if tamper:
            catalog["packages"][0]["sha256"] = "0" * 64
        files["index.json"] = json.dumps(catalog, ensure_ascii=False).encode("utf-8")

    def run_it() -> str:
        return reply_text(tool_call(device, "exec", "prog://" + name))

    def approved(sentence: str) -> str:
        record = device.text(sentence)
        records = [record] + device.resolve_all(approve=True)
        return json.dumps(records, ensure_ascii=False)

    catalog_url = base + "/pkg/index.json"
    try:
        publish(v1)
        text = reply_text(tool_call(device, "read", "pkg://catalog?url=" + catalog_url))
        report.add("S12", "catalog_read", name + " v1" in text and "未安装" in text, text[:200])
        text = reply_text(tool_call(device, "write", "pkg://" + name + "/install", ""))
        report.add("S12", "install_needs_confirm", "未执行" in text, text[:160])
        if quick():  # the rest takes four model turns
            return
        approved(f"安装在线目录里的「{name}」")
        text = run_it()
        report.add("S12", "install_runs", "安装测试 v1" in text, text[:160])
        publish(v2)
        text = reply_text(tool_call(device, "read", "pkg://catalog?url=" + catalog_url))
        report.add("S12", "catalog_update_available", "可更新" in text, text[:200])
        approved(f"把「{name}」更新到在线目录里的新版本")
        text = run_it()
        report.add("S12", "update_runs", "安装测试 v2" in text, text[:160])
        approved(f"把「{name}」退回上一版")
        text = run_it()
        report.add("S12", "rollback_runs", "安装测试 v1" in text, text[:160])
        publish(v2, tamper=True)
        tool_call(device, "read", "pkg://catalog?url=" + catalog_url)
        records = approved(f"把「{name}」更新到在线目录里的新版本")
        text = run_it()
        report.add("S12", "tampered_refused", "SHA-256 校验失败" in records and "安装测试 v1" in text,
                   (records[-160:] + " | " + text)[:200])
    finally:
        for path in (f"/vat/things/lua/{name}.lua", f"/vat/things/lua/old/{name}.lua"):
            device.post("/api/delete", path=path)
        tool_call(device, "write", "pkg://scan", "")


def phase_s12(device: Device, report: Report) -> None:
    """The http package against a server on this host: GET, status errors, the
    size limit, saving to the card, headers, secrets that only the device takes,
    writes that need confirmation, and programs that need an @uses per site."""
    device.wait_idle()
    server, base, seen = http_fixture(device)
    try:
        text = reply_text(tool_call(device, "read", base + "/json"))
        report.add("S12", "get_json", '"n": 42' in text, text[:160])
        text = reply_text(tool_call(device, "read", base + "/missing"))
        report.add("S12", "status_error", "HTTP 404" in text and "not here" in text, text[:160])
        text = reply_text(tool_call(device, "read", base + "/big", max_bytes=1000))
        report.add("S12", "too_large", "超过 1000 字节" in text, text[:160])
        text = reply_text(tool_call(device, "read", base + "/big", save="file:///vat/cache/e2e-http.txt"))
        report.add("S12", "save_to_card", "已保存 20000 字节" in text, text[:160])
        text = reply_text(tool_call(device, "read", base + "/headers", headers={"X-Test": "fy-1"}))
        report.add("S12", "custom_header", seen.get("headers", {}).get("X-Test") == "fy-1", text[:160])
        text = reply_text(tool_call(device, "read", base + "/json",
                                    headers={"Authorization": "Bearer {secret:e2e-none}"}))
        report.add("S12", "secret_needs_device", "未执行" in text and "密" in text, text[:160])
        text = reply_text(tool_call(device, "read", base + "/json", save="file:///vat/secrets.json"))
        report.add("S12", "save_refuses_secrets", ("密钥" in text and "不能" in text) or "拒绝访问凭据" in text, text[:160])
        text = reply_text(tool_call(device, "write", base + "/echo", {"a": 1}))
        report.add("S12", "post_needs_confirm", "未执行" in text, text[:160])
        # A saved program declares the sites it talks to; code mode (lua://run)
        # asks the user instead, call by call.
        script = 'local v, e = vat.read("%s/json") print(e)' % base
        tool_call(device, "write", "prog://e2e联网", script)
        text = reply_text(tool_call(device, "exec", "prog://e2e联网"))
        report.add("S12", "program_needs_uses", "@uses read " + base in text, text[:200])
        tool_call(device, "write", "prog://e2e联网/delete", "")
        device.resolve_all(approve=True)
        code = 'local v, e = vat.read("%s/json") return v or e' % base  # a web read is its text
        record = tool_call(device, "write", "lua://run", code)
        asked = record.get("status") == "asking" and bool(device.status()["screen"]["pending"])
        final = (device.resolve_all(approve=True) or [record])[-1]
        report.add("S12", "code_mode_asks", asked and '"n": 42' in reply_text(final),
                   f"status={record.get('status')} {reply_text(final)[:120]}")
        record = tool_call(device, "write", "lua://run", code)
        final = (device.resolve_all(approve=False) or [record])[-1]
        report.add("S12", "code_mode_rejected", record.get("status") == "asking" and "拒绝" in reply_text(final),
                   reply_text(final)[:120])
        if not quick():  # one model turn and an approval
            seen.pop("body", None)
            device.text(f'用 POST 把 JSON {{"hello": "fish"}} 发到 {base}/echo')
            device.resolve_all(approve=True)
            body = seen.get("body", "")
            try:
                ok = json.loads(body) == {"hello": "fish"} and "json" in seen.get("type", "")
            except ValueError:
                ok = False
            report.add("S12", "post_after_approve", ok, body[:160])
        pkg_install_cases(device, report, base, seen)
    finally:
        server.shutdown()
        server.server_close()


def phase_s11(device: Device, report: Report) -> None:
    """v0.6: the web token, the arena report, the context length, ui panels
    (choice, refused fields, choice as a trigger) and local apps (Fn+A)."""
    device.wait_idle()

    def status_code(headers: dict) -> int:
        try:
            request = urllib.request.Request(device.base + "/api/status", headers=headers)
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status
        except urllib.error.HTTPError as exc:
            return exc.code
    report.add("S11", "token_required", status_code({}) == 401)
    report.add("S11", "token_wrong_refused", status_code({"Authorization": "Bearer " + "A" * 26}) == 401)
    report.add("S11", "token_accepted", status_code({"Authorization": f"Bearer {device.token}"}) == 200)
    try:
        urllib.request.urlopen(device.base + "/m", timeout=10)
        phone_code = 200
    except urllib.error.HTTPError as exc:
        phone_code = exc.code
    report.add("S11", "phone_page_refused_without_token", phone_code == 401, str(phone_code))

    arena = device.status().get("arena") or {}
    leases = [lease.get("name") for lease in arena.get("leases", [])]
    report.add("S11", "arena_reported", arena.get("floor") == 48000 and "tls_reserve" in leases,
               json.dumps(arena, ensure_ascii=False)[:160])
    text = reply_text(tool_call(device, "read", "settings://context"))
    report.add("S11", "context_length_read", "KB" in text, text[:60])

    panel = {"title": "E2E 面板", "lines": ["选一个"], "choices": ["甲", "乙", "丙"]}
    text = reply_text(tool_call(device, "write", "ui://panel/e2e面板", json.dumps(panel, ensure_ascii=False)))
    report.add("S11", "panel_shown", "已显示面板" in text, text[:60])
    on_screen = until(lambda: device.status()["screen"].get("overlay") == "panel", 2)  # within the 2 s it used to wait
    report.add("S11", "panel_on_screen", on_screen, str(device.status()["screen"].get("overlay")))
    device.key(".")
    device.key("enter")
    deadline = time.time() + 1  # polled for the 1 s it used to wait before one read
    while True:
        text = reply_text(tool_call(device, "read", "ui://panel/e2e面板"))
        if "乙" in text or time.time() >= deadline:
            break
        time.sleep(0.2)
    report.add("S11", "panel_choice_read", "乙" in text, text[:60])
    text = reply_text(tool_call(device, "write", "ui://panel/e2e坏", json.dumps({"title": "x", "script": "rm"})))
    report.add("S11", "panel_unknown_field_refused", "不认识的字段" in text, text[:60])

    # A choice is an event: a trigger runs a program, which gets it as input.event.
    tool_call(device, "write", "prog://e2e面板记录",
              "-- @about 记下面板的选择\n"
              "vat.write('memory://notes', 'E2E 面板选了 ' .. input.event.choice, {mode = 'append'})\n"
              "return input.event.choice")
    trigger = {"when": "ui://event/panel/e2e触发", "run": "prog://e2e面板记录"}
    text = reply_text(tool_call(device, "write", "trigger://new", json.dumps(trigger, ensure_ascii=False)))
    trigger_id = re.search(r"#?(\d+)", text)
    report.add("S11", "panel_trigger_saved", "触发" in text and trigger_id is not None, text[:80])
    tool_call(device, "write", "ui://panel/e2e触发", json.dumps({"choices": ["红", "绿"]}, ensure_ascii=False))
    until(lambda: device.status()["screen"].get("overlay") == "panel", 2)  # the panel is up before the key
    device.key("enter")
    noted = bool(until(lambda: "E2E 面板选了 红" in device.read_file("/vat/memory/notes.txt", tail=2000), 20, 1))
    report.add("S11", "panel_choice_runs_trigger", noted)
    if trigger_id:  # deleted, not paused: paused ones piled up to the 32-trigger limit over many runs
        device.wait_idle()
        device.text(f"删除触发 #{trigger_id.group(1)}")
        device.resolve_all(approve=True)

    app = {"title": "电量", "source": {"read": "device://local/battery", "every": "10s"}}
    text = reply_text(tool_call(device, "write", "ui://apps/e2e应用", json.dumps(app, ensure_ascii=False)))
    report.add("S11", "app_saved", "应用" in text, text[:60])
    text = reply_text(tool_call(device, "read", "ui://apps/list"))
    report.add("S11", "app_listed", "e2e应用" in text, text[:80])
    # Fn+A: the program page lists programs and local apps; Enter opens the app
    # on the graphics page.
    device.key("fn+a")

    def menu_rows():
        return [line for line in device.status()["screen"].get("body", "").split("\n") if line]
    until(lambda: "[应用] e2e应用" in menu_rows(), 4)  # drawn from the cache first, refreshed on the next pass
    page = device.status()["screen"]
    listed = menu_rows()
    menu = page.get("page") if "[应用] e2e应用" in listed else f"not listed: {listed[:6]}"
    for _ in range(len(listed) + 2):  # the cursor stays where the page was left: up to the top first
        device.key(";")
    for _ in range(listed.index("[应用] e2e应用") if "[应用] e2e应用" in listed else 0):
        device.key(".")
    device.key("enter")
    until(lambda: device.status()["screen"].get("overlay") == "panel", 2.5)
    # Kept fixed on purpose: the app reads every 10 s and the case checks it is
    # still open after one refresh; the screen model shows no refresh to wait on.
    time.sleep(13)
    opened = device.status()["screen"].get("overlay")
    device.key("esc")
    report.add("S11", "apps_menu_fn_a_open", menu == "program" and opened == "panel", f"{menu} -> {opened}")
    device.post("/api/delete", path="/vat/things/apps/e2e应用.json")

    # The program page: newest first, Del then Y deletes the selected item.
    tool_call(device, "write", "prog://测试-可删", "return 1")
    device.key("fn+a")
    def program_rows():
        return [line for line in device.status()["screen"].get("body", "").split("\n") if line]
    until(lambda: program_rows()[:1] == ["[程序] 测试-可删"], 4)  # drawn from the cache first, refreshed on the next pass
    listed = program_rows()
    report.add("S11", "program_list_newest_first", bool(listed) and listed[0] == "[程序] 测试-可删", str(listed[:3]))
    if "[程序] 测试-可删" in listed:
        for _ in range(len(listed) + 2):  # the cursor stays where the page was left: up to the top first
            device.key(";")
        for _ in range(listed.index("[程序] 测试-可删")):
            device.key(".")
        device.key("del")
        device.key("y")
    remaining = reply_text(tool_call(device, "read", "prog://list"))
    report.add("S11", "program_page_delete", "测试-可删" not in remaining, remaining[:120])
    device.action("page_agent")

    # v0.6.2 canvas variables: an update without animate runs every 33 ms.
    def screen() -> dict:
        return device.status()["screen"]
    ball = {"vars": {"x": 20, "vx": 6},
            "update": "x = x + vx  if x > 220 or x < 20 then vx = -vx end",
            "ops": [["fillScreen", "#101820"], ["fillCircle", "{x}", 67, 8, "cyan"], ["drawString", "{x}", 4, 4]]}
    text = reply_text(tool_call(device, "write", "ui://canvas/e2e弹球", json.dumps(ball)))
    up = until(lambda: screen().get("overlay") == "canvas", 2)
    first = screen().get("canvas_frame", 0)
    time.sleep(1.0)
    after = screen()
    frames = after.get("canvas_frame", 0) - first
    report.add("S11", "canvas_vars_animate", "ERROR" not in text and up and frames >= 10 and not after.get("canvas_error"),
               f"{text[:40]} frames/s={frames} error={after.get('canvas_error')!r}")
    broken = {"vars": {"x": 1}, "update": "x = x + nothing", "ops": [["fillScreen", "red"]]}  # nil arithmetic
    tool_call(device, "write", "ui://canvas/e2e坏画布", json.dumps(broken))
    error = until(lambda: screen().get("canvas_error"), 2)
    report.add("S11", "canvas_update_error_shown", bool(error), repr(error)[:80])
    device.key("esc")
    unknown = {"update": "x = = 1", "ops": [["fillScreen", "red"]]}  # refused when saved: a syntax error
    text = reply_text(tool_call(device, "write", "ui://canvas/e2e坏画布", json.dumps(unknown)))
    report.add("S11", "canvas_unknown_var_refused", "ERROR" in text and "update" in text, text[:80])

    # v0.6.2 settings package (runs on the UI task) and D9.
    rows = reply_text(tool_call(device, "read", "settings://"))
    report.add("S11", "settings_read", "动画：" in rows and "顶栏：" in rows, rows[:120])
    before_bar = re.search(r"顶栏：(.*)", rows)
    text = reply_text(tool_call(device, "write", "settings://animation", "减弱"))
    after = reply_text(tool_call(device, "read", "settings://"))
    tool_call(device, "write", "settings://animation", "开")
    report.add("S11", "settings_animation", "已设为" in text and "动画：减弱" in after, text[:60])
    text = reply_text(tool_call(device, "write", "settings://statusbar", "时间,内存"))
    report.add("S11", "settings_statusbar", "时间·剩余内存" in text, text[:60])
    restore = before_bar.group(1).strip() if before_bar else "时间,电量,信号"
    tool_call(device, "write", "settings://statusbar", "" if restore == "不显示" else restore.replace("·", ","))
    text = reply_text(tool_call(device, "write", "settings://statusbar", "天气"))
    report.add("S11", "settings_statusbar_unknown_refused", "ERROR" in text and "没有这些项" in text, text[:60])
    text = reply_text(tool_call(device, "write", "settings://key/deepseek/open", ""))
    opened = until(lambda: device.status()["screen"].get("setup"), 2)
    for _ in range(4):  # form -> keys -> menu -> closed
        if not device.status()["screen"].get("setup"):
            break
        device.key("`")
    report.add("S11", "settings_open_key_form", "输入框" in text and bool(opened) and not device.status()["screen"].get("setup"),
               text[:60])
    text = reply_text(tool_call(device, "write", "settings://portal/open", ""))
    report.add("S11", "settings_portal_needs_confirm", "未执行" in text and not device.status()["screen"].get("setup"), text[:60])
    # D9 is now @uses: a program declaring a call that asks is confirmed when
    # saved (a direct call cannot confirm); one that only reads is not.
    declared = ("-- @input what battery\n-- @uses exec ssh://开发机\n"
                "return vat.exec('ssh://开发机', 'touch', {'/tmp/vat-e2e/d9'})")
    text = reply_text(tool_call(device, "write", "prog://e2e变量", declared))
    plain = "-- @input what battery\nreturn vat.read('device://local/' .. input.what)"
    saved = reply_text(tool_call(device, "write", "prog://e2e常量", plain))
    ran = reply_text(tool_call(device, "exec", "prog://e2e常量"))
    # Fixed names: later runs overwrite them (deleting asks, which a direct call cannot answer).
    report.add("S11", "d9_uses_confirmed_at_save",
               "未执行" in text and ("已保存程序" in saved or "已修改程序" in saved) and "电量" in ran,
               (text + " | " + saved + " | " + ran)[:160])
    report.add("S11", "heap_after", device.status().get("free_heap", 0) > 60000, str(device.status().get("free_heap")))


def start_prompt_lab(device: Device, out_dir: Path) -> dict:
    """Release tier: every intent case through tools/prompt_lab.py on this host,
    in the background while the device runs its phases.  The SSH profile names
    and the agent model come from the device here (prompt_lab's own device
    reads carry no token), so prompt_lab runs offline: no device requests,
    discover answered from tests/fixtures/discover_cache.json."""
    names = device.status().get("profiles", "")
    try:
        model = json.loads(device.read_file("/vat/config.json") or "{}").get("agent", {}).get("model", "")
    except ValueError:
        model = ""
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    result, log = out_dir / f"prompt-lab-{stamp}.json", out_dir / f"prompt-lab-{stamp}.log"
    command = [sys.executable, str(ROOT / "tools" / "prompt_lab.py"), "--json", str(result)]
    if names:
        command += ["--profiles", names]
    if model:
        command += ["--model", model]
    with open(log, "w") as stream:
        process = subprocess.Popen(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
    print(f"prompt_lab: every intent case on the host in the background -> {log}", flush=True)
    return {"process": process, "result": result, "log": log, "started": time.time()}


def finish_prompt_lab(lab: dict, report: Report, timeout: float = 600) -> None:
    """One S2I result: every prompt_lab case passed (a flaky case fails it, like on the device)."""
    process = lab["process"]
    try:
        process.wait(timeout=max(1, lab["started"] + timeout - time.time()))
    except subprocess.TimeoutExpired:
        process.kill()
        report.add("S2I", "prompt_lab_all_intents", False, f"timed out after {timeout:.0f} s; log={lab['log']}")
        return
    try:
        results = json.loads(lab["result"].read_text())
    except (OSError, ValueError):
        results = []
    failed = sorted({r["id"] for r in results if not r.get("passed")})
    passed = sum(1 for r in results if r.get("passed"))
    tail = lab["log"].read_text(errors="replace").strip().splitlines()[-1:] if lab["log"].exists() else []
    report.add("S2I", "prompt_lab_all_intents", process.returncode == 0 and bool(results) and not failed,
               f"passed {passed}/{len(results)} exit={process.returncode} failed={failed} "
               f"seconds={round(time.time() - lab['started'])} {tail[0][:80] if tail else ''} log={lab['log']}",
               failed=failed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=os.environ.get("VAT_DEVICE", "http://vat.local"))
    parser.add_argument("--phases", default="auto",
                        help="'quick' (a case or two per phase, <= 5 min), 'auto' (phases impacted by "
                             "git diff --since), 'all' (release, <= 15 min: trimmed, S2I sampled on the device "
                             "plus prompt_lab on the host), 'full' (every case on the device, the old 'all', "
                             "~30 min), or a comma list of s0,s1,s2,s2i,s3..s11")
    parser.add_argument("--since", default="HEAD~1", help="git ref for --phases auto (default HEAD~1)")
    parser.add_argument("--only", default="", help="comma list of case-id substrings for the S1/S2 matrices")
    parser.add_argument("--out", default=str(ROOT / "logs"))
    parser.add_argument("--no-sync-profiles", action="store_true",
                        help="leave the device's test SSH profiles as they are (default: point them at this host)")
    parser.add_argument("--no-prompt-lab", action="store_true",
                        help="'all' only: skip tools/prompt_lab.py (needs ../.deepseek_key and the model API)")
    args = parser.parse_args()
    ONLY.extend(part for part in args.only.split(",") if part)
    global TIER
    if args.phases == "quick":
        TIER = "quick"
        phases = [p for p in ALL_PHASES if p != "s2i"]
    elif args.phases in ("all", "full"):
        TIER = args.phases
        phases = ALL_PHASES
    elif args.phases == "auto":
        phases = impacted_phases(args.since)
    else:
        phases = args.phases.split(",")
    print(f"phases: {','.join(phases)} tier: {TIER}" + (f" only: {args.only}" if ONLY else ""), flush=True)
    seconds = {}
    device = Device(args.device)
    report = Report()
    if not args.no_sync_profiles:
        print(sync_profiles(device, args.device), flush=True)
    started = time.time()
    lab = None
    if release() and "s2i" in phases and not ONLY and not args.no_prompt_lab:
        lab = start_prompt_lab(device, Path(args.out))
    try:
        for phase in phases:
            before = device.status()
            expected = device.expected_reboots
            phase_started = time.time()
            device.action("page_agent")  # each phase starts on the Agent page
            globals()[f"phase_{phase}"](device, report)
            seconds[phase.upper()] = round(time.time() - phase_started)
            after = device.status()
            rebooted = after["uptime_s"] < before["uptime_s"]
            unexpected = rebooted and device.expected_reboots == expected
            report.add(phase.upper(), "no_unexpected_reboot", not unexpected,
                       f"uptime {before['uptime_s']}s -> {after['uptime_s']}s reset={after.get('reset_reason')}")
    finally:
        if lab:
            try:
                finish_prompt_lab(lab, report)
            except Exception as exc:  # noqa: BLE001 - the evidence file is written regardless
                report.add("S2I", "prompt_lab_all_intents", False, f"{type(exc).__name__}: {exc}")
        cleanup()
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        out = Path(args.out) / f"standalone-e2e-{stamp}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        log_tail = device.read_file("/vat/logs/runtime.jsonl", tail=20000)
        out.write_text(json.dumps({
            "started": datetime.fromtimestamp(started).isoformat(timespec="seconds"),
            "elapsed_s": round(time.time() - started),
            "device": args.device, "phases": phases, "tier": TIER, "phase_seconds": seconds,
            "summary": report.summary(), "results": report.results,
            "device_runtime_log_tail": log_tail,
        }, ensure_ascii=False, indent=1))
        print(json.dumps(report.summary(), ensure_ascii=False))
        print("seconds:", json.dumps(seconds), "total", round(time.time() - started))
        print(f"evidence: {out}")
    return 0 if all(item["passed"] for item in report.results) else 1


if __name__ == "__main__":
    sys.exit(main())
