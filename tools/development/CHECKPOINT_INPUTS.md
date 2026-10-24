# Live checkpoint inputs and platform boundary

`live_checkpoint.py` is the normal entry point. `prepare` snapshots **public**
paths, source, tool/image hashes and complete inventory; `preflight` configures
the real registrar with empty synthetic environments. Neither reads credentials,
executes owning images or contacts a provider. `run` alone opens a separately
reviewed protected finite operational admission and composes the checked-in
resource, fixture, input and CTest lifecycle. There is no private Python import.

```sh
python3 -B tools/development/live_checkpoint.py prepare --recipe /absolute/public/recipe.json --output /absolute/new/plan.json
python3 -B tools/development/live_checkpoint.py preflight --plan /absolute/new/plan.json --output /absolute/new/preflight
# Only after human resume, exact-source green applicable CI and existing admission:
python3 -B tools/development/live_checkpoint.py run --plan /absolute/new/plan.json --admission /absolute/private/admission.json
```

Windows uses the same entry through its **local WSL** resource owner. Use POSIX
paths for recipe/plan/admission/output in this form, and matching checked-in
Windows and WSL source revisions. AWS/admin credentials stay in WSL. The native
backend receives only test-principal inputs on stdin, holds owner/System-only
files and newly owned HKCU DSNs, runs one bounded configure/CTest collection in
Windows kill-on-close jobs, and verifies reader/input/DSN/lease cleanup before
WSL performs exact-principal/fixture cleanup and pause. No SSH, scheduled task,
generic executor or GUI test is involved in the live backend.

```powershell
python -B tools/development/live_checkpoint.py preflight --wsl-source /home/vahid/code/odbcpp-workflow --plan /absolute/plan.json --output /absolute/new/preflight
python -B tools/development/live_checkpoint.py run --wsl-source /home/vahid/code/odbcpp-workflow --plan /absolute/plan.json --admission /absolute/private/admission.json
```

The native working directory must first be a fresh private directory created
locally with the current user's exact SID (no elevation or registry mutation):

```powershell
python -B tools/development/windows_live_backend.py --initialize-state C:/absolute/new/checkpoints --sid <current-user-SID>
```

Windows driver registration remains an existing approved installation/fixture
prerequisite. The receiver verifies the registered exact driver and mapping; it
does not install/overwrite HKLM entries. Native GUI checks and privileged full
PostgreSQL/System DSN tests remain separate local desktop commands in `COMMANDS.md`.
Package/vendor/macOS-only and unsupported-width boundaries remain explicit unrun
entries in the accepted applicability record; they are not silently omitted.

## Public recipe

The JSON recipe has these fields. Paths are absolute. This is a data interface,
not executable callbacks or scripts. `prepare` writes a new plan and refuses an
existing output; use fresh owned names. It computes pins/counts rather than
requiring manual hash/count reconstruction. Never supply credential-bearing files
as public inventory, profiles, specs, tools or receipt inputs.

| Field | Contents |
| --- | --- |
| `state_directory`, `run_name` | Existing private POSIX state root and one-use run name; native names start `windows-live-` |
| `index`, `expected` | Public complete applicable registrar index and independently sourced ordered expected inventory |
| `profiles`, `specs` | Public current profile definitions and corresponding ordered runtime specs; no connection values |
| `tools` | Absolute `cmake`, `ctest`, `aws`, `psql`, `python` executables on the resource host |
| `ci_receipt` | Default run gate: exact `source_head` and `applicable_ci_green: true`, with retained CI evidence. A missing/pending/failed CI receipt never becomes green through local validation |
| `validation_mode`, `validation_profile`, `local_validation` | Omit for the default CI gate. The explicit human-directed local exception below requires all three, a pinned public local receipt and its actual complete offline evidence |
| `authority_receipts` | Existing `authority`, `applicability`, `cleanup_delegation` receipt paths for standing fixture authority |
| `resource` | Legacy/default `provisioned`: existing `account`, `arn`, `cluster`, `created`, `region` fixed to `eu-north-1`, `profile`, `node_type`. Explicit `kind: serverless` uses the strict existing workgroup/namespace binding below; no infrastructure expansion |
| `security_groups` | Approved current public security-group describe snapshot, required for run and compared exactly before resume; preflight may report it pending |
| `ca_sha256` | Approved CA hash; actual CA/hostname/config remain protected |
| `timeout_fixture` | `table` and `expected_principal` (`database`, `owner`, `owner_oid`, `schema_oid`, `schema_owner_oid`) from current approved identities; no historical OID defaults |
| `native_windows` | Omit for POSIX-owning binaries; native fields below for Windows |

Serverless resource keys are exactly `kind`, `account`, `arn`, `region`, `profile`,
`workgroup`, `workgroup_id`, `namespace`, `namespace_id`, `namespace_created`,
`base_capacity`, `max_capacity`, `endpoint`, `port`, `security_group_ids`, and
`max_query_execution_time`. Only the existing `odbcpp-redshift-pilot` workgroup
and namespace are accepted, with their independently recorded exact IDs and
creation timestamp, existing endpoint/security group, 4/4 RPU caps and 30-second
query cap. Acquisition checks current identity, TLS, current single-address
network ingress and all resource bindings; it never creates, updates, resumes
or pauses Serverless. `AVAILABLE` metadata is not SQL readiness or physical
compute-idle evidence. Serverless cleanup separately verifies owned readers,
principals, queries and fixture cleanup, then publishes
`owned_activity_quiescent`; `paused_verified` and
`physical_compute_idle_verified` remain false.

For Serverless, `timeout_fixture` contains exactly `table`,
`expected_principal: null`, `allow_current_identity: true`, and
`resource_kind: "serverless"`. An admitted read-only identity/privilege query
binds the current existing database/test principal/schema OIDs before any
fixture mutation. Missing or changed identities refuse; historical provisioned
OIDs, automatic principal creation and guessed passwords are not substitutes.
The normalized protected config still has the same eight keys; a historical
Serverless config containing `ca_file`, `schema` or `table` must be explicitly
normalized and privately pinned before use.

`prepare` obtains actual source directory/head from its checkout. The normal run
requires that checkout clean at that exact head. For Windows, public recipe
`index` contains native absolute image paths; `prepare` writes its resource-host
mapped index and a native model containing exactly the same cases/specs/profiles.
It does not create a smaller Windows inventory from a count.

Native fields: `source_directory`, `working_directory`, new public `model` path,
`ca`, `python`, `sid`, `driver: {path}`, `driver_description`,
`tools: {cmake, ctest, ninja, git}`, and `runtime_directories`. These are Windows
absolute paths. The preparation command computes model/driver/tool/module/DLL/
owning-image pins. Resource preflight checks that source, CA, profile/input and
ordered inventory bindings agree across the boundary. Native execution checks
the clean actual source, SID/default owner, held pins, registration and one-use
state before staging test inputs. All native reader/cleanup receipts remain raw.

## Protected admission

The existing operator produces a private owner-only 0600 JSON admission, not Git
content. Fields consumed by this entry: `reviewed: true`, `plan_sha256`,
`source_head`, `inventory_scope: "complete_applicable"`, `run_name`, one-use
`nonce`, `expires_utc`, `credential_source`, `database_config` and `bounds`.
`bounds` uses the existing lifecycle owner/child/aggregate/execution/availability/
cleanup_reserve/recovery/case fields and maxima; no runtime increase is implied.
The admission must cover the complete owner plus cleanup horizon.
`expires_utc` is the earliest applicable operator or credential-session expiry;
the entry rechecks its remaining horizon immediately before resource acquisition.
Plan/source
pins and exclusive state consume one run; no automatic renewal/retry/reset.
This schema records an already granted authority, never creates it.

The protected database config retains exactly the existing host/port/database,
admin/test users and passwords, and CA path. Resource acquisition reuses
`native_auth_inputs`, verified TLS, exact AWS/cluster identity and strict network
checks. Existing 40-helper/450-inclusive fixture limits and AWS operation bounds
remain. Billing stays manual. Complete fixture data/SQL is checked in with
extraction provenance; old source074/089/092 files and consumed history are evidence
only. Actual provider/TLS/driver execution acceptance of this refactor is pending
the next separately authorized checkpoint. Offline proof grants no live authority.

## Explicit human-directed Windows-first local validation

An explicit human sequencing direction may replace the normal green-CI prerequisite
with a pinned, reviewed coordinator local-qualification receipt. This uses the same
trusted assessment boundary as the existing CI receipt; the launcher does not
reimplement CI's XML/inventory/configuration analysis. CI remains pending, not
accepted. All source/image, private-input, identity/TLS/network, cap, deadline,
fixture, one-use operational admission and cleanup checks remain unchanged.

Set recipe `validation_mode: "human_directed_local"`, `validation_profile` to
`windows-redshift` (with `native_windows`) or `unixodbc-redshift` /
`linux-iodbc-redshift` (without it), and `local_validation` to the public receipt
path. `prepare` pins it. Missing/null CI is permitted only in this explicit mode;
an unknown ignore flag, monkeypatch or fabricated green-CI record is not supported.
The coordinator produces this receipt only after actual local success:

```json
{
  "schema_version": 1,
  "kind": "human-directed-local-validation-v1",
  "reviewed": true,
  "status": "completed",
  "human_direction": "windows-first-then-wsl",
  "human_direction_ref": {"path": "/absolute/public/human-direction.json", "sha256": "<hash>"},
  "complete_applicable_offline": true,
  "ci_acceptance": false,
  "ci_status": "pending_not_accepted",
  "source_head": "<actual clean checkpoint control head>",
  "producer_head": "<actual clean binary build/test head>",
  "profile": "windows-redshift",
  "build_exit": 0,
  "ctest_exit": 0,
  "runtime_index_sha256": "<same index hash as plan>",
  "profiles_sha256": "<same profiles hash as plan>",
  "evidence": {"path": "/absolute/public/local-assessment.json", "sha256": "<hash>"}
}
```

The pinned human record retains the actual human direction, rather than a magic
mode string standing in for authorization. The pinned coordinator assessment
retains successful actual terminal build/full applicable offline CTest, complete
unfiltered inventory, top-level native XML/body totals and explicit skips, actual
commands/environment, owning source/cache/product/DM/SQLWCHAR/crypto profile,
current images and cleanup evidence. Original failures remain retained. Root
checks these associations before issuing `reviewed: true`; the launcher checks
the pinned review, completed integer0 outcomes, source/profile and runtime index /
profile bindings. Nested intentional negative-control XML retains its enclosing
CTest semantics. A focused subset or arbitrary synthetic cache is not sufficient
for the coordinator to issue a real qualification receipt. Public synthetic unit
test receipts test this contract and never qualify a product or admit resources.

`producer_head` may differ only when Git proves the complete source difference
confined to `live_checkpoint.py`, `test_live_checkpoint.py` and this document.
This allows the reviewed control-only change after a frozen build; a C++, build,
header, fixture or other runtime-control change needs fresh local validation.
Current live owning-image/driver bytes are still checked by the normal plan.
Windows and local WSL source checkouts must be clean at the admitted control head.

Before WSL, the local receipt also needs `windows_success: {path, sha256}` pointing
to the coordinator's completed Windows qualification receipt. That record has
`reviewed: true`, `status: "completed"`, `profile: "windows-redshift"`, actual
`source_head`, `complete_applicable_windows_live: true`, `cleanup_complete: true`,
integer `actual_exit: 0`, and a pinned `evidence` assessment. The coordinator's
assessment binds the complete Windows native and relationship partition union,
actual body results and physical cleanup; one unrelated or native-only successful
run is insufficient. The same product-source rule permits an earlier control
revision. For this Windows sequencing check only, changes confined to the POSIX
`redshift_profile_inputs.py` stager and its `test_redshift_profile_inputs.py`
regression are also permitted: WindowsOperations overrides input opening and
staging and does not execute that stager. This exception does not permit reuse
of an older WSL compiled-producer/local-validation receipt after a stager change;
fresh WSL local validation and live fixture qualification are still required.
This is sequencing evidence, never new operational authority. Full CI,
all required platform/profile and release gates remain debt until actually passed.
