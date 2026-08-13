import logging

from requests import exceptions
from requests import get as requests_get

from anthias_common.utils import connect_to_redis
from anthias_server.lib.diagnostics import get_git_short_hash

logger = logging.getLogger(__name__)

r = connect_to_redis()

# Cache the latest-commit lookup for 24h. Lines up with how often the
# fork actually merges/releases so we don't hammer the API but still
# surface a new build within a day.
LATEST_COMMIT_TTL = 60 * 60 * 24

# Suspend further GitHub API requests for 5 minutes after a non-404
# error (rate limit, 5xx, network blip).
ERROR_BACKOFF_TTL = 60 * 5

# Cache key for the latest commit's short SHA (TTL'd).
LATEST_COMMIT_SHA_KEY = 'latest-commit-sha'
# Cache-key prefix for the most recent successfully-computed verdict.
# Scoped by local hash so an upgrade during a GitHub outage doesn't
# reuse the old build's verdict (the previous "up to date" might no
# longer apply, and vice versa). Written on every fresh comparison and
# read only as the fallback when both the SHA cache and a fresh fetch
# fail. No TTL — overwritten on the next successful check.
LAST_VERDICT_KEY_PREFIX = 'is-up-to-date:last-verdict'

DEFAULT_REQUESTS_TIMEOUT = 5  # seconds

# This is a fork with its own release cadence: it doesn't cut GitHub
# Releases (see tools/image_builder — CI publishes a floating
# latest-<board> and an immutable <short-hash>-<board> tag to GHCR on
# every merge, no version tags). So "is this device up to date" can't
# be a CalVer/release comparison the way upstream Anthias does it —
# it means "does it match the tip of mupitech-custom", checked via the
# commit SHA baked into the image at build time (GIT_SHORT_HASH).
GITHUB_COMMIT_URL = (
    'https://api.github.com/repos/pedrom20/mupitech-player'
    '/commits/mupitech-custom'
)
GITHUB_API_ACCEPT = 'application/vnd.github+json'

# GitHub's own abbreviated-SHA length, and what tools/image_builder
# slices GIT_SHORT_HASH to (SHORT_HASH_LENGTH) — kept in sync manually
# since importing the image_builder package here would pull a
# dev-only tool into the runtime image for one constant.
SHORT_HASH_LENGTH = 7


def _set_github_error_backoff(action: str) -> None:
    r.set('github-api-error', action)
    r.expire('github-api-error', ERROR_BACKOFF_TTL)


def handle_github_error(
    exc: exceptions.RequestException,
    action: str,
) -> None:
    _set_github_error_backoff(action)

    if exc.response is not None:
        # .text (decoded str) rather than .content (bytes) so the log
        # line reads as the server's message, not a b'...' repr.
        errdesc = exc.response.text
    else:
        # Surface the exception detail — a bare 'no data' hid the
        # host/timeout info str(exc) carries (e.g. 'read timed out').
        errdesc = str(exc) or 'no data'

    # Warning, not error: a signage device that can't reach
    # api.github.com (offline installs, locked-down networks, GitHub
    # outages, rate limits) is a routine condition the caller already
    # degrades through gracefully — backoff above, cached verdict
    # fallback in is_up_to_date(). An ERROR-level log would land in
    # Sentry on every offline device (ANTHIAS-8).
    logger.warning(
        '%s fetching %s from GitHub: %s', type(exc).__name__, action, errdesc
    )


def _fetch_latest_commit_sha() -> str | None:
    """Return the short SHA of the tip of mupitech-custom on GitHub,
    hitting the API at most once per ``LATEST_COMMIT_TTL`` and
    short-circuiting while a prior error backoff is active. Returns
    ``None`` on any failure.
    """
    cached = r.get(LATEST_COMMIT_SHA_KEY)
    if cached:
        return cached

    if r.get('github-api-error') is not None:
        logger.warning('GitHub requests suspended due to prior error')
        return None

    try:
        resp = requests_get(
            GITHUB_COMMIT_URL,
            headers={'Accept': GITHUB_API_ACCEPT},
            timeout=DEFAULT_REQUESTS_TIMEOUT,
        )
        resp.raise_for_status()
    except exceptions.RequestException as exc:
        handle_github_error(exc, 'latest commit')
        return None

    # Trip the same 5-minute backoff for malformed bodies as for
    # transport failures. Without this, a bad JSON body or a payload
    # with no usable sha from a 200 response would re-fire the GitHub
    # call on every page render until the API response recovered.
    try:
        payload = resp.json()
    except ValueError:
        logger.warning('Malformed JSON from GitHub /commits')
        _set_github_error_backoff('latest commit: malformed JSON')
        return None

    sha = payload.get('sha') if isinstance(payload, dict) else None
    if not isinstance(sha, str) or not sha:
        logger.warning('No usable sha in GitHub /commits response')
        _set_github_error_backoff('latest commit: no usable sha')
        return None

    short_sha = sha[:SHORT_HASH_LENGTH]
    r.set(LATEST_COMMIT_SHA_KEY, short_sha)
    r.expire(LATEST_COMMIT_SHA_KEY, LATEST_COMMIT_TTL)
    return short_sha


def _verdict_cache_key(local_hash: str) -> str:
    return f'{LAST_VERDICT_KEY_PREFIX}:{local_hash}'


def _fallback_verdict(local_hash: str) -> bool:
    """Return the last verdict cached for this exact local build, or
    ``False`` if there isn't one (don't claim "up to date" when we
    don't actually know — and don't reuse a verdict computed against a
    different build)."""
    cached = r.get(_verdict_cache_key(local_hash))
    if cached is None:
        return False
    return cached == '1'


def is_up_to_date() -> bool:
    """Return ``True`` if this device is running the tip of
    mupitech-custom.

    Compares ``GIT_SHORT_HASH`` (baked into the image at build time by
    CI, see ``tools/image_builder``) against the short SHA of the tip
    of ``mupitech-custom`` on GitHub. Caches the remote SHA for 24h and
    falls back to the last computed verdict if GitHub is unreachable.

    Returns ``True`` (suppressing the "Update available" indicator)
    when there's no local hash to compare against — e.g. a host run
    with no ``GIT_SHORT_HASH`` env var — since there's no useful
    comparison to make and the indicator should not shout at
    developers.
    """
    local_hash = get_git_short_hash()
    if not local_hash:
        return True
    local_hash = local_hash[:SHORT_HASH_LENGTH]

    latest_sha = _fetch_latest_commit_sha()
    if not latest_sha:
        return _fallback_verdict(local_hash)

    verdict = local_hash == latest_sha
    r.set(_verdict_cache_key(local_hash), '1' if verdict else '0')
    return verdict
