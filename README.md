# cAI

Tiny terminal chat for SharkDeck. Talks to an **OpenAI-compatible** HTTP API (xAI by default).

## Build

```bash
sudo apt install -y libcurl4-openssl-dev
cd /home/working/cAI
make
sudo install -m 755 cAI /usr/local/bin/cAI
```

## First run

```bash
cAI -k          # paste API key, stored in ~/.cai.key (0600)
cAI             # chat
```

Or:

```bash
export XAI_API_KEY=xai-...
cAI
```

`XAI_API_KEY` / `OPENAI_API_KEY` override the file.

## Commands in chat

| | |
|--|--|
| `/quit` | leave |
| `/clear` | drop history |
| `/key` | replace key |
| `/model grok-4` | switch model (saved) |
| `/base https://api.x.ai/v1` | switch endpoint (saved) |

One-shot:

```bash
echo "what is 2+2" | cAI -q
```

Flags: `-m MODEL` `-u BASEURL` `-q` `-k`

Config: `~/.cai.conf`  
Default model `grok-4`, default base `https://api.x.ai/v1`.
