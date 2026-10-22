# Historical workflow snapshots

These files preserve the exact policies and presets from main
`0b330e4214b415565a45d1529024c7f9ad35d92d` before the human-authorized
2026-10-20 simplification. `manifest.json` records SHA256 and original paths.
They are evidence, **not active instructions**. In particular, historical worker,
branch, timer, financial-probe, migration-release and continuation instructions
must not be executed from this archive. The current canonical workflow is
[DEVELOPMENT_WORKFLOW.md](../../../DEVELOPMENT_WORKFLOW.md).
Snapshot suffixes prevent archived agent/skill files from acting as active policy.

Private handoffs and protected admission/input receipts remain private. The
Windows coordinator archives the complete superseded handoff byte-for-byte next
to its compact replacement under the existing migration state directory. No
implementation, tests, raw results, original failures or protected inputs are
deleted by this workflow change. Existing migration qualification debt remains
open until separately authorized work actually satisfies it.
