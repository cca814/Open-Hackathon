#!/usr/bin/env python3
"""Start controller/controller.py unchanged, also on checkouts without rc_state.py.

controller.py imports rc_state unconditionally but only uses it with
--rc-config; main does not ship rc_state.py (it lives on the rc-state branch).
An empty stand-in module keeps the plain ICRC repair loop runnable.
"""

import importlib.util
import runpy
import sys
import types
from pathlib import Path

CONTROLLER = Path(__file__).resolve().parent.parent / "controller"
sys.path.insert(0, str(CONTROLLER))
if importlib.util.find_spec("rc_state") is None:
    if "--rc-config" in sys.argv:
        sys.exit("--rc-config needs controller/rc_state.py")
    sys.modules["rc_state"] = types.ModuleType("rc_state")
sys.argv[0] = str(CONTROLLER / "controller.py")
runpy.run_path(sys.argv[0], run_name="__main__")
