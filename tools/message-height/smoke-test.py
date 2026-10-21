# SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
# SPDX-License-Identifier: MIT

import ctypes
import json
import math
import os
from pathlib import Path
import sys


directory = Path(sys.argv[1]).resolve()
dependencies = []
if os.name == "nt":
    dll_directory = os.add_dll_directory(str(directory))
    for name in ("icudt78.dll", "icuuc78.dll"):
        dependencies.append(ctypes.CDLL(str(directory / name)))
    filename = "MessageHeightTwitch.dll"
else:
    filename = "MessageHeightTwitch.so"

library = ctypes.CDLL(str(directory / filename))
library.ChatterinoHeightInit.argtypes = [ctypes.c_char_p]
library.ChatterinoHeightInit.restype = ctypes.c_int
library.ChatterinoHeightMeasure.argtypes = [ctypes.c_char_p]
library.ChatterinoHeightMeasure.restype = ctypes.c_float
if library.ChatterinoHeightInit(str(directory).encode()) != 1:
    raise RuntimeError("Couldn't load MessageHeightTwitch")


def measure(text, *, action=False, providers=None):
    request = {
        "text": text,
        "login": "iore",
        "displayName": "Iore",
        "badges": 2,
        "action": action,
        "providers": providers or {},
    }
    height = library.ChatterinoHeightMeasure(json.dumps(request).encode())
    if not math.isfinite(height) or height <= 0:
        raise RuntimeError(f"Couldn't measure {text!r}: {height}")
    return height


plain = measure("test")
wrapped = measure("test " * 50)
tall = measure("Tall", providers={
    "seventv": [{"name": "Tall", "width": 28, "height": 100}],
})
action = measure("test", action=True)
unicode = measure("é 福森 🙂")
if wrapped <= plain or tall <= plain:
    raise RuntimeError("Wrapped text or emote dimensions weren't measured")
print(f"MessageHeightTwitch: text={plain}, wrapped={wrapped}, "
      f"emote={tall}, action={action}, unicode={unicode}")
