"""subzero: .sbs/.sbsar materials rendered by sbsrender, without ArmorPaint.

The modules are the ones ArmorPaint Studio runs as flat scripts (import subzero_render, ...): this package puts its own
folder at the front of sys.path, then exposes main() and __version__ from subzero_app.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from subzero_app import __version__, main  # noqa: E402

__all__ = ["__version__", "main"]
