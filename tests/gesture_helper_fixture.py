"""仅供离屏 Qt 回归测试使用，独立于实机助手。"""

import json
import os
import sys


mode = sys.argv[1]
scenario = os.environ["GESTURE_GUI_TEST_SCENARIO"]
if mode == "probe":
    if scenario == "probe_failed":
        print(json.dumps({"event": "error", "message": "Remote I/O error"}))
        sys.exit(1)
    print(json.dumps({"event": "chip", "id": "0x7620"}))
    sys.exit(0)
print(json.dumps({"event": "ready", "mode": mode}))
print(json.dumps({"event": "result", "unlocked": scenario != "denied"}))
sys.exit(1 if scenario == "failed" else 0)
