#!/usr/bin/env python3
"""One tool call on the device, for trying packages by hand.

    python3 tools/ble_call.py read ble://status
    python3 tools/ble_call.py --approve write ble://power on

--approve answers the confirmation as the device's 'approve' key would;
without it a gated call is left waiting and the prompt is printed.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import vat_e2e  # noqa: E402

args = sys.argv[1:]
approve = "--approve" in args
args = [a for a in args if a != "--approve"]
device = vat_e2e.Device(os.environ.get("VAT_DEVICE", "http://vat.local"))
tool, target = args[0], args[1]
content = args[2] if len(args) > 2 else None
record = vat_e2e.tool_call(device, tool, target, content, timeout=120)
if record.get("status") == "awaiting_approval":
    if not approve:
        print("等待确认：" + device.status()["screen"].get("pending", ""))
        sys.exit(0)
    record = vat_e2e.answer(device, "approve")
print(record.get("reply") or record.get("error") or json.dumps(record, ensure_ascii=False)[:800])
