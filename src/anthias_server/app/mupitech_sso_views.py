"""Fleet Manager SSO login callback — MupiTech addition, not upstream
Anthias.

Verifies a short-lived token minted by mupiteck's players/sso.py using
a per-device secret shared only between this device and that Fleet
Manager instance (provisioned over SSH into settings['sso_secret'] —
see that secret's own docstring in anthias_server/settings.py). On a
valid token, logs the request in as this device's single operator
account (see lib/auth.py's _persisted_operator — Anthias assumes one
local admin account, so SSO doesn't create per-FM-user accounts here,
it just authenticates as that one).

Kept as its own file (not merged into app/views.py, an upstream-owned
file) so periodic merges from upstream/master never touch this. Wired
into routing from django_project/urls.py with one addition.
"""

import logging

from django.contrib.auth import login as django_login
from django.core import signing
from django.http import HttpRequest, HttpResponse, HttpResponseRedirect
from django.shortcuts import render

from anthias_server.lib.auth import _persisted_operator
from anthias_server.settings import settings

logger = logging.getLogger(__name__)

SSO_TOKEN_SALT = 'mupitech-sso-login'
SSO_TOKEN_MAX_AGE = 60  # seconds — must match players/sso.py on the Fleet Manager side


def sso_callback(request: HttpRequest) -> HttpResponse:
    device_secret = settings['sso_secret']
    token = request.GET.get('token', '')

    if not device_secret or not token:
        return render(request, 'mupitech_sso_error.html', status=400)

    try:
        payload = signing.loads(
            token, key=device_secret, salt=SSO_TOKEN_SALT, max_age=SSO_TOKEN_MAX_AGE,
        )
    except signing.BadSignature:
        logger.warning('SSO callback: bad token signature')
        return render(request, 'mupitech_sso_error.html', status=403)
    except signing.SignatureExpired:
        logger.info('SSO callback: expired token')
        return render(request, 'mupitech_sso_error.html', status=403)

    operator = _persisted_operator()
    if operator is None:
        # No local account exists at all — auth_backend is presumably
        # disabled too, so the dashboard is already open without SSO.
        return HttpResponseRedirect('/')

    django_login(request, operator)
    logger.info(
        'SSO login accepted (requested by Fleet Manager user %r)',
        payload.get('requested_by', '?'),
    )
    return HttpResponseRedirect('/')
