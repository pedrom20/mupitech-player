"""Screenshot capture — MupiTech addition, not upstream Anthias.

Dispatches by display backend:
  - cage/Wayland (x86, arm64, pi5): `grim` against the compositor.
  - eglfs/KMS (pi4-64, pi3-64): ffmpeg kmsgrab — see
    mupitech_screenshot_eglfs.py.
  - linuxfb (pi2, pi3): not implemented yet, reports "not supported".
    Lower priority given the older hardware — tracked as follow-up work.

Kept in its own module (not merged into anthias_viewer/__init__.py) so
upstream merges never need to touch it — only the one command-dict
entry and its handler function in __init__.py reference it.
"""

import base64
import os
import subprocess

_GRIM_TIMEOUT_S = 10


def _capture_via_grim() -> tuple[bool, str]:
    try:
        completed = subprocess.run(
            ['grim', '-t', 'png', '-'],
            capture_output=True,
            timeout=_GRIM_TIMEOUT_S,
        )
    except FileNotFoundError:
        return False, 'grim is not installed in this image.'
    except subprocess.TimeoutExpired:
        return False, 'grim timed out.'

    if completed.returncode != 0 or not completed.stdout:
        detail = completed.stderr.decode(errors='replace').strip() or f'exit code {completed.returncode}'
        return False, f'grim failed: {detail}'

    return True, base64.b64encode(completed.stdout).decode('ascii')


def capture_screenshot_b64() -> tuple[bool, str]:
    """Capture the display's current output, using whichever strategy
    matches this board's QT_QPA_PLATFORM.

    Returns (ok, message) — on success `message` is the base64-encoded
    PNG; on failure it's a human-readable error, mirroring
    lib/diagnostics.py's (ok, message) convention for CEC.
    """
    from anthias_viewer import _is_wayland_board

    qpa = os.environ.get('QT_QPA_PLATFORM', '')
    if _is_wayland_board():
        return _capture_via_grim()
    if qpa.startswith('eglfs'):
        from anthias_viewer.mupitech_screenshot_eglfs import capture_screenshot_b64 as capture_eglfs
        return capture_eglfs()
    return False, 'Screenshot is not supported on this board yet (linuxfb capture not implemented).'
