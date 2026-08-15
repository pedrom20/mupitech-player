// Minimal Alpine-only bundle for pre-auth pages (login.html,
// mupitech_sso_error.html) that deliberately skip base.html/vendor.js
// to avoid vendor.js's unconditional WebSocket connect (see
// login.html's own comment) — but vendor.js is also the only place
// Alpine.start() gets called, so those pages' `x-data`/`@click`
// directives (e.g. login.html's device/Fleet-Manager account tab
// toggle) were silently inert with no JS running them at all. This
// bundle is Alpine and nothing else: no htmx, no flatpickr, no
// WebSocket.
import Alpine from 'alpinejs'

declare global {
  interface Window {
    Alpine: typeof Alpine
  }
}

window.Alpine = Alpine
Alpine.start()
