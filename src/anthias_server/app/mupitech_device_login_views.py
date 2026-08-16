"""Login to this device using Fleet Manager credentials directly, typed
into this device's own login page — MupiTech addition, not upstream
Anthias.

The reverse direction from mupitech_sso_views.py's callback: that one
only ever *receives* a call (the FM signs a token, the device verifies
it locally). This one *makes* a call — the device relays the typed
username/password to the FM's own /api/auth/device-login/ for it to
check, proving its own identity with a signed proof built from the
same per-device secret SSO already provisions (settings['sso_secret']).

An MFA-enrolled FM account no longer dead-ends here with "use SSO
instead" — /api/auth/device-login/ returns the same {mfa_required,
challenge_id, ...} shape /api/auth/login/ does, and fm_login_mfa below
relays a code/push challenge through it exactly like the FM's own React
login screen does, calling the *same* /api/auth/mfa/* verify endpoints
a browser would. See fleet_manager/urls.py::auth_device_login and
mfa/challenge.py's 'device_login' cache flag on the FM side, which is
what makes those shared verify endpoints hand back a role instead of
establishing an FM session for this path.

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
from typing import Any

import requests
from django.contrib.auth import login as django_login
from django.contrib.messages import error as flash_error
from django.core import signing
from django.http import HttpRequest, HttpResponse
from django.shortcuts import redirect, render
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
# Duo/privacyIDEA push verify endpoints block server-side on the FM
# for up to ~60s waiting on the operator's phone — the device's own
# call to them needs headroom past that, not the plain 10s used for
# every other (near-instant) request here.
_PUSH_VERIFY_TIMEOUT_S = 70
_SESSION_KEY = 'fm_mfa_challenge'


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


def _verify_kind(method: str, push_methods: list[str]) -> str:
    return 'push' if method in push_methods else 'code'


def _verify_path(method: str, kind: str) -> str:
    if kind == 'push':
        return 'privacyidea-push-verify' if method == 'privacyidea' else 'duo-verify'
    if method == 'privacyidea':
        return 'privacyidea-verify'
    if method == 'email':
        return 'email-verify'
    return 'verify'


def _send_email_code(challenge: dict[str, Any]) -> None:
    """Emails a fresh code for the 'email' method — the device-side
    equivalent of login.tsx's sendEmailOtp() useEffect trigger on the
    FM's own React login page. Fire-and-forget from this view's
    perspective: a failure here just means the code form the user's
    about to see won't have a valid code yet, and 'Resend code' lets
    them try again; it never blocks rendering the page."""
    try:
        requests.post(
            f"{settings['fm_base_url']}/api/auth/mfa/email-send/",
            json={'challenge_id': challenge['challenge_id']},
            timeout=_REQUEST_TIMEOUT_S,
        )
    except requests.RequestException as exc:
        logger.warning('Fleet Manager email-otp send request failed: %s', exc)


def _finish_device_login(request: HttpRequest, data: dict[str, Any], next_url: str) -> HttpResponse:
    operator = _persisted_operator()
    if operator is None:
        return redirect(reverse('anthias_app:home'))
    django_login(request, operator)
    request.session['fm_sso_role'] = data.get('role', '')
    request.session.pop(_SESSION_KEY, None)
    logger.info('Fleet Manager device-login accepted (role %r)', data.get('role'))
    return redirect(next_url or reverse('anthias_app:home'))


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
        data = resp.json()
        if data.get('mfa_required'):
            request.session[_SESSION_KEY] = {
                'challenge_id': data['challenge_id'],
                'method': data['method'],
                'available_methods': data.get('available_methods') or [data['method']],
                'push_methods': data.get('push_methods') or [],
                'dual_required': data.get('dual_required', False),
                'stage': data.get('stage', 1),
                'push_attempted': False,
                'email_sent': False,
                'next': next_url,
            }
            return redirect(reverse('mupitech_fm_login_mfa'))
        return _finish_device_login(request, data, next_url)

    if resp.status_code == 401:
        flash_error(request, _('Invalid Fleet Manager username or password.'))
    elif resp.status_code == 429:
        flash_error(request, _('Too many attempts — try again shortly.'))
    else:
        flash_error(request, _("Couldn't verify those credentials with the Fleet Manager."))
    return redirect(f"{reverse('anthias_app:login')}?next={next_url}")


def fm_login_mfa(request: HttpRequest) -> HttpResponse:
    """Second step of the FM-account login, only ever reached once
    fm_login above stashed a pending challenge in the session (an
    MFA-enrolled account). GET renders the challenge (or, for a push
    method, blocks on the auto-triggered push exactly once — see
    'push_attempted' below); POST either submits a typed code, retries
    a push, switches to a different enrolled method, or backs out."""
    challenge = request.session.get(_SESSION_KEY)
    if not challenge:
        return redirect(reverse('anthias_app:login'))

    kind = _verify_kind(challenge['method'], challenge['push_methods'])

    if request.method == 'POST':
        if request.POST.get('back'):
            next_url = challenge.get('next', '')
            request.session.pop(_SESSION_KEY, None)
            return redirect(f"{reverse('anthias_app:login')}?next={next_url}")

        if request.POST.get('switch_method'):
            new_method = request.POST['switch_method']
            if new_method in challenge['available_methods']:
                challenge['method'] = new_method
                challenge['push_attempted'] = False
                challenge['email_sent'] = False
                request.session[_SESSION_KEY] = challenge
            return redirect(reverse('mupitech_fm_login_mfa'))

        if request.POST.get('resend_email'):
            _send_email_code(challenge)
            return redirect(reverse('mupitech_fm_login_mfa'))

        code = request.POST.get('code', '') if kind == 'code' else ''
        return _submit_verify(request, challenge, code)

    # Push-kind: auto-trigger exactly once per (challenge, method) pair
    # — same as login.tsx's useEffect — so landing on this page sends
    # the notification without an extra click, but a failed/denied/
    # timed-out attempt doesn't loop straight back into another push
    # the instant this same GET re-renders.
    if kind == 'push' and not challenge.get('push_attempted'):
        challenge['push_attempted'] = True
        request.session[_SESSION_KEY] = challenge
        return _submit_verify(request, challenge, '')

    if challenge['method'] == 'email' and not challenge.get('email_sent'):
        challenge['email_sent'] = True
        request.session[_SESSION_KEY] = challenge
        _send_email_code(challenge)

    return render(request, 'login_mfa.html', {
        'method': challenge['method'],
        'kind': kind,
        'other_methods': [m for m in challenge['available_methods'] if m != challenge['method']],
        'stage': challenge.get('stage', 1),
        'dual_required': challenge.get('dual_required', False),
    })


def _submit_verify(request: HttpRequest, challenge: dict[str, Any], code: str) -> HttpResponse:
    kind = _verify_kind(challenge['method'], challenge['push_methods'])
    path = _verify_path(challenge['method'], kind)
    payload = {'challenge_id': challenge['challenge_id']}
    if kind == 'code':
        payload['code'] = code

    try:
        resp = requests.post(
            f"{settings['fm_base_url']}/api/auth/mfa/{path}/",
            json=payload,
            timeout=_PUSH_VERIFY_TIMEOUT_S if kind == 'push' else _REQUEST_TIMEOUT_S,
        )
    except requests.RequestException as exc:
        logger.warning('Fleet Manager MFA verify request failed: %s', exc)
        flash_error(request, _("Couldn't reach the Fleet Manager — try again shortly."))
        return redirect(reverse('mupitech_fm_login_mfa'))

    if resp.status_code == 200:
        data = resp.json()
        if data.get('mfa_required'):
            # Dual-MFA: first factor passed, same challenge_id carries
            # over to a second, different-provider one.
            challenge['method'] = data['method']
            challenge['available_methods'] = data.get('available_methods') or [data['method']]
            challenge['push_methods'] = data.get('push_methods') or []
            challenge['stage'] = data.get('stage', 2)
            challenge['push_attempted'] = False
            challenge['email_sent'] = False
            request.session[_SESSION_KEY] = challenge
            return redirect(reverse('mupitech_fm_login_mfa'))
        return _finish_device_login(request, data, challenge.get('next', ''))

    if resp.status_code == 400:
        # Challenge expired/invalid (TTL ran out, or too many earlier
        # bad attempts) — no way to recover in place, restart from
        # credentials.
        next_url = challenge.get('next', '')
        request.session.pop(_SESSION_KEY, None)
        flash_error(request, _('The login attempt expired — please try again.'))
        return redirect(f"{reverse('anthias_app:login')}?next={next_url}")

    if resp.status_code == 401:
        detail = {}
        try:
            detail = resp.json()
        except ValueError:
            pass
        if detail.get('detail') == 'timeout':
            flash_error(request, _('The push notification timed out — try again.'))
        elif detail.get('detail') == 'denied':
            flash_error(request, _('The push notification was denied.'))
        else:
            flash_error(request, _('Invalid code.'))
        return redirect(reverse('mupitech_fm_login_mfa'))

    if resp.status_code == 429:
        flash_error(request, _('Too many attempts — try again shortly.'))
        return redirect(reverse('mupitech_fm_login_mfa'))

    flash_error(request, _("Couldn't verify that with the Fleet Manager — try again."))
    return redirect(reverse('mupitech_fm_login_mfa'))
