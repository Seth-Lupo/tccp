# tccp

I wanted to write ML code like I write any other code. Locally. Open my editor, change a file, run it. No SLURM scripts, no scp, no SSH tunnels, no quota math, no environment debugging.

The cluster should be invisible. tccp abstracts away the compute.

```
$ tccp setup              # store credentials (once)
$ tccp register           # create tccp.yaml (once)
$ tccp                    # connect
tccp> run train           # that's it
```

One SSH connection. One Duo push. Your code syncs, the container pulls, the venv builds, the job runs. Attach, detach, reattach. Jobs survive disconnects.

## Install

Download the latest binary from [releases](https://github.com/sethlupo/tccp/releases/latest):

| Platform | Binary |
|----------|--------|
| macOS (Apple Silicon) | [tccp-macos-arm64](https://github.com/sethlupo/tccp/releases/latest/download/tccp-macos-arm64) |
| Linux (x64) | [tccp-linux-x64](https://github.com/sethlupo/tccp/releases/latest/download/tccp-linux-x64) |
| Windows (x64) | [tccp-windows-x64.exe](https://github.com/sethlupo/tccp/releases/latest/download/tccp-windows-x64.exe) |

```
chmod +x tccp-macos-arm64
sudo mv tccp-macos-arm64 /usr/local/bin/tccp
```

## Docs

Full documentation: **[sethlupo.github.io/tccp](https://sethlupo.github.io/tccp)**

- [How it works](https://sethlupo.github.io/tccp/architecture.html) -- architecture, SSH topology, design decisions
- [Commands](https://sethlupo.github.io/tccp/commands.html) -- every command with examples
- [Usage](https://sethlupo.github.io/tccp/usage.html) -- VPN setup, tccp.yaml reference, interactivity, secrets

## Network

Requires Tufts network access:
- **Cisco VPN** (recommended) -- no repeated Duo pushes
- **Campus WiFi** -- works, but each session triggers a Duo push

## License

MIT
