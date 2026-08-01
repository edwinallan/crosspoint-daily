# CrossPoint Daily Development Instructions

## Required context

Before changing code, read:

1. `.skills/SKILL.md` — authoritative CrossPoint firmware rules.
2. `docs/DAILY_BRIEF.md` — requirements and boundaries for the Daily Brief feature.
3. Any relevant `.claude/skills/*/SKILL.md` files for the work being performed.

Do not duplicate or override `.skills/SKILL.md` in this file.

## Project scope

This fork adds a minimal Daily Brief feature to the standard CrossPoint reader firmware for the Xteink X3.

The existing e-reading experience is the primary product. Preserve:

- EPUB reading
- Library browsing
- Reading progress
- Existing settings
- Sleep and wake behavior
- Existing storage and cache formats

Do not turn the firmware into a general-purpose app platform.

## Development environment

Host platform: macOS.

Repository remotes:

- `origin`: personal `crosspoint-daily` fork
- `upstream`: official `crosspoint-reader/crosspoint-reader`

Development branch:

```text
feature/daily-brief
```

Always inspect the current branch, remotes, and working tree before Git operations:

```bash
uname -s
git branch --show-current
git remote -v
git status --short
```

## Build requirement

Before reporting firmware work as complete, run:

```bash
pio run -e default
```

Do not flash hardware unless explicitly requested.

The Xteink X3 has already been successfully flashed with official CrossPoint through the web installer.

## Simulator

Use `crosspoint-reader/crosspoint-simulator` for UI and interaction testing where supported.

Keep simulator-specific code behind existing simulator abstractions or compile-time configuration. Do not weaken the hardware build to accommodate the simulator.

A simulator pass does not replace:

- A successful `default` firmware build
- Memory validation on the ESP32-C3
- Final testing on the physical Xteink X3

## Implementation rules

- Follow the existing activity lifecycle.
- Reuse CrossPoint network, rendering, settings, input, storage, and power abstractions.
- Use HAL APIs instead of direct SDK access.
- Use logical mapped buttons instead of raw GPIO button numbers.
- Use translated strings for user-facing text.
- Preserve orientation support.
- Avoid unnecessary dependencies.
- Avoid background tasks unless their lifecycle and memory costs are justified.
- Cache the last successful Daily Brief so it remains usable offline.
- Never store calendar-provider credentials or OAuth secrets directly in source control.
- Never commit generated files, `.pio/`, credentials, device tokens, or local PlatformIO overrides.

## Scope discipline

Implement the feature incrementally:

1. Static Daily Brief activity in the simulator.
2. Navigation between Daily Brief and the normal reader.
3. Local fixture-data loading.
4. Network retrieval from one personal endpoint.
5. SD-card cache and offline fallback.
6. Sleep-screen integration.
7. Hardware validation.
8. Scheduled wake experiments only after the core feature is stable.

Do not implement scheduled wake, calendar-provider OAuth, RSS parsing, article conversion, or a general plugin system as part of the initial activity.

## Git rules

Do not commit, push, merge, rebase, or open a pull request unless explicitly requested.

When asked to commit:

1. Inspect `git status`.
2. Confirm the firmware builds.
3. Confirm generated and ignored files are not staged.
4. Use a conventional commit message.
5. Push only to `origin` unless explicitly instructed otherwise.
