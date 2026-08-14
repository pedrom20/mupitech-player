"""Video standby wrapper page — MupiTech addition, not upstream Anthias.

The viewer's view_image()/view_video() split is image-vs-blocking-media-
player; neither fits a standby video, which needs to loop indefinitely
in the background browser the same way a webpage asset does. This view
renders a tiny page with a looping <video> tag and the viewer loads it
via view_webpage() instead — see show_standby() in anthias_viewer.

Kept as its own file (not merged into app/views.py, an upstream-owned
file) so periodic merges from upstream/master never touch this. Wired
into routing from django_project/urls.py with one addition.
"""

import os

from django.conf import settings
from django.http import HttpRequest, HttpResponse
from django.shortcuts import render

STANDBY_VIDEO_FILENAMES = ('standby.mp4', 'standby.webm')


def _find_standby_video():
    """Return the static-relative path of whichever standby video file
    exists (mp4 checked first), or None. Mirrors the priority a device
    branding push already resolves to — see mupiteck's
    push_standby_image_to_player, which never leaves both present."""
    static_img_dir = os.path.join(settings.STATIC_ROOT, 'img')
    for filename in STANDBY_VIDEO_FILENAMES:
        if os.path.isfile(os.path.join(static_img_dir, filename)):
            return f'img/{filename}'
    return None


def standby_video(request: HttpRequest) -> HttpResponse:
    video_path = _find_standby_video()
    return render(request, 'mupitech_standby_video.html', {
        'video_url': f'{settings.STATIC_URL}{video_path}' if video_path else None,
    })
