#!/usr/bin/env python3
"""Prompt lab: the device's agent request, replayed against the model API.

Sends exactly what the firmware sends for a fresh turn (system prompt and
tools from agent_prompt.h, the SSH profile names, the user text), answers
`discover` from a cache of real device answers, and stops at the first
operation the device would execute.  Every intent case runs in parallel, so a
prompt change is checked in about a minute instead of a full E2E run.

    VAT_DEVICE=http://<device-ip> python3 tools/prompt_lab.py   # all intent cases, current prompt
    python3 tools/prompt_lab.py --prompt variant.txt # a candidate system prompt (plain text)
    python3 tools/prompt_lab.py --only led,timer --repeat 3

Judging is vat_e2e.validate_expect: the same rule as the device E2E.
The API key is read from ../.deepseek_key and never printed.
"""
from __future__ import annotations

import argparse
import concurrent.futures as futures
import json
import os
import threading
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from vat_e2e import validate_expect  # noqa: E402

SRC = ROOT / "firmware/standalone_agent/src"
HEADER = SRC / "agent_prompt.h"
CACHE = ROOT / "tests/fixtures/discover_cache.json"
OLD_CACHE = ROOT / "logs/discover_cache.json"
KEY_FILE = ROOT.parent / ".deepseek_key"
CASE_FILES = [ROOT / "tests/cases/daily_intent_cases.json", ROOT / "tests/cases/ssh_intent_cases.json",
              ROOT / "tests/cases/v05_intent_cases.json", ROOT / "tests/cases/v053_program_cases.json"]
MANIFEST = ROOT / "tests/pcm/manifest.json"
MAX_ROUNDS = 4  # model rounds before the operation
MAX_DISCOVER = 2  # agent.cpp kMaxDiscoverCalls


def raw_literal(name: str, tag: str) -> str:
    """The R"TAG(...)TAG" literal of `name` in agent_prompt.h."""
    text = HEADER.read_text()
    match = re.search(rf'{name}\[\] = R"{tag}\((.*?)\){tag}"', text, re.S)
    if not match:
        raise SystemExit(f"{name} not found in {HEADER}")
    return match.group(1)


INDEX_HEAD = "能力包（要用某个包而不清楚它的地址或参数时，先 discover scope=capabilities query=包名 取它的用法；已经清楚的直接调用）："


def system_prompt() -> str:
    """What agent.cpp streams: kCorePrompt, the usage of every package without an
    index() (registry::usages(), builtin.cpp order), the index of the others,
    kClosingPrompt.  \\n escapes in the literals are newlines to the model."""
    core = text_of("core.txt", raw_literal("kCorePrompt", "PROMPT")).replace("\\n", "\n")
    closing = text_of("closing.txt", raw_literal("kClosingPrompt", "PROMPT")).replace("\\n", "\n")
    kept, lines = [], []
    for p in packages():
        if not p["usage"]:
            continue
        if p["index"] is None:
            kept.append(p["usage"])
        else:
            lines.append(f"{p['id']}：{p['summary']}（{p['index']}）")
    return core + "".join(kept) + INDEX_HEAD + "\n" + "\n".join(lines) + "\n" + closing


def string_fields(text: str, start: int, count: int) -> list[str]:
    """The first `count` comma-separated C++ string fields after `start` (adjacent
    literals concatenated): the id, summary, keywords of a Manifest."""
    fields, current, i, in_field = [], "", start, False
    while i < len(text) and len(fields) < count:
        c = text[i]
        if c == '"':
            end = i + 1
            while text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            current += text[i + 1:end]
            in_field = True
            i = end + 1
            continue
        if c in ",}" and in_field:
            fields.append(current)
            current, in_field = "", False
        i += 1
    return fields


def packages() -> list[dict]:
    """Every builtin package in registration order: id, summary, usage (kUsage, '' if none)."""
    out = []
    order = re.findall(r"add\(make_(\w+)_package\(\)\)", (SRC / "packages/builtin.cpp").read_text())
    for name in order:
        text = (SRC / "packages" / f"{name}_package.cpp").read_text()
        at = text.find("Manifest m{")
        if at < 0:
            continue
        pid, summary = string_fields(text, at + len("Manifest m{"), 2)
        match = re.search(r'constexpr char kUsage\[\] =\s*((?:\s*"[^"\\]*")+);', text)
        usage = "".join(re.findall(r'"([^"\\]*)"', match.group(1))) if match else ""
        found = re.search(r'const char\* index\(\) const override \{ return (?:place::available\(\) \? )?"([^"]*)"', text)
        index = found.group(1) if found else None
        if TEXTS and (TEXTS / "always.txt").exists():  # a draft decides what is resident
            drafted = json.loads((TEXTS / "index.json").read_text()) if (TEXTS / "index.json").exists() else {}
            index = None if pid in ALWAYS else drafted.get(pid, "")
        out.append({"id": pid, "summary": summary, "usage": text_of(f"usage/{pid}.txt", usage), "index": index})
    return out


ALWAYS: set[str] = set()  # --texts always.txt: the resident packages of a draft
TEXTS: Path | None = None  # --texts: a directory overriding the firmware's texts


def text_of(name: str, fallback: str) -> str:
    """TEXTS/name when it exists (a draft being tried), else the firmware's text."""
    if TEXTS and (TEXTS / name).exists():
        return (TEXTS / name).read_text().strip()
    return fallback


def package_usage(arguments: dict) -> str | None:
    """discover(capabilities) naming builtin packages: their summary and usage, as
    registry::package_help gives them.  None when no builtin package is named."""
    if arguments.get("scope", "capabilities") != "capabilities":
        return None
    words = set(re.split(r"[\s,，、:：/]+", (arguments.get("query") or "").lower()))
    named = [p for p in packages() if p["id"] in words and p["usage"] and p["index"] is not None]
    if not named:
        return None
    return "\n\n".join(f"{p['id']}：{p['summary']}\n\n{p['usage']}" for p in named[:2])


def load_cases(only: list[str]) -> list[dict]:
    cases = []
    for path in CASE_FILES:
        if path.exists():
            for case in json.loads(path.read_text()):
                cases.append({"id": case["id"], "text": case["input"], "expect": case["expect"], "file": path.name})
    for case in json.loads(MANIFEST.read_text()):
        if "expect" in case:
            cases.append({"id": "pcm:" + case["id"], "text": case["text"], "expect": case["expect"], "file": "manifest"})
    return [c for c in cases if not only or any(part in c["id"] for part in only)]


class Discover:
    """Real device answers to discover, cached in tests/fixtures (the registry is deterministic).

    Misses are asked from the device as a worker job (`/api/job kind=discover`): the
    catalog packages probe remote programs over SSH, which only the worker may do.
    The device runs one job at a time, so device queries are serialized here.
    """

    def __init__(self, device: str | None):
        self.device = device
        source = CACHE if CACHE.exists() else OLD_CACHE
        self.cache = json.loads(source.read_text()) if source.exists() else {}
        self.misses: set[str] = set()
        self.lock = threading.Lock()

    @staticmethod
    def key(arguments: dict) -> str:
        return json.dumps({k: arguments.get(k) for k in sorted(arguments)}, ensure_ascii=False)

    def ask_device(self, arguments: dict) -> str | None:
        body = urllib.parse.urlencode({"kind": "discover", "args": json.dumps(arguments, ensure_ascii=False)}).encode()
        with self.lock:
            for _ in range(60):  # the device may be busy with someone else's job
                try:
                    with urllib.request.urlopen(self.device + "/api/job", data=body, timeout=20) as response:
                        job = json.loads(response.read())["id"]
                    break
                except urllib.error.HTTPError as error:
                    if error.code != 409:
                        return None
                    time.sleep(1)
                except (urllib.error.URLError, TimeoutError):
                    return None
            else:
                return None
            for _ in range(120):
                time.sleep(0.5)
                try:
                    with urllib.request.urlopen(f"{self.device}/api/job?id={job}", timeout=20) as response:
                        record = json.loads(response.read())
                except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
                    continue
                if record.get("status") not in ("queued", "running"):
                    return record.get("reply") if record.get("status") == "done" else None
        return None

    def answer(self, arguments: dict) -> str:
        key = self.key(arguments)
        if key in self.cache:
            return self.cache[key]
        reply = self.ask_device(arguments) if self.device else None
        if reply is not None:
            self.cache[key] = reply
            return reply
        self.misses.add(key)
        return '{"error":"prompt lab: no cached discover answer"}'

    def refresh(self) -> int:
        """Re-ask the device every cached question (after a package changed its description)."""
        changed = 0
        for key in list(self.cache):
            reply = self.ask_device(json.loads(key))
            if reply is not None and reply != self.cache[key]:
                self.cache[key] = reply
                changed += 1
        return changed

    def save(self) -> None:
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        CACHE.write_text(json.dumps(self.cache, ensure_ascii=False, indent=1, sort_keys=True) + "\n")


def call_model(api: str, key: str, body: dict) -> dict:
    request = urllib.request.Request(api, data=json.dumps(body, ensure_ascii=False).encode(),
                                     headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.loads(response.read())
        except urllib.error.HTTPError as error:
            if error.code < 500 and error.code != 429:
                raise RuntimeError(f"HTTP {error.code} {error.read()[:200]!r}") from None
        except (urllib.error.URLError, TimeoutError):
            pass
        time.sleep(2 * (attempt + 1))
    raise RuntimeError("model API unreachable")


def run_case(case: dict, args, system: str, tools: list, profiles: str, discover: Discover, key: str) -> dict:
    messages = [{"role": "system", "content": system}, {"role": "system", "content": profiles},
                {"role": "user", "content": case["text"]}]
    calls, reply, tokens, started, discovers = [], "", 0, time.time(), 0
    for _ in range(MAX_ROUNDS):
        body = {"model": args.model, "temperature": 0, "max_tokens": 700, "tool_choice": "auto",
                "reasoning_effort": "none", "messages": messages, "tools": tools}
        result = call_model(args.api, key, body)
        tokens = max(tokens, result.get("usage", {}).get("prompt_tokens", 0))
        message = result["choices"][0]["message"]
        tool_calls = message.get("tool_calls") or []
        if not tool_calls:
            reply = message.get("content") or ""
            break
        parsed = []
        for tc in tool_calls:
            try:
                arguments = json.loads(tc["function"]["arguments"] or "{}")
            except json.JSONDecodeError:
                arguments = {"_unparsed": tc["function"]["arguments"]}
            parsed.append((tc, tc["function"]["name"], arguments))
        if any(name != "discover" for _, name, _ in parsed):
            # The device would execute these now; that is what the cases judge.
            calls += [{"name": name, "arguments": arguments} for _, name, arguments in parsed]
            break
        messages.append({"role": "assistant", "content": message.get("content") or "", "tool_calls": tool_calls})
        for tc, name, arguments in parsed:
            calls.append({"name": name, "arguments": arguments})
            discovers += 1
            usage = package_usage(arguments)  # registry::discover: a named on-demand package's usage
            content = (usage or discover.answer(arguments)) if discovers <= MAX_DISCOVER else '{"error":"discover 次数已达上限"}'
            messages.append({"role": "tool", "tool_call_id": tc["id"], "content": content})
    record = {"calls": calls, "reply": reply or ("(tool)" if calls else "")}
    failures = validate_expect(case["expect"], record)
    return {"id": case["id"], "passed": not failures, "failures": failures, "tokens": tokens,
            "seconds": round(time.time() - started, 1), "discovers": discovers, "calls": calls, "reply": reply[:120]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--prompt", help="plain-text system prompt to test instead of agent_prompt.h")
    parser.add_argument("--only", default="", help="comma list of case-id substrings")
    parser.add_argument("--repeat", type=int, default=1, help="runs per case (stability)")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--model", default="", help="default: the device's agent model (config.json), else the firmware default")
    parser.add_argument("--api", default="https://api.deepseek.com/chat/completions")
    parser.add_argument("--device", default=os.environ.get("VAT_DEVICE", ""), help="for discover cache misses and profile names; '' = offline")
    parser.add_argument("--profiles", default="", help="comma list overriding the device's SSH profile names")
    parser.add_argument("--json", help="write full results here")
    parser.add_argument("--print-prompt", action="store_true", help="print the composed system prompt and exit")
    parser.add_argument("--refresh", action="store_true", help="re-ask the device every cached discover answer first")
    parser.add_argument("--tools", help="JSON tool list to test instead of kToolsJson")
    parser.add_argument("--texts", help="a draft to try instead of the firmware's texts: core.txt, closing.txt, usage/<id>.txt, "
                                        "always.txt (resident packages), index.json (the others' index lines), tools.json")
    args = parser.parse_args()

    global TEXTS
    TEXTS = Path(args.texts) if args.texts else None
    if TEXTS and (TEXTS / "always.txt").exists():
        ALWAYS.update(p for p in (TEXTS / "always.txt").read_text().strip().split(",") if p)
    if TEXTS and not args.tools and (TEXTS / "tools.json").exists():
        args.tools = str(TEXTS / "tools.json")
    if not args.model:
        args.model = re.search(r'String agent_model = "([^"]+)"', (ROOT / "firmware/standalone_agent/src/config.h").read_text()).group(1)
        if args.device:
            try:
                with urllib.request.urlopen(args.device + "/api/file?path=/vat/config.json", timeout=10) as response:
                    args.model = json.loads(response.read()).get("agent", {}).get("model") or args.model
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
                pass
    system = (Path(args.prompt).read_text().strip() if args.prompt
              else system_prompt())
    if args.print_prompt:
        print(system)
        return 0
    tools = json.loads(Path(args.tools).read_text() if args.tools else raw_literal("kToolsJson", "TOOLS"))
    key = KEY_FILE.read_text().strip()
    names = args.profiles
    if not names and args.device:
        try:
            with urllib.request.urlopen(args.device + "/api/status", timeout=10) as response:
                names = json.loads(response.read()).get("profiles", "")
        except (urllib.error.URLError, TimeoutError):
            names = ""
    # Same text as agent.cpp profiles_message().
    profiles = f"当前可见 SSH profile（只含名称）: {names}。" if names else "当前没有可见的 SSH profile。"
    discover = Discover(args.device or None)
    if args.refresh:
        if not args.device:
            raise SystemExit("--refresh needs VAT_DEVICE / --device")
        print(f"refreshed discover cache: {discover.refresh()} of {len(discover.cache)} answers changed", flush=True)
    cases = load_cases([p for p in args.only.split(",") if p])
    print(f"model={args.model} cases={len(cases)} x{args.repeat} prompt={'variant ' + args.prompt if args.prompt else 'agent_prompt.h'} "
          f"system_chars={len(system)}", flush=True)

    results = []
    with futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = {pool.submit(run_case, case, args, system, tools, profiles, discover, key): case
                for case in cases for _ in range(args.repeat)}
        for job in futures.as_completed(jobs):
            case = jobs[job]
            try:
                result = job.result()
            except Exception as error:  # noqa: BLE001 - reported per case
                result = {"id": case["id"], "passed": False, "failures": [str(error)], "tokens": 0, "seconds": 0, "calls": []}
            results.append(result)
            if not result["passed"]:
                print(f"FAIL {result['id']}: {'; '.join(result['failures'])[:200]}", flush=True)
    discover.save()

    passed = sum(r["passed"] for r in results)
    flaky = sorted({r["id"] for r in results if not r["passed"]} & {r["id"] for r in results if r["passed"]})
    tokens = [r["tokens"] for r in results if r["tokens"]]
    seconds = sorted(r["seconds"] for r in results if r.get("seconds"))
    rounds = [r.get("discovers", 0) for r in results]
    print(f"passed {passed}/{len(results)}  flaky={flaky}  prompt_tokens~{max(tokens) if tokens else 0}  "
          f"discover_misses={len(discover.misses)}  discovers/case={sum(rounds) / max(len(rounds), 1):.2f}  "
          f"seconds_p50={seconds[len(seconds) // 2] if seconds else 0}")
    if args.json:
        Path(args.json).write_text(json.dumps(results, ensure_ascii=False, indent=1))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
