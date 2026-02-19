# tccp

I just wanted to code. Open my editor, tweak something, run it on a GPU. The same way I'd run anything locally. I didn't want to think about SLURM, or Singularity, or syncing files, or which filesystem has space for what, or setting up environments on a cluster every time something gets wiped.

So I built tccp. It makes the cluster disappear. You write code on your laptop, and tccp gets it running on a GPU.

```
$ tccp setup              # save your credentials once
$ tccp register           # set up your project once
$ tccp                    # connect
tccp> run train           # go
```

Your code syncs, the container and venv get set up if needed, and the job runs. Runtime caches go to fast local scratch so they don't fill your home quota. Container images are pulled on compute nodes where there's space, then cached on NFS so they're only pulled once. If your laptop sleeps, the job keeps going. Reconnect whenever.

## Install

**Homebrew (macOS / Linux):**
```
brew tap seth-lupo/tap
brew install tccp
```

**Shell script (macOS / Linux):**
```
curl -fsSL https://seth-lupo.github.io/tccp/install.sh | sh
```

**Windows (PowerShell):**
```
irm https://seth-lupo.github.io/tccp/install.ps1 | iex
```

Or download binaries directly from [releases](https://github.com/seth-lupo/tccp/releases/latest).

## Docs

Everything else lives at **[sethlupo.github.io/tccp](https://sethlupo.github.io/tccp)**:

- [How it works](https://sethlupo.github.io/tccp/architecture.html) -- architecture, storage strategy, design decisions
- [Commands](https://sethlupo.github.io/tccp/commands.html) -- every command, with examples
- [Usage guide](https://sethlupo.github.io/tccp/usage.html) -- config, secrets, interactivity, caching, tips

## Network

You need to be on the Tufts network:
- **Cisco VPN** (recommended)
- **Campus WiFi** works too, but requires MFA each session

## License

MIT
