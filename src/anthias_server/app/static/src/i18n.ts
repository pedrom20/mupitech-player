// home.js is a static bundle (bun build, image-build time) — it has no
// access to Django's {% translate %} at runtime. Pages that need this
// bundle's handful of user-facing strings translated inject them as a
// plain dict on window.__i18n (see home.html); pages that don't (or a
// stale cached bundle predating a given key) just get the English
// fallback passed at each call site.

declare global {
  interface Window {
    __i18n?: Record<string, string>
  }
}

export function t(key: string, fallback: string): string {
  return window.__i18n?.[key] || fallback
}
