# Trusted time for provisioned named roles (#100)

The provisioned native named-profile owner now takes a selected `TrustedUtcSource`.
Its production entry selects the Darwin system source; static-key profiles still
use their existing path without requiring clock quality. A role never falls back
to the source principal's keys. Unsupported or unsynchronized platforms refuse
before STS. The existing native owner remains Darwin-only; this does not add a
Windows/Linux native provider or establish live Redshift qualification.

## Contract and ownership

| Component | Physical owner | Boundary/proof |
| --- | --- | --- |
| UTC reading, bracket, horizon and suspend-aware retained guard | `sdk/internal/odbcpp/auth/trusted_clock_sample.h`, `sdk/src/auth/trusted_clock_sample.cpp` | Provider-neutral, standard C++/chrono only; focused clock GoogleTests |
| Read-only host source | `sdk/src/auth/system_trusted_clock.cpp` | SDK platform primitive; Darwin kernel APIs, no AWS/network calls |
| Role acquisition and immutable credential expiry | `sdk/src/auth/aws/named_profile_acquisition.cpp` | Optional AWS owner; reuses `convert_temporary_validity` |
| Later signing/dispatch checks | `sdk/src/auth/aws/provisioned_native_owner.cpp` | Frozen source must satisfy both expiry clocks at use, including after callbacks |
| Credential-free signing evidence | `sdk/src/auth/aws/provisioned_native_http_ingress.cpp` | Compares signed credential/token to frozen source; retains booleans only |

The private header is owned in `sdk/header_owners.txt` and staged in the existing
AuthCore manifest. No backend protocol semantics or ODBC behavior moves into this
contract. The optional native consumer continues to use the same SDK archive.

## What the host source trusts

The OS time discipline is the selected UTC authority. This is **not** proof from
an ordinary `system_clock::now()` read, an authenticated remote time service, or a
hardware accuracy certification. A compromised host, time daemon or its upstream
clock is outside this source's trust boundary, just as a compromised trust store
is outside TLS validation's trust boundary.

Darwin `ntp_gettime` reports UTC and maximum/estimated error. A read-only
`ntp_adjtime` call (`modes=0`) supplies synchronization status and frequency
 tolerance. The adapter requires `TIME_OK` and active discipline, and refuses
unsynchronized/hardware-error/leap/PPS-error states, unavailable/negative bounds,
range overflow or bounds exceeding the admission policy. The maximum of both
reported error estimates is used, plus one microsecond for conversion precision.
The source does not enable NTP, change host time, alter configuration or query a
network service.

Darwin's kernel exports `tolerance = MAXFREQ * SCALE_PPM`, with the intentionally
truncated `SCALE_PPM = 65536 / 1000`. The adapter inverts that actual ABI and uses
checked integer ceiling to derive the full requested-horizon drift allowance.
Treating this value as ordinary exact scaled ppm would underestimate Darwin's
error-growth model. The bound is conditional on the host time discipline's
reported quality and oscillator model; it is not an independently measured rate.
See Apple's [kernel NTP implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_ntptime.c)
and [time discipline ABI](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/timex.h).

## Suspend and exclusive expiry

The UTC read is bracketed by the existing steady clock and by Darwin
`CLOCK_MONOTONIC_RAW`, which advances during sleep. That raw elapsed clock is
retained with the frozen credential. The original checked converter subtracts
UTC uncertainty and the horizon drift bound and derives an exclusive steady
expiry from the **earliest** bracket point. A second exclusive expiry is derived
from the same conservative duration and the earlier suspend-aware bracket point.
Both bounds must pass; zero *additional* suspend allowance is valid only with
this mandatory second guard. It must never be interpreted as a claim that the
ordinary steady clock advances during sleep.

See Apple's [continuous time documentation](https://developer.apple.com/documentation/kernel/mach_continuous_time()),
which distinguishes continuous time from absolute time and recommends
`CLOCK_MONOTONIC_RAW` for nanoseconds.

A missing elapsed read, rollback or expiry permanently retires the guard; it
cannot heal after time moves back. UTC is never resampled to extend existing
credentials. The raw issuer UTC horizon is checked separately, so subtracting
errors cannot disguise an issuer lifetime beyond the source's quality horizon.
The named owner retains the existing 10-second sample age/bracket/error ceilings
and one-hour maximum quality horizon. These are refusal limits, not invented
accuracy margins. STS still requests 900 seconds.

## Evidence and acceptance

Offline GoogleTests exercise inline role and `source_profile`, frozen signing
with the assumed key/session token, source-file retirement after freezing,
generation retirement during/after acquisition, short expiry, suspend expiry and
suspend during signing, missing/excessive quality, bounded slow UTC, stale reads,
rollback, range overflow and equality at expiry. Refusal assertions distinguish
zero STS sends from zero database-credential sends.

The system-source test performs read-only local clock queries and truthfully
skips if the host cannot supply quality. Synthetic fixtures establish contract
behavior; neither they nor a successful host clock read establish successful
AWS role acquisition or verified-TLS Redshift login. Provisioned Redshift live
acceptance remains separately scheduled, after independent review and required
platform builds. Serverless is not a replacement for that provisioned path.
