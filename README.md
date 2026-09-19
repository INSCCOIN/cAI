# cAI

Tiny terminal chat. Uses the **curl program**, not libcurl headers.

## Build

```bash
sudo apt install -y curl
cd /home/working/cAI
rm -f cAI *.o
make
sudo install -m 755 cAI /usr/local/bin/cAI
```

## Use

```bash
cAI
```

First launch opens **setup**:

1. pick provider (`1` = xAI/Grok, `2` = OpenAI)
2. paste the API key

Key is stored in `~/.cai.key` (mode 600). Provider is stored in `~/.cai.conf`.

Change later:

```bash
cAI setup
```

or type `setup` inside the chat.

In chat: `setup`  `/clear`  `/quit`

One-shot:

```bash
echo "what is 2+2" | cAI -q
```
