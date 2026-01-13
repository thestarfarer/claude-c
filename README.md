# claude-c

Minimal Claude API client in C. 31KB binary, zero Node.js dependency.

## Build

```bash
# Requires libcurl
make
```

## Usage

```bash
# Basic query
echo "Hello" | ./claude-c -p

# With system prompt
echo "What is 2+2?" | ./claude-c -p -s "Be concise, reply with just the answer"

# Explicit messages (API format)
./claude-c -p -m '[{"role":"user","content":"Hello"}]'

# Streaming output (shows response as it generates)
./claude-c -p --stream -m '[{"role":"user","content":"Tell me a story"}]'

# With image
./claude-c -p -i photo.jpg "What is in this image?"

# Multiple images
./claude-c -p -i img1.png -i img2.png "Compare these images"

# Multi-turn from file (for large payloads with images)
./claude-c -p -m @conversation.json

# Custom model
./claude-c -p --model claude-opus-4-5-20251101 -m '[{"role":"user","content":"Hello"}]'

# With persona
./claude-c -p -s "$(cat persona.md)" -m '[{"role":"user","content":"Hey"}]'
```

## Options

| Flag | Description |
|------|-------------|
| `-p, --print` | Single message mode (required) |
| `-S, --stream` | Stream response as it arrives |
| `-s, --system-prompt` | Custom system prompt |
| `-m, --messages` | Messages JSON array or `@file.json` |
| `-i, --image` | Attach image file (can use multiple times, max 16) |
| `-M, --model` | Model ID (default: claude-sonnet-4-5-20250929) |
| `-t, --max-tokens` | Max output tokens (default: 16384) |
| `-r, --request` | Full request body JSON file (`@file.json`) |
| `-J, --json-output` | Output raw API response JSON |

## Raw Request Mode

For programmatic use (e.g., from other applications), claude-c supports raw request mode:

```bash
# Full request with tools
cat > request.json << 'EOF'
{
  "model": "claude-sonnet-4-5-20250929",
  "max_tokens": 16384,
  "stream": false,
  "messages": [{"role": "user", "content": "What is 2+2?"}],
  "tools": [
    {
      "name": "calculator",
      "description": "Perform calculations",
      "input_schema": {
        "type": "object",
        "properties": {"expression": {"type": "string"}},
        "required": ["expression"]
      }
    }
  ]
}
EOF

# Get JSON response (includes tool_use blocks if any)
./claude-c -p -r @request.json -J
```

The `-r` option accepts a full API request body. Claude-c automatically:
- Injects the required identity string into the system array
- Adds metadata (user_id) for OAuth authentication

The `-J` option outputs the raw API response JSON instead of extracting text.

## JSON Message Structure

### Request Body

Sent to `POST https://api.anthropic.com/v1/messages`:

```json
{
  "model": "claude-sonnet-4-5-20250929",
  "max_tokens": 16384,
  "stream": true,
  "system": [
    {"type": "text", "text": "You are a Claude agent, built on Anthropic's Claude Agent SDK."},
    {"type": "text", "text": "Your custom system prompt here"}
  ],
  "messages": [
    {"role": "user", "content": "Hello"},
    {"role": "assistant", "content": [{"type": "text", "text": "Hi!"}]},
    {"role": "user", "content": "How are you?"}
  ],
  "metadata": {"user_id": "user_{64hex}_account_{uuid}_session_{uuid}"}
}
```

### Messages Format

**Simple content** (string):
```json
{"role": "user", "content": "Hello"}
```

**Block content** (array):
```json
{"role": "assistant", "content": [
  {"type": "text", "text": "Response text here"}
]}
```

### Content Block Types

| Type | Structure |
|------|-----------|
| `text` | `{"type": "text", "text": "..."}` |
| `image` | `{"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "..."}}` |
| `tool_use` | `{"type": "tool_use", "id": "toolu_xxx", "name": "Bash", "input": {...}}` |
| `tool_result` | `{"type": "tool_result", "tool_use_id": "toolu_xxx", "content": "..."}` |
| `thinking` | `{"type": "thinking", "thinking": "..."}` |

### Streaming Events (SSE)

```
event: message_start
data: {"type": "message_start", "message": {...}}

event: content_block_start
data: {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}

event: content_block_delta
data: {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Hello"}}

event: content_block_stop
data: {"type": "content_block_stop", "index": 0}

event: message_stop
data: {"type": "message_stop"}
```

### Required Headers

```
Content-Type: application/json
anthropic-version: 2023-06-01
x-api-key: sk-ant-xxx              # API key auth
Authorization: Bearer xxx          # OR OAuth auth
anthropic-beta: oauth-2025-04-20   # Required for OAuth
x-app: cli
```

## Images

Supports PNG, JPEG, GIF, and WEBP images up to ~3.7 MB (5 MB base64).

**Single image:**
```bash
./claude-c -p -i photo.jpg "What is this?"
```

**Multiple images:**
```bash
./claude-c -p -i before.png -i after.png "What changed?"
```

**Multi-turn with images** (use `@file.json` for large payloads):
```json
[
  {"role": "user", "content": [
    {"type": "text", "text": "Remember this image"},
    {"type": "image", "source": {
      "type": "base64",
      "media_type": "image/png",
      "data": "<base64-data>"
    }}
  ]},
  {"role": "assistant", "content": "I see a red circle. Noted."},
  {"role": "user", "content": "What shape was it?"}
]
```

```bash
./claude-c -p -m @conversation.json
```

Image type is detected from magic bytes, not file extension.

## Authentication

Checks in order:
1. `ANTHROPIC_API_KEY` environment variable
2. OAuth token from `~/.claude/.credentials.json`

The OAuth token is created by the official `claude` CLI. Just run `claude` once to authenticate, then `claude-c` will use the same credentials.

## How It Works

Uses the same API as Claude Code with the required identity string for OAuth authentication:

```
"You are a Claude agent, built on Anthropic's Claude Agent SDK."
```

This identity string must be present as a separate system prompt block for the Claude Code OAuth credentials to work.

## Metadata Spoofing

Mimics the real Claude Code CLI metadata format:
```
user_{userId}_account_{accountUuid}_session_{sessionId}
```

- **userId**: 64-char hex, generated once and persisted to `~/.claude/claude-c.json`
- **accountUuid**: Fetched from `/api/oauth/profile` on first OAuth use, cached locally
- **sessionId**: UUID v4, generated fresh each run

On first run with OAuth, makes an extra API call to fetch the account profile. Subsequent runs use the cached `accountUuid`.

## Files

```
claude-c/
├── main.c      # Entry point, argument parsing
├── api.c       # HTTP requests, request/response handling
├── auth.c      # OAuth token loading
├── state.c     # Persistent state and metadata spoofing
├── stream.c    # SSE stream parser
├── json.c      # Minimal JSON parser
├── image.c     # Image loading, base64 encoding, MIME detection
└── Makefile
```

## Size Comparison

| | claude-c | Node.js CLI |
|-|----------|-------------|
| Binary | 48 KB | 11 MB |
| Source | 2,600 lines | 538,000 lines |
| Startup | instant | ~500ms |
| Dependencies | libcurl | Node.js runtime |

## Models

| ID | Description |
|----|-------------|
| `claude-sonnet-4-5-20250929` | Sonnet 4.5 (default) |
| `claude-opus-4-5-20251101` | Opus 4.5 |
| `claude-sonnet-4-20250514` | Sonnet 4 |
| `claude-opus-4-20250514` | Opus 4 |
| `claude-3-5-sonnet-20241022` | Sonnet 3.5 |
| `claude-3-5-haiku-20241022` | Haiku 3.5 |

## License

Do whatever you want with it.
