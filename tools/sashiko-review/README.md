# Sashiko review, with your own model key (ADR-0028)

`qos-review` lets your AI get a [Sashiko](https://github.com/sashiko-dev/sashiko) review of a commit in one of your
repositories. The review runs on your machine, with your model provider and your key, inside a token cap you set.

- Your key never leaves your machine, and it never reaches Kannaka Labs.
- Nothing is posted anywhere. The findings come back to whoever asked.

Sashiko is an AI patch reviewer from the Linux kernel community (Apache-2.0). Kannaka Labs keeps a fork,
[sashiko-kannaka](https://github.com/kannaka-labs/sashiko-kannaka), with a `kannaka` profile for service code (Rust,
TypeScript, JavaScript, Python). The `sashiko` profile is upstream's own profile for service code.

## Setup

1. **Install Sashiko** on Linux or macOS (it does not build on Windows):
   `cargo install --git https://github.com/kannaka-labs/sashiko-kannaka` (or `cargo install sashiko` for upstream).
2. **Write the config:** `qos-review example > ~/.qos-review/review.json`, then edit it.
   - `binary`: where step 1 put Sashiko.
   - `provider` and `model`:
     - `claude`, `openai` or `gemini` take a key.
     - `openai-compatible` and `vllm` also take a key, plus `provider_settings`.
     - `ollama` (local) and `claude-cli` (your own Claude Code subscription) use no key here.
   - The key itself:
     - `key_env`: the name of a variable that holds your key; or
     - `key_file`: an owner-only file (`chmod 600`) that holds it.

     The key is never written in the config. The server hands it only to the Sashiko process.
   - `max_total_tokens` (**required**): the most uncached input plus output tokens one review may use. Sashiko
     enforces it and stops the review when it is passed. `max_total_output_tokens` caps output alone.
   - `price` (optional): your provider's current prices, in dollars per million tokens. They are used only for
     estimates. The example's numbers are placeholders.
   - `repos`: the checkouts your AI may name, as `{"name": "/path"}`. It refers to them by name only.
3. **Check it:** run `qos-review check --config ~/.qos-review/review.json`. It shows:
   - whether the binary and the key are present (never the key itself);
   - the cap;
   - the repositories.

## Use

From your AI, run it as an MCP server (`qos-review serve --config ...`), or put it behind the hub (the `review`
entry in `hub.example.json`). The hub asks you before every `review_start` and allows the read-only tools.

| Tool | What it does |
|---|---|
| `review_setup` | What is configured, and whether the key is present |
| `review_quote` | The commit, its diff size, the cap and the worst case in dollars. Runs nothing |
| `review_start` | Queues a review. It spends your tokens, up to the cap |
| `review_status` | `queued`, `running`, `done`, `failed`, `cancelled` or `interrupted` |
| `review_result` | Findings (severity, problem, file and line), Sashiko's summary, and the tokens used |
| `review_cancel` | Stops a review. Tokens already used are spent |
| `review_jobs` | Recent jobs |

From a shell, the same tools are subcommands, for example:

```
qos-review quote --config review.json myproject HEAD
qos-review start --config review.json myproject HEAD --wait
```

## What a review costs

The figures below are measured, from the Kannaka Labs trial on QuantumOS pull requests. One review used about **1.1
million input tokens and 60 thousand output tokens** and took about ten minutes.
- A larger change uses more.
- Sashiko's own default cap is 5 million.
- The example sets 3 million.

Before you start a review, `review_quote` shows the worst case at your prices. Two things the cap does not cover:
- **Cached input.** Sashiko does not count cached input tokens toward the cap, since a cached read costs a tenth or
  less. They are reported and priced afterwards.
- **A review that hits the cap.** It ends without a report, and the tokens used until then are spent.

## What it will not do

- Hold, print, log or return your key.
- Run a revision that does not resolve to a commit, or a repository you did not list.
- Pass a prompt of the agent's choosing to Sashiko (`--custom-prompt` is not exposed).
- Run two reviews at once.
- Re-run a job that was running when the server stopped. It is marked `interrupted`, because running it again would
  spend again.
- Post a review. Publishing one is up to you.

Findings are an AI's. Check each one before acting on it.
