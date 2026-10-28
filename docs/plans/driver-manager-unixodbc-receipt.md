# Producing the unixODBC 2.3.14 manager receipt

unixODBC reports `03.52.0002.0003` for several patch releases, so the known
unixODBC 2.3.14 Driver Manager deviations apply only when the build is given a
reviewed receipt for the exact manager library. Without one, every manager test
keeps its direct expectation. `cmake/DmQualifiedIdentity.cmake` validates the
receipt; `tests/test_dm_identity.h` (`qualified_unixodbc2314`) consumes it.

The receipt is a private build input. Keep it beside the run evidence, not in
the repository, and produce a new one for every host whose manager library
differs.

## Schema

```json
{
  "schema_version": 1,
  "family": "unixODBC",
  "version": "2.3.14",
  "library_path": "/usr/lib/x86_64-linux-gnu/libodbc.so.2.0.0",
  "library_sha256": "<64 lowercase hex digits>",
  "platform": "Linux",
  "pointer_bytes": 8,
  "sqlwchar_bytes": 2,
  "driver_sqlwchar_bytes": 2,
  "odbc_version": 3,
  "evidence": "<how the identity above was established>"
}
```

Configure rejects the receipt unless all of these hold:

| Field | Requirement |
|---|---|
| `schema_version`, `family`, `version` | Exactly `1`, `unixODBC`, `2.3.14` |
| `platform` | Equals `CMAKE_SYSTEM_NAME`, which must be `Linux` or `Darwin` |
| `pointer_bytes` | `8`, and the build is 64-bit |
| `sqlwchar_bytes`, `driver_sqlwchar_bytes` | `2`, matching `ODBCPP_EXPECT_DM_SQLWCHAR_SIZE` and `ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE` |
| `odbc_version` | `3` |
| `library_path` | Absolute and existing; its real path equals the real path of the selected `ODBC_DRIVER_MANAGER_LIBRARY` |
| `library_sha256` | Lowercase SHA256 of that file, rechecked against a build-local snapshot |
| `evidence` | Non-empty |

The build must also select `ODBC_DRIVER_MANAGER_FLAVOR=UNIXODBC`.

## Establishing the identity on Debian/Ubuntu

Resolve the library the build will link, then tie it to its package and version:

```sh
lib=$(readlink -f "$(pkg-config --variable=libdir odbc)/libodbc.so")
pkg=$(dpkg -S "$lib" | cut -d: -f1)
dpkg-query -W -f='${Package} ${Version} source=${source:Package} ${source:Version}\n' "$pkg"
dpkg --verify "$pkg"                      # no output: installed files are unmodified
cat /var/lib/dpkg/info/"$pkg":*.md5sums | grep " ${lib#/}\$"
md5sum "$lib"                             # must equal the md5sums entry
sha256sum "$lib"                          # library_sha256
strings "$lib" | grep -E -x '2\.3\.[0-9]+' | sort -u   # embedded release: 2.3.14
```

Accept the identity only when the package source version is 2.3.14, `dpkg
--verify` is clean, the file's md5 equals the package's md5sums entry and the
embedded release string is `2.3.14`. On other platforms or installs (Homebrew,
a source build), record the equivalent provenance: the package manifest or build
log naming 2.3.14 and a check that the installed file is the one it produced.

Write `evidence` from these facts only, for example: "Ubuntu package libodbc2
2.3.14-1 (source unixodbc 2.3.14-1); dpkg --verify clean; md5sums entry equals
the file; embedded release 2.3.14". Don't claim the library matches one used in
another run unless you compared the hashes.

## Using it

Pass the receipt at configure time, in a separate build tree so an unqualified
tree keeps its own images:

```sh
cmake --preset unixodbc-redshift -B build-unixodbc-redshift-dmq \
  -DODBCPP_DM_QUALIFIED_UNIXODBC_RECEIPT=/path/to/unixodbc-2.3.14-receipt.json
```

Confirm it applied before running tests:

```sh
grep -E 'QUALIFIED_(VERSION|SHA256|PLATFORM)' build-unixodbc-redshift-dmq/dm-identity/dm_qualified_identity.h
```

`ODBCPP_DM_QUALIFIED_VERSION` must be `"2.3.14"` and the SHA256 must match the
receipt; an empty version means the receipt wasn't applied. The value is cached,
so a reconfigure of the same tree keeps it. At run time the tests also compare
the loaded manager's bytes with the snapshot, so replacing the library after
configure makes them fall back to direct expectations.

The issue #117 Driver Manager twins used this procedure on WSL Ubuntu
(`libodbc2` 2.3.14-1) and passed 487/487 at `e933945`.
