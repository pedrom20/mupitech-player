# MupiTech fork — maintenance notes

This is a fork of [Screenly/Anthias](https://github.com/Screenly/Anthias),
maintained for the MupiTech Fleet Manager project. It exists to add
feature parity with a third-party fork we're moving away from
(`alex1981-tech/Anthias_play`, an unmaintained, architecturally older
fork) without depending on unmaintained third-party images.

## Branches

- `master` — kept as a clean mirror of `upstream/master`. Never commit
  here directly; only fast-forward it when syncing.
- `mupitech-custom` — our long-lived working branch. All of our
  additions live here. This is what CI builds and what devices run.

## Rebase cadence

Merge `upstream/master` into `mupitech-custom` on a regular cadence:

- Monthly, as a baseline.
- Ad hoc, immediately, for any upstream security fix.

Since our additions are almost entirely new files (not edits to
existing upstream files), merge conflicts should be rare. Resolve with
a normal `git merge upstream/master` — do not attempt to squash or
rebase our history onto upstream, since that would rewrite published
tags devices may already be running.

## Versioning

Tag releases as `<upstream-version>-mupitech.<n>`, e.g.
`v1.2.0-mupitech.3`, so it's always clear which upstream baseline a
given build tracks.

## What we've added on top of upstream

See `mupiteck`'s (the Fleet Manager repo) `docs/anthias-version-analysis.md`
for the full feature-by-feature breakdown and status. Summary:

- **CEC (display power)** — already in upstream master
  (`POST /api/v2/display/<state>`). No changes needed here; the Fleet
  Manager side was adjusted instead.
- **IR** — not in upstream. Built here, pure Python subprocess wrapper
  around `ir-ctl`, no viewer/Qt changes.
- **Screenshot** — not in upstream. Wayland/`grim` capture added here
  for x86/Pi5/arm64. Pi4-64 (DRM/`kmsgrab`-based) and Pi2/Pi3 (fbdev)
  are not implemented yet — `/v2/screenshot` reports "not supported"
  on those boards in the meantime.
- **Scheduling** — adopted from upstream's own in-progress
  `schedule-slots` branch (per-asset `play_days`/`play_time_from`/
  `play_time_to` fields), not the third-party fork's separate-slots
  model.
- **Self-update** — not in upstream. Thin proxy to the Watchtower
  sidecar's HTTP API, since we already run Watchtower fleet-wide.

## CI

`.github/workflows/docker-build.yaml` builds and publishes to
`ghcr.io/pedrom20/mupitech-player-*` on push to `mupitech-custom`, for
`x86`, `pi5` and `pi4-64` (Pi2/Pi3 not built yet — see the Fleet
Manager repo's plan for the full phase breakdown). Pi5/Pi4-64 images
build and publish, but haven't yet been validated against real
hardware — do that before pointing a production device at them.
