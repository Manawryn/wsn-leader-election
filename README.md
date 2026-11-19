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

If `motelist` shows `(none)` in the Reference column (our motes do), the
upload finds no motes and only prints `Done`. Name the ports yourself, and
check that every port prints `+++++ Erasing / Programming / Resetting`:

```bash
make TARGET=sky MOTES="/dev/ttyUSB0 /dev/ttyUSB1" leader-election.upload
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

---

# The project

How the protocol works and why: **[DESIGN.md](DESIGN.md)**.

## Files

| File | What it does |
|---|---|
| `leader-election.c` | Main process: roles, TDMA frame, election rounds, failover, logging |
| `le-election.c/.h` | LEACH-C scoring and ranking (pure functions, unit-tested) |
| `le-link.c/.h` | RSSI EWMA, link matrix, WMEWMA PRR |
| `le-energy.c/.h` | Simulated battery |
| `le-stats.c/.h` | Message counters, Energest radio time, stack usage |
| `le-sensor.c/.h` | SHT11 temperature + light |
| `le-msg.h` | Message formats |
| `le-conf.h` | **All protocol parameters** (nodes, timing, weights, energy model, experiments) |
| `project-conf.h` | Contiki-NG stack settings (CSMA, Energest, logging) |
| `scripts/analyze.py` | Computes all evaluation metrics from the logs |
| `scripts/capture.sh` | Logs all connected motes to one file each |
| `tests/test-election.c` | PC unit tests for the election and link estimation |
| `sim/leader-election.csc` | Cooja simulation with 5 sky motes |

## 1. Put your motes in the node table (once)

Every mote looks up its own ID in `LE_NODE_IDS` (`le-conf.h`). The ID comes
from the mote's serial-number chip and is printed at boot as `LE ... BOOT id=<N>`,
or `LE ? 0 ERROR id=<N> is not in LE_NODE_IDS` if it is missing from the
table. Ignore Contiki's own `Node ID:` line: with NullNet it is 4608 on every
mote. Cooja mote IDs (1, 2, 3, ...) also match, so use `{ 1, 2, 3, 4, 5 }` there.

1. Flash all motes and open a console on each (press reset to see the boot lines).
2. Put the IDs into `LE_NODE_IDS`, then `make TARGET=sky clean` and flash again.
   The order defines the node index; index 0 starts the network at boot.

Our motes so far: 45524, 57609.

If two motes show the same ID, flash them one at a time with
`make TARGET=sky clean && make TARGET=sky DEFINES=LE_FORCE_INDEX=<i> leader-election.upload`.

## 2. Run it

Power the motes up in any order. After about 6 s index 0 starts the network
and the first election runs. LEDs: **blue** = leader, **green** = member
(toggles on every send), **red** = failed/dead.

Serial commands (type into `make login`, one per line):

| Command | Effect |
|---|---|
| `fail [s]` | Simulated crash: radio off for `s` seconds (0 = until reset), then the node rejoins. Default `LE_FAIL_DURATION_S` (60). |
| `drain <n>` | Remove `n` units of simulated energy (forces a low-energy relinquish when done on the leader) |
| `elect` | Leader starts an election next round |
| `loss <pct>` | Drop `pct` % of received broadcast frames (beacon/ackmap/result loss) |
| `status` | Print role, term, leader, round, energy |

The **user button** does the same as `fail`.

## 3. Capture logs

Each mote prints lines like `LE <index> <ms> <EVENT> key=value ...`. Log all
connected motes at once, one file per mote (Ctrl+C to stop):

```bash
bash scripts/capture.sh results/exp1
echo "fail 0" > /dev/ttyUSB2        # send a command to one mote meanwhile
```

or one terminal per mote: `make TARGET=sky login PORT=/dev/ttyUSB0 | tee results/exp1/mote0.log`.

Then:

```bash
python3 scripts/analyze.py results/exp1/*.log --firmware build/sky/leader-election.sky
```

The script prints election times, control messages per round, overhead, data
loss and delay around every re-election, energy and duty cycle per node, and
memory. `--json out.json` saves everything; `--csv-row study.csv --label k=2`
appends one summary line per run (for the timeout study).

## 4. Experiments (one per evaluation item)

Every parameter can be changed at build time without editing files:
`make TARGET=sky clean && make TARGET=sky DEFINES=NAME=value,NAME2=value leader-election.upload`.

| Requirement / metric | How to run it |
|---|---|
| Initial election, data collection | Default firmware, 5 min. `ELECT reason=bootstrap`, then `RND`/`RD` lines at the leader. |
| Relinquish on low energy | `drain 1300` on the leader → `ELECT reason=low_energy` next round. |
| Simulated failure + detection | `fail 0` (or button) on the leader → members `MISS`, backup `FAILDET` + `TAKEOVER`, members `NEWLDR gap_ms=...`. |
| Periodic rotation | Default: every 24 rounds (`LE_ROTATION_ROUNDS`). Compare the lifetime with `LE_ROTATION_ROUNDS=0` and with a fixed leader (`LE_ROTATION_ROUNDS=0,LE_E_LOW=0`): the script reports the round of the first death. |
| Failure-detection timeout | Flash with `LE_FAILURE_TIMEOUT_K=1` (then 2, 3, 4), plus `LE_FAIL_AT_ROUND=20,LE_FAIL_EVERY_ROUNDS=30,LE_FAIL_DURATION_S=40,LE_RX_LOSS_PCT=10` so the leader crashes every 30 rounds on a lossy channel. Run 30 min each and collect with `--csv-row study.csv --label k=<K>`: detection time, recovery gap and false failovers per K. |
| Multiple clusters | `LE_TWO_CLUSTERS=1`: clusters {0,1,2} and {3,4}, each with its own leader. The script reports each cluster separately. |
| Memory | `make TARGET=sky size` + `BOOT ram_static=` + `STAT stack=` (all summarised by the script). |
| Link metric with motes on one desk | At 0 dBm all RSSIs are similar; try `LE_TX_POWER_DBM=-15` so link quality matters in the election. |

Use a channel other than the other groups' (`LE_RF_CHANNEL`, default 26).

## 5. Without hardware: Cooja and unit tests

```bash
# unit tests of the election logic (on any PC with gcc)
cc -I. -o /tmp/t tests/test-election.c le-election.c le-link.c && /tmp/t

# simulation: 5 sky motes, the leader crashes at 60 s, energy drain at 180 s
cd ~/contiki-ng/tools/cooja && ./gradlew run      # GUI, needs X11 forwarding
#   File > Open simulation > examples/wsn-leader-election/sim/leader-election.csc
#   the log of the run is COOJA.testlog; analyse it with scripts/analyze.py
```
