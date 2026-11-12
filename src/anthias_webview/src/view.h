#pragma once

#include <QWidget>
#include <QWebEngineView>
#include <QAuthenticator>
#include <QByteArray>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QList>
#include <QNetworkAccessManager>
#include <QImage>
#include <QMovie>
#include <QPair>
#include <QPropertyAnimation>
#include <QVariantAnimation>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <functional>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include "videoview.h"
#endif

// Defined in view.cpp. A QWebEngineUrlRequestInterceptor installed on
// the shared profile that attaches the current webpage asset's custom
// headers to same-origin requests (#2215). Forward-declared so View can
// hold a pointer without pulling the QtWebEngineCore interceptor headers
// into every translation unit that includes view.h.
class RequestHeaderInterceptor;
class QPainter;

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
    // Scales/rotates/paints ``image`` exactly like the old single-image
    // paintEvent body used to, but at an arbitrary opacity and callable
    // twice per paint — the building block both the normal (opacity 1)
    // paint and the image->image crossfade (old then new, blended) are
    // written in terms of. const: never mutates view state, just draws.
    void drawScaledImage(QPainter& painter, const QImage& image, qreal opacity) const;

private slots:
    void handleAuthRequest(const QUrl& requestUrl, QAuthenticator* authenticator);

private:
    void configureWebView(QWebEngineView* view);
    void stopAnimation();
    // Fade-through-black between one asset's content and the next —
    // see fadeOverlay's own comment for why this is a solid black
    // widget fading in/out rather than a true crossfade of two live
    // surfaces. ``swapContent`` runs at the exact moment the screen is
    // fully black (fade-out finished), so it's always safe for it to
    // mutate currentImage / start a QMovie / flip webview visibility —
    // nothing is on screen to flicker. Call sites must only invoke this
    // once the new content is already fully ready (decoded image data
    // in hand, page already loaded) — never to bridge a network wait,
    // or the screen would sit black for however long that takes.
    void fadeToContent(std::function<void()> swapContent);
    // Real crossfade for image->image — see crossfadeFromImage's own
    // comment for why only this specific case gets one. Both images
    // must already be fully decoded/ready; ``toImage`` becomes
    // currentImage immediately (nothing defers it, unlike
    // fadeToContent — there's no "screen is black" moment to hide a
    // swap behind here, both frames are visible throughout). Takes
    // ``fromImage`` explicitly rather than reading currentImage
    // itself: setupAnimation's caller must snapshot it *before*
    // calling QMovie::start(), which may synchronously fire
    // frameChanged and overwrite currentImage with the new GIF's own
    // first frame before this ever runs.
    void startCrossfade(const QImage &fromImage, const QImage &toImage);
    // True exactly when it's safe to crossfade into the next image
    // instead of fading through black: both webviews are hidden (no
    // webpage/video on screen) and something is actually currently
    // painted (not the very first asset ever, nothing to fade *from*).
    bool previousAssetWasImage() const;
    // Single persistent QPropertyAnimation's finished handler (see
    // fadeAnimation's own comment) — advances fadePhase from FadingOut
    // to FadingIn (running the queued swapContent then reversing the
    // animation) or back to Idle.
    void onFadeAnimationFinished();
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
    // How far the bar's own left edge is inset from x=0 when a logo is
    // loaded — 0 with none. Tucks just the bar's rounded corner (not
    // the logo's whole footprint) behind the image, so the bar's dark
    // background never sits behind most of the logo — only enough of
    // it disappears there to read as "emerging from behind" it. See
    // updateFooterGeometry()'s use of this for the full picture.
    int footerBarLeftOffset() const;
    // Extra gap the scrolling text stops short of the bar's own left
    // edge by, before wrapping/hiding — without this the marquee would
    // scroll every last pixel of a line out at the bar's true x=0,
    // which (now that the bar's left edge itself sits tucked right up
    // against the image, see footerBarLeftOffset()) reads as text
    // butting straight up against the logo instead of trailing off
    // behind it with some visual breathing room first.
    int footerTextLeftMargin() const;
    void refreshFooterLabelMetrics();
    void updateFooterGeometry();
    void tickFooterScroll();
    // Moves footerLabel to footerScrollX, translated into
    // footerTextClip's local coordinate space (offset left by
    // footerTextLeftMargin()) — every footerScrollX assignment must
    // route through this rather than footerLabel->move() directly, or
    // the text reappears flush with the bar's true left edge instead
    // of clipped footerTextLeftMargin() short of it.
    void positionFooterLabel();
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
    // The logo must only ever be on screen while the bar itself
    // genuinely is — shown together, hidden together (including during
    // a cycling gap, not just a real disable). footerBar->isVisible()
    // is the correct signal for that (see its own field comment: stays
    // true for the whole hide animation, flips false only once it
    // finishes), not footerEnabled, which stays true throughout a
    // cycling gap. Called wherever either side of that AND changes:
    // a fresh pixmap arriving, or the bar's own visible/hidden flip.
    void refreshFooterLogoVisibility();
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

    // Asset-to-asset transition. A solid black QWidget overlaying the
    // whole view (parent=this, constructed before footerBar so it
    // stacks *below* the footer ticker — see that field's own comment
    // in view.cpp — the footer is persistent chrome, not part of what's
    // transitioning). Its QGraphicsOpacityEffect is animated 0->1
    // (reveal black, hiding whatever's about to change) then 1->0
    // (reveal the new content) by the one persistent fadeAnimation
    // below, rather than a *real* crossfade of the old and new content
    // simultaneously on screen: issue #2954 (see the dual-webview
    // comment in the constructor) is exactly what happens when this
    // codebase keeps two live composited surfaces around at once on
    // Qt6/Wayland/cage boards — a hidden/occluded QWebEngineView is
    // frame-callback-throttled and can reveal a stale GPU frame.
    // Fading a single opaque black widget over the *existing* surface
    // sidesteps that whole bug class: there is only ever one thing
    // rendering the "real" content at a time, this overlay just dims
    // it.
    QWidget* fadeOverlay;
    QGraphicsOpacityEffect* fadeOpacityEffect;
    QPropertyAnimation* fadeAnimation;
    enum class FadePhase { Idle, FadingOut, FadingIn };
    FadePhase fadePhase = FadePhase::Idle;
    // Set by fadeToContent() for onFadeAnimationFinished() to run once
    // the screen is fully black; cleared immediately after.
    std::function<void()> pendingContentSwap;

    // Real crossfade for image->image only (no black flash) — a
    // deliberately narrower case than fadeOverlay above. Both the
    // outgoing and incoming frame are plain QImage paints (no
    // QWebEngineView involved at all), so there is no analogue of
    // #2954's stale-GPU-surface mechanism to worry about here: this
    // is just two drawImage() calls blended by ordinary alpha
    // compositing in one paintEvent, never two live composited
    // widgets. loadAsStaticImage/setupAnimation only take this path
    // when the *previous* on-screen content was itself a plain image
    // (see their own call sites) — anything involving a webpage still
    // goes through fadeToContent's fade-through-black above.
    QImage crossfadeFromImage;
    qreal crossfadeProgress = 0.0;
    bool isCrossfading = false;
    QVariantAnimation* crossfadeAnimation;

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
    // Child of footerBar, parent of footerLabel — exists purely to clip
    // the marquee text at footerTextLeftMargin() px in from footerBar's
    // own left edge (Qt clips a child's painting to its own parent's
    // rect, so without this intermediate widget footerLabel would clip
    // at footerBar's edge, x=0, same as before footerTextLeftMargin()
    // existed). No stylesheet/background of its own — it must stay
    // invisible as a shape, or it reads as yet another bar-like strip
    // the way footerTextViewport once did before that whole approach
    // was replaced by insetting footerBar itself (see
    // footerBarLeftOffset()). Resized alongside footerBar in
    // updateFooterGeometry().
    QWidget* footerTextClip;
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

    // Logo at the bar's left edge (Fleet Manager's optional fleet-wide
    // footer logo upload). Parented to the View itself, not footerBar
    // — the bar's own rect is inset to start near the logo's right
    // edge (see footerBarLeftOffset()) rather than running underneath
    // the whole logo, so there's no dark background behind most of
    // the image, only immediately behind its rounded corner. The logo
    // is sized a bit taller than the bar and raised above it so it
    // still visually "sits in front of" that one tucked-under corner.
    QLabel* footerLogoLabel;
    // Last URL actually applied to footerLogoLabel — applyFooterLogo()
    // skips the network fetch entirely when called again with the same
    // value (every reload/settings poll re-sends the current footer
    // state, not just changes).
    QString footerLogoUrl;
    // Whether footerLogoLabel currently holds a real (non-null) pixmap
    // — tracked separately rather than checking footerLogoLabel->pixmap()
    // itself, since that method's return type differs between Qt5
    // (const QPixmap*, this codebase's Pi 1/2/3 target) and Qt6 (QPixmap
    // by value); a plain bool works identically on both. Read by
    // refreshFooterLogoVisibility() (alongside footerBar->isVisible() —
    // the logo only ever shows when both are true) and by
    // footerBarLeftOffset() (no logo loaded yet means nothing to inset
    // the bar's own rect for).
    bool footerLogoHasPixmap = false;

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
