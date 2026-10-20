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
| `ci_receipt` | Optional during preparation; required for run: exact `source_head` and `applicable_ci_green: true`, with retained CI evidence |
| `authority_receipts` | Existing `authority`, `applicability`, `cleanup_delegation` receipt paths for standing fixture authority |
| `resource` | Existing `account`, `arn`, `cluster`, `created`, `region` fixed to `eu-north-1`, `profile`, `node_type`; no infrastructure expansion |
| `security_groups` | Approved current public security-group describe snapshot, required for run and compared exactly before resume; preflight may report it pending |
| `ca_sha256` | Approved CA hash; actual CA/hostname/config remain protected |
| `timeout_fixture` | `table` and `expected_principal` (`database`, `owner`, `owner_oid`, `schema_oid`, `schema_owner_oid`) from current approved identities; no historical OID defaults |
| `native_windows` | Omit for POSIX-owning binaries; native fields below for Windows |

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
