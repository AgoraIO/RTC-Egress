Here’s a focused review of the RTC‑Egress image build workflows for production and debug, with concrete recommendations.

  What’s good

  - Single build → reuse binaries: You build once (builder stage), extract artifacts, and then build each service image from those artifacts. This avoids per‑image rebuilding and keeps versions consistent.
  - Clear separation: Two workflows (prod vs debug). Debug adds “-debug” tags and uses dedicated Dockerfile.debug targets.
  - Sensible tagging scheme: Branch/PR tags; “latest” on default branch; “stable” on tagged releases; date+sha tags for traceability.
  - GHCR login + metadata labels: Using docker/login-action and metadata-action is sound.

  Gaps and risks

  - Single‑arch only: platforms: linux/amd64. Missing linux/arm64 limits users on ARM nodes (EKS Graviton, M1/M2).
  - No build cache: Buildx is set up, but there’s no cache-from/to (type=gha). Cold builds every time → slower CI, higher cost.
  - No SBOM/provenance/signing: sbom: false, provenance: false. No cosign attestation. For production, this is a missed security/supply‑chain control.
  - No image scanning: No Trivy/Grype scan before push. Vulnerabilities could slip through.
  - Tagging misses plain semver: You generate semver-derived tags with date+sha prefixes, but not the simple semver tag (e.g., v1.2.11). Many consumers expect a clean semver tag.
  - Not multi-stage monorepo caching: You extract binaries via a temp builder container each run. It works, but you could use native Go caching (actions/setup-go + go build) or cache the docker build itself to speed it up.
  - Debug workflow is manual-only (workflow_dispatch): Fine by design, but consider optional auto-trigger on main tags or branches if you want always-available debug images.

  