# QtTest-based unit tests for AnthiasViewer's QtMultimedia
# pipeline (issue #2904). Built and run via bin/test_webview_cpp.sh
# inside a container or on a host with Qt 6 (qt6-multimedia-dev +
# qt6-declarative-dev). Not wired into the main viewer Docker image;
# the production Dockerfile only builds AnthiasViewer.pro (no test
# sources or test runner are shipped to devices).

TEMPLATE = app
TARGET = AnthiasViewerTests

QT += core gui testlib widgets multimedia quick quickwidgets dbus
CONFIG += c++17 console testcase

# Re-use the production sources verbatim — tests instantiate
# VideoView directly and call the rotation / cookie-consent helpers.
# ``main.cpp`` is excluded because the test binary provides its own
# combined entry point (test_videoview.cpp's main() runs
# TestVideoView, TestRotation and TestCookieConsent — QTEST_MAIN only
# hosts one class). The qrc carries the QML scene (videoview.qml) the
# production widget loads. rotation.cpp / cookie_consent.cpp are
# deliberately QtCore-only (no View / QtWebEngine) so these tests link
# without the webengine modules.
SOURCES += \
    ../src/videoview.cpp \
    ../src/rotation.cpp \
    ../src/cookie_consent.cpp \
    test_videoview.cpp \
    test_rotation.cpp \
    test_cookie_consent.cpp

HEADERS += \
    ../src/videoview.h \
    ../src/rotation.h \
    ../src/cookie_consent.h

RESOURCES += ../src/videoview.qrc

INCLUDEPATH += ../src
