# WSN Mini-project 6: Leader Election and Leader Rotation

Leader election, failure detection and leader rotation on 5+ TelosB motes,
built with **Contiki-NG release v4.9** (TelosB = `sky` target).

## Layout

This repo lives inside a Contiki-NG checkout, because the Vagrant VM only
mounts the `contiki-ng` folder (as `/home/vagrant/contiki-ng`) and the
Makefile uses `CONTIKI = ../..`.

```
contiki-ng/                    <- upstream Contiki-NG, release/v4.9 (not ours)
├── tools/vagrant/             <- Vagrantfile
└── examples/
    └── wsn-leader-election/   <- this repo
```

## First-time setup

```bash
# Contiki-NG v4.9 (skip if you already have it; check with `git describe --tags`)
git clone -b release/v4.9 https://github.com/contiki-ng/contiki-ng.git
cd contiki-ng && git submodule update --init --recursive

# this repo, inside the tree
cd examples
git clone https://github.com/Manawryn/wsn-leader-election.git

# hide it from the contiki-ng repo
echo "examples/wsn-leader-election/" >> ../.git/info/exclude
```

Notes:
- Run the `echo` line in Git Bash, not PowerShell. PowerShell's `>>` writes
  UTF-16, which git ignores. Alternatively, add the line to the file in a text editor.
- We build in a Linux VM, so keep LF line endings:
  `git config --global core.autocrlf input`. The repo's `.gitattributes`
  enforces LF for our own files.

## Build and flash (inside the VM)

```bash
cd ~/contiki-ng/examples/wsn-leader-election
make TARGET=sky                                   # build
make TARGET=sky motelist                          # list connected motes
make TARGET=sky leader-election.upload            # flash all connected motes
make TARGET=sky login                             # serial console
make TARGET=sky serialdump                        # serial console + timestamped log file
```

`leader-election` is the program name (`CONTIKI_PROJECT` / `leader-election.c`).
Keep the logs you use in `results/`, e.g. `results/2026-11-05_timeout-2s_run1.log`.

## Workflow

- `main` is kept working. Do each task on its own branch.
  Merge it into `main` through a pull request.
- Pull before starting work and push when done.
- Run git on the Windows host and build in the VM. Both see the same files.

```bash
git switch main && git pull
git switch -c <branch name>
# ... work ...
git add . && git commit -m "<you commit message>"
git push -u origin <branch name>     # then open a PR on GitHub
```