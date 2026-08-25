#include <QDebug>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFont>
#include <QLocale>
#include <QUrl>
#include <QStandardPaths>
#include <QStringList>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QWebEngineCertificateError>
#include <QWebEngineScript>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <QMovie>
#include <QBuffer>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QtGlobal>

#include "view.h"
#include "cookie_consent.h"
#include "rotation.h"

// Attaches the operator-configured per-asset request headers (#2215) to
// requests whose origin matches the asset's own origin (scheme + host +
// port; see originKey). Same-origin scoping is the security boundary: a
// bearer token meant for a private Grafana dashboard must never ride
// along on the requests that page makes to a third-party CDN, font host,
// or analytics domain (nor across a scheme downgrade or a different port
// on the same host). The main-frame navigation and same-origin
// XHR/subresources (which a dashboard needs to carry the token to render
// panels) get the headers; everything else is left untouched.
//
// interceptRequest may run on a Chromium worker thread (Qt 5 invokes the
// profile interceptor off the UI thread), while setHeaders/clear are
// called from the UI thread — so the shared state is guarded by a mutex.
// No Q_OBJECT: the class adds no signals/slots, so it needs no moc and
// can live entirely in this translation unit.

// Canonical origin key ``scheme://host:port`` (both lower-cased, with the
// scheme's default port filled in when the URL omits it) so scoping is by
// full origin, not just host. Matching on host alone would still attach an
// Authorization token across a scheme downgrade (https→http on the same
// host) or to a different service on another port; comparing the whole
// origin closes those leaks while remaining an exact same-origin match for
// the page's own XHR/subresources.
static QString originKey(const QUrl& url)
{
    int port = url.port();
    if (port == -1) {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("https")) {
            port = 443;
        } else if (scheme == QLatin1String("http")) {
            port = 80;
        }
    }
    return url.scheme().toLower() + QStringLiteral("://")
        + url.host().toLower() + QStringLiteral(":")
        + QString::number(port);
}

class RequestHeaderInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit RequestHeaderInterceptor(QObject* parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent)
    {
    }

    void setHeaders(
        const QString& origin,
        const QList<QPair<QByteArray, QByteArray>>& headers)
    {
        QMutexLocker locker(&m_mutex);
        m_origin = origin;
        m_headers = headers;
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_origin.clear();
        m_headers.clear();
    }

    void interceptRequest(QWebEngineUrlRequestInfo& info) override
    {
        QMutexLocker locker(&m_mutex);
        if (m_headers.isEmpty() || m_origin.isEmpty()) {
            return;
        }
        // Same-origin (scheme + host + port) only. Anything else — a
        // cross-origin redirect target, an https→http downgrade, or a
        // different port on the same host — is deliberately excluded so
        // the operator's headers can't leak off the asset's own origin.
        if (originKey(info.requestUrl()) != m_origin) {
            return;
        }
        for (const auto& header : m_headers) {
            info.setHttpHeader(header.first, header.second);
        }
    }

private:
    QMutex m_mutex;
    QString m_origin;
    QList<QPair<QByteArray, QByteArray>> m_headers;
};

namespace {
QString getServerHost()
{
    const QByteArray value = qgetenv("LISTEN");

    if (value.isEmpty()) {
        return QStringLiteral("anthias-server");
    }

    return QString::fromUtf8(value);
}

int getServerPort()
{
    bool ok = false;
    const int value = qgetenv("PORT").toInt(&ok);

    if (!ok || value <= 0 || value > 65535) {
        return 8080;
    }

    return value;
}

// QWebEnginePage that can be told, per navigation, to proceed past TLS
// certificate errors — the web-page counterpart of the image loader's
// ignoreSslErrors() path. Drives the per-asset ``skip_ssl_verify``
// flag (composed with the device-wide ``verify_ssl`` setting on the
// Python side) so a web-page asset served over HTTPS with a
// self-signed / untrusted-CA cert renders instead of showing Chromium's
// certificate-error page.
//
// The API is version-split: Qt5 exposes ``certificateError`` as a
// virtual returning true-to-proceed; Qt6 replaced it with a signal
// (connected in configureWebView()) and dropped the virtual. The flag
// lives on the page either way, so ``loadPage`` sets it on the target
// view's page before navigating and both paths read the same bit.
//
// No Q_OBJECT: the class declares no new signals/slots (the Qt6 signal
// it uses is inherited), so it needs no moc pass and the call sites
// reach it via static_cast rather than qobject_cast.
class AnthiasWebEnginePage : public QWebEnginePage
{
public:
    AnthiasWebEnginePage(QWebEngineProfile* profile, QObject* parent)
        : QWebEnginePage(profile, parent)
    {
    }

    void setSkipSslVerify(bool skip) { m_skipSslVerify = skip; }
    bool skipSslVerify() const { return m_skipSslVerify; }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt5: return true to ignore the error and continue loading.
    bool certificateError(const QWebEngineCertificateError& error) override
    {
        Q_UNUSED(error);
        if (m_skipSslVerify) {
            qDebug() << "loadPage: proceeding past certificate error "
                        "(skip_ssl_verify enabled)";
            return true;
        }
        return false;
    }
#endif

private:
    bool m_skipSslVerify = false;
};

// Build an Accept-Language header value from the system locale so
// multi-language URL assets serve content in the operator's configured
// language (issue #480). QLocale::system().uiLanguages() reads LANGUAGE
// (colon-separated) then LC_ALL/LANG on Linux and returns BCP47 tags
// like "nl-NL", "nl", "en-US", "en" in preference order — exactly what
// Accept-Language wants. Returns an empty string when the system is on
// the C/POSIX locale so we leave QtWebEngine's default in place rather
// than poisoning the header with "C".
QString detectAcceptLanguage()
{
    QStringList tags;
    const auto append = [&tags](const QString& tag) {
        if (tag.isEmpty()
            || tag == QLatin1String("C")
            || tag == QLatin1String("POSIX")) {
            return;
        }
        if (!tags.contains(tag, Qt::CaseInsensitive)) {
            tags.append(tag);
        }
    };

    for (const QString& lang : QLocale::system().uiLanguages()) {
        append(lang);
        // Qt 5.15 sometimes returns only the region-qualified form
        // (e.g. "nl-NL"); RFC 7231 servers will then miss a "nl"-only
        // catalog. Append the base language as a softer fallback so a
        // site that only ships generic Dutch still matches.
        const int dash = lang.indexOf(QLatin1Char('-'));
        if (dash > 0) {
            append(lang.left(dash));
        }
    }

    if (tags.isEmpty()) {
        return QString();
    }

    QString header = tags.first();
    for (int i = 1; i < tags.size(); ++i) {
        double q = 1.0 - i * 0.1;
        if (q < 0.1) {
            q = 0.1;
        }
        header += QStringLiteral(",") + tags.at(i)
                + QStringLiteral(";q=") + QString::number(q, 'f', 1);
    }
    return header;
}
}


namespace {
// Issue #2999 — page-load watchdog interval. Chromium has no overall
// navigation timeout, so a fetch whose packets stop arriving (WiFi
// dropout mid-load; the TCP socket stays ESTABLISHED with no FIN/RST)
// keeps the navigation pending indefinitely. 30 s is deliberately
// more generous than the asset-rotation cadence and the old
// VIDEO_TIMEOUT=20 the Python side used for stalled videos: a complex
// dashboard on a slow uplink may legitimately take 15–20 s, and a
// too-eager watchdog would cancel loads that were about to succeed.
// Operators can tune it via ANTHIAS_WEBPAGE_TIMEOUT_S on the viewer
// container. Clamped so a garbage value can't disable the watchdog
// entirely (the whole point is that there's always *some* timeout)
// or overflow the QTimer's millisecond int.
constexpr int kDefaultPageLoadTimeoutS = 30;
constexpr int kMinPageLoadTimeoutS = 5;
constexpr int kMaxPageLoadTimeoutS = 3600;

// Footer bar's own rounded-corner radius (view.h's footerBarLeftOffset()
// insets the bar's left edge by roughly this much when a logo is
// loaded, so exactly the rounded corner — not the logo's whole
// footprint — is what tucks behind the image).
constexpr int kFooterBarCornerRadiusPx = 14;

int pageLoadTimeoutMs()
{
    bool ok = false;
    int seconds =
        qEnvironmentVariableIntValue("ANTHIAS_WEBPAGE_TIMEOUT_S", &ok);
    if (!ok || seconds <= 0) {
        seconds = kDefaultPageLoadTimeoutS;
    }
    return qBound(kMinPageLoadTimeoutS, seconds, kMaxPageLoadTimeoutS)
        * 1000;
}
}

View::View(QWidget* parent) : QWidget(parent)
{
    imageRotation = rotation::linuxfbRotationOverride();
    if (imageRotation != 0) {
        qDebug() << "View: linuxfb screen rotation" << imageRotation
                 << "— rotating still images in paintEvent (the linuxfb "
                    "QPA ignores rotation=N)";
    }

    webView1 = new QWebEngineView(this);
    configureWebView(webView1);

    // Single-QWebEngineView rendering. ``webView2`` is aliased onto
    // ``webView1`` so the rest of the file's historical dual-buffer
    // logic still compiles and runs — every operation that would have
    // targeted the off-screen ``webView2`` now lands on the same
    // widget, and switchToNextWebView() takes its single-view fast path
    // (show the one view, no hide/show toggle or pointer swap). A
    // loadPage() call keeps the current page composited until
    // the next one has painted (QtWebEngine holds the last frame across
    // an in-place navigation), so transitions are seamless with at most
    // a brief black frame on a slow load, never a foreign page.
    //
    // This was previously gated behind ``ANTHIAS_LOW_RAM=1`` (1 GB
    // boards) purely to save the ~100 MB a second resident Chromium
    // renderer costs. It is now unconditional because the preloaded
    // second buffer was also *broken*: issue #2954 — on the single
    // fullscreen surface used by the Qt6 boards (Wayland/cage on
    // Pi 5 / x86, eglfs on pi4-64) a hidden or sibling-occluded
    // QWebEngineView is frame-callback-throttled by Chromium and never
    // composites, so ``loadFinished`` on the off-screen buffer did NOT
    // mean it had painted the new page. Revealing it in
    // switchToNextWebView() therefore showed its *stale* GPU surface —
    // the page it last displayed two rotations earlier — for ~1 frame
    // before the new page painted, flashing an unrelated asset on every
    // webpage→webpage transition. Live-reproduced and the fix
    // live-verified on a Pi 5 (grim burst capture, 2026-07-08): every
    // transition flashed the (n-2) page before, none after. The Qt5
    // 1 GB boards already ran this path, so only the 2 GB+ Qt6 boards
    // change behaviour — exactly where #2954 was reported. See
    // docs/board-enablement.md.
    webView2 = webView1;

    // Both webViews share the default profile, so the HTTP-cache setup
    // is per-process, not per-view. Use in-memory only — the default
    // on-disk cache caused URL assets to linger stale for days across
    // viewer restarts because QtWebEngine kept serving the old response
    // from /data/.cache/... (forum 983 — most-viewed bug). Memory-only
    // means the cache is dropped on every viewer restart; within a
    // single session QtWebEngine still honors the response's
    // cache-control headers. Clear once at startup to drop any disk
    // cache left behind by older builds so users upgrading from a
    // stale-cache version see fresh content on their next load.
    QWebEngineProfile* profile = QWebEngineProfile::defaultProfile();
    profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile->clearHttpCache();

    const QString acceptLanguage = detectAcceptLanguage();
    if (!acceptLanguage.isEmpty()) {
        profile->setHttpAcceptLanguage(acceptLanguage);
        qDebug() << "Accept-Language:" << acceptLanguage;
    }

    // Append the Anthias product token to QtWebEngine's default
    // User-Agent so sites still see a normal "Mozilla/5.0 ...
    // Chrome/... Safari/..." string (preserving compatibility) while
    // ops at the receiving end can spot that the request came from an
    // Anthias screen and which release. The token ("Anthias/<version>")
    // is composed once in Python by get_anthias_product_token() and
    // passed through ANTHIAS_UA_TOKEN by _build_webview_env() in
    // src/anthias_viewer/__init__.py, so the format lives in a single
    // place. Left untouched when the token is absent — the Python side
    // always sets it, so this only guards a standalone launch.
    const QByteArray uaToken = qgetenv("ANTHIAS_UA_TOKEN");
    if (!uaToken.isEmpty()) {
        const QString userAgent = profile->httpUserAgent()
            + QStringLiteral(" ") + QString::fromUtf8(uaToken);
        profile->setHttpUserAgent(userAgent);
        qDebug() << "User-Agent:" << userAgent;
    }

    // Per-asset custom request headers (#2215). Installed on the shared
    // profile once; the headers themselves are staged per asset by
    // setRequestHeaders and scoped to the target origin in startPageLoad.
    // ``setUrlRequestInterceptor`` on QWebEngineProfile is available
    // identically on Qt 5.13+ and Qt 6, so no version macro is needed.
    // Parent the interceptor to the *profile* (process-lifetime), not to
    // ``this``: the default profile can outlive the View, and if the
    // interceptor were destroyed first the profile would be left holding
    // a dangling pointer (use-after-free at teardown). ~View also detaches
    // it defensively.
    headerInterceptor = new RequestHeaderInterceptor(profile);
    profile->setUrlRequestInterceptor(headerInterceptor);

    currentWebView = webView1;
    nextWebView = webView2;
    nextWebViewReady = false;

    connect(webView1->page(), &QWebEnginePage::authenticationRequired,
            this, &View::handleAuthRequest);
    if (webView2 != webView1) {
        // Skip the duplicate connect when low-RAM aliased webView2
        // onto webView1 — Qt's connect would otherwise fire the auth
        // handler twice per challenge.
        connect(webView2->page(), &QWebEnginePage::authenticationRequired,
                this, &View::handleAuthRequest);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // QtMultimedia-backed video surface. Created hidden — only
    // made visible when ``playVideo`` fires. The QMediaPlayer +
    // QML VideoOutput live for the lifetime of this widget so
    // repeated plays don't pay pipeline-rebuild cost on every
    // asset. Qt 5 boards (Pi 1 / Pi 2 / Pi 3) skip this — video plays via
    // GstFbdevMediaPlayer painting straight to the framebuffer.
    videoView = new VideoView(this);
    videoView->setVisible(false);
    connect(videoView, &VideoView::videoEnded, this, &View::videoEnded);
#endif

    networkManager = new QNetworkAccessManager(this);
    movie = nullptr;
    isAnimatedImage = false;
    loadGenerationId = 0;
    reloadTimer = nullptr;
    pendingReloadIntervalS = 0;

    // Issue #2999 — see the member's comment in view.h. Connected
    // once here; startPageLoad re-arms it per attempt.
    pageLoadWatchdog = new QTimer(this);
    pageLoadWatchdog->setSingleShot(true);
    pageLoadWatchdog->setInterval(pageLoadTimeoutMs());
    connect(pageLoadWatchdog, &QTimer::timeout,
            this, &View::handlePageLoadTimeout);

    // Footer ticker bar (Fleet Manager's footer_messages module). Built
    // last so it stacks above webView1/webView2/videoView by Qt's
    // default child z-order, hidden until the first setFooter(true, …).
    // Rounded top corners + stopping short of the right edge (see
    // footerRightMargin()) so it reads as a floating pill rather than
    // a full-width strip flush with the screen. Its left edge is
    // additionally inset when a logo is loaded (footerBarLeftOffset())
    // — kFooterBarCornerRadiusPx here must match the value baked into
    // that computation, since the whole point is for exactly the
    // rounded corner (not the logo's whole footprint) to be what tucks
    // behind the image.
    footerBar = new QWidget(this);
    footerBar->setAttribute(Qt::WA_StyledBackground, true);
    footerBar->setStyleSheet(QStringLiteral(
        "background-color: rgba(0, 0, 0, 180);"
        "border-top-left-radius: %1px;"
        "border-top-right-radius: %1px;"
    ).arg(kFooterBarCornerRadiusPx));
    footerBar->setVisible(false);

    footerTextClip = new QWidget(footerBar);
    footerLabel = new QLabel(footerTextClip);
    footerLabel->setStyleSheet(QStringLiteral(
        "color: white; background: transparent;"
    ));
    footerLabel->setWordWrap(false);
    footerLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    // Optional logo (Fleet Manager's fleet-wide footer logo upload),
    // deliberately parented to the View itself rather than footerBar:
    // it needs to sit fully in front of, and taller than, the bar
    // (see updateFooterGeometry()), with only its own rounded corner
    // hidden underneath — not clipped to footerBar's own (now inset)
    // rect. Raised above footerBar on every geometry update.
    footerLogoLabel = new QLabel(this);
    footerLogoLabel->setStyleSheet(QStringLiteral("background: transparent;"));
    footerLogoLabel->setScaledContents(true);
    footerLogoLabel->setVisible(false);

    footerSlideAnimation = new QPropertyAnimation(footerBar, "geometry", this);
    footerSlideAnimation->setDuration(400);
    connect(footerSlideAnimation, &QPropertyAnimation::finished, this, [this]() {
        // Covers both directions with one persistent connection: a
        // show-finish (footerEnabled true, not cycling) is a no-op
        // here. A hide-finish — either a genuine disable (!footerEnabled)
        // or a cycling gap just starting (footerCycling) — marks the
        // bar (and, in lockstep, the logo) truly hidden so neither is
        // painted at all until the next show, instead of just sitting
        // off-screen.
        if (!footerEnabled || footerCycling) {
            footerBar->setVisible(false);
            refreshFooterLogoVisibility();
        }
    });

    footerScrollTimer = new QTimer(this);
    footerScrollTimer->setInterval(20);
    connect(footerScrollTimer, &QTimer::timeout, this, &View::tickFooterScroll);
    footerScrollX = 0;

    footerCycleTimer = new QTimer(this);
    footerCycleTimer->setSingleShot(true);
    connect(footerCycleTimer, &QTimer::timeout, this, [this]() {
        // The interval elapsed while deliberately hidden between
        // cycles — show again and restart the marquee from the right
        // edge, same as a fresh setFooter(true, …) would.
        footerCycling = false;
        if (!footerEnabled || footerLabel->text().isEmpty()) {
            return;
        }
        slideFooterIn();
        footerScrollX = footerBar->width();
        positionFooterLabel();
        footerScrollTimer->start();
    });

    updateFooterGeometry();
}

View::~View()
{
    if (pageLoadConnection) {
        QObject::disconnect(pageLoadConnection);
    }
    // Detach the request interceptor from the (process-lifetime) default
    // profile before we go away, so no request can be intercepted against
    // torn-down View state. The interceptor object itself is owned by the
    // profile (see ctor), so we only clear the profile's pointer here.
    QWebEngineProfile::defaultProfile()->setUrlRequestInterceptor(nullptr);
    stopReloadTimer();
    stopAnimation();
}

void View::configureWebView(QWebEngineView* view)
{
    // Install our certificate-error-aware page (see AnthiasWebEnginePage)
    // in place of the default one so ``skip_ssl_verify`` web-page assets
    // can proceed past a self-signed cert. Parented to the view, which
    // takes ownership on setPage(). Done first so the settings/background
    // tweaks below apply to the page that will actually be used.
    auto* page =
        new AnthiasWebEnginePage(QWebEngineProfile::defaultProfile(), view);
    view->setPage(page);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6: certificateError is a signal. Accept or reject based on the
    // per-navigation flag loadPage() set on this page. Capturing the
    // typed page pointer avoids a qobject_cast in the handler.
    connect(page, &QWebEnginePage::certificateError, this,
        [page](QWebEngineCertificateError error) {
            if (page->skipSslVerify()) {
                qDebug() << "loadPage: accepting certificate error "
                            "(skip_ssl_verify enabled)";
                error.acceptCertificate();
            } else {
                error.rejectCertificate();
            }
        });
#endif

    view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    view->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, false);
    // Match the widget's black backdrop so dark-themed URL assets don't
    // flash white between the page-load start and the first paint.
    page->setBackgroundColor(Qt::black);

    // linuxfb webpage rotation: the QPA plugin doesn't rotate the
    // QtWebEngine surface (see linuxfbRotationOverride), so images/video
    // rotate elsewhere (paintEvent / GStreamer videoflip) but web pages
    // would stay upright. Inject a stylesheet that turns the page root to
    // match the physically-rotated screen so every asset type rotates on
    // linuxfb. No-op on eglfs / wayland (imageRotation is 0 there — those
    // rotate the whole compositor output already).
    //
    // Injected imperatively from loadFinished rather than as a declarative
    // QWebEngineScript: on this Qt 5.15 (qt5pi) build a page-collection
    // QWebEngineScript in ApplicationWorld silently never fires, whereas
    // runJavaScript() executes reliably. ApplicationWorld (an isolated
    // world) is used so the injection bypasses any page Content-Security-
    // Policy that would otherwise block a stylesheet. The handler is
    // persistent (not the one-shot swap handler in startPageLoad) so the
    // rotation re-applies after every auto-refresh reload; the in-page
    // MutationObserver (see webpageRotationScript) keeps it asserted across
    // SPA DOM rewrites within a single document.
    //
    // Injected regardless of loadFinished's ``ok`` flag: a failed load
    // (DNS failure, refused connection, captive portal) still leaves a
    // QtWebEngine error-page document on screen, which must rotate too so
    // a physically-turned panel never shows sideways text. runJavaScript
    // on a cancelled/empty document is a harmless no-op.
    if (imageRotation != 0) {
        const QString script = rotation::webpageRotationScript(imageRotation);
        connect(page, &QWebEnginePage::loadFinished, this,
            [page, script](bool) {
                page->runJavaScript(
                    script, QWebEngineScript::ApplicationWorld);
            });
    }

    // Cookie/GDPR consent banner auto-dismiss (MupiTech addition, not
    // upstream Anthias): unattended signage has no one to click
    // "Accept" — see cookie_consent.h for why this is best-effort
    // rather than universal. Same injection shape as the rotation
    // script above (imperative runJavaScript on loadFinished rather
    // than a declarative QWebEngineScript — that silently never fires
    // on the Qt 5.15/qt5pi build; isolated ApplicationWorld so it
    // bypasses the page's CSP); unconditional (every web-page asset,
    // not gated on rotation) since the goal here applies regardless of
    // screen orientation.
    {
        const QString script = cookieConsent::dismissScript();
        connect(page, &QWebEnginePage::loadFinished, this,
            [page, script](bool) {
                page->runJavaScript(
                    script, QWebEngineScript::ApplicationWorld);
            });
    }

    view->setVisible(false);
}

void View::stopAnimation()
{
    if (movie) {
        movie->stop();
        delete movie;
        movie = nullptr;
    }
    isAnimatedImage = false;
}

void View::loadPage(const QString &uri, bool skipSslVerify)
{
    qDebug() << "Type: Webpage";

    const quint64 requestId = ++loadGenerationId;

    // The page load goes into nextWebView (startPageLoad), so set the
    // per-navigation certificate policy on that view's page now. It
    // persists on the page across watchdog retries of the same URI.
    // static_cast is safe: configureWebView() installs an
    // AnthiasWebEnginePage on every view.
    static_cast<AnthiasWebEnginePage*>(nextWebView->page())
        ->setSkipSslVerify(skipSslVerify);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Drop back to the web/image surface in case the previous asset
    // was a video. Stops the QMediaPlayer (frees its decoder
    // pipeline + audio device) and hides the graphics view so the
    // QWebEngineView paints are visible.
    hideVideoSurface();
#endif
    currentImage = QImage();
    stopAnimation();
    // Drop any per-asset reload timer left over from the previous
    // webpage AND the prior asset's pending interval — the viewer
    // calls setReloadInterval right after this with the new asset's
    // value, so any old pending value would be wrong if it leaked
    // into the swap that's about to happen.
    stopReloadTimer();
    pendingReloadIntervalS = 0;
    nextWebViewReady = false;

    startPageLoad(uri, requestId);

    qDebug() << "Loading web page:" << uri;
}

namespace {
// Defensive re-validation of the D-Bus header payload. The server side
// (validate_asset_headers) is the primary gate and the viewer only
// forwards sanitised headers, but the ``setRequestHeaders`` slot is a
// trust-no-one boundary (anything on the session bus can call it), so we
// re-check here rather than put unsafe bytes on the wire — same posture
// as kMaxReloadIntervalS. Mirrors the server's limits.
constexpr int kMaxRequestHeaders = 20;
constexpr int kMaxHeaderNameLen = 256;
constexpr int kMaxHeaderValueLen = 4096;

// RFC 7230 field-name is 1*tchar.
bool isValidHeaderName(const QByteArray& name)
{
    if (name.isEmpty() || name.size() > kMaxHeaderNameLen) {
        return false;
    }
    for (const char c : name) {
        const bool tchar =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || QByteArray("!#$%&'*+-.^_`|~").contains(c);
        if (!tchar) {
            return false;
        }
    }
    return true;
}

// Reject CR / LF / NUL (header/request splitting) and oversized values.
bool isValidHeaderValue(const QByteArray& value)
{
    if (value.size() > kMaxHeaderValueLen) {
        return false;
    }
    return !value.contains('\r') && !value.contains('\n')
        && !value.contains('\0');
}
}  // namespace

void View::setRequestHeaders(const QString &headersJson)
{
    // Parse the JSON object the viewer sent into a name/value list. We
    // don't apply it to the interceptor here — startPageLoad does that
    // once it knows the target origin — because the header set is only
    // meaningful together with the URL it belongs to. Store it so the
    // very next loadPage picks it up. An empty / malformed payload
    // leaves ``pendingHeaders`` empty, which clears any prior headers on
    // the next load. Entries that fail validation are dropped, not
    // applied.
    QList<QPair<QByteArray, QByteArray>> parsed;
    const QJsonDocument doc = QJsonDocument::fromJson(headersJson.toUtf8());
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (parsed.size() >= kMaxRequestHeaders) {
                break;
            }
            if (!it.value().isString()) {
                continue;
            }
            const QByteArray name = it.key().toUtf8();
            const QByteArray value = it.value().toString().toUtf8();
            if (!isValidHeaderName(name) || !isValidHeaderValue(value)) {
                continue;
            }
            // Brace-init the QPair rather than qMakePair(), which Qt 6
            // deprecates — keeps the same source building warning-free
            // on both the Qt 5 and Qt 6 toolchains.
            parsed.append({name, value});
        }
    }
    pendingHeaders = parsed;
}

void View::startPageLoad(const QString &uri, quint64 requestId)
{
    // Drop any prior loadFinished handler before stop() — a synchronous
    // loadFinished(false) emission from the previous in-flight load
    // would otherwise reach the (still-attached) handler and run with
    // ok=false, before the new load() takes effect. With the lambda
    // detached, stop() can fire whatever it likes harmlessly.
    if (pageLoadConnection) {
        QObject::disconnect(pageLoadConnection);
        pageLoadConnection = QMetaObject::Connection{};
    }

    nextWebView->stop();

    pendingPageUri = uri;

    // Scope the staged headers (#2215) to this URL's origin and hand them
    // to the interceptor before the load fires. An empty ``pendingHeaders``
    // clears any headers left from a previous asset, so a header-less
    // page never inherits the prior page's Authorization.
    if (headerInterceptor) {
        if (pendingHeaders.isEmpty()) {
            headerInterceptor->clear();
        } else {
            headerInterceptor->setHeaders(
                originKey(QUrl(uri)), pendingHeaders);
        }
    }

    pageLoadConnection = connect(
        nextWebView->page(),
        &QWebEnginePage::loadFinished,
        this,
        [this, requestId](bool ok) {
            // One-shot: detach unconditionally on first fire so neither
            // a stale completion (superseded by a later load) nor a
            // re-emission (e.g., JS-driven redirect after the swap)
            // can run this lambda again.
            QObject::disconnect(pageLoadConnection);
            pageLoadConnection = QMetaObject::Connection{};

            if (requestId != loadGenerationId) {
                qDebug() << "Ignoring stale page load result";
                return;
            }

            if (ok) {
                pageLoadWatchdog->stop();
                pendingPageUri.clear();
                qDebug() << "Web page loaded successfully";
                nextWebViewReady = true;
                switchToNextWebView();
            } else {
                // Deliberately leave the watchdog running: when it
                // fires, handlePageLoadTimeout re-issues this URI. A
                // load that failed fast (DNS / connection refused
                // while the network is down) thereby gets a paced
                // retry, which is the only retry a single-webpage
                // playlist will ever see — view_webpage() skips the
                // loadPage D-Bus call when the URL hasn't changed
                // (issue #2999).
                qDebug() << "Web page failed to load";
                nextWebViewReady = false;
            }
        }
    );

    // (Re)arm the watchdog for this attempt — single-shot, so each
    // retry gets a full interval.
    pageLoadWatchdog->start();

    nextWebView->load(QUrl(uri));
}

void View::handlePageLoadTimeout()
{
    // Defensive: every path that cancels the pending navigation
    // (loadImage / playVideo / a successful load) stops the watchdog
    // and clears the URI, but if a stray timeout slips through an
    // empty URI means there is nothing to recover.
    if (pendingPageUri.isEmpty()) {
        return;
    }

    qWarning() << "Webpage load did not finish within"
               << pageLoadWatchdog->interval() / 1000
               << "seconds — cancelling and retrying:" << pendingPageUri;

    // startPageLoad stop()s the wedged navigation (cancelling its
    // pending network I/O so stuck sockets don't pile up in
    // Chromium's connection pool), re-issues the URI on a fresh
    // request, and re-arms the watchdog. The generation ID is
    // unchanged — this is still the same asset; any later asset
    // bumps the generation and supersedes the retry's handler.
    startPageLoad(pendingPageUri, loadGenerationId);
}

void View::loadImage(const QString &preUri, bool skipSslVerify)
{
    qDebug() << "Type: Image";
    const quint64 requestId = ++loadGenerationId;

    // ``view_image('null')`` in src/anthias_viewer/__init__.py:495
    // is called AFTER ``media_player.play()`` to sweep any
    // lingering web/image background out of the way of the new
    // video — it is NOT a request to take down the freshly-
    // started video surface. Skipping ``hideVideoSurface`` for the
    // sentinel ``'null'`` URI keeps the just-started video alive;
    // calling stop() here interrupted the QMediaPlayer mid-
    // decoder init for Pi 5's Hantro G2 on 4K60 HEVC (~66 ms after
    // the first PLAYING event) and left position stuck at 0 for
    // the full 60 s asset_loop window. For a real image URI the
    // prior video must still be torn down, so the call is
    // preserved there.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (preUri != QLatin1String("null")) {
        hideVideoSurface();
    }
#endif

    // Cancel any pending page load so we don't keep streaming a web
    // page in the background after the user has switched to image
    // playback. Without this the QWebEngineView would continue fetching
    // and rendering until completion, even though the result would be
    // ignored by the (now stale) loadFinished handler.
    if (pageLoadConnection) {
        QObject::disconnect(pageLoadConnection);
        pageLoadConnection = QMetaObject::Connection{};
    }
    // ...and its watchdog, so a stale timeout doesn't retry a webpage
    // the playlist has rotated away from (issue #2999).
    pageLoadWatchdog->stop();
    pendingPageUri.clear();
    // Webpage auto-refresh only applies while a webpage is on screen;
    // killing the timer (and clearing the pending interval) here keeps
    // a stale reload from firing into the (now hidden) QWebEngineView
    // after the viewer rotates to an image.
    stopReloadTimer();
    pendingReloadIntervalS = 0;
    webView1->stop();
    webView2->stop();

    webView1->setVisible(false);
    webView2->setVisible(false);

    stopAnimation();

    QFileInfo fileInfo = QFileInfo(preUri);
    QString src;

    if (fileInfo.isFile())
    {
        qDebug() << "Location: Local File";
        qDebug() << "File path:" << fileInfo.absoluteFilePath();

        QUrl url;
        url.setScheme("http");
        url.setHost(getServerHost());
        url.setPort(getServerPort());
        url.setPath("/anthias_assets/" + fileInfo.fileName());

        src = url.toString();
        qDebug() << "Generated URL:" << src;
    }
    else if (preUri == "null")
    {
        qDebug() << "Black page";
        currentImage = QImage();
        update();
        return;
    }
    else
    {
        qDebug() << "Location: Remote URL";
        src = preUri;
    }

    qDebug() << "Loading image from:" << src;

    QNetworkRequest request(src);
    QNetworkReply* reply = networkManager->get(request);

    if (skipSslVerify) {
        // Per-asset opt-in (Asset.skip_ssl_verify) or the device-wide
        // verify_ssl setting is off — trust self-signed / untrusted-CA
        // hosts for this image. ignoreSslErrors() must be armed on the
        // reply before the handshake completes, so connect it here
        // rather than waiting for the finished handler. Without this a
        // self-signed HTTPS image fails with SslHandshakeFailedError
        // and renders blank (forum "web content doesn't display").
        // Stable API on both Qt5 and Qt6.
        connect(reply, &QNetworkReply::sslErrors, reply,
            [reply](const QList<QSslError>&) {
                reply->ignoreSslErrors();
            });
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        reply->deleteLater();

        if (requestId != loadGenerationId) {
            qDebug() << "Ignoring stale image response";
            return;
        }

        if (reply->error() == QNetworkReply::NoError) {
            QByteArray data = reply->readAll();
            qDebug() << "Received image data size:" << data.size();

            if (!tryLoadAsAnimatedGif(data)) {
                loadAsStaticImage(data);
            }
        } else {
            qDebug() << "Network error:" << reply->errorString();
        }
    });

    connect(reply, &QNetworkReply::errorOccurred, this,
        [this, reply, requestId](QNetworkReply::NetworkError error) {
            if (requestId != loadGenerationId) {
                return;
            }
            qDebug() << "Network error occurred:" << error;
            qDebug() << "Error string:" << reply->errorString();
        });
}

bool View::tryLoadAsAnimatedGif(const QByteArray& data)
{
    QBuffer testBuffer;
    testBuffer.setData(data);
    testBuffer.open(QIODevice::ReadOnly);

    QImageReader reader(&testBuffer);
    if (!reader.supportsAnimation() && reader.imageCount() <= 1) {
        return false;
    }

    QMovie* nextMovie = new QMovie(this);
    QBuffer* buffer = new QBuffer(nextMovie);
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    nextMovie->setDevice(buffer);

    if (!nextMovie->isValid()) {
        qDebug() << "Failed to load animated image, falling back to static image";
        delete nextMovie;
        loadAsStaticImage(data);
        return true;
    }

    qDebug() << "Animated image loaded successfully. Frame count:" << nextMovie->frameCount();
    movie = nextMovie;
    setupAnimation();
    return true;
}

void View::loadAsStaticImage(const QByteArray& data)
{
    // Decode through a QImageReader with auto-transform enabled so the
    // EXIF Orientation tag is honoured. QImage::loadFromData() ignores
    // orientation, which left camera/phone photos taken in portrait
    // (stored landscape + Orientation 6/8) drawn sideways on screen
    // (issue 3232). setAutoTransform applies the rotation/flip during
    // decode for every format that carries the tag (JPEG, HEIC, ...).
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);

    QImage newImage = reader.read();
    if (!newImage.isNull()) {
        qDebug() << "Successfully loaded static image. Size:" << newImage.size();
        nextImage = newImage;
        webView1->setVisible(false);
        webView2->setVisible(false);
        currentImage = nextImage;
        update();
    } else {
        qDebug() << "Failed to load image from data:" << reader.errorString();
    }
}

void View::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), Qt::black);

    if (currentImage.isNull()) {
        return;
    }

    if (imageRotation == 0) {
        QSize scaledSize = currentImage.size();
        scaledSize.scale(size(), Qt::KeepAspectRatio);
        painter.drawImage(
            QRect(
                (width() - scaledSize.width()) / 2,
                (height() - scaledSize.height()) / 2,
                scaledSize.width(),
                scaledSize.height()),
            currentImage);
        return;
    }

    // linuxfb-only manual rotation (see linuxfbRotationOverride): rotate
    // about the widget centre. A 90/270 turn swaps the drawable box, so
    // the image is fit into the widget's transposed dimensions and
    // centred — a landscape image on a portrait-turned screen is
    // pillar-boxed, matching the GStreamer videoflip path.
    const QSize box =
        (imageRotation % 180 == 0) ? size() : QSize(height(), width());
    QSize scaledSize = currentImage.size();
    scaledSize.scale(box, Qt::KeepAspectRatio);
    painter.translate(width() / 2.0, height() / 2.0);
    painter.rotate(imageRotation);
    painter.drawImage(
        QRect(
            -scaledSize.width() / 2,
            -scaledSize.height() / 2,
            scaledSize.width(),
            scaledSize.height()),
        currentImage);
}

void View::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    webView1->setGeometry(rect());
    webView2->setGeometry(rect());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (videoView) {
        videoView->setGeometry(rect());
    }
#endif
    updateFooterGeometry();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void View::playVideo(const QString &uri, const QVariantMap &options)
{
    qDebug() << "Type: Video";
    ++loadGenerationId;

    // Cancel any pending QWebEngineView load so a slow page-load
    // completion doesn't race the video onto the screen mid-play.
    // Mirrors the loadImage path's handling.
    if (pageLoadConnection) {
        QObject::disconnect(pageLoadConnection);
        pageLoadConnection = QMetaObject::Connection{};
    }
    pageLoadWatchdog->stop();
    pendingPageUri.clear();
    stopReloadTimer();
    pendingReloadIntervalS = 0;
    webView1->stop();
    webView2->stop();
    webView1->setVisible(false);
    webView2->setVisible(false);
    // Blank the image canvas so an old still doesn't flash through
    // before the first mpv frame paints.
    stopAnimation();
    currentImage = QImage();
    update();

    if (!videoView) {
        qWarning() << "View::playVideo: VideoView not constructed";
        return;
    }
    videoView->setGeometry(rect());
    videoView->raise();
    videoView->setVisible(true);
    videoView->play(uri, options);
    // videoView->raise() above would otherwise put the video surface on
    // top of the footer bar (and its logo) too — re-assert the footer's
    // stacking order so a video asset doesn't briefly (or permanently)
    // cover it.
    footerBar->raise();
    footerLogoLabel->raise();
}

void View::stopVideo()
{
    if (videoView) {
        videoView->stop();
    }
}

void View::hideVideoSurface()
{
    if (!videoView || !videoView->isVisible()) {
        return;
    }
    videoView->stop();
    videoView->setVisible(false);
}
#endif

void View::handleAuthRequest(const QUrl& requestUrl, QAuthenticator*)
{
    qDebug() << "Authentication required for:" << requestUrl;

    const QUrl accessDeniedUrl = QUrl::fromLocalFile(
        QStandardPaths::locate(QStandardPaths::AppDataLocation, "res/access_denied.html")
    );
    QWebEnginePage* page = qobject_cast<QWebEnginePage*>(sender());
    if (page) {
        page->load(accessDeniedUrl);
    } else {
        currentWebView->load(accessDeniedUrl);
    }
}

void View::setupAnimation()
{
    isAnimatedImage = true;
    webView1->setVisible(false);
    webView2->setVisible(false);

    connect(movie, &QMovie::frameChanged, this, [this](int) {
        if (!movie || !isAnimatedImage) {
            return;
        }

        const QImage newFrame = movie->currentImage();
        if (!newFrame.isNull()) {
            currentImage = newFrame;
            update();
        }
    });

    movie->start();
    movie->jumpToFrame(0);
    currentImage = movie->currentImage();
    update();
}

// Mirrors the v2 serializer's REFRESH_INTERVAL_S_MAX. Caps a hostile
// or buggy D-Bus caller — the multiplication ``seconds * 1000`` later
// in armReloadTimer would otherwise overflow ``int`` for values north
// of ~2.1M and produce a wraparound cadence (small or negative). The
// server side validates the range on write but the D-Bus contract is
// trust-no-one (anything on the session bus could call this).
static constexpr int kMaxReloadIntervalS = 86400;

void View::setReloadInterval(int seconds)
{
    // Per-asset auto-refresh. The viewer calls this right after each
    // loadPage() with the asset's metadata.refresh_interval_s value
    // (0 when the field is absent or explicitly disabled). Stash the
    // requested cadence and only arm the QTimer once the new page is
    // actually visible — a load is in flight when ``pageLoadConnection``
    // is set, in which case currentWebView is still the *previous*
    // page and arming now would race the swap and reload the wrong
    // page. When no load is pending — the common
    // URL-unchanged-since-last-tick case where the viewer skips
    // loadPage() — arm immediately. ``seconds`` is clamped to
    // [0, kMaxReloadIntervalS] to defend against int-overflow on the
    // millisecond multiplication done at arm time.
    if (seconds <= 0) {
        pendingReloadIntervalS = 0;
    } else if (seconds > kMaxReloadIntervalS) {
        pendingReloadIntervalS = kMaxReloadIntervalS;
    } else {
        pendingReloadIntervalS = seconds;
    }
    stopReloadTimer();

    if (!pageLoadConnection) {
        armReloadTimer();
    }
}

void View::armReloadTimer()
{
    // Idempotent: callers may invoke this multiple times around a
    // single load (setReloadInterval, then switchToNextWebView), and
    // we always want a single live timer attached to the now-visible
    // currentWebView.
    stopReloadTimer();

    if (pendingReloadIntervalS <= 0 || !currentWebView) {
        return;
    }

    reloadTimer = new QTimer(this);
    reloadTimer->setInterval(pendingReloadIntervalS * 1000);
    // Don't qDebug() the reload itself — short intervals (5–10s) would
    // flood journald to the point of unusability for very little
    // diagnostic value. A failure to load shows up via the existing
    // pageLoadConnection / loadFinished path; reload() succeeding is
    // the boring case.
    connect(reloadTimer, &QTimer::timeout, this, [this]() {
        if (currentWebView) {
            currentWebView->reload();
        }
    });
    reloadTimer->start();
}

void View::stopReloadTimer()
{
    if (reloadTimer) {
        reloadTimer->stop();
        reloadTimer->deleteLater();
        reloadTimer = nullptr;
    }
}

void View::switchToNextWebView()
{
    if (!nextWebViewReady) {
        qDebug() << "Next web view not ready yet, keeping current one visible";
        return;
    }

    nextWebViewReady = false;

    if (currentWebView == nextWebView) {
        // Single-view mode (the only mode today — see the constructor).
        // There is no off-screen buffer to reveal: the freshly-loaded
        // page is already in the one view. Just make sure it is shown —
        // it may have been hidden by a preceding image/video asset
        // (loadImage()/playVideo() hide the web views) — and skip the
        // dual-buffer hide/show + pointer swap, which on a single
        // aliased widget would only toggle its visibility for no reason
        // (and risk an avoidable flicker).
        currentWebView->setVisible(true);
        currentWebView->clearFocus();
        armReloadTimer();
        qDebug() << "Showing loaded web page (single-view)";
        return;
    }

    qDebug() << "Switching to next web view";

    currentWebView->setVisible(false);
    nextWebView->setVisible(true);
    nextWebView->clearFocus();

    QWebEngineView* temp = currentWebView;
    currentWebView = nextWebView;
    nextWebView = temp;

    qDebug() << "Successfully switched to next web view";

    // The new page is now visible — safe to arm the auto-refresh
    // timer against it. setReloadInterval may have been called while
    // the load was in flight; it stashed the cadence in
    // pendingReloadIntervalS and deferred to here. No-op if the asset
    // didn't request auto-refresh.
    armReloadTimer();
}

int View::footerBarHeight() const
{
    // Scales with the screen instead of a fixed pixel value — this
    // widget runs on everything from a small Pi HAT display to a 4K
    // TV. Clamped so it stays readable on a tiny screen and doesn't
    // dominate a huge one.
    return qBound(28, height() / 12, 72);
}

int View::footerRightMargin() const
{
    // Same screen-relative-with-clamp shape as footerBarHeight() — the
    // bar stops this many px short of the right edge instead of
    // running flush with it.
    return qBound(12, width() / 40, 40);
}

int View::footerBarLeftOffset() const
{
    if (!footerLogoHasPixmap) {
        return 0;
    }
    // Same 1.4x sizing as the logo itself in updateFooterGeometry() —
    // duplicated here (rather than a shared constant) because it's
    // only ever used together with footerBarHeight() at each call
    // site anyway.
    const int logoSize = qRound(footerBarHeight() * 1.4);
    return qMax(0, logoSize - kFooterBarCornerRadiusPx);
}

int View::footerTextLeftMargin() const
{
    if (!footerLogoHasPixmap) {
        return 0;
    }
    // No real physical DPI available here to target an exact "~1.5cm"
    // (how the gap was actually specified) — scales the same
    // screen-relative-with-clamp way as the other footer margins,
    // landing in roughly that range on typical signage displays.
    return qBound(24, width() / 48, 60);
}

void View::refreshFooterLabelMetrics()
{
    const int barHeight = footerBarHeight();
    QFont font = footerLabel->font();
    font.setPixelSize(qMax(12, barHeight / 2));
    font.setBold(true);
    footerLabel->setFont(font);
    // adjustSize() first so sizeHint() reflects the new font/text
    // before the label is resized — only its width comes from that;
    // the height is pinned to the bar so AlignVCenter centres reliably
    // regardless of scroll position (footerLabel is only ever moved
    // along x, never y).
    footerLabel->adjustSize();
    footerLabel->resize(footerLabel->sizeHint().width(), barHeight);
}

void View::updateFooterGeometry()
{
    const int barHeight = footerBarHeight();
    const int barLeftOffset = footerBarLeftOffset();
    const int barWidth = qMax(0, width() - footerRightMargin() - barLeftOffset);
    const int y = footerEnabled ? (height() - barHeight) : height();
    footerBar->setGeometry(barLeftOffset, y, barWidth, barHeight);
    footerBar->raise();
    refreshFooterLabelMetrics();

    // See footerTextClip's member comment in view.h — clips the
    // marquee text footerTextLeftMargin() px short of footerBar's own
    // (bar-local) left edge instead of flush with it.
    const int textMargin = footerTextLeftMargin();
    footerTextClip->setGeometry(
        textMargin, 0, qMax(0, barWidth - textMargin), barHeight
    );

    // Logo sized a bit taller than the bar so it overlaps the bar's
    // top edge and reads as sitting in front of it — see the
    // constructor's comment on footerLogoLabel's parent. Always at
    // x=0 (never inset like the bar itself): it's the fixed anchor
    // the bar's own left edge is positioned relative to, not the
    // other way around. Moves off-screen together with the bar since
    // its y is derived from the same ``y`` above.
    const int logoSize = qRound(barHeight * 1.4);
    const int logoY = y + (barHeight - logoSize) / 2;
    footerLogoLabel->setGeometry(0, logoY, logoSize, logoSize);
    footerLogoLabel->raise();
}

void View::slideFooterIn()
{
    // The bar's left inset and the logo's own geometry are only ever
    // (re)computed here and in updateFooterGeometry() — never animated
    // in step with footerBar's slide — so this must run against the
    // *visible* resting position now, while footerEnabled is already
    // true (callers set it before calling this). Without this call
    // they'd still be sitting wherever they were left by the
    // *previous* updateFooterGeometry() call — the constructor's, bar
    // hidden and no logo loaded yet — and never move, since a fixed-
    // resolution screen has no further resize event to ever recompute
    // them.
    updateFooterGeometry();

    const int barHeight = footerBarHeight();
    const int barLeftOffset = footerBarLeftOffset();
    const int barWidth = qMax(0, width() - footerRightMargin() - barLeftOffset);
    const QRect hiddenGeometry(barLeftOffset, height(), barWidth, barHeight);
    const QRect visibleGeometry(barLeftOffset, height() - barHeight, barWidth, barHeight);

    footerSlideAnimation->stop();
    footerBar->setGeometry(hiddenGeometry);
    footerBar->raise();
    footerLogoLabel->raise();
    footerBar->setVisible(true);
    refreshFooterLogoVisibility();
    footerSlideAnimation->setEasingCurve(QEasingCurve::OutCubic);
    footerSlideAnimation->setStartValue(hiddenGeometry);
    footerSlideAnimation->setEndValue(visibleGeometry);
    footerSlideAnimation->start();
}

void View::slideFooterOut()
{
    const int barHeight = footerBarHeight();
    const int barLeftOffset = footerBarLeftOffset();
    const int barWidth = qMax(0, width() - footerRightMargin() - barLeftOffset);
    const QRect hiddenGeometry(barLeftOffset, height(), barWidth, barHeight);

    footerSlideAnimation->stop();
    footerSlideAnimation->setEasingCurve(QEasingCurve::InCubic);
    footerSlideAnimation->setStartValue(footerBar->geometry());
    footerSlideAnimation->setEndValue(hiddenGeometry);
    // footerBar->setVisible(false) happens in the ``finished`` handler
    // connected once in the constructor, not here — see its comment
    // for why a per-call connection would be wrong.
    footerSlideAnimation->start();
}

void View::positionFooterLabel()
{
    footerLabel->move(footerScrollX - footerTextLeftMargin(), 0);
}

void View::tickFooterScroll()
{
    // Classic single-pass marquee: slide left by a couple of px per
    // tick, and once the label has scrolled off (stopping
    // footerTextLeftMargin() short of the bar's true left edge, not
    // all the way to it — see that method), either restart it just
    // past the right edge of the bar (default,
    // footerCycleIntervalMinutes == 0 — continuous loop, unchanged from
    // the original behavior) or, when a cycle interval is configured,
    // hide the bar instead and let footerCycleTimer bring it back after
    // that many minutes — see the constructor's footerCycleTimer
    // connection for the reappearance half of this state machine.
    footerScrollX -= 2;
    if (footerScrollX + footerLabel->width() < footerTextLeftMargin()) {
        if (footerCycleIntervalMinutes > 0) {
            footerScrollTimer->stop();
            footerCycling = true;
            slideFooterOut();
            footerCycleTimer->start(footerCycleIntervalMinutes * 60000);
            return;
        }
        footerScrollX = footerBar->width();
    }
    positionFooterLabel();
}

void View::applyFooterLogo(const QString &url)
{
    if (url.isEmpty()) {
        footerLogoUrl.clear();
        footerLogoLabel->setPixmap(QPixmap());
        footerLogoHasPixmap = false;
        refreshFooterLogoVisibility();
        updateFooterGeometry();
        return;
    }
    if (url == footerLogoUrl) {
        // Already applied (or already in flight for this exact URL) —
        // every reload/settings poll re-sends the current footer
        // state wholesale, not just on an actual change, so this is
        // the common case rather than the exception.
        return;
    }
    footerLogoUrl = url;

    QNetworkRequest request{QUrl(url)};
    QNetworkReply* reply = networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        // A newer applyFooterLogo() call superseded this one while the
        // request was in flight (the logo was changed/removed again
        // before this fetch completed) — drop the now-stale result
        // instead of flashing an outdated image back in.
        if (url != footerLogoUrl) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Footer logo fetch failed:" << reply->errorString();
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) {
            qDebug() << "Footer logo response was not a decodable image:" << url;
            return;
        }
        footerLogoLabel->setPixmap(pixmap);
        footerLogoHasPixmap = true;
        refreshFooterLogoVisibility();
        updateFooterGeometry();
    });
}

void View::refreshFooterLogoVisibility()
{
    footerLogoLabel->setVisible(footerBar->isVisible() && footerLogoHasPixmap);
}

void View::setFooter(
    bool enabled, const QString &text,
    int cycleIntervalMinutes, const QString &logoUrl
)
{
    const bool shouldShow = enabled && !text.trimmed().isEmpty();
    footerLabel->setText(text);
    refreshFooterLabelMetrics();
    footerCycleIntervalMinutes = qMax(0, cycleIntervalMinutes);
    applyFooterLogo(logoUrl);

    if (footerCycling) {
        // Deliberately hidden between cycles right now — text/logo/
        // interval are already updated above for whichever content is
        // showing next, but a routine settings refresh arriving mid-
        // gap must not itself force an early reappearance or otherwise
        // disturb a wait already in progress.
        footerEnabled = shouldShow;
        if (!footerEnabled) {
            // Nothing left to show at all — cancel the pending
            // reappearance outright rather than popping up an empty
            // bar later.
            footerCycleTimer->stop();
            footerCycling = false;
        }
        return;
    }

    if (shouldShow == footerEnabled) {
        // Same on/off state as before — still worth restarting the
        // scroll from the right edge so an edited message (list
        // changed while already showing) doesn't jump mid-word, but
        // no slide animation: the bar is already where it should be.
        if (shouldShow) {
            footerScrollX = footerBar->width();
            positionFooterLabel();
            footerScrollTimer->start();
        }
        return;
    }
    footerEnabled = shouldShow;

    if (footerEnabled) {
        slideFooterIn();
        footerScrollX = footerBar->width();
        positionFooterLabel();
        footerScrollTimer->start();
    } else {
        footerScrollTimer->stop();
        slideFooterOut();
    }
}
