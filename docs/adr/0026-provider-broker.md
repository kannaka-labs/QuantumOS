# 26. QuantumOS as the Agent's Provider Broker

Date: 2026-10-24
Status: Proposed

## Context

Nick's direction (2026-10-24): QuantumOS should be the interface through which a person
or their AI gets real resources (machines, storage, GPUs, quantum computers) from OCI,
qBraid and whatever providers people ask for, paid for by the users who request them,
with Kannaka services (memory, Scientist, art) offered along the way. "Point Claude or
your AI of choice at it and off you go."

An agent can already drive a cloud CLI directly. What it cannot do safely is hold a
person's cloud account. Nothing stops it from overspending, nothing records what it
did in a form the person can audit, and nothing guarantees that what it started gets
torn down. Those are exactly the properties QuantumOS is built around: capabilities as
the only authority (ADR-0001), declared intents and enforced quotas (ADR-0008), and an
authority ledger (ADR-0009).

Part of this exists already, for one provider. The qBraid broker (#262, #269, #272,
`tools/qpu-broker`) runs guest-submitted circuits on qBraid:

- The credential lives only in the broker and never reaches the guest.
- Every money rule is enforced in the broker, not the UI: simulator by default, an
  estimate before submit from live pricing, a per-job cap, a daily budget, and per-session
  and per-client limits.
- Each job is reconciled: broker job id, provider job id and the billed cost are stored
  together.
- It went live on 2026-10-24 and ran a real Rigetti job inside a 10-credit daily budget.

The off-site backup (#281, PR #282) added a second provider (OCI Object Storage) with a
least-privilege identity: one user, one group, a policy naming one bucket, and an API
key generated on the host so that only the public half was ever pasted into OCI.

This ADR turns that pattern into the product.

## Decision

### 1. One broker, many providers

A host service, the **provider broker**, generalizes `qpu_broker.py`. Each provider is a
plugin with the same small interface:

| Call | Meaning |
|---|---|
| `offerings()` | what can be provisioned, with live prices where the provider publishes them |
| `quote(spec)` | an estimate before anything is created: cost per hour, per unit or per job, and free-tier eligibility, each labelled estimate or exact |
| `provision(spec, lease)` | create it, tagged with the lease id at the provider |
| `status(resource)` | state and metered cost so far |
| `teardown(resource)` | delete it; idempotent |
| `billed(resource)` | the provider's own cost figure, once it exists |

The first plugins are qBraid (the existing broker behind this interface) and OCI
(Object Storage buckets, then Always Free compute). GPU providers, other QPU vendors and
Kannaka services come later, as further plugins. A Kannaka service is a provider like
any other: its quote is in our credits, and its ledger rows look the same.

### 2. Lifecycle: quote, approve, provision, lease, teardown

Nothing is created without a quote, and every resource has a **lease**: an owner, an
expiry and a budget line. When the lease expires, a reaper tears the resource down.
Teardown finds resources by the lease tag, and only ever deletes ids the broker created.
That lesson is already recorded: a stray-sweep that terminated other people's pods.

### 3. Budgets the agent cannot raise

The person, not the agent, sets an **envelope**: for example, up to $5/day, these
resource classes, leases of at most 24 h. Inside the envelope the agent acts on its own.
Outside it, the broker refuses, or asks for **approval** through a channel the agent
cannot answer for: a link the person opens, or a confirmation in the console page. This
is the qBraid broker's rule, generalized: caps live in the broker, never in the prompt.
It is also ADR-0008 applied to money: a declared intent plus an enforced quota.

### 4. A ledger of what was done and what it cost

Every quote, approval, provision, teardown and billed amount is a row in the broker's
ledger: who, what, under which lease, the estimate, and the provider's billed figure
next to it. Where a QuantumOS guest made the request, the row carries the guest's
audit-ledger entry (ADR-0009), as `AUDIT_QSUBMIT` already does for QPU jobs. The person
can read their ledger at any time. Estimates and billed costs are always shown side by
side, never merged.

### 5. Who pays: their own accounts first

**Phase 1: bring your own account, self-hosted.** The person runs the broker themselves,
from the host package (ADR-0018), with their own OCI or qBraid credentials, and points
their AI at it over MCP.

- The credentials never leave their machine.
- We hold no money and resell nothing, so there are no reseller-terms issues, and abuse
  of a cloud account is the account owner's.
- For each provider, the broker prints the least-privilege setup, the way #281 did for
  OCI: the policy statements and a host-generated key whose public half the person
  pastes in.
- Revenue in this phase comes from the services on top: Kannaka memory, Scientist and
  art as paid providers, and the managed console.

**Phase 2: hosted, with credits (a separate ADR).** We run the broker for people who don't
want to. They pay us in prepaid credits (Stripe or USDC), and we provision under our own
accounts with a margin. This brings custody of money, fraud and crypto-mining abuse,
know-your-customer questions and providers' resale terms. It waits until people ask for it.

### 6. The agent surface

New MCP tools: `providers`, `quote`, `provision`, `status`, `teardown`, `budget` and
`ledger`. Every result carries the lease id and the estimate/billed split. These tools
are **added alongside** the frozen v1 tool surface (ADR-0020) and change none of its
existing tools. The golden schema gains them under a new minor version.

### 7. The guest path

QuantumOS itself reaches the broker the way it reaches the QPU today: a capability-gated
request over the serial relay, which the host broker answers. The broker never parses
the guest's intent beyond the spec grammar (ADR-0013). A citizen that holds a
provision capability can then provision within its manifest's quota. This makes
QuantumOS more than a wrapper: the OS's own capability and audit machinery governs real
cloud spend. This is a later phase; the host MCP path comes first, as an honest slice
(ADR-0017).

## Consequences

- One codebase carries every provider's money rules. A bug there spends real money, so
  every plugin needs mocked tests plus one documented real run, as #269 did, and the
  first release ships with real spending off by default.
- Leases make "forgot to turn it off" the default failure that cannot happen; the cost is
  that long-lived resources need explicit renewal.
- Self-hosted first means no custody and no billing system to build now. It also means
  users must set up their own provider accounts, which is a real barrier for
  non-technical users. Phase 2 removes it, at the cost of becoming a reseller.
- Provider APIs drift. Each plugin pins an SDK version and has a contract test that runs
  against a mock, so drift shows up as a failing plugin rather than a wrong bill.
- Prices are estimates until billed. Free-tier eligibility depends on the account (home
  region, existing usage). Quotes say so, and the ledger reconciles.

## Not decided here

- Accounts and identity for the hosted phase (an API token hashed at rest, as the
  console's resume keys are, versus an existing identity such as OBC/KAX).
- Credential storage in the hosted phase: a host key with scoped policies to limit the
  damage, versus encryption under a key derived from the user's token. The latter means
  the broker cannot tear down a resource while the user is away.
- Pricing and margin for credits, and which payment rails.

## First slice (issues)

1. #284 Broker core: plugin interface, quote → provision → teardown lifecycle, leases and
   reaper, envelopes and approval, the ledger. Mocked provider; spending off by default.
2. #285 OCI plugin: Object Storage buckets, then Always Free compute (E2.1.Micro / A1), with
   the least-privilege setup printed for the user.
3. #286 qBraid plugin: the existing broker behind the interface, with behaviour unchanged.
4. #287 MCP tools: `providers`, `quote`, `provision`, `status`, `teardown`, `budget` and
   `ledger`, added to the tool surface with a golden-schema update.

## Evidence (what this builds on)

- qBraid broker: `tools/qpu-broker/qpu_broker.py` (caps, estimates, reconciliation,
  calibration reserve); #262, #269, #272, #275. First real job `q49d3e636d8`, 4.17 credits,
  2026-10-24.
- OCI least-privilege setup and encrypted backup: `tools/qos-console/backup_saves.py`,
  #281, PR #282 (bucket-scoped policy; listing other buckets and compute refused).
- MCP host surface: `scripts/qos_mcp.py`, `contracts/mcp/v1-tools.json` (ADR-0015, ADR-0020).
