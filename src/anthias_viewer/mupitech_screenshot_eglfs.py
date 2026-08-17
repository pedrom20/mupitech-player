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

On a rotated screen (screen_rotation != 0), the raw plane framebuffer
kmsgrab reads is what Qt's QOpenGLCompositor rendered — pre-turned so
that scanning it out, as-is, to the physically-mounted panel looks
upright to a viewer standing in front of it (see _build_webview_env's
QT_QPA_EGLFS_ROTATION in anthias_viewer/__init__.py). That's the
opposite of what a screenshot needs: kmsgrab's buffer keeps the
panel's native (unrotated) width/height, with the actual content
turned inside it, so displaying it as-is anywhere else (the Fleet
Manager's browser) shows it sideways — confirmed on real Pi 4 hardware
at screen_rotation=270 (content readable only if you turn the
FM's screenshot preview itself). Compensating needs the INVERSE of
Qt's own pre-turn, i.e. (360 - screen_rotation) % 360 — see
_rotation_compensation_filter — applied to the captured PNG as a
second ffmpeg pass before returning it.
"""

import base64
import glob
import subprocess

_FFMPEG_TIMEOUT_S = 15

# ffmpeg -vf filter for each cardinal angle a captured eglfs screenshot
# needs turned to compensate for Qt's own pre-rotation (see module
# docstring). Keyed by (360 - screen_rotation) % 360, not screen_rotation
# directly — verified empirically against a real Pi 4 at
# screen_rotation=270 (needs the 90 entry, not the 270 one).
_ROTATION_FILTERS = {
    0: None,
    90: 'transpose=1',  # 90° clockwise, no flip
    180: 'hflip,vflip',
    270: 'transpose=2',  # 90° counterclockwise, no flip
}


def _rotation_compensation_filter(screen_rotation: int) -> str | None:
    """ffmpeg -vf value to turn a raw eglfs screenshot upright for the
    given (clockwise) screen_rotation setting, or None if no
    compensation is needed. See module docstring for why this is the
    inverse angle, not screen_rotation itself."""
    angle = (360 - (screen_rotation % 360)) % 360
    return _ROTATION_FILTERS.get(angle)


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

    png_bytes = completed.stdout

    from anthias_viewer import _rotation_value

    rotation_filter = _rotation_compensation_filter(_rotation_value())
    if rotation_filter:
        try:
            rotated = subprocess.run(
                [
                    'ffmpeg', '-hide_banner', '-loglevel', 'error',
                    '-f', 'image2', '-i', '-',
                    '-vf', rotation_filter,
                    '-f', 'image2', '-c:v', 'png', '-',
                ],
                input=png_bytes,
                capture_output=True,
                check=False,
                timeout=_FFMPEG_TIMEOUT_S,
            )
        except subprocess.TimeoutExpired:
            return False, 'ffmpeg (rotation compensation) timed out.'
        if rotated.returncode == 0 and rotated.stdout:
            png_bytes = rotated.stdout
        # A failed compensation pass still returns the un-rotated
        # capture rather than nothing — a sideways screenshot is more
        # useful for diagnosing a problem than no screenshot at all.

    return True, base64.b64encode(png_bytes).decode('ascii')
