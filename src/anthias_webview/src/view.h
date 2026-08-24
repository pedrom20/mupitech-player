#pragma once

#include <QWidget>
#include <QWebEngineView>
#include <QAuthenticator>
#include <QByteArray>
#include <QLabel>
#include <QList>
#include <QNetworkAccessManager>
#include <QImage>
#include <QMovie>
#include <QPair>
#include <QPropertyAnimation>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include "videoview.h"
#endif

// Defined in view.cpp. A QWebEngineUrlRequestInterceptor installed on
// the shared profile that attaches the current webpage asset's custom
// headers to same-origin requests (#2215). Forward-declared so View can
// hold a pointer without pulling the QtWebEngineCore interceptor headers
// into every translation unit that includes view.h.
class RequestHeaderInterceptor;

class View : public QWidget
{
    Q_OBJECT

public:
    explicit View(QWidget* parent);
    ~View();

    void loadPage(const QString &uri, bool skipSslVerify = false);
    void loadImage(const QString &uri, bool skipSslVerify = false);
    void setReloadInterval(int seconds);
    // Per-asset custom HTTP request headers (#2215). ``headersJson`` is
    // a JSON object of ``{name: value}`` string pairs (an empty object
    // clears them). The viewer calls this right before loadPage so the
    // interceptor is armed when the navigation fires; the headers are
    // scoped to the loaded URL's origin (scheme+host+port) at load time.
    void setRequestHeaders(const QString &headersJson);
    // Fleet Manager's footer_messages module — a scrolling text bar
    // pinned to the bottom of the screen, layered above whatever asset
    // is currently showing (image/webpage/video alike). Not gated
    // behind QT_VERSION: unlike playVideo/stopVideo it doesn't touch
    // QtMultimedia, so it applies identically on every board. ``enabled``
    // with a blank/empty ``text`` is treated as disabled — no point
    // reserving screen space for an empty bar.
    //
    // ``cycleIntervalMinutes`` (0 = disabled, the original always-on
    // behavior) makes the bar hide itself after one full marquee pass
    // and reappear ``cycleIntervalMinutes`` minutes later, repeating —
    // see tickFooterScroll()'s docstring in view.cpp for the state
    // machine. ``logoUrl`` (absolute HTTP(S) URL, or empty for none) is
    // fetched and painted at the bar's left edge, overlapping it — see
    // applyFooterLogo().
    void setFooter(
        bool enabled, const QString &text,
        int cycleIntervalMinutes, const QString &logoUrl
    );
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Hands the URI + option dict to VideoView (QtMultimedia
    // QMediaPlayer rendering into a QML VideoOutput) and switches
    // visibility so the video surface is on top of the
    // QWebEngineView pair / image canvas. Pauses background URL
    // loads so a parked QWebEngineView doesn't keep streaming while
    // video plays.
    //
    // Qt 5 boards (Pi 1 / Pi 2 / Pi 3) route video through GstFbdevMediaPlayer
    // painting straight to the framebuffer (see
    // ``MediaPlayerProxy.get_instance`` in
    // ``src/anthias_viewer/media_player.py``), so the in-process
    // playback surface and its EOF signal are Qt6-only.
    void playVideo(const QString &uri, const QVariantMap &options);
    void stopVideo();

signals:
    void videoEnded();
#endif

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void handleAuthRequest(const QUrl& requestUrl, QAuthenticator* authenticator);

private:
    void configureWebView(QWebEngineView* view);
    void stopAnimation();
    bool tryLoadAsAnimatedGif(const QByteArray& data);
    void loadAsStaticImage(const QByteArray& data);
    void setupAnimation();
    void switchToNextWebView();
    // Issues a (re)load of ``uri`` into nextWebView: detaches any
    // stale loadFinished handler, cancels the in-flight navigation,
    // attaches a fresh one-shot handler tagged with ``requestId``,
    // and (re)arms the page-load watchdog. Called by loadPage for
    // the initial attempt and by handlePageLoadTimeout for retries.
    void startPageLoad(const QString &uri, quint64 requestId);
    // Issue #2999 — runs when a webpage navigation has neither
    // finished nor been superseded within the watchdog interval
    // (e.g. stalled mid-fetch by a network dropout). Cancels the
    // wedged load and retries the same URI so the device self-heals
    // once connectivity returns.
    void handlePageLoadTimeout();
    // Footer bar geometry/animation — see the member fields' comments
    // below for the overall design.
    int footerBarHeight() const;
    // Right-hand gap the bar stops short of, and the top-corner radius
    // — both scale with the screen like footerBarHeight().
    int footerRightMargin() const;
    void refreshFooterLabelMetrics();
    void updateFooterGeometry();
    void tickFooterScroll();
    // Slides footerBar in/out (shared by setFooter's own show/hide and
    // the automatic cycle in tickFooterScroll) — factored out so both
    // call sites stay in sync instead of hand-rolling the same
    // QPropertyAnimation setup twice.
    void slideFooterIn();
    void slideFooterOut();
    // Fetches ``url`` (a plain QNetworkAccessManager::get(), independent
    // of the content-asset loadImage() pipeline — no SSL-verify opt-out
    // or animated-GIF handling needed for a small fixed badge image)
    // and paints it into footerLogoLabel once it arrives. No-ops if
    // ``url`` is blank or unchanged from what's already applied.
    void applyFooterLogo(const QString &url);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Hides VideoView and re-enables the web/image surface. Called
    // by loadPage / loadImage so a switch from video back to a web
    // page or image doesn't leave the GL widget on top of the
    // QWebEngineView.
    void hideVideoSurface();
#endif

    QNetworkAccessManager* networkManager;
    QImage currentImage;
    QImage nextImage;
    QMovie* movie;
    bool isAnimatedImage;
    quint64 loadGenerationId;

    // Manual rotation applied in paintEvent to raster image assets —
    // still images and animated GIFs, which share the ``currentImage``
    // draw path — in degrees clockwise (0/90/180/270). Set once at
    // construction from linuxfbRotationOverride(); non-zero only on
    // linuxfb boards whose QPA plugin ignores ``rotation=N``.
    int imageRotation;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // QtMultimedia-backed video widget (issue #2904). Sibling of
    // the web / image widgets — visibility is toggled rather than
    // re-parented so the QMediaPlayer + Quick scene survive
    // across plays (no pipeline rebuild per asset).
    VideoView* videoView;
#endif

    // Request interceptor + the headers staged for the next page load
    // (#2215). ``pendingHeaders`` is set by setRequestHeaders and applied
    // — together with the loaded URL's origin (scheme+host+port) — to
    // ``headerInterceptor`` in startPageLoad. Only touched on the UI
    // thread; the interceptor's own
    // copy is mutex-guarded because Chromium may invoke interceptRequest
    // on a different thread (Qt 5).
    RequestHeaderInterceptor* headerInterceptor;
    QList<QPair<QByteArray, QByteArray>> pendingHeaders;

    // Dual web view system
    QWebEngineView* webView1;
    QWebEngineView* webView2;
    QWebEngineView* currentWebView;
    QWebEngineView* nextWebView;
    bool nextWebViewReady;

    // Connection handle for the currently-pending loadFinished slot, so
    // we can drop it before issuing stop() on the next loadPage and
    // avoid a synchronous loadFinished(false) racing into a stale slot.
    QMetaObject::Connection pageLoadConnection;

    // Issue #2999 — page-load watchdog. QtWebEngine has no built-in
    // navigation timeout: a fetch interrupted mid-flight (WiFi AP
    // drop, no FIN/RST) leaves the request pending forever, so
    // loadFinished never fires, the dual-view swap never happens and
    // the screen freezes on the previous asset until the container is
    // restarted. Single-shot; armed by startPageLoad, stopped on a
    // successful load and by every path that cancels the pending
    // navigation (loadImage / playVideo). On timeout — or after a
    // *failed* load, where the handler deliberately leaves it running
    // as a delayed-retry tick — handlePageLoadTimeout stops the wedged
    // navigation and re-issues the same URI. The retry matters: the
    // viewer's view_webpage() only sends loadPage when the URL
    // *changes*, so with a single-webpage playlist no further D-Bus
    // call would ever arrive to unwedge a stalled load.
    QTimer* pageLoadWatchdog;

    // URI of the loadPage navigation currently in flight (empty when
    // none). Read by handlePageLoadTimeout to retry; cleared whenever
    // the pending navigation is cancelled in favor of an image/video.
    QString pendingPageUri;

    // Per-asset auto-refresh timer. When non-null and active, fires
    // currentWebView->reload() every ``pendingReloadIntervalS`` seconds.
    // Cleared on every loadPage / loadImage so a fresh asset starts
    // from a clean slate. Owned by the View (parent=this).
    QTimer* reloadTimer;

    // Most recently requested auto-refresh cadence, in seconds. 0 = no
    // auto-refresh. Held separately from the timer because
    // setReloadInterval can land *while a page load is still in flight*
    // (loadPage queues a load into nextWebView, then the viewer calls
    // setReloadInterval before the swap completes); arming a QTimer
    // immediately would target the still-visible *previous* page via
    // currentWebView->reload(). We instead remember the value here and
    // arm the timer in switchToNextWebView() once the new page is
    // actually visible.
    int pendingReloadIntervalS;
    void stopReloadTimer();
    void armReloadTimer();

    // Footer ticker bar (setFooter). A plain QWidget/QLabel pair rather
    // than a layout — every other visual in this class (webViews,
    // videoView) is positioned by manual setGeometry() in resizeEvent,
    // so this follows the same convention. footerBar is always the
    // last-constructed (hence topmost-stacked) child, so it draws over
    // the web/image/video surfaces without needing an explicit raise()
    // on every asset change — playVideo() still raises it defensively
    // after raising videoView, in case that ever changes.
    QWidget* footerBar;
    QLabel* footerLabel;
    // Slides footerBar's ``geometry`` between just-below-the-visible-
    // area (hidden) and flush with the bottom edge (visible) — see
    // setFooter(). A single persistent animation + one connected
    // ``finished`` handler (in the constructor), not re-created per
    // call, so repeated show/hide cycles don't accumulate connections.
    QPropertyAnimation* footerSlideAnimation;
    // Marquee scroll: ticks footerLabel's x position leftward, wrapping
    // back to footerBar's right edge once the label has fully scrolled
    // off the left — a single repeating pass with a blank gap between
    // loops, not a seamless double-buffered tile (fine for a first
    // version; see setFooter()'s docstring in view.cpp).
    QTimer* footerScrollTimer;
    int footerScrollX;
    // Whether the footer is currently shown (or animating in) — distinct
    // from footerBar->isVisible(), which stays true for the whole
    // hide animation and only flips false once it finishes.
    bool footerEnabled = false;

    // Logo overlapping the bar's left edge (Fleet Manager's optional
    // fleet-wide footer logo upload). Parented to the View itself, not
    // footerBar — footerBar clips its children to its own geometry, and
    // the logo is deliberately sized a bit taller than the bar so it
    // visually "sits in front of" it rather than being flush inside it.
    // Raised above footerBar on every update so the bar always appears
    // to run out from behind it, never on top.
    QLabel* footerLogoLabel;
    // Last URL actually applied to footerLogoLabel — applyFooterLogo()
    // skips the network fetch entirely when called again with the same
    // value (every reload/settings poll re-sends the current footer
    // state, not just changes).
    QString footerLogoUrl;

    // 0 = disabled (original always-visible behavior). When positive,
    // tickFooterScroll() stops the marquee after exactly one full pass
    // instead of wrapping, slides the bar out, and footerCycleTimer
    // (below) re-shows it after this many minutes.
    int footerCycleIntervalMinutes = 0;
    // Singleshot; armed in tickFooterScroll() when a cycle's visible
    // pass finishes, consumed by its own timeout handler which slides
    // the bar back in and restarts the scroll from the right edge.
    QTimer* footerCycleTimer;
    // True only during the deliberate "hidden between cycles" gap —
    // distinct from !footerEnabled (no messages at all): a change to
    // footer content while cycling is still honored on the *next*
    // show, but a bare interval change must never itself interrupt a
    // pass already in progress.
    bool footerCycling = false;
};
