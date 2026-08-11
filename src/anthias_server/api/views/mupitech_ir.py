"""IR remote-control API views — MupiTech addition, not upstream Anthias.

Kept separate from api/views/v2.py (an upstream-owned file) so
periodic merges from upstream/master never touch this. Wired into
routing from api/urls/v2.py with two small additions.
"""

from drf_spectacular.utils import extend_schema
from rest_framework import serializers, status
from rest_framework.request import Request
from rest_framework.response import Response
from rest_framework.views import APIView

from anthias_server.lib.auth import authorized
from anthias_server.lib.mupitech_ir import get_ir_device, ir_available, send_ir_test


class IrStatusSerializer(serializers.Serializer):
    ir_available = serializers.BooleanField(read_only=True)
    ir_device = serializers.CharField(read_only=True, allow_null=True)


class IrTestSerializer(serializers.Serializer):
    protocol = serializers.CharField()
    scancode = serializers.CharField()


class IrTestResultSerializer(serializers.Serializer):
    success = serializers.BooleanField(read_only=True)
    error = serializers.CharField(read_only=True, required=False)


class IrStatusViewV2(APIView):
    serializer_class = IrStatusSerializer

    @extend_schema(
        summary='Get IR hardware availability',
        responses={200: IrStatusSerializer},
    )
    @authorized
    def get(self, request: Request) -> Response:
        return Response({
            'ir_available': ir_available(),
            'ir_device': get_ir_device(),
        })


class IrTestViewV2(APIView):
    serializer_class = IrTestSerializer

    @extend_schema(
        summary='Send a test IR scancode',
        request=IrTestSerializer,
        responses={200: IrTestResultSerializer, 400: IrTestResultSerializer},
    )
    @authorized
    def post(self, request: Request) -> Response:
        serializer = IrTestSerializer(data=request.data)
        serializer.is_valid(raise_exception=True)
        ok, message = send_ir_test(
            serializer.validated_data['protocol'],
            serializer.validated_data['scancode'],
        )
        if ok:
            return Response({'success': True})
        return Response(
            {'success': False, 'error': message},
            status=status.HTTP_400_BAD_REQUEST,
        )
