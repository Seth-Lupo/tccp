# tccp

I just wanted to code. Open my editor, tweak something, run it on a GPU. The same way I'd run anything locally. I didn't want to think about SLURM, or Singularity, or syncing files, or setting up environments on a cluster, or getting Duo-pushed five times in a row.

So I built tccp. It makes the cluster disappear. You write code on your laptop, and tccp gets it running on a GPU. That's it.

```
$ tccp setup              # save your credentials once
$ tccp register           # set up your project once
$ tccp                    # connect (one Duo push)
tccp> run train           # go
```

Your code syncs, the container and venv get set up if needed, and the job runs. You can attach to it, detach, come back later. If your WiFi drops or your laptop sleeps, the job keeps going. Reconnect whenever and pick up where you left off.

## Install

**macOS / Linux:**
```
curl -fsSL https://sethlupo.github.io/tccp/install.sh | sh
```

**Windows (PowerShell):**
```
irm https://sethlupo.github.io/tccp/install.ps1 | iex
```

Or download binaries directly from [releases](https://github.com/sethlupo/tccp/releases/latest).

## Docs

Everything else lives at **[sethlupo.github.io/tccp](https://sethlupo.github.io/tccp)**:

- [How it works](https://sethlupo.github.io/tccp/architecture.html) -- what's happening under the hood
- [Commands](https://sethlupo.github.io/tccp/commands.html) -- every command, with examples
- [Usage guide](https://sethlupo.github.io/tccp/usage.html) -- config, secrets, interactivity, tips

## Network

You need to be on the Tufts network. Either:
- **Cisco VPN** (recommended) -- connect once, no repeated Duo pushes
- **Campus WiFi** -- works fine, but you'll get a Duo push each time you start tccp

## License

MIT
