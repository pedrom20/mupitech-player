import pytest

from anthias_viewer import mupitech_screenshot_eglfs as screenshot_eglfs


@pytest.mark.parametrize(
    ('screen_rotation', 'expected_filter'),
    [
        (0, None),
        # Verified against a real Pi 4 at screen_rotation=270: the
        # captured buffer only reads upright after 'transpose=1' (90°
        # clockwise), NOT the naive same-angle guess of 'transpose=2'
        # (90° counterclockwise) — see the module docstring for why the
        # compensation angle is the inverse of screen_rotation.
        (270, 'transpose=1'),
        (90, 'transpose=2'),
        (180, 'hflip,vflip'),
        # Same cardinal angle regardless of how many full turns it's
        # expressed with (630 % 360 == 270).
        (360, None),
        (630, 'transpose=1'),
    ],
)
def test_rotation_compensation_filter(
    screen_rotation: int, expected_filter: str | None
) -> None:
    assert (
        screenshot_eglfs._rotation_compensation_filter(screen_rotation)
        == expected_filter
    )
