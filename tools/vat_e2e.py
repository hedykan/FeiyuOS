#!/usr/bin/env python3
"""Standalone Cardputer end-to-end acceptance (S0-S9).

The device does all the work.  This script only talks to the device's own
web API over Wi-Fi (no USB data link) and checks real side effects on the
SSH host it runs on.  Run it on the machine that the device's SSH profiles
point at, e.g.:

    VAT_DEVICE=http://<device-ip> python3 tools/vat_e2e.py --phases all
"""
from __future__ import annotations

import argparse
import ipaddress
import json
import re
import os
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
ALL_PHASES = ["s0", "s1", "s2", "s2i", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11"]
# quick: a representative case or two per phase (~3 min, after every flash);
# normal: impacted phases (auto) or a named list; all: everything (release).
TIER = "normal"
# Voice cases that test speech itself (device settings and recognition
# corrections); the other recordings repeat S2i intents and run only in `all`.
CORE_PCM = ["local_status", "local_settings", "set_brightness_96", "set_timeout_180", "local_history",
            "read_hostname", "start_codex_agent", "tmux_create", "restart_nginx", "docker_logs"]
QUICK_PCM = ["local_status", "read_hostname"]
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
    "firmware/standalone_agent/src/packages/program_package": ["s10"],
    "firmware/standalone_agent/src/packages/ask_package": ["s10"],
    "tests/programs/": ["s10"],
    "firmware/standalone_agent/src/packages/": ["s6"],
    "firmware/standalone_agent/src/ui": ["s4"],
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


def wanted(case_id: str) -> bool:
    return not ONLY or any(part in case_id for part in ONLY)
HOST_TMP = "/tmp/vat-e2e"
HOME = os.path.expanduser("~")


# --------------------------------------------------------------------------- device client

class Device:
    def __init__(self, base: str) -> None:
        self.base = base.rstrip("/")
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
        self.post("/api/reboot")
        time.sleep(8)
        after = {}
        for _ in range(30):
            try:
                after = self.status()
                if after.get("uptime_s", 9999) < 120:
                    break
            except RuntimeError:
                pass
            time.sleep(2)
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
            self.wait_idle()
            if screen.get("secret_prompt"):
                self.action("cancel")  # abort: tests never type real passwords here
                time.sleep(1)
                continue
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


def validate_expect(expect: dict, record: dict) -> list[str]:
    """Same rules as the reference intent evaluation: tool names must match in
    order; expected argument keys must be equal (subset match)."""
    failures = []
    calls = operations(record)
    if expect.get("no_tool"):
        if calls:
            failures.append(f"expected clarification, got {[c['name'] for c in calls]}")
        if not record.get("reply"):
            failures.append("expected a clarification message")
        return failures
    names = [c["name"] for c in calls]
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
    keep = None if TIER == "all" else QUICK_PCM if quick() else CORE_PCM
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
        latencies.append(record.get("elapsed_ms", 0))
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
        report.add("S2", f"runtime:{case['id']}", not failures, "; ".join(failures) or record.get("reply", "")[:120])



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
    for case_id, text, expected, mode in expectations[:2] if quick() else expectations:
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
    for text, word in (("连接 SSH 配置开发机。", "已连接"), ("列出当前 SSH 连接。", "开发机"),
                       ("断开开发机的 SSH 连接。", "断开")):
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
    record = device.text("在当前终端输入 Control C。")
    keys = [c["arguments"].get("keys") for c in operations(record)]
    report.add("S4", "agent_ctrl_c", any(k and "CTRL_C" in [x.upper() for x in k] for k in keys), str(keys))
    record = device.text("查看当前终端的输出。")
    report.add("S4", "agent_observe", mark in raw(record), record.get("reply", "")[-80:])

    # UI pages through the same actions as Btn0 / Fn+Btn0.
    device.action("page_ssh")
    screen = device.status()["screen"]
    report.add("S4", "ui_ssh_page", screen["page"] == "ssh" and mark in screen["body"], screen["header"])
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
        screen = ""
        for _ in range(20):
            time.sleep(4)
            screen = device.job("observe").get("reply", "")
            if "FeiyuOS" in screen or "Voice Agent Terminal" in screen:
                break
        report.add("S4", f"agent_{name}_start", started, record.get("reply", "")[:120])
        report.add("S4", f"agent_{name}_answer", "FeiyuOS" in screen or "Voice Agent Terminal" in screen, screen[-160:])
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
    for index in range(20):
        text = ["在开发机执行 uptime。", "查看这台 Cardputer 的本机状态。", "在开发机执行 whoami。",
                "再查看这台机器的主机名。"][index % 4]
        record = device.text(text)
        device.resolve_all(approve=False)
        latencies.append(record.get("elapsed_ms", 0))
        failures += 0 if record.get("status") == "done" and record.get("reply") else 1
        summaries.append(len(record.get("reply", "")))
    status = device.status()
    report.add("S5", "twenty_rounds", failures == 0 and status["uptime_s"] > start_uptime,
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
        return reply_text(tool_call(device, "write", f"ir://{name}/{target}", content))

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
    for key, text in (("led", "把本机的 RGB 灯调成绿色"), ("imu", "设备现在是平放的吗"),
                      ("beep", "响一声提示音"), ("led_off", "把本机 RGB 灯关掉")):
        device.wait_idle()
        device.action("clear_history")
        record = device.text(text)
        results[key] = (record.get("status"), raw(record))
        device.resolve_all(approve=False)
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
    scanned = calls and calls[0]["name"] == "read" and calls[0]["arguments"].get("target") == "wifi://scan"
    report.add("S6", "wifi_scan_voice", bool(scanned) and current in raw(record) and bool(record.get("reply")),
               f"calls={[c['arguments'].get('target') for c in calls]} summary={record.get('reply', '')[:60]!r}")

    device.action("clear_history")
    record = device.text("现在连的是哪个 WiFi？")
    calls = operations(record)
    report.add("S6", "wifi_status_voice",
               bool(calls) and calls[0]["arguments"].get("target") in ("wifi://status", "wifi://scan")
               and current in raw(record) + record.get("reply", ""),
               record.get("reply", "")[:80])
    display_overlay(device, report)
    hardware_controls(device, report)
    files_timer_grove(device, report)
    ir_confirm_flow(device, report)
    ac_choose_flow(device, report)
    if quick():
        return

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


def fresh_test_box() -> bool:
    """A disposable SSH host that needs a password for login and sudo."""
    host(f"docker rm -f {TEST_BOX} >/dev/null 2>&1")
    started = host(f"docker run -d --name {TEST_BOX} -p 2222:2222 -e PUID=1000 -e PGID=1000 -e USER_NAME=vat "
                   f"-e USER_PASSWORD={TEST_BOX_PASSWORD} -e PASSWORD_ACCESS=true -e SUDO_ACCESS=true "
                   "linuxserver/openssh-server:latest").returncode == 0
    for _ in range(30):
        if host("docker logs vat-e2e-box 2>&1 | grep -q 'sshd is listening\\|Server listening\\|ready'").returncode == 0:
            break
        time.sleep(1)
    time.sleep(3)
    return started


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

    # Secrets on a disposable host that needs passwords.
    if not fresh_test_box():
        report.add("S7", "test_box", False, "docker run failed")
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

    # Every file the device stored, read whole (the file API streams any size).
    leaks, scanned = [], 0
    for path, size in device.files("/vat"):
        if path.endswith(".pcm"):
            continue
        scanned += 1
        if TEST_BOX_PASSWORD in device.read_file(path):
            leaks.append(path)
    report.add("S7", "secret_never_stored", scanned > 10 and not leaks,
               f"scanned={scanned} files found_in={leaks or 'nowhere'}")

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

    if quick():
        heap = device.status()
        report.add("S8", "heap_low_water", heap["min_free_heap"] >= 40000, f"min_free_heap={heap['min_free_heap']}")
        return

    # 3. After the window is cleared, the answer is only in the archive.
    device.action("clear_history")
    record = device.text("我之前查过一个 long.log，第 4 个 ERROR 的错误码是什么？不要重新读取文件。", timeout=180)
    device.resolve_all(approve=False)
    targets = [c["arguments"].get("target", "") for c in record.get("calls", [])]
    report.add("S8", "history_search", fourth in record.get("reply", "") and
               not any(t.startswith("ssh://") for t in targets), f"targets={targets} reply={record.get('reply', '')[:80]!r}")

    # 4. A case survives a reboot and comes back into the conversation by itself.
    device.action("clear_history")
    device.text("开始一个叫 e2e-排查 的案卷。")
    device.resolve_all(approve=True)
    device.text(f"在案卷里记录：long.log 的第 4 个错误 {fourth} 来自 worker-{errors[3][0] % 7}。")
    device.resolve_all(approve=True)
    findings = device.read_file("/vat/cases/e2e-排查/findings.jsonl")
    report.add("S8", "case_finding_written", fourth in findings, findings[:100])
    device.reboot()
    device.action("clear_history")
    record = device.text("当前案卷里记录了哪个错误码？直接回答。")
    device.resolve_all(approve=False)
    report.add("S8", "case_after_reboot", fourth in record.get("reply", ""), record.get("reply", "")[:80])
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
    device.action("clear_history")
    small = timed(3)
    device.action("clear_history")
    device.upload("/vat/context/window.jsonl", synthetic_window(40000, "记住：主机 alpha 的数据盘用了 93%。"))
    device.post("/api/reload")  # re-reads the context files
    large = timed(3)
    report.add("S8", "latency_full_window", large <= 2 * small, f"median empty={small}ms window40k={large}ms")

    # 6. Over budget: the oldest part is folded into the summary, the fact survives.
    # The budget is a setting (device://local/context/length, KB) since v0.6.
    length = reply_text(tool_call(device, "read", "device://local/context/length"))
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
               and "空" not in record.get("reply", ""),
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
    report.add("S9", "lora_status", any(t.startswith("lora://status") or t == "lora://" for t in targets)
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
    host("tmux kill-session -t vat-e2e 2>/dev/null; tmux kill-session -t vat-pi-voice-agent-terminal 2>/dev/null;"
         " tmux kill-session -t vat-shell 2>/dev/null; tmux kill-session -t vat-pw 2>/dev/null;"
         f" docker rm -f {TEST_BOX} >/dev/null 2>&1;"
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
        if not wanted(cid):
            continue
        if case.get("save"):
            program = json.loads((PROGRAMS / case["save"]).read_text(encoding="utf-8"))
            saved = tool_call(device, "write", "prog://" + program["name"], program)
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
        if case.get("then_runs_has"):  # queued (wait / ask inside a conversation): watch prog://runs
            deadline = time.time() + case.get("within_s", 60)
            runs = ""
            while ok and time.time() < deadline:
                time.sleep(3)
                runs = reply_text(tool_call(device, "read", "prog://runs"))
                if case["then_runs_has"] in runs:
                    break
            ok = ok and case["then_runs_has"] in runs
            text += " | runs: " + runs
        report.add("S10", cid, ok, text[:200])


def trigger_cases(device: Device, report: Report) -> None:
    # One-off: fires by itself, runs a program, then disappears from the list.
    if wanted("trigger_once"):
        made = reply_text(tool_call(device, "write", "trigger://new",
                                {"when": "time://in/8s", "run": {"exec": "prog://测试-灯"}}))
        tid = re.search(r"#(\d+)", made)
        report.add("S10", "trigger_once_created", "已新建触发" in made and bool(tid), made[:160])
        time.sleep(20)
        runs = reply_text(tool_call(device, "read", "prog://runs"))
        listed = reply_text(tool_call(device, "read", "trigger://list"))
        fired = bool(tid) and f"触发 #{tid.group(1)} 完成" in runs
        gone = bool(tid) and f"#{tid.group(1)} " not in listed
        report.add("S10", "trigger_once_fired_and_removed", fired and gone, (runs + " | " + listed)[:200])

    # Daily: pause / resume without confirmation; deleting needs the user.
    if wanted("trigger_daily"):
        made = reply_text(tool_call(device, "write", "trigger://new",
                                {"when": "time://every/03:33", "run": {"write": "device://local/led", "content": "off"}}))
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
    report.add("S10", "list_shows_signatures", "测试-被调(x, y=默认)" in listed and "调用：测试-被调" in listed, listed[:200])
    outline = reply_text(tool_call(device, "read", "prog://测试-调用"))
    report.add("S10", "outline", "运行 测试-被调" in outline and "→ r" in outline, outline[:200])
    helped = reply_text(tool_call(device, "read", "prog://help"))
    report.add("S10", "help_examples", "【例子】" in helped and "trigger://new" in helped, helped[:80])
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
        device.resolve_all(approve=True)
        calls = [(c.get("name"), c["arguments"].get("target", ""), c["arguments"].get("content", ""))
                 for c in record.get("calls", [])]
        made = any(t == "trigger://new" and "time://every/03:44" in str(content) for _, t, content in calls)
        report.add("S10", "intent_daily_trigger", made, str(calls)[:200])
        listed = reply_text(tool_call(device, "read", "trigger://list"))
        found = re.search(r"#(\d+) 每天 03:44", listed)
        if found:  # clean up: deleting needs the user's confirmation
            device.action("clear_history")
            device.text(f"删除触发 #{found.group(1)}")
            device.resolve_all(approve=True)


def phase_s10(device: Device, report: Report) -> None:
    """Programs (prog://) and triggers (trigger://): the test programs in
    tests/programs/ are saved and run through single tool calls, so results do
    not depend on the model; a few cases then check the agent uses them."""
    device.wait_idle()
    program_cases(device, report)
    program_reads(device, report)
    trigger_cases(device, report)
    program_intents(device, report)
    tool_call(device, "write", "device://local/led", "off")


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
    text = reply_text(tool_call(device, "read", "device://local/context/length"))
    report.add("S11", "context_length_read", "KB" in text, text[:60])

    panel = {"title": "E2E 面板", "lines": ["选一个"], "choices": ["甲", "乙", "丙"]}
    text = reply_text(tool_call(device, "write", "ui://panel/e2e面板", json.dumps(panel, ensure_ascii=False)))
    report.add("S11", "panel_shown", "已显示面板" in text, text[:60])
    time.sleep(2)
    report.add("S11", "panel_on_screen", device.status()["screen"].get("overlay") == "panel",
               str(device.status()["screen"].get("overlay")))
    device.key(".")
    device.key("enter")
    time.sleep(1)
    text = reply_text(tool_call(device, "read", "ui://panel/e2e面板"))
    report.add("S11", "panel_choice_read", "乙" in text, text[:60])
    text = reply_text(tool_call(device, "write", "ui://panel/e2e坏", json.dumps({"title": "x", "script": "rm"})))
    report.add("S11", "panel_unknown_field_refused", "不认识的字段" in text, text[:60])

    # A choice is an event: a trigger runs a step with {event.choice}.
    trigger = {"when": "ui://panel/e2e触发", "run": {"write": "memory://notes", "mode": "append",
                                                    "content": "E2E 面板选了 {event.choice}"}}
    text = reply_text(tool_call(device, "write", "trigger://new", json.dumps(trigger, ensure_ascii=False)))
    trigger_id = re.search(r"#?(\d+)", text)
    report.add("S11", "panel_trigger_saved", "触发" in text and trigger_id is not None, text[:80])
    tool_call(device, "write", "ui://panel/e2e触发", json.dumps({"choices": ["红", "绿"]}, ensure_ascii=False))
    time.sleep(2)
    device.key("enter")
    noted = False
    for _ in range(20):
        time.sleep(1)
        if "E2E 面板选了 红" in device.read_file("/vat/memory/notes.txt", tail=2000):
            noted = True
            break
    report.add("S11", "panel_choice_runs_trigger", noted)
    if trigger_id:
        device.wait_idle()
        device.post("/api/job", kind="call", call=json.dumps(
            {"tool": "write", "arguments": {"target": f"trigger://{trigger_id.group(1)}/pause", "confirm_required": False}}))

    app = {"title": "电量", "source": {"read": "device://local/battery", "every": "10s"}}
    text = reply_text(tool_call(device, "write", "ui://apps/e2e应用", json.dumps(app, ensure_ascii=False)))
    report.add("S11", "app_saved", "应用" in text, text[:60])
    text = reply_text(tool_call(device, "read", "ui://apps"))
    report.add("S11", "app_listed", "e2e应用" in text, text[:80])
    device.key("fn+a")
    time.sleep(2.5)
    menu = device.status()["screen"].get("overlay")
    device.key("enter")
    time.sleep(13)
    opened = device.status()["screen"].get("overlay")
    device.key("esc")
    report.add("S11", "apps_menu_fn_a_open", menu == "panel" and opened == "panel", f"{menu} -> {opened}")
    device.post("/api/delete", path="/vat/things/apps/e2e应用.json")
    report.add("S11", "heap_after", device.status().get("free_heap", 0) > 60000, str(device.status().get("free_heap")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default=os.environ.get("VAT_DEVICE", "http://vat.local"))
    parser.add_argument("--phases", default="auto",
                        help="'quick' (a case or two per phase, ~3 min), 'auto' (phases impacted by "
                             "git diff --since), 'all' (release), or a comma list of s0,s1,s2,s2i,s3..s11")
    parser.add_argument("--since", default="HEAD~1", help="git ref for --phases auto (default HEAD~1)")
    parser.add_argument("--only", default="", help="comma list of case-id substrings for the S1/S2 matrices")
    parser.add_argument("--out", default=str(ROOT / "logs"))
    parser.add_argument("--no-sync-profiles", action="store_true",
                        help="leave the device's test SSH profiles as they are (default: point them at this host)")
    args = parser.parse_args()
    ONLY.extend(part for part in args.only.split(",") if part)
    global TIER
    if args.phases == "quick":
        TIER = "quick"
        phases = [p for p in ALL_PHASES if p != "s2i"]
    elif args.phases == "all":
        TIER = "all"
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
    try:
        for phase in phases:
            before = device.status()
            expected = device.expected_reboots
            phase_started = time.time()
            globals()[f"phase_{phase}"](device, report)
            seconds[phase.upper()] = round(time.time() - phase_started)
            after = device.status()
            rebooted = after["uptime_s"] < before["uptime_s"]
            unexpected = rebooted and device.expected_reboots == expected
            report.add(phase.upper(), "no_unexpected_reboot", not unexpected,
                       f"uptime {before['uptime_s']}s -> {after['uptime_s']}s reset={after.get('reset_reason')}")
    finally:
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
