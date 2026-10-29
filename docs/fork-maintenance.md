# Downstream branch maintenance

`r11234567/xquic:main` is the product branch consumed by the downstream mqvpn
fork. It is not a mirror of Alibaba's main. The integration upstream is
`mp0rta/xquic:mqvpn-main`; that upstream owns the Alibaba mirror and its own
`mqvpn-main` / `mqvpn-dev` layout. Those two branch names are not required in
this fork.

Synchronize by full merges so upstream commits remain ancestors. Resolve
conflicts using the current upstream implementation and retain only the
needed downstream increments. Do not restore an old algorithm where upstream
has a better replacement. Keep STREAM traffic on the measured MinRTT path.
Use a merge commit for the final synchronization PR; squash and rebase merges
would discard the history relationship that the consumer needs.

`sync/**` names integration PR branches. Build and CodeQL run on their pushes
and on PRs targeting main. Fix work starts on a scoped `fix/` branch and is
added to an already-open integration PR without a force push.

The upstream maintenance workflow proposes missing mp0rta history as a draft
PR, and reuses a pending PR that already contains it. It never synchronizes
remote main through GitHub's mirror API. The BoringSSL workflow also targets
main through a PR and never changes main directly. Events authored with the
default GITHUB_TOKEN do not normally trigger other workflows. The dependency
workflow explicitly dispatches Build and CodeQL; upstream-sync proposals
require a maintainer to start validation or push a follow-up commit.

The integration retains the fork's RFC-oriented transport and TLS fixes,
HTTP/3 urgency queue, proxy backpressure/write notifications, external PMTU
API, configurable raise interval, and bounded send-supply statistics. It
keeps upstream's current black-hole recovery, queue-aware WLB weights,
effective-PTO handling, STREAM MinRTT fallback, connection-allocation cleanup,
closed-path/CID traversal, and BoringSSL pin instead of restoring older
implementations. The urgent queue participates in PMTU resize cleanup as
well as ordinary teardown, retry, and 0-RTT handling.

The former Alibaba cherry-pick workflow is superseded by full-history
mp0rta synchronization. `.github/upstream-sync-base` remains only as the
historical record of the fork's last selective Alibaba audit; it does not
claim that Alibaba main is an ancestor of this fork.

Record current-head results honestly. A ready-for-review PR is not an approval
to merge, and pending Actions are not passing evidence. The downstream mqvpn
gitlink must point to a reviewed commit retained on a long-lived branch when
the paired synchronization PRs eventually merge.
