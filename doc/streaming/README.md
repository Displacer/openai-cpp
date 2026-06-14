# Streaming support for Chat Completions

This document describes the streaming entry point added to the C++ OpenAI
wrapper: `openai::chat().createStream(input, on_chunk)`. It mirrors the
existing `create()` API but delivers incremental deltas to a user-supplied
callback as they arrive, and returns a final aggregated JSON object with the
same shape as the non-streaming call.

A self-contained example is included as
[`examples/13-chat-stream.cpp`](../../examples/13-chat-stream.cpp).

## Summary

This adds support for OpenAI's Server-Sent Events (SSE) streaming on the
`chat/completions` endpoint. The new entry point lets you consume tokens as
they are produced by the model and still get a fully aggregated response at
the end, with no changes to existing APIs.

## What's new

- `CategoryChat::createStream(Json input, std::function<bool(const Json&)> on_chunk)`
  - Forces `"stream": true` on the request.
  - Invokes `on_chunk` once per parsed SSE event (the raw chunk JSON as sent
    by the server).
  - Returning `false` from `on_chunk` aborts the transfer cleanly (libcurl
    write callback returns 0).
  - Aggregates `delta.content`, `delta.reasoning_content`, `delta.tool_calls`
    (merged by `index`, with `function.name` / `function.arguments`
    concatenated), `delta.annotations` (latest value wins), `role` and
    `finish_reason` into a final response shaped like a regular
    `chat/completions` reply (`choices[0].message.*`).
  - Mid-stream `error` objects are first forwarded to the callback, then
    surfaced on the returned JSON, and stop the stream.
  - Payloads that fail to parse as JSON are silently skipped; the terminal
    `[DONE]` marker is filtered out and never reaches `on_chunk`.
- Low-level plumbing in `Session` / `OpenAI`:
  - `Session::makeStreamRequest` — SSE-aware write callback that buffers
    partial chunks and dispatches complete `data:` payloads (handles
    multi-line events, `\r\n` line endings, and the terminal `[DONE]`
    marker).
  - `OpenAI::postStream` — streaming counterpart to `post`, sends the body
    and forwards SSE payloads to a user callback.
- New example: `examples/13-chat-stream.cpp` (registered in
  `examples/CMakeLists.txt`), prints tokens to `stdout` as they arrive and
  then dumps the aggregated final response.

## API

```cpp
openai::Json createStream(openai::Json input,
                          std::function<bool(const openai::Json&)> on_chunk);
```

- `input` — same JSON body you'd pass to `create()`; `stream: true` is set
  automatically.
- `on_chunk` — called for every streamed event with the parsed chunk JSON.
  Return `true` to keep going, `false` to abort.
- Return value — an aggregated final JSON (same shape as `create()`'s
  response), populated from the streamed deltas; on errors, includes an
  `error` field.

## Field-by-field aggregation

How each field in `delta` is folded into the final `choices[0].message`:

- `content`, `reasoning_content` — **concatenated** in arrival order.
- `tool_calls` — merged by `index` into a single array, ordered by `index`
  ascending:
  - `function.arguments` and `function.name` — **concatenated** (the server
    streams `arguments` as a JSON string in fragments; it only becomes valid
    JSON once the stream ends).
  - `id`, `type` — **last value wins** (`type` defaults to `"function"` if
    the server never sends it).
- `annotations` — **last value wins** (replaced wholesale by the most
  recent delta).
- `role`, `finish_reason` — **last non-empty value wins** (`role` defaults
  to `"assistant"`).

Note: this aggregation does **not** change the model's output relative to a
non-streaming `create()` call. The resulting `choices[0].message.*` is
equivalent to what `create()` would return for the same request. The
practical differences are:

- Top-level response fields that the server only sends with the final
  non-streaming reply (`id`, `created`, `model`, `usage`,
  `system_fingerprint`, ...) are **not** present on the aggregated object —
  the streaming protocol does not include them.
- Only `choices[0]` is aggregated. If you request `n > 1`, the additional
  choices are still delivered to `on_chunk` as they arrive, but they will
  not appear in the returned aggregated JSON.

## Callback contract

Things worth knowing about the callback contract:

- `createStream` is **synchronous**: it blocks until the stream ends (or the
  callback returns `false`). Capturing locals by reference is therefore safe
  — they're guaranteed to outlive the call.
- The callback runs on the **calling thread**, from inside libcurl's write
  callback. Don't issue another request on the same `OpenAI` instance from
  within the callback — the session is guarded by a mutex and would
  deadlock.
- Returning `false` is the clean way to cancel; libcurl will report the
  aborted transfer, and `createStream` will still return the partially
  aggregated response.
- Exceptions thrown from the callback will propagate up through libcurl;
  prefer catching inside the callback if you need fine-grained control.

## Usage example

```cpp
#include "openai.hpp"
#include <iostream>

int main() {
    openai::start();

    openai::Json input = R"({
        "model": "gpt-3.5-turbo",
        "messages":[{"role":"user","content":"Write a short haiku about the sea."}],
        "max_tokens": 64,
        "temperature": 0
    })"_json;

    std::string collected;
    auto on_chunk = [&](const openai::Json& chunk) -> bool {
        const auto& choices = chunk.value("choices", openai::Json::array());
        if (choices.empty()) return true;
        const auto& delta = choices[0].value("delta", openai::Json::object());
        if (delta.contains("content") && delta["content"].is_string()) {
            const auto piece = delta["content"].get<std::string>();
            std::cout << piece << std::flush;
            collected += piece;        // mutate caller's local state, no globals
        }
        return true;                   // return false to cancel mid-stream
    };

    auto final_response = openai::chat().createStream(input, on_chunk);
    std::cout << "\n\nFinal aggregated response:\n"
              << final_response.dump(2) << '\n';
}
```

## Backward compatibility

- Purely additive. No existing function signatures, types, or behavior are
  changed.
- Non-streaming `create()` is untouched.
- SSE parsing is implemented on top of the existing libcurl integration.
