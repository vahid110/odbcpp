# Redshift development and qualification cadence

Human approved on 2026-10-09. This changes execution cadence, not product scope,
release quality, native-test authority or the rules for parked lanes. It supersedes
older instructions to run the entire local graph and full CI for every feature
commit. Existing goal deliverables and final acceptance gates remain intact.

- Develop coherent workflow batches on macOS/iODBC UCS4. Run meaningful focused
  tests while coding, then the relevant primary-profile regression, staging,
  architecture and package checks. Keep independent consequential source and
  integration review, exact source/binary evidence and owned-process cleanup.
- Before choosing extra checks, record the affected contracts and concrete risk.
  Unicode representation or pointer/layout changes need affected width and
  sanitizer checks; platform code needs its platform; crypto/provider, public ABI,
  build and package changes need affected configurations. Run these promptly,
  rather than deferring a known risk to tomorrow. A filename alone is not a reason
  to repeat every profile. Review cannot prove an unexecuted platform passed.
- Reviewed, primary-qualified intermediate commits may be pushed while full
  cross-platform acceptance is pending. `.github/workflows/primary.yml` runs the
  existing UCS4 unit, ordinary PostgreSQL and verified-TLS job on pushes. That
  PostgreSQL product profile is primary regression, not native Redshift proof.
- The full `.github/workflows/ci.yml` retains every existing job and runs on pull
  requests or explicit dispatch. Root uses the existing 15-minute continuation
  to dispatch it on the exact pushed branch at the first eligible wake when
  24 hours since the previous full start is due and changed commits need
  qualification; record host/active-run delays as overdue debt. Dispatch sooner for
  a concrete cross-platform risk or release acceptance. Do not replay an already
  accepted unchanged head. There is no new timer or default-branch-only cron.
- Record the exact workflow, branch, head, run ID and UTC start durably in private
  `continuation/ci-cadence.json`. Check actual active runs before dispatch; adopt
  an existing matching run. Distinct primary/full concurrency groups explicitly
  use noncancelling queue:max (GitHub limit100 pending); do not dispatch when a
  queue is full, and report that concrete capacity wait. One root-owned full-CI
  watcher follows one immutable
  commit. Do not cancel it on a newer push or start a duplicate. Later work may
  proceed; its bytes are not qualified by the earlier run. Overdue work stays
  explicitly overdue while a full run is active, then tests the latest ready head.
- Collect actual failures and make one coherent reviewed correction batch where
  related. Re-run affected checks; retain unrelated accepted evidence only when
  source and executed inputs prove applicability. Repeat native tests only for
  changed native behavior or a concrete execution gap, under existing authority.
- A feature is implemented/primary-qualified before it is cross-platform
  accepted. Full goal/release acceptance still needs its required complete graph,
  exact-head green full CI, independent final audit, reaps and PostgreSQL shutdown.
  A primary green, an ancestor green or elapsed 24 hours cannot establish this.

Keep the dedicated API worker implementing the next ready product package while
root and reviewers qualify and integrate. Native Redshift and application proof
remain distinct. This cadence creates no cloud, credential, principal, resource,
network, spending or authorization-horizon extension. Both existing timers and
periodic/ad hoc reviewed build cleanup remain active.
