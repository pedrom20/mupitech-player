"""Remote self-update API view — MupiTech addition, not upstream Anthias.

Thin proxy to the Watchtower sidecar's own HTTP API — we already run
Watchtower fleet-wide (docker-compose-player-x86.yml), so this doesn't
implement any OTA logic itself, just triggers Watchtower's existing
update check/pull for this device's containers.

Kept as its own file (not merged into api/views/v2.py, an
upstream-owned file) so periodic merges from upstream/master never
touch this. Wired into routing from api/urls/v2.py with one addition.
"""

import os

import requests
from drf_spectacular.utils import extend_schema
from rest_framework import serializers, status
from rest_framework.request import Request
from rest_framework.response import Response
from rest_framework.views import APIView

from anthias_server.lib.auth import authorized

WATCHTOWER_URL = 'http://watchtower:8080/v1/update'
_REQUEST_TIMEOUT_S = 15


class TriggerUpdateResultSerializer(serializers.Serializer):
    success = serializers.BooleanField(read_only=True)
    error = serializers.CharField(read_only=True, required=False)


class TriggerUpdateViewV2(APIView):
    serializer_class = TriggerUpdateResultSerializer

    @extend_schema(
        summary='Trigger a Watchtower update check/pull for this device',
        responses={200: TriggerUpdateResultSerializer, 502: TriggerUpdateResultSerializer},
    )
    @authorized
    def post(self, request: Request) -> Response:
        token = os.environ.get('WATCHTOWER_TOKEN', '')
        try:
            resp = requests.post(
                WATCHTOWER_URL,
                headers={'Authorization': f'Bearer {token}'},
                timeout=_REQUEST_TIMEOUT_S,
            )
        except requests.RequestException as exc:
            return Response(
                {'success': False, 'error': f'Could not reach watchtower: {exc}'},
                status=status.HTTP_502_BAD_GATEWAY,
            )
        if resp.status_code >= 400:
            return Response(
                {'success': False, 'error': f'watchtower returned {resp.status_code}'},
                status=status.HTTP_502_BAD_GATEWAY,
            )
        return Response({'success': True})
