"""anthias_server.django_project URL Configuration

The `urlpatterns` list routes URLs to views. For more information please see:
    https://docs.djangoproject.com/en/5.2/topics/http/urls/
Examples:
Function views
    1. Add an import:  from my_app import views
    2. Add a URL to urlpatterns:  path('', views.home, name='home')
Class-based views
    1. Add an import:  from other_app.views import Home
    2. Add a URL to urlpatterns:  path('', Home.as_view(), name='home')
Including another URLconf
    1. Import the include() function: from django.urls import include, path
    2. Add a URL to urlpatterns:  path('blog/', include('blog.urls'))
"""

from typing import Any

from django.contrib import admin
from django.http import HttpRequest, HttpResponse
from django.urls import include, path, re_path
from drf_spectacular.views import SpectacularAPIView, SpectacularRedocView

from anthias_server.app import views_files
from anthias_server.app.mupitech_standby_views import standby_video
from anthias_server.lib.auth import authorized


class APIDocView(SpectacularRedocView):
    @authorized
    def get(
        self,
        request: HttpRequest,
        *args: Any,
        **kwargs: Any,
    ) -> HttpResponse:
        return super().get(request, *args, **kwargs)


urlpatterns = [
    path('admin', admin.site.urls),
    # django.views.i18n.set_language — POST target for the navbar's
    # language switcher (see _navbar.html). Writes the chosen language
    # into the session/cookie that LocaleMiddleware reads on every
    # subsequent request.
    path('i18n/', include('django.conf.urls.i18n')),
    # MupiTech addition — see mupitech_standby_views.py's module
    # docstring. Loaded by the viewer's show_standby() when a custom
    # standby video override exists on this device.
    path('standby-video/', standby_video, name='mupitech_standby_video'),
    path('api/', include('anthias_server.api.urls')),
    path('api/schema/', SpectacularAPIView.as_view(), name='schema'),
    path('api/docs/', APIDocView.as_view(url_name='schema'), name='redoc'),
    re_path(
        r'^anthias_assets/(?P<filename>.+)$',
        views_files.anthias_assets,
        name='anthias_assets',
    ),
    re_path(
        r'^static_with_mime/(?P<filename>.+)$',
        views_files.static_with_mime,
        name='static_with_mime',
    ),
    path('', include('anthias_server.app.urls')),
]

# @TODO: Write custom 403 and 404 pages.
