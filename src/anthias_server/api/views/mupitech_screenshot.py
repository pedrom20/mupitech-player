"""Screenshot capture API view — MupiTech addition, not upstream Anthias.

Request/reply over the same Redis mechanism `current_asset_id` already
uses (settings.ReplyCollector / ViewerPublisher / ReplySender) — the
compositor and Wayland socket only exist inside the anthias-viewer
container, so the server has to ask it to capture and wait for the
reply rather than doing this itself.

Kept as its own file (not merged into api/views/v2.py, an
upstream-owned file) so periodic merges from upstream/master never
touch this. Wired into routing from api/urls/v2.py with one addition.
"""

import base64
import uuid

from django.http import HttpResponse
from drf_spectacular.utils import extend_schema
from rest_framework import serializers, status
from rest_framework.request import Request
from rest_framework.response import Response
from rest_framework.views import APIView

from anthias_common.errors import ReplyTimeoutError
from anthias_server.lib.auth import authorized
from anthias_server.settings import ReplyCollector, ViewerPublisher

_REPLY_TIMEOUT_MS = 10_000


class ScreenshotErrorSerializer(serializers.Serializer):
    error = serializers.CharField(read_only=True)
    code = serializers.CharField(read_only=True, required=False)


class ScreenshotViewV2(APIView):
    serializer_class = ScreenshotErrorSerializer

    @extend_schema(
        summary='Capture a screenshot of what the viewer is currently displaying',
        responses={
            200: {'type': 'string', 'format': 'binary'},
            404: ScreenshotErrorSerializer,
            502: ScreenshotErrorSerializer,
        },
    )
    @authorized
    def get(self, request: Request) -> Response:
        correlation_id = uuid.uuid4().hex
        ViewerPublisher.get_instance().send_to_viewer(f'screenshot&{correlation_id}')

        try:
            result = ReplyCollector.get_instance().recv_json(correlation_id, _REPLY_TIMEOUT_MS)
        except ReplyTimeoutError:
            return Response(
                {'error': 'The viewer did not respond to the screenshot request in time.'},
                status=status.HTTP_502_BAD_GATEWAY,
            )

        if not result.get('success'):
            error = result.get('error', 'Screenshot failed.')
            # Distinguish "this board doesn't support it" (client should
            # show an unsupported state, not retry) from a genuine
            # transient failure — mirrors diagnostics.py's CEC error
            # taxonomy, which draws the same distinction for the same
            # reason.
            if 'not supported' in error.lower():
                return Response(
                    {'error': error, 'code': 'not_supported'},
                    status=status.HTTP_404_NOT_FOUND,
                )
            return Response({'error': error}, status=status.HTTP_502_BAD_GATEWAY)

        png_bytes = base64.b64decode(result['png_base64'])
        return HttpResponse(png_bytes, content_type='image/png')
