# Docker build modernization

## Problem

`.github/workflows/docker-build-push.yml` builds `linux/amd64,linux/arm64`
in one job using QEMU cross-emulation. Every push waits ~17 min, almost
all of it spent emulating ARM64 (full UnrealIRCd C compile + obbypy
CPython link). Slows iteration; bumping the obby-stack pin is gated
on the same single slow build.

## Goal

Build per-architecture in parallel on native runners, then assemble a
multi-arch manifest. Wall-clock target: ~4 min (vs current ~17 min).

## Design

Single workflow file with three jobs:

```yaml
jobs:
  build:
    strategy:
      matrix:
        include:
          - arch: amd64
            runner: ubuntu-24.04
          - arch: arm64
            runner: ubuntu-24.04-arm   # native ARM, free on public repos
    runs-on: ${{ matrix.runner }}
    outputs:
      digest: ${{ steps.build.outputs.digest }}
    steps:
      - checkout
      - docker/setup-buildx-action
      - docker/login-action
      - docker/build-push-action with:
          platforms: linux/${{ matrix.arch }}
          tags: ${{ secrets.DOCKERHUB_USERNAME }}/${{ inputs.image_name }}:sha-${short_sha}-${{ matrix.arch }}
          cache-from: type=registry,ref=${USER}/${IMAGE}:cache-${{ matrix.arch }}
          cache-to:   type=registry,ref=${USER}/${IMAGE}:cache-${{ matrix.arch }},mode=max
          push: true
          provenance: false  # keep manifest small

  manifest:
    needs: build
    runs-on: ubuntu-24.04
    steps:
      - docker/login-action
      - docker buildx imagetools create \
          --tag ${USER}/${IMAGE}:sha-${short_sha} \
          --tag ${USER}/${IMAGE}:latest \
          --tag ${USER}/${IMAGE}:pr-${PR_NUMBER}        # only on PR events
          --tag ${USER}/${IMAGE}:${{ github.ref_name }} # only on branch push
          ${USER}/${IMAGE}:sha-${short_sha}-amd64 \
          ${USER}/${IMAGE}:sha-${short_sha}-arm64
```

## Caching

Per-arch registry cache (`type=registry,ref=...:cache-amd64`/`cache-arm64`)
beats GHA cache:
- Survives the 10 GB GHA cache limit
- Cross-runner (different runners hit the same registry cache)
- No eviction churn between branches

## Tag scheme

| Tag | When | Notes |
|---|---|---|
| `:sha-<7>-amd64`, `:sha-<7>-arm64` | every build | per-arch artefacts; safe to GC after manifest |
| `:sha-<7>` | every build | multi-arch manifest (what stack pins) |
| `:pr-<N>` | PR builds | updated each push to the PR head |
| `:<branch-name>` | branch push | useful for "follow main" deploys |
| `:latest` | unreal60_dev push only | (current behavior preserved) |
| `:cache-amd64`, `:cache-arm64` | every build | cache layer registry refs |

## Migration plan

The reusable workflow file (`docker-build-push.yml`) currently lives
on `feat/h4ks-fixes`, not `unreal60_dev`. Branch off `feat/h4ks-fixes`
so the rewrite is an in-place modification — PR'ing from `unreal60_dev`
would introduce a duplicate file and guarantee a merge conflict the
day `feat/h4ks-fixes` lands.

1. Branch off `feat/h4ks-fixes` as `feat/faster-docker-builds`.
2. Rewrite `.github/workflows/docker-build-push.yml` to the
   matrix-build + manifest-merge shape (drop QEMU).
3. Caller `docker-obbyircd.yml` stays untouched.
4. Smoke-test via PR-triggered build: three green checks (amd64 build,
   arm64 build, manifest) and a multi-arch `:pr-N` tag on Docker Hub.
5. Verify a no-source-change push reuses cache and completes in
   <2 min per arch.
6. PR to `feat/h4ks-fixes`. When `feat/h4ks-fixes` later merges to
   `unreal60_dev`, the modernization rides along.
7. Downstream branches (`feat/cotturn-support`, future h4ks work)
   inherit on rebase.

## Risks

- **`ubuntu-24.04-arm` availability**: free public-repo runners since
  Apr 2025 per GitHub blog. Confirmed available for this org if it
  has public repos in good standing — verify at workflow dispatch time.
- **Manifest race on concurrent pushes**: two PRs landing seconds apart
  could interleave their `:latest` updates. Acceptable for `:latest`;
  the per-sha tags are deterministic so deploys can pin those.
- **Cache poisoning across branches**: shared `cache-amd64` ref between
  branches. Buildx handles this safely (content-addressed) but feature
  branches may cold-start the cache after main rebases. Tolerable.

## Out of scope

- Replacing `mattfly/*` Docker Hub repos with GHCR — separate decision
  about hosting/billing.
- Per-PR ephemeral environments.
- Speeding up the C build itself (e.g. ccache layer). Easy follow-up
  once the matrix is in place.

## Wall-clock estimate

| Phase | Current | Target |
|---|---|---|
| amd64 build | (parallel) ~5 min | ~3-4 min (native, cached layers) |
| arm64 build | (qemu, ~15 min) | ~3-4 min (native) |
| manifest | n/a | ~10 sec |
| **total wall-clock** | **~17 min** | **~4 min** |
