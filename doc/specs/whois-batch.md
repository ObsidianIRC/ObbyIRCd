---
title: "`obby.world/whois` Batch Type"
layout: spec
work-in-progress: true
copyrights:
  -
    name: "Valerie Pond"
    period: "2026"
    email: "v.a.pond@outlook.com"
---

## Notes for implementing work-in-progress version

This is a vendor-specific extension defined under the `obby.world/`
namespace. Software implementing this specification MUST use the
`obby.world/whois` and `obby.world/whois-session` batch type names
verbatim. The IRCv3 `BATCH` extension is a prerequisite. If this
extension is later promoted to an IRCv3 draft, the unprefixed name
(e.g. `whois`) will be reserved by the IRCv3 spec; until then no
unprefixed alias is implied.

## Introduction

`WHOIS` has remained a stream of independent numerics terminated by
`RPL_ENDOFWHOIS` (`318`) since the original spec. As IRCds grew
features that benefit from per-user disclosure — TLS state, GeoIP,
custom metadata, multiple concurrent sessions for one account — the
reply grew an open-ended set of numerics (`311`, `312`, `313`,
`317`, `319`, `320`, `330`, `338`, `378`, `379`, `671`, `276`, …)
with no structural relationship between them. RFC 2812 §3.6.2 also
states that, with the exception of `RPL_WHOISCHANNELS` (`319`), each
numeric MUST appear at most once per reply. This makes it difficult
to cleanly disclose information that is inherently per-session (such
as the IP, hostname, and TLS state of each connected client of a
multi-session account).

This extension adds two cooperating IRCv3 `BATCH` types,
`obby.world/whois` and `obby.world/whois-session`, that wrap the
existing `WHOIS` numerics so that:

1. The full `WHOIS` reply is grouped under a single
   `obby.world/whois` batch identified by the queried target's nick,
   letting clients render it as a cohesive block (a profile card,
   collapsible group, transient toast, etc.).
2. Per-session details (`RPL_WHOISHOST` `378`, `RPL_WHOISMODES`
   `379`, `RPL_WHOISSECURE` `671`, `RPL_WHOISCERTFP` `276`, and
   vendor numerics carrying GeoIP / ASN / TLS information) for a
   multi-session account are grouped under nested
   `obby.world/whois-session` sub-batches, each tagged with a
   1-based session ordinal, so the same numeric can appear once per
   session without ambiguity to the receiving client.

Numerics keep their existing semantics. Clients that have
negotiated `batch` see the structure; clients that have not
negotiated `batch` see the same numerics they always did. No new
WHOIS numeric is introduced by this extension.

## Implementation

### Capability

This extension defines a single client capability:

    obby.world/whois

Servers that implement this extension MUST advertise this capability
and MUST also advertise the standard [`batch`][batch] capability,
which `obby.world/whois` depends on.

Clients that wish to receive batched WHOIS MUST negotiate BOTH
`batch` and `obby.world/whois`. Servers MUST NOT emit
`obby.world/whois` or `obby.world/whois-session` batches to clients
that have not negotiated this capability, even if the client has
negotiated `batch`. This is intentional: although the IRCv3
[`batch`][batch] specification requires consumers to tolerate
unknown batch types, the per-session sub-batch path emits multiple
`RPL_WHOISHOST` / `RPL_WHOISMODES` / `RPL_WHOISSECURE` numerics
per `WHOIS`, and strict RFC 2812 §3.6.2 parsers may discard the
duplicates. The explicit opt-in lets the server give legacy
clients exactly one of each numeric.

### `obby.world/whois` batch

When a client that has negotiated `batch` issues `WHOIS` (or
`WHOIS <server> <nick>`), the server MUST wrap all reply numerics
emitted for that query, including `RPL_ENDOFWHOIS` (`318`), inside
a single `obby.world/whois` batch:

    :server BATCH +<ref> obby.world/whois <target-nick>
    @batch=<ref> :server 311 <querier> <target-nick> <user> <host> * :<realname>
    @batch=<ref> :server 312 <querier> <target-nick> <server-name> :<server-info>
    @batch=<ref> :server 319 <querier> <target-nick> :<channel-list>
    @batch=<ref> :server 317 <querier> <target-nick> <idle> <signon> :seconds idle, signon time
    @batch=<ref> :server 318 <querier> <target-nick> :End of /WHOIS list.
    :server BATCH -<ref>

The single parameter on the `BATCH +<ref>` line is the queried
target's nick, exactly as it appears in the contained `RPL_*`
numerics. This lets clients deduplicate concurrent `WHOIS` queries
by the parameter rather than by the batch reference.

The server MUST NOT emit unbatched WHOIS numerics when both `batch`
and an `obby.world/whois` batch are in scope for the same query.

### `obby.world/whois-session` batch

When the queried target has more than one connected session under
its account (typically because the persistence / multi-client
feature is in use) AND the querier has the privilege to see
connection-level details (typically: the querier is the target, or
is an IRC operator with the relevant access privilege), the server
MAY emit one or more `obby.world/whois-session` sub-batches nested
inside the parent `obby.world/whois` batch.

A `obby.world/whois-session` batch carries a 1-based session
ordinal as its first parameter, and an OPTIONAL count of total
sessions as the second parameter. The opening line additionally
carries the vendor `obby.world/since` message tag, whose value is
the ISO 8601 UTC timestamp at which this session's TCP connection
registered with the server:

    @batch=<parent-ref>;obby.world/since=<iso8601> :server BATCH +<sub-ref> obby.world/whois-session <ordinal> [<total>]

Nesting under the parent `obby.world/whois` batch is signalled by
the `batch` message tag on both the opening and closing `BATCH`
lines, per the IRCv3 [`batch`][batch] specification. The session
ordinal is a server-assigned 1-based integer; the optional total
allows clients to label each block (e.g. "Session 1 of 3"). The
closing `BATCH -<sub-ref>` line MUST also carry the
`@batch=<parent-ref>` tag.

Inside the sub-batch the server emits whichever per-session
numerics it has to disclose:

  - `RPL_WHOISHOST` (`378`) — the session's real hostname and IP
  - `RPL_WHOISSECURE` (`671`) — the session's TLS state and cipher
  - `RPL_WHOISCERTFP` (`276`) — the session's TLS client certificate
    fingerprint, if any
  - `RPL_WHOISIDLE` (`317`) — the session's idle time (seconds since
    last activity on this connection) and signon time (when this
    connection registered)
  - `RPL_WHOISCOUNTRY` (`344`) — GeoIP country code / name for the
    session's IP, if GeoIP data is available
  - `RPL_WHOISASN` (`569`) — ASN / AS name for the session's IP, if
    available

`RPL_WHOISMODES` (`379`) is intentionally NOT per-session. In an
implementation that synchronises umode and snomask changes across
every attached session (as obbyircd does via its persistence module),
all sessions share identical flags by construction, so emitting `379`
N times would be wire-level duplication for the same value. The
server MUST emit `379` once inside the parent `obby.world/whois`
batch alongside the other account-level numerics.

The server SHOULD emit per-session numerics in ascending ordinal
order and SHOULD NOT emit the same per-session numeric outside the
session sub-batches for the same query.

Each sub-batch MUST be closed with `BATCH -<sub-ref>` before the
parent batch is closed.

### Session-count summary

When the queried account has two or more live sessions but the
querier does not satisfy the per-session disclosure gate (i.e. the
querier is neither the target nor an IRC operator), the server
SHOULD emit a single `RPL_WHOISSPECIAL` (`320`) line inside the
parent batch:

    @batch=<parent-ref> :server 320 <querier> <target> :is connected from <N> sessions

This lets the receiving client render a "multi-session" affordance
(e.g. a small badge or summary) without exposing IPs, hosts, or TLS
state of any individual session.

### `obby.world/whois-security-groups` sub-batch

The legacy WHOIS form crams every security-group the target is in
into a single `320` line as a comma-separated string in the trailing
("`is in security-groups: known-users,tls-users,websocket-users`"),
which forces client renderers to either show it as one wall of text
or split the string heuristically. To give clients a structured
list, servers emitting the parent `obby.world/whois` batch MUST
replace the comma-joined `320` line with a nested
`obby.world/whois-security-groups` sub-batch containing one `320`
line per group, where the trailing carries only the group name:

    @batch=<parent-ref> :server BATCH +<sg-ref> obby.world/whois-security-groups <count>
    @batch=<sg-ref> :server 320 <querier> <target> :<group-1>
    @batch=<sg-ref> :server 320 <querier> <target> :<group-2>
    ...
    @batch=<parent-ref> :server BATCH -<sg-ref>

The first parameter of the BATCH open line is the integer count of
groups. The first group MUST be either `known-users` or
`unknown-users` (the synthetic "this account is recognised" marker
that the legacy form leads with).

Security-group membership is evaluated against the target's
canonical client, so the membership is account-level: every session
of the account observes the same group set. The legacy
comma-joined `320` line is NOT emitted to cap-on clients; non-cap
clients (no `obby.world/whois`) continue to receive it unchanged.

### Compatibility with RFC 2812

RFC 2812 §3.6.2 states that, with the exception of `RPL_WHOISCHANNELS`,
each WHOIS numeric MUST appear only once. The modern IRC document at
modern.ircdocs.horse drops this restriction. This extension takes
advantage of the modern interpretation: under the sub-batch
discriminator, repeated `RPL_WHOISHOST`, `RPL_WHOISMODES`, etc., are
unambiguous because each is contained in a `obby.world/whois-session`
sub-batch carrying a distinct ordinal. Clients that strictly parse RFC
2812 may discard later occurrences of these numerics, but no
deployed-client survey found such behaviour; in practice IRC clients
stream-print unknown / repeated WHOIS lines verbatim.

### Querier without `batch`

A querier that has not negotiated `batch` MUST receive a legacy
WHOIS reply. The server has two valid strategies:

  1. **Single arbitrary session.** Emit one set of per-session
     numerics for one arbitrarily chosen session (e.g. the most
     recently active). This is the Ergo precedent.
  2. **Concatenated text.** Collapse per-session detail into a few
     human-readable lines that fit one occurrence of each numeric.

Either is acceptable; this extension only governs the batched
form.

## Examples

A querying operator who has negotiated `batch` doing a `WHOIS` of a
two-session account `Valware`:

    C: WHOIS Valware
    S: :obby.t3ks.com BATCH +q1 obby.world/whois Valware
    S: @batch=q1 :obby.t3ks.com 311 oper Valware valware bt-net.range31-104.btcentralplus.com * :Valerie Pond
    S: @batch=q1 :obby.t3ks.com 312 oper Valware obby.t3ks.com :ObbyNet hub
    S: @batch=q1 :obby.t3ks.com 313 oper Valware :is a network administrator
    S: @batch=q1 :obby.t3ks.com 319 oper Valware :@#opers @#general +#weather #lol
    S: @batch=q1 :obby.t3ks.com 379 oper Valware +iSwx bcdfkoqsxBOS
    S: @batch=q1 :obby.t3ks.com 330 oper Valware Valware :is logged in as

    S: @batch=q1;obby.world/since=2026-05-18T08:12:33.000Z :obby.t3ks.com BATCH +q1s1 obby.world/whois-session 1 2
    S: @batch=q1s1 :obby.t3ks.com 378 oper Valware :is connecting from valware@bt-net.range31-104.btcentralplus.com 1.2.3.4
    S: @batch=q1s1 :obby.t3ks.com 671 oper Valware :is using a Secure Connection [TLSv1.3-CHACHA20-POLY1305]
    S: @batch=q1s1 :obby.t3ks.com 276 oper Valware :has client certificate fingerprint a1b2c3d4e5f6...
    S: @batch=q1s1 :obby.t3ks.com 317 oper Valware 42 1747526400 :seconds idle, signon time
    S: @batch=q1s1 :obby.t3ks.com 344 oper Valware GB :is connecting from United Kingdom
    S: @batch=q1s1 :obby.t3ks.com 569 oper Valware 2856 :is connecting from AS2856 [British Telecommunications PLC]
    S: @batch=q1 :obby.t3ks.com BATCH -q1s1

    S: @batch=q1;obby.world/since=2026-05-18T11:47:02.000Z :obby.t3ks.com BATCH +q1s2 obby.world/whois-session 2 2
    S: @batch=q1s2 :obby.t3ks.com 378 oper Valware :is connecting from valware@cgnat-public.example 10.0.0.5
    S: @batch=q1s2 :obby.t3ks.com 671 oper Valware :is using a Secure Connection [TLSv1.3-AES-256-GCM]
    S: @batch=q1s2 :obby.t3ks.com 317 oper Valware 1207 1747549322 :seconds idle, signon time
    S: @batch=q1s2 :obby.t3ks.com 344 oper Valware US :is connecting from United States
    S: @batch=q1s2 :obby.t3ks.com 569 oper Valware 15169 :is connecting from AS15169 [Google LLC]
    S: @batch=q1 :obby.t3ks.com BATCH -q1s2

    S: @batch=q1 :obby.t3ks.com 318 oper Valware :End of /WHOIS list.
    S: :obby.t3ks.com BATCH -q1

The same `WHOIS` issued by a non-operator who has negotiated `batch`
and `obby.world/whois`: the server elides the per-session
sub-batches (because the operator gate is not satisfied) and emits
one consolidated `378`, `379`, `671` for the canonical session
inside the parent batch alongside the existing public numerics,
plus a single `RPL_WHOISSPECIAL` line indicating the total session
count:

    C: WHOIS Valware
    S: :obby.t3ks.com BATCH +q2 obby.world/whois Valware
    S: @batch=q2 :obby.t3ks.com 311 user Valware valware bt-net.range31-104.btcentralplus.com * :Valerie Pond
    S: @batch=q2 :obby.t3ks.com 319 user Valware :@#opers @#general +#weather #lol
    S: @batch=q2 :obby.t3ks.com 312 user Valware obby.t3ks.com :ObbyNet hub
    S: @batch=q2 :obby.t3ks.com 313 user Valware :is a network administrator
    S: @batch=q2 :obby.t3ks.com 671 user Valware :is using a Secure Connection
    S: @batch=q2 :obby.t3ks.com 320 user Valware :is connected from 2 sessions
    S: @batch=q2 :obby.t3ks.com 330 user Valware Valware :is logged in as
    S: @batch=q2 :obby.t3ks.com 317 user Valware 42 1747526400 :seconds idle, signon time
    S: @batch=q2 :obby.t3ks.com 318 user Valware :End of /WHOIS list.
    S: :obby.t3ks.com BATCH -q2

## Design notes (non-normative)

- Sub-batches were chosen over message-tag annotations on each
  numeric because message tags inflate per-line bytes and require
  recipient parsing for every line; sub-batches are a single
  containment boundary the client reads once.
- The session ordinal is a server-assigned 1-based integer per
  query; the server is free to choose any stable ordering (e.g. by
  session connect time). The ordinal is NOT a globally meaningful
  session id and MUST NOT be relied on across separate `WHOIS`
  queries.
- Future per-session metadata (custom keys via `draft/metadata-2`,
  TLS cipher suite details, presence indicators) can be added to
  the per-session sub-batches as new vendor numerics or message
  tags without changing the batch shape.

[batch]: ../extensions/batch.html
