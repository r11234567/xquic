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
default GITHUB_TOKEN do not normally trigger other workflows: a maintainer
must run Build and CodeQL or push a follow-up commit before merging those PRs.

Record current-head results honestly. A ready-for-review PR is not an approval
to merge, and pending Actions are not passing evidence. The downstream mqvpn
gitlink must point to a reviewed commit retained on a long-lived branch when
the paired synchronization PRs eventually merge.
