# tccp - Tufts Cluster Command Prompt

Edit locally, run on the cluster. tccp handles SLURM, containers, syncing, and storage for you.

**[seth-lupo.github.io/tccp](https://seth-lupo.github.io/tccp)**

## Install

```
brew tap seth-lupo/tap
brew install tccp
```

Or: [other install methods](https://seth-lupo.github.io/tccp/install.html)

## Usage

```
$ tccp setup              # save credentials (once)
$ tccp register           # set up project (once)
$ tccp                    # connect
tccp> run train           # go
```

See the [docs](https://seth-lupo.github.io/tccp) for commands, settings, and more.

## License

MIT
