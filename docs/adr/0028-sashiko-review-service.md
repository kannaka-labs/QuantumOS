# 28. Sashiko Review as a Service the Person Pays For With Their Own Model Key

Date: 2026-10-25
Status: Accepted (2026-10-25, Nick: "yes, let's offer Sashiko review where users pay with their own model key")

## Context

Sashiko (sashiko-dev/sashiko, Apache-2.0) is an AI patch reviewer from the Linux kernel community. Kannaka Labs keeps
a fork, `kannaka-labs/sashiko-kannaka`, that adds a `kannaka` review profile for our own code. Since 2026-10-10 a
pre-registered trial has reviewed every eligible kannaka-labs pull request with both the stock and the Kannaka profile.
QuantumOS is the trial's busiest repository.

ADR-0026 made QuantumOS the place an agent gets resources inside a budget the person sets, and ADR-0027 put the
ecosystem's tools behind one policy-gated endpoint. Nick asked to offer Sashiko review the same way, as a Kannaka
service that people pay for with their own model key.

A review is not cheap. On QuantumOS pull requests one arm has used about 1.1 million input tokens and 60 thousand
output tokens, taking about ten minutes. A person needs to know what a review can cost before it starts, and it must
not be able to cost more.

## Decision

1. **A review server, `tools/sashiko-review`** (installed as `qos-review`). It is an MCP server and a CLI, with these
   tools: `review_setup`, `review_quote`, `review_start`, `review_status`, `review_result`, `review_cancel` and
   `review_jobs`. It runs the person's own Sashiko binary on a repository the person has listed, and returns the
   findings to the caller. It never posts a review anywhere. Publishing one is the person's own act.
2. **The person's key, the person's machine.** The model provider and key belong to the person: Anthropic, OpenAI,
   Gemini, any OpenAI-compatible endpoint (including a local one), or the Claude Code CLI with their own subscription.
   - The key is read from an environment variable or an owner-only file that the person names.
   - It is handed only to the Sashiko process, under the variable that provider expects.
   - It never appears in the config, argv, the job's files, the logs or any result. Any output that contains it is
     redacted.
   - Kannaka Labs never holds a person's key, and it does not run reviews on its own machines for other people.
3. **A hard cap on every review.** The config must set `max_total_tokens` and has no default. The server passes it to
   Sashiko's own budget (`review.max_total_tokens`), which aborts the review when uncached input plus output passes
   the cap.
   - `review_quote` shows the cap before anything runs, along with the diff size and, if the person gave prices, the
     worst case in dollars.
   - Cached input tokens are not counted by Sashiko's cap. They are reported after the review and priced when a price
     was given.
4. **Ask before spending.** In the hub, `review_start` defaults to `ask`, since it spends the person's money. The
   read-only tools default to `allow`.
5. **Only what the person listed.** Repositories are named in the config, and the agent refers to them by name, never
   by path. A revision must resolve to a commit with `git rev-parse --verify --end-of-options`, and only the resolved
   ids reach Sashiko. The profile comes from a fixed set (`kannaka`, `sashiko`). Sashiko's `--custom-prompt` is not
   exposed.
6. **One review at a time, kept as files.** Jobs queue, and each job has a directory that only the owner can open,
   holding its settings, report and redacted stderr. A job that was running when the server stopped is marked
   `interrupted`. It is not re-run, because that would spend again.

Later slices:
- prebuilt `sashiko-kannaka` binaries (Linux x86_64 and aarch64) with checksums;
- a leased mode, where the broker rents a Linux VM in the person's own OCI or qBraid account, runs the review there
  over SSH with the key passed on standard input, and tears the VM down;
- a QuantumOS kernel guide for the Kannaka profile, after the trial ends on 2026-11-07, since the trial reviews with
  the Kannaka build current at review time.

## Consequences

- Anyone can get a Sashiko review of their own code through their AI, at their own provider's price, and no Kannaka
  Labs machine sees their key or their code.
- The person needs a Linux or macOS machine with Sashiko installed, since Sashiko does not build on Windows. Until the
  binaries ship, that means `cargo install`.
- The cap bounds uncached input plus output, not the bill. Cached reads and the provider's own price changes are
  outside it. The quote says so, and the dollar figures are estimates from prices the person typed in.
- A review that hits its cap ends without a report, and the tokens already spent are spent. The cap has to be set with
  that in mind (the README gives the trial's measured sizes).
- Findings are an AI's. The trial has not finished, so this ADR claims nothing about how accurate they are.

## Evidence

- `tools/sashiko-review/sashiko_review.py`; tests in `tools/sashiko-review/test_sashiko_review.py` against a stand-in
  Sashiko (`fake_sashiko.py`), covering:
  - the key reaching only the child and redacted everywhere else;
  - the cap reaching Sashiko's settings;
  - refused revisions and repositories;
  - queueing, cancel and interruption;
  - a full run over MCP stdio.
