#!/usr/bin/env python3
"""Synthesize the S1 voice fixtures (tests/pcm/*.pcm) with a TTS model.

The committed fixtures were produced by this script with OpenRouter
`x-ai/grok-voice-tts-1.0`, voice `ara`; none of them is a human recording.
Regenerate only when you add a case or want to test another voice:

    OPENROUTER_API_KEY=... python3 tools/gen_pcm.py            # only missing files
    OPENROUTER_API_KEY=... python3 tools/gen_pcm.py --force    # all of them

Output: raw PCM16 little-endian, mono, 16 kHz (what the device uploads to the
speech recognizer).  Needs ffmpeg.  The key may also live in ../.openrouter_voice_key
(outside the repository); it is never printed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PCM = ROOT / "tests" / "pcm"
MANIFESTS = {"manifest.json": "generated.json", "standalone_manifest.json": "standalone_manifest.generated.json"}
TTS_URL = "https://openrouter.ai/api/v1/audio/speech"


def api_key() -> str:
    key = os.environ.get("OPENROUTER_API_KEY", "")
    if not key and (ROOT.parent / ".openrouter_voice_key").exists():
        key = (ROOT.parent / ".openrouter_voice_key").read_text().strip()
    if not key:
        raise SystemExit("set OPENROUTER_API_KEY (or ../.openrouter_voice_key)")
    return key


def synthesize(text: str, key: str, model: str, voice: str) -> bytes:
    body = {"model": model, "input": text, "voice": voice, "response_format": "mp3"}
    request = urllib.request.Request(TTS_URL, data=json.dumps(body, ensure_ascii=False).encode(), method="POST",
                                     headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise SystemExit(f"TTS HTTP {error.code}: {error.read(300).decode(errors='replace')}") from None


def to_pcm(mp3: bytes, output: Path) -> None:
    with tempfile.NamedTemporaryFile(suffix=".mp3") as source:
        source.write(mp3)
        source.flush()
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", source.name,
                        "-ac", "1", "-ar", "16000", "-f", "s16le", "-acodec", "pcm_s16le", str(output)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", default="x-ai/grok-voice-tts-1.0")
    parser.add_argument("--voice", default="ara")
    parser.add_argument("--force", action="store_true", help="re-synthesize files that already exist")
    args = parser.parse_args()

    key = None
    for manifest, metadata in MANIFESTS.items():
        generated = []
        for case in json.loads((PCM / manifest).read_text()):
            output = PCM / f"{case['id']}.pcm"
            if args.force or not output.exists():
                key = key or api_key()
                to_pcm(synthesize(case["text"], key, args.model, args.voice), output)
            pcm = output.read_bytes()
            generated.append({"id": case["id"], "file": output.name, "format": "s16le-mono-16000hz",
                              "bytes": len(pcm), "duration_seconds": round(len(pcm) / 32000, 3),
                              "sha256": hashlib.sha256(pcm).hexdigest(), "tts_model": args.model,
                              "tts_voice": args.voice})
            print(f"{case['id']}: {len(pcm) / 32000:.2f}s")
        (PCM / metadata).write_text(json.dumps(generated, ensure_ascii=False, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
