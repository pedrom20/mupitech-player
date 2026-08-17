#pragma once

#include <QString>

// Best-effort auto-dismissal of cookie/GDPR consent banners on
// web-page assets (MupiTech addition, not upstream Anthias). Unattended
// digital signage has no one to click "Accept" — a banner left up
// covers real content indefinitely. There is no single standard every
// site follows, so this is deliberately layered: known consent-platform
// JS APIs first (exact, no visual guessing), then a generic fallback
// that only clicks a button/link inside a container whose id/class
// looks cookie-related AND whose visible text matches a common accept
// phrase (PT/EN — this fork's two shipped locales) — requiring both
// signals keeps it from clicking unrelated "Accept"/"OK" buttons
// elsewhere on a page.
//
// Kept free of View / QtWebEngine so the script text itself is
// unit-testable against QtCore alone (see tests/tests.pro), same
// rationale as rotation.h.
namespace cookieConsent
{
// JavaScript, safe to run in QWebEngineScript::ApplicationWorld on
// every page load (and re-run via its own MutationObserver across SPA
// DOM rewrites): tries known consent-platform APIs, then a scoped
// selector+text-match fallback. No-op (throws nothing, changes
// nothing) on a page with no consent banner.
QString dismissScript();
}  // namespace cookieConsent
