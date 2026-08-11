"""Screenshot capture — MupiTech addition, not upstream Anthias.

Only implemented for the cage/Wayland boards (x86, arm64, pi5) so far —
`_is_wayland_board()` gates it, so eglfs (pi4-64) and linuxfb (pi2/pi3)
correctly report "not supported" rather than attempting something that
doesn't apply to their display backend. Those boards need their own
capture strategy (DRM/kmsgrab for eglfs, framebuffer read for linuxfb),
tracked as follow-up work.

Kept in its own module (not merged into anthias_viewer/__init__.py) so
upstream merges never need to touch it — only the one command-dict
entry and its handler function in __init__.py reference it.
"""

import base64
import subprocess

_GRIM_TIMEOUT_S = 10


def capture_screenshot_b64() -> tuple[bool, str]:
    """Capture the compositor's current output via `grim`.

    Returns (ok, message) — on success `message` is the base64-encoded
    PNG; on failure it's a human-readable error, mirroring
    lib/diagnostics.py's (ok, message) convention for CEC.
    """
    from anthias_viewer import _is_wayland_board

    if not _is_wayland_board():
        return False, 'Screenshot is not supported on this board (no Wayland compositor).'

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
