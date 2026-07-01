# AutoCoder bigtest -- end-to-end agent scenarios

Scripted end-to-end tests that drive a real `autocoder_cli` turn against the
live LLM API in a sandboxed temp directory, then verify the produced project
programmatically (no human in the loop). These burn real tokens and real
minutes -- they are NOT part of `autocoder_tests` (the offline unit suite).

## Prerequisites

- `autocoder_cli.exe` built (`cmake --build --preset default`).
- An API key for the provider: if the AutoCoder app has one saved in its
  Settings, nothing to do -- the CLI reads the same Credential Manager entry.
  Otherwise set `DEEPSEEK_API_KEY` / `ANTHROPIC_API_KEY` (the env var also
  overrides the stored key when both exist).
- CMake + VS 2022 on the machine (the agent and the verifiers both build).

## Environment variables

| Variable | Effect |
|---|---|
| `AUTOCODER_PROVIDER` | `deepseek` (default) or `claude` |
| `AUTOCODER_CLI` | Explicit path to autocoder_cli.exe (overrides auto-detect) |

## Levels

| Script | Time / cost | What it proves |
|---|---|---|
| `run_smoke.ps1` | ~2-5 min, cheap | Plumbing: empty dir -> hello-world C + CMake, built and run by the agent. Run this often. |
| `run_fixbug.ps1` | ~5-10 min | Comprehension: fixture project (`fixture_calc/`) with a planted off-by-one; agent must fix the library, not the tests (hash-checked). Exercises read/grep/outline + targeted edits. |
| `run_dx12.ps1` | ~10-30 min, expensive | Flagship: empty dir -> DX12 triangle on the WARP adapter, offscreen, self-verifying via `out.bmp`. Run before releases. |

Each script exits 0 on PASS, 1 on FAIL, and is independent of the others.

## How a run works

1. A fresh sandbox is created under `%TEMP%\autocoder_bigtest\<name>-<stamp>`.
2. `autocoder_cli` runs one turn there with `--auto-approve` (sandboxed dir,
   so gated commands such as running the built exe are allowed), `--max-iter`
   raised for the task size, and `--check "cmake --build build"` so the
   auto-verify loop fires on compile errors.
3. A hard timeout kills hung runs (exit 124).
4. The verifier asserts on artifacts, never on transcript text: exit code,
   expected executable output, untouched test hashes, BMP pixel content.
5. Everything worth keeping lands in `bigtest/results/<name>-<stamp>/`:
   `transcript.txt`, `stderr.txt`, `metrics.json` (LLM requests, tool calls,
   cached/uncached/output tokens, wall time), `workdir-files.txt`, and for
   DX12 the produced `out.bmp`.

## Reading metrics.json

The metrics double as a benchmark for agent efficiency. Things to watch
across runs:

- `tokens.cached_input` vs `tokens.uncached_input` -- prompt caching health
  (Claude: should be heavily cached from the second iteration; DeepSeek
  caches automatically).
- `tool_calls` by name -- discovery calls (glob/ls/read) should stay low at
  task start now that the repo map is injected.
- `tool_errors` and `auto_verify` entries -- how often the agent needed the
  check-command loop to recover.
- `wall_ms`, `llm_requests` -- end-to-end efficiency.

## Notes

- LLM runs are nondeterministic: a single FAIL is a signal, not a verdict --
  check the transcript before blaming a code change. Repeated or clustered
  failures after a change are the regression signal.
- The unattended CLI never blocks: `ask_user` gets a canned "no user
  available" answer, and gated commands are auto-approved (or denied without
  `--auto-approve`). HardBlock commands stay blocked regardless.
- `results/` is ignored by VCS; prune it occasionally.
