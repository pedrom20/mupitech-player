"""IR remote-control support — MupiTech addition, not upstream Anthias.

Pure Python, shells out to `ir-ctl` (from v4l-utils) against the
kernel's lirc device node — no Qt/C++ viewer changes needed, unlike
the older Anthias architectures. Kept in its own module (not merged
into lib/diagnostics.py) so upstream merges never need to touch it.

CLI reference confirmed against the actual v4l-utils source
(utils/ir-ctl/ir-ctl.1.in): `ir-ctl -d <device> --scancode=<protocol>:
<scancode>` sends one IR scancode in the given protocol; `-d` defaults
to /dev/lirc0 if omitted. There is no separate "status" subcommand —
availability is a device-node probe, same pattern as
diagnostics.cec_available().
"""

import os

from anthias_server.lib.diagnostics import _run_bounded

IR_DEVICE = '/dev/lirc0'
_IR_TIMEOUT_S = 10


def ir_available() -> bool:
    """Cheap render-time gate for whether to show IR controls."""
    return os.path.exists(IR_DEVICE)


def get_ir_device() -> str | None:
    return IR_DEVICE if ir_available() else None


def send_ir_test(protocol: str, scancode: str) -> tuple[bool, str]:
    """Send one IR scancode via `ir-ctl --scancode=<protocol>:<scancode>`.

    Returns (ok, message) for direct surfacing to the operator/API
    caller. Uses the same bounded-reap subprocess helper CEC does
    (lib/diagnostics._run_bounded) so a wedged ir-ctl process can't
    hang the request thread.
    """
    if not ir_available():
        return False, f'No IR device found at {IR_DEVICE}.'

    argv = [
        'ir-ctl', '-d', IR_DEVICE,
        f'--scancode={protocol}:{scancode}',
    ]
    completed = _run_bounded(argv, _IR_TIMEOUT_S)
    if completed is None:
        return False, 'ir-ctl timed out — IR transmitter unresponsive.'

    output, stderr, returncode = completed
    if returncode == 0:
        return True, f'Sent {protocol}:{scancode}.'
    detail = stderr.strip() or output.strip() or f'exit code {returncode}'
    return False, f'ir-ctl failed: {detail}'
