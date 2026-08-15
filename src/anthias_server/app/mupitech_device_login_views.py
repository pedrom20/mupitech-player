"""Login to this device using Fleet Manager credentials directly, typed
into this device's own login page — MupiTech addition, not upstream
Anthias.

The reverse direction from mupitech_sso_views.py's callback: that one
only ever *receives* a call (the FM signs a token, the device verifies
it locally). This one *makes* a call — the device relays the typed
username/password to the FM's own /api/auth/device-login/ for it to
check, proving its own identity with a signed proof built from the
same per-device secret SSO already provisions (settings['sso_secret']).

Requires fm_base_url + fm_player_id, both pushed alongside sso_secret
by players/sso.py::push_sso_secret_to_player on the Fleet Manager side
— empty on a device that's never had SSO provisioned, in which case
this whole path is unavailable and the login page falls back to
device-only credentials (see fm_login_available in views.py::login).

Kept as its own file (not merged into app/views.py, an upstream-owned
file) so periodic merges from upstream/master never touch this. Wired
into routing from django_project/urls.py with one addition.
"""

import logging

import requests
from django.contrib.auth import login as django_login
from django.contrib.messages import error as flash_error
from django.core import signing
from django.http import HttpRequest, HttpResponse
from django.shortcuts import redirect
from django.urls import reverse
from django.utils.translation import gettext as _

from anthias_server.lib.auth import _persisted_operator
from anthias_server.settings import settings

logger = logging.getLogger(__name__)

# Must match players/sso.py::DEVICE_AUTH_PROOF_SALT on the Fleet
# Manager side exactly — separate repos, so this is a literal string on
# both ends rather than a shared import.
DEVICE_AUTH_PROOF_SALT = 'mupitech-device-auth'
_REQUEST_TIMEOUT_S = 10


def fm_login_available() -> bool:
    # Re-read anthias.conf rather than trusting this worker's in-memory
    # copy: players/sso.py::push_sso_secret_to_player on the Fleet
    # Manager side writes these three keys via a short-lived `docker
    # exec ... python -c "...; settings.save()"` call, a *different*
    # process from the one serving this request — that process's own
    # AnthiasSettings.save() correctly reloads its own copy, but it
    # never touches the long-running web server's separate singleton
    # (module-level `settings = AnthiasSettings()` in settings.py,
    # instantiated once at worker start). Confirmed live: a push
    # landed on disk immediately, but the login page kept hiding this
    # tab until the anthias-server container was restarted. A cheap
    # small-INI-file read on every login page view is a fair trade for
    # never hitting that again.
    settings.load()
    return bool(settings['fm_base_url'] and settings['fm_player_id'] and settings['sso_secret'])


def fm_login(request: HttpRequest) -> HttpResponse:
    next_url = request.POST.get('next') or ''

    if not fm_login_available():
        flash_error(request, _("This device hasn't been set up for Fleet Manager login yet."))
        return redirect(f"{reverse('anthias_app:login')}?next={next_url}")

    username = request.POST.get('fm_username') or ''
    password = request.POST.get('fm_password') or ''

    proof = signing.dumps(
        {'player_id': settings['fm_player_id']},
        key=settings['sso_secret'],
        salt=DEVICE_AUTH_PROOF_SALT,
        compress=True,
    )

    try:
        resp = requests.post(
            f"{settings['fm_base_url']}/api/auth/device-login/",
            json={
                'player_id': settings['fm_player_id'],
                'proof': proof,
                'username': username,
                'password': password,
            },
            timeout=_REQUEST_TIMEOUT_S,
        )
    except requests.RequestException as exc:
        logger.warning('Fleet Manager device-login request failed: %s', exc)
        flash_error(request, _("Couldn't reach the Fleet Manager — try again shortly."))
        return redirect(f"{reverse('anthias_app:login')}?next={next_url}")

    if resp.status_code == 200:
        operator = _persisted_operator()
        if operator is None:
            return redirect(reverse('anthias_app:home'))
        django_login(request, operator)
        data = resp.json()
        request.session['fm_sso_role'] = data.get('role', '')
        logger.info('Fleet Manager device-login accepted for %r (role %r)', username, data.get('role'))
        return redirect(next_url or reverse('anthias_app:home'))

    if resp.status_code == 409:
        flash_error(request, _('This account has two-factor authentication enabled — use the "Open local dashboard" link from the device page in the Fleet Manager instead.'))
    elif resp.status_code == 401:
        flash_error(request, _('Invalid Fleet Manager username or password.'))
    elif resp.status_code == 429:
        flash_error(request, _('Too many attempts — try again shortly.'))
    else:
        flash_error(request, _("Couldn't verify those credentials with the Fleet Manager."))
    return redirect(f"{reverse('anthias_app:login')}?next={next_url}")
