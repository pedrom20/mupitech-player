"""Interactive first-time pairing with a Fleet Manager.

The device asks to join (mupiteck's players/pairing_views.py::
pairing_request), prints a short pairing code, and waits for an admin
to approve or reject it from the Fleet Manager's own UI — no SSH, no
typing this device's IP/SSH credentials into the FM up front, and no
Fleet Manager username/password (with its own MFA) typed into this
device's CLI. See players/models.py::PendingPairing on the mupiteck
side for the full design rationale.

Run inside the anthias-server container:

    manage.py pair_fleet_manager
    manage.py pair_fleet_manager --fm-url https://fleet.example.com
"""

from __future__ import annotations

import socket
import time
from typing import Any

import requests
from django.core.management.base import BaseCommand, CommandParser

from anthias_common.utils import get_node_ip, get_node_mac_address
from anthias_server.settings import settings

POLL_INTERVAL_S = 3
REQUEST_TIMEOUT_S = 10


class Command(BaseCommand):
    help = 'Pair this device with a Fleet Manager instance.'

    def add_arguments(self, parser: CommandParser) -> None:
        parser.add_argument(
            '--fm-url',
            default=None,
            help='Fleet Manager base URL (asked interactively if omitted).',
        )

    def handle(self, *args: Any, **options: Any) -> None:
        fm_url = (options.get('fm_url') or '').strip().rstrip('/')
        if not fm_url:
            fm_url = input(
                'Fleet Manager URL (e.g. https://fleet.example.com): ',
            ).strip().rstrip('/')
        if not fm_url:
            self.stderr.write(self.style.ERROR('A Fleet Manager URL is required.'))
            return

        self.stdout.write("Detecting this device's network address...")
        # get_node_ip() can block up to ~80s on bare metal (waiting on
        # the host-agent's Redis-published cache) — acceptable here,
        # same tradeoff /api/v2/info's own (unpolled, human-triggered)
        # get_ip_addresses() already makes for the same reason.
        node_ip = get_node_ip()
        if node_ip in ('Unknown', 'Unable to retrieve IP.', ''):
            self.stderr.write(self.style.ERROR(
                f"Couldn't detect this device's IP address ({node_ip}). Aborting.",
            ))
            return
        # First token only, same simplification register_player's own
        # phone-home script effectively makes (resolve the default-
        # route interface, fall back to `hostname -I`'s first token) —
        # good enough for the common single-NIC case this interactive
        # command targets.
        url = f'http://{node_ip.split()[0]}'
        mac_address = get_node_mac_address()
        device_name = socket.gethostname()

        try:
            resp = requests.post(
                f'{fm_url}/api/pairing/request/',
                json={'device_name': device_name, 'mac_address': mac_address, 'url': url},
                timeout=REQUEST_TIMEOUT_S,
            )
            resp.raise_for_status()
        except requests.RequestException as exc:
            self.stderr.write(self.style.ERROR(f"Couldn't reach {fm_url}: {exc}"))
            return

        data = resp.json()
        pairing_id = data['pairing_id']
        poll_token = data['poll_token']
        ttl_minutes = data.get('ttl_minutes', 15)

        self.stdout.write('')
        self.stdout.write(self.style.SUCCESS(f"Pairing code: {data['pairing_code']}"))
        self.stdout.write(
            'Ask an admin to approve this device in the Fleet Manager '
            '(Players > Devices awaiting approval).',
        )
        self.stdout.write(f'This request expires in {ttl_minutes} minutes.')
        self.stdout.write('')

        # A little slack past the server's own TTL so a slow first poll
        # right at the boundary still gets one last real answer from
        # the server (which reports 'expired' itself) instead of this
        # loop timing out a moment earlier with a less specific message.
        deadline = time.monotonic() + ttl_minutes * 60 + 30
        while time.monotonic() < deadline:
            try:
                status_resp = requests.get(
                    f'{fm_url}/api/pairing/{pairing_id}/status/',
                    params={'poll_token': poll_token},
                    timeout=REQUEST_TIMEOUT_S,
                )
                status_resp.raise_for_status()
                status_data = status_resp.json()
            except requests.RequestException:
                time.sleep(POLL_INTERVAL_S)
                continue

            current_status = status_data.get('status')
            if current_status == 'approved':
                settings['sso_secret'] = status_data['sso_secret']
                settings['fm_player_id'] = status_data['fm_player_id']
                settings['fm_base_url'] = status_data['fm_base_url']
                settings.save()
                self.stdout.write(self.style.SUCCESS(
                    'Approved! This device is now paired with the Fleet Manager.',
                ))
                return
            if current_status == 'rejected':
                self.stderr.write(self.style.ERROR('The admin rejected this pairing request.'))
                return
            if current_status == 'expired':
                self.stderr.write(self.style.ERROR(
                    'This pairing request expired before an admin approved it — run this command again.',
                ))
                return
            time.sleep(POLL_INTERVAL_S)

        self.stderr.write(self.style.ERROR('Timed out waiting for approval.'))
