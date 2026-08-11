"""Screenshot capture for eglfs/KMS boards (Pi4-64, Pi3-64) — MupiTech
addition, not upstream Anthias.

Wayland boards capture via `grim` talking to the cage compositor (see
mupitech_screenshot.py). eglfs boards have no compositor at all — the
anthias-webview C++ process is itself the sole Qt QPA client doing
direct KMS modesetting, so there's nothing to ask over a compositor
protocol. Capture here instead reads the current scanout framebuffer
straight off the DRM device via ffmpeg's kmsgrab input device, which
does NOT require being (or coordinating with) the DRM master — it
exports the active plane's framebuffer read-only via
drmModeGetFB2/drmPrimeHandleToFD, so it can run alongside the webview
holding master without conflict (confirmed against kmsgrab.c and the
ffmpeg-devices documentation; there's no prior art for this in either
upstream Anthias or the third-party fork this project is moving away
from, so this was verified from first principles rather than adapted
from an existing implementation).

kmsgrab's default `/dev/dri/card0` isn't reliable on these boards: the
vc4 (display) and v3d (render-only) DRM nodes race during probe, and
whichever wins can land on either card number (see the identical
problem solved for Qt's own eglfs_kms config in
bin/lib/viewer/platform_eglfs.sh::detect_eglfs_kms_card). This module
re-derives the same card by scanning for a connected connector rather
than assuming card0.
"""

import base64
import glob
import subprocess

_FFMPEG_TIMEOUT_S = 15


def _find_kms_display_card() -> str | None:
    """Return e.g. '/dev/dri/card1' for the DRM card that owns a
    connected display connector, or None if none is found. Mirrors
    detect_eglfs_kms_card's connector-scan in platform_eglfs.sh so both
    land on the same card even when vc4/v3d probe order flips it."""
    fallback_card = None
    for status_file in sorted(glob.glob('/sys/class/drm/card*-*/status')):
        connector = status_file.split('/')[-2]  # e.g. card1-HDMI-A-1
        if 'riteback' in connector:  # KMS "Writeback" connector isn't a real output
            continue
        card = connector.split('-', 1)[0]
        device = f'/dev/dri/{card}'
        try:
            with open(status_file) as f:
                status = f.read().strip()
        except OSError:
            continue
        if status == 'connected':
            return device
        if fallback_card is None:
            fallback_card = device
    return fallback_card


def capture_screenshot_b64() -> tuple[bool, str]:
    """Capture the current KMS scanout framebuffer via ffmpeg's kmsgrab
    device, converting the hardware (DRM_PRIME) frame to a regular PNG.

    Returns (ok, message) — on success `message` is the base64-encoded
    PNG; on failure it's a human-readable error, mirroring
    lib/diagnostics.py's (ok, message) convention for CEC.
    """
    device = _find_kms_display_card()
    if not device:
        return False, 'No connected DRM display connector found.'

    try:
        completed = subprocess.run(
            [
                'ffmpeg', '-hide_banner', '-loglevel', 'error',
                '-device', device,
                '-f', 'kmsgrab', '-i', '-',
                '-frames:v', '1',
                '-vf', 'hwdownload,format=bgr0',
                '-f', 'image2', '-c:v', 'png', '-',
            ],
            capture_output=True,
            timeout=_FFMPEG_TIMEOUT_S,
        )
    except FileNotFoundError:
        return False, 'ffmpeg is not installed in this image.'
    except subprocess.TimeoutExpired:
        return False, 'ffmpeg (kmsgrab) timed out.'

    if completed.returncode != 0 or not completed.stdout:
        detail = completed.stderr.decode(errors='replace').strip() or f'exit code {completed.returncode}'
        return False, f'ffmpeg kmsgrab failed: {detail}'

    return True, base64.b64encode(completed.stdout).decode('ascii')
