// QtTest unit tests for the cookie-consent auto-dismiss script
// (src/cookie_consent.cpp). The helper only builds a QString, so these
// run against QtCore alone — no display, no QtWebEngine. Hosted by the
// runCookieConsentTests factory that test_videoview.cpp's main() execs
// (QTEST_MAIN can only host one class per binary), same pattern as
// TestRotation.

#include <QObject>
#include <QString>
#include <QTest>

#include "cookie_consent.h"

class TestCookieConsent : public QObject
{
    Q_OBJECT

private slots:
    // Known consent-platform APIs are tried first, before any DOM
    // scanning — cheapest and least likely to misfire.
    void triesKnownApisFirst()
    {
        const QString js = cookieConsent::dismissScript();
        QVERIFY(js.contains(QStringLiteral("window.OneTrust")));
        QVERIFY(js.contains(QStringLiteral("AcceptAll")));
        QVERIFY(js.contains(QStringLiteral("window.Didomi")));
        QVERIFY(js.contains(QStringLiteral("setUserAgreeToAll")));
    }

    // The generic fallback requires BOTH a cookie-related container
    // hint and accept-like text before it clicks anything — this is
    // what keeps it from clicking an unrelated "Accept"/"OK" button
    // elsewhere on the page.
    void scopesGenericFallbackToBannerContainers()
    {
        const QString js = cookieConsent::dismissScript();
        QVERIFY(js.contains(QStringLiteral("BANNER_HINT")));
        QVERIFY(js.contains(QStringLiteral("cookie|consent|gdpr")));
        QVERIFY(js.contains(QStringLiteral("findBannerContainers")));
        QVERIFY(js.contains(QStringLiteral("clickAcceptIn")));
    }

    // Both shipped locales (PT/EN) are covered in the accept-phrase list.
    void coversBothShippedLocales()
    {
        const QString js = cookieConsent::dismissScript();
        QVERIFY(js.contains(QStringLiteral("accept all")));
        QVERIFY(js.contains(QStringLiteral("aceitar tudo")));
        QVERIFY(js.contains(QStringLiteral("concordo")));
    }

    // Re-applies across SPA DOM rewrites within a single document, same
    // robustness pattern as the rotation script.
    void reassertsViaMutationObserver()
    {
        const QString js = cookieConsent::dismissScript();
        QVERIFY(js.contains(QStringLiteral("MutationObserver")));
        QVERIFY(js.contains(QStringLiteral("subtree:true")));
    }

    // Only clicks currently-visible elements — a hidden/collapsed
    // banner (already dismissed, or not yet rendered) must not be acted
    // on.
    void onlyActsOnVisibleElements()
    {
        const QString js = cookieConsent::dismissScript();
        QVERIFY(js.contains(QStringLiteral("getBoundingClientRect")));
        QVERIFY(js.contains(QStringLiteral("offsetParent")));
    }
};

int runCookieConsentTests(int argc, char** argv)
{
    TestCookieConsent tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_cookie_consent.moc"
