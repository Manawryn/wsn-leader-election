# Design: leader election and leader rotation

This document describes the protocol implemented in `leader-election.c`. It
follows the group's design notes: NullNet, CSMA with sleep periods, a
beacon-synchronised TDMA frame borrowed from LEACH, and a LEACH-C election
centralised on the leader. It also explains the few points where the
implementation adds to or deviates from those notes, and why.

## 1. Stack

| Layer | Choice | Why |
|---|---|---|
| Network | **NullNet** (`MAKE_NET = MAKE_NET_NULLNET`) | 5 motes in one room, single hop: no routing needed. |
| MAC | **CSMA** (Contiki-NG default) | Carrier sense, link-layer ACKs and retries for unicast come for free. |
| Duty cycling | Our own, on top of CSMA | The application switches the radio off between its slots (`NETSTACK_RADIO.off()`), like S-MAC. CSMA itself keeps the radio on. |
| Sync | Beacon receive time | Each node resynchronises on every beacon (see §3). |

`project-conf.h` sets `CSMA_CONF_MIN_BE 0`. By default CSMA waits a random 0–6
clock ticks (0–47 ms on the sky, where `CLOCK_SECOND = 128`) before the
*first* transmission attempt. That delay would push frames out of their slots.
Slots are already collision-free, so the first attempt goes out immediately.
After a busy channel CSMA still backs off as usual.

## 2. Roles and states

```
            boot / recovered
                   |
                   v
   +-------> LISTEN  (radio on, wait for any beacon)
   |           |  beacon heard ---------------> MEMBER
   |           |  no beacon for BOOT_WAIT*(1+rank) -> LEADER (bootstrap election)
   |
   |  MEMBER --- RESULT names me ------------> LEADER (next period)
   |  MEMBER --- K (+j) silent rounds, I am backup j -> LEADER (failover election)
   |  LEADER --- election picked someone else -> MEMBER (waits for new leader)
   |  LEADER --- hears a better beacon (higher term) -> MEMBER (step down)
   +--- LEADER - all members pruned (orphan, probably another leader exists)
   |  any    --- "fail" / button / auto-fail --> FAILED --(after LE_FAIL_DURATION_S)--+
   +----------------------------------------------------------------------------------+
     any     --- energy reaches 0 -----------> DEAD
```

LEDs: blue = leader, green = member (toggles on every uplink), red = failed or dead.

## 3. Frame (one round = one beacon period, 5 s)

All offsets are relative to `t0`, the moment the beacon ended. On the leader
this is the MAC sent-callback; on a member it is the receive callback. Both
sides compute every later instant from `t0` with the same formula, so clock
offsets between motes cancel out. The beacon is sent twice (§9); a member that
only hears copy c sets `t0` = receive time − c × 15 ms.

```
t0            BEACON  (round, term, DATA/ELECT flag, slot table, backup list)
+15 ms        BEACON  copy 1, same content (time diversity, §9)
+40 ms        slot 0  member slot[0] -> leader      DATA (unicast, ACK, 3 attempts)
+80 ms        slot 1  member slot[1] -> leader          or REPORT (broadcast) in an
...                                                     ELECT round
+40+n*40      contention slot: JOIN from nodes without a slot
+40+(n+1)*40+30  ACKMAP: bit i = "got node i's packet"            (= tb)
tb+40+i*40    retry slot i, only for members whose bit is 0 (also members
              that missed the beacon but heard the ackmap, §9)
tb+40+n*40+30 RESULT (ELECT rounds only)
...           everybody sleeps until t0 + 5000 - 30 ms (guard)
```

* The **leader** keeps its radio on for the whole frame: about 230–280 ms in
  a normal round, about 500 ms in an election round.
* A **member** wakes 30 ms before the expected beacon, sleeps until its slot,
  sends, and sleeps again. If the link-layer ACK arrived, it skips the ackmap,
  because it already knows the leader has the packet (`LE_SKIP_ACKMAP_ON_LLACK`).
  Without an ACK it wakes for the ackmap and retries in its retry slot if its
  bit is 0. This saves energy compared with always waiting for the bitmap, as
  in the original notes, and gives the same guarantee.
* **Guard time is 30 ms, not 10 ms.** The sky clock ticks every 7.8 ms and both
  ends round to a tick, so the error can reach about 2 ticks. Clock drift
  (crystal, ±20 ppm) is only 0.1 ms per period and does not matter.
* Sensor reading (the SHT11 blocks for ~0.2 s) and all `printf`s (the UART on
  the sky is blocking) happen only after the frame, in the quiet part of the
  round.

## 4. Messages

All messages share an 8-byte header: `magic, type, cluster, src index, term,
round`. The magic byte filters out other groups' NullNet traffic in the lab.

| Type | Direction | Payload | Size (B) |
|---|---|---|---|
| BEACON | leader → all | flags, copy number, reason, leader energy, slot table, backup list | 26 |
| DATA | member → leader, unicast | energy, beacon RSSI, beacon PRR, 1–6 readings (seq, temp, light) | 13 + 6 per reading |
| REPORT | member → all, broadcast | DATA + RSSI at which the sender hears every node | 21 + 6 per reading |
| ACKMAP | leader → all | received bitmap, expected bitmap, ELECT flag | 13 |
| RESULT | leader → all | new leader, new term, alive bitmap, score, backup list | 20 |
| JOIN | node → leader | energy | 10 |

## 5. Election criterion and its justification

The leader computes the election when it receives the REPORTs, like LEACH-C's
base station:

```
score(c) = (70 * energy_norm(c) + 30 * link_norm(c)) / 100
energy_norm = remaining energy / initial energy           (0..1000)
link_norm   = average RSSI between c and the other participants,
              mapped from [-95, -30] dBm to [0, 1000]
```

Eligibility, as in LEACH-C, with each rule relaxed only if nobody qualifies:

1. energy ≥ average energy of the participants (the LEACH-C rule);
2. the worst known link ≥ −90 dBm, so the leader can reach every member;
3. energy ≥ `LE_E_LOW`;
4. on a *rotation* or *low-energy* election the current leader may not win.

Ties go to the lower node index.

**Why this criterion.** The leader is the node whose battery runs out first:
its radio is on for the whole frame. In Cooja, Energest measured 5.5 % radio-on
time for the leader against 0.8 % for a member. Remaining energy should therefore dominate (weight 70). That
alone is what spreads the load when the role rotates.

A leader that some member cannot hear causes lost beacons and failovers, so
link quality is the second component (weight 30). The minimum-link rule removes
candidates that would cut a member off.

Plain node ID is not a good criterion here. It is energy-blind, so the same
node would always win and be the first to die. It is used only for the
bootstrap coordinator, where nothing has been measured yet, and as a
tie-breaker. Number of neighbours carries no information in a single-hop
group, where everybody hears everybody.

**Where the link information comes from.** NullNet has no link estimator, so the protocol builds its own:

* Every received packet updates an RSSI EWMA for its sender
  (`packetbuf_attr(PACKETBUF_ATTR_RSSI)`, read inside the input callback). The
  CC2420 driver already converts it to dBm (register value + `RSSI_OFFSET` of
  −45) and sets the attribute in `cc2420_read()`, so it works under NullNet.
  This was verified in Cooja; check the values on the motes.
* In an ELECT round all members keep their radio on and overhear each other's
  REPORTs, so each REPORT contains a fresh RSSI table. For every pair (i, j),
  the node in the later slot has just heard the one in the earlier slot, so
  one ELECT round gives a complete matrix, even at bootstrap. Every node
  stores the matrix, so a future leader has it too.
* The packet reception ratio of the beacon link (members) and the uplink
  (leader) is estimated with WMEWMA (window T = 4, α = 0.6). It is reported in
  every DATA packet and logged.

## 6. When the leader relinquishes its role

| Trigger | What happens |
|---|---|
| Bootstrap | At boot everybody listens. The lowest index of the cluster starts it after `BOOT_WAIT` (6 s) and its first round is an election ("lowest ID starts as coordinator and runs the first election"). If that node is missing, the next index starts after 2·`BOOT_WAIT`, and so on. |
| Periodic rotation | Every `LE_ROTATION_ROUNDS` (24 rounds = 2 min) the leader runs an election in which it cannot win, unless every other node is below `LE_E_LOW` while the leader is not: then the leader keeps the role rather than hand it to a weaker node. |
| Low energy | When the leader's energy drops below `LE_E_LOW` and at least one active member is above it, the leader runs an election in which it cannot win. If everybody is low, rotation keeps spreading the load (this avoids an election every round). |
| Failure | Members detect the silence and a backup takes over (§7). |
| Manual | Serial command `elect`. |

**Planned handover.** The old leader sends RESULT at the end of the ELECT
frame. The new leader sends the next beacon exactly one period later, on the
old schedule. Members see no gap at all: their beacon arrives on time, from a
different sender with term + 1. The old leader listens for that beacon; if it
does not come (for example, the winner missed RESULT), it resumes leadership
and retries the election (`HANDOVER fail` in the log).

## 7. Failure detection and failover

* Members count consecutive rounds in which they heard **nothing** from the
  leader: no beacon copy and no ackmap (§9). After **K** such rounds
  (`LE_FAILURE_TIMEOUT_K`, default 3) the leader is considered failed.
* No election messages are needed at that point. Every beacon carries a
  **succession list** of 3 backups. The leader recomputes it every round with
  the same scoring, from the energy values that members send in their DATA.
  Every member still in the slot table is a candidate, also one whose last
  packet was lost; otherwise a single lost packet just before a link outage
  empties the list and nobody may take over.
  Backup j takes over after K + j silent rounds, so if backup 0 is dead too,
  backup 1 follows one period later. Within one window backup j also waits
  j × 80 ms longer (`LE_BACKUP_GAP_MS`). A backup that has counted one lost
  beacon more than backup 0 then hears backup 0's beacon first and follows it,
  instead of taking over at the same time.
* **Confirmation before takeover.** A backup does not take over as soon as one
  beacon is missing. It keeps listening until the leader's ackmap was due. If
  it hears the ackmap, the leader is alive and only the beacon was lost
  (`ALIVE` in the log). Members that suspect a failure stay awake for the same
  window, so they also hear the takeover beacon. With 10 % broadcast loss this
  cut false takeovers with K = 1 from 9 to 1 in 15 simulated minutes, and it
  costs about 0.35 s of extra recovery time.
* The new leader's first round is an election with reason `failover`. Its
  REPORTs carry the members' readings, so no data round is lost, and they give
  fresh energy and link information.
* **Two leaders.** Every leadership has a term number. A leader that hears a
  beacon with a higher term (or the same term from a lower index) steps down.
  Members follow the beacon with the highest term. A failed leader that
  recovers goes back to LISTEN, hears the current beacons and asks for a slot
  with JOIN.
* **Split brain.** A leader that hears no member for 3 rounds removes them
  all from its slot table. Its members are then almost certainly following
  another leader whose frames do not overlap with its own, so the two never
  hear each other. Such an *orphan* leader goes back to LISTEN with the radio
  on, finds the other leader and joins it (`ORPHAN` in the log). The analysis
  script counts the rounds in which two nodes led at the same time. A Cooja
  run with failures every 30 rounds and 10 % loss had 4–27 such rounds per run
  before these two rules were added (staggered backups and orphan leaders).
* The leader removes a member from the slot table after 3 rounds without its
  packet. A member without a slot sends JOIN in the contention slot, with
  random jitter on top of CSMA.

## 8. Keeping data flowing during re-election

Members sample every round, even while there is no leader, because they keep
following the predicted schedule. Each member keeps up to 6 unconfirmed
readings (`LE_BACKLOG`) and sends all of them in its next DATA or REPORT, so
an outage of up to 6 rounds loses no samples; they only arrive late. 6 covers
the worst case seen on the motes: the leader prunes a silent member after
3 rounds and gives up as an orphan, and the member takes over after K = 3
more. A planned election causes no gap, because REPORTs carry the data.

## 9. Operating under interference

A real 2.4 GHz room is noisy, and the protocol has to work in it rather than
in a quiet lab. On our desk (laptop Wi-Fi and USB ports nearby, motes 80 cm
apart) the first hardware runs lost 40–60 % of the beacons although the
signal was strong (−42 to −59 dBm), on channel 26 and on channel 20 alike.
The beacons did arrive, but with a failed CRC (`failed to parse 0` in the
log). The noise measurement (`noise=lowest,highest` in STAT, sampled just
before each beacon) showed why. The floor was −92 dBm on one mote and
−84 dBm on the other, and a third of the samples hit bursts of −50 to −33 dBm
from a nearby transmitter. With a single beacon and K = 2, this caused false
failovers and rounds with two leaders.

The protocol uses the error-control tools from the lectures:

| Tool (lecture) | In our protocol |
|---|---|
| ARQ with cumulative ACK and selective repeat (10B) | The ackmap is the cumulative ACK of the round; `expected & ~received` is a NACK list. Every member named in it retries in its retry slot. This includes a member that missed the beacon: it keeps listening until the ackmap was due, rebuilds the slot table from the expected bitmap (slots go in node-index order) and the ELECT flag, and retries (`ALIVE ... retry=1`). Retries are bounded: one retry round. |
| Time diversity against burst errors (10A interleaving, 10B bursty channels) | Every beacon and every RESULT is sent twice, 15 ms apart (`LE_BEACON_COPIES`, `LE_RESULT_COPIES`, `LE_BEACON_GAP_MS`). A burst that destroys one copy usually misses the other. |
| Passive link estimation by overhearing (10B) | Failure detection counts a round as missed only if nothing at all came from the leader. |
| A NACK needs a receiver that knows a packet was due (12) | In the TDMA frame a member knows when the beacon is due, and the leader knows who owns each slot, so missing frames are detectable on both sides. |

FEC is not used. The CC2420 drops a frame with a failed CRC in hardware, so
the software never sees the damaged bytes it would correct, and per-chunk
checksums (10B) do not pay off for 13–26-byte frames. The clear-channel check
(CCA, as in B-MAC, lecture 8) stays off inside TDMA slots. A busy channel
during a node's own slot can only be interference, and waiting for it pushes
the frame out of its slot. A noise-floor-adaptive CCA before the beacon is
possible future work.

Cost: one extra beacon per round (26 B, about 1 ms on air), so a normal
round has 3 control messages instead of 2, and one extra RESULT (20 B) per
election. A member that missed every beacon copy listens 70–130 ms longer
for the ackmap. Flash +0.8 KB; RAM +48 B, plus 252 B for the 6-reading
backlog (3.37 KB of 10 KB in all).

Cooja, 5 motes, 15 min, every frame lost with probability 40 % (UDGM
success ratio 0.6):

| | Before (1 beacon, K = 2) | After (2 copies, ackmap NACK, K = 3) |
|---|---|---|
| Slot success | 88.6 % | 98.7 % |
| Samples lost | 11 / 891 | 0 / 891 |
| Rounds in which a member heard nothing (`MISS`) | 73 | 8 |
| Missed beacons recovered via ackmap + retry | 0 | 15 |
| Handover fallbacks | 1 | 0 |
| Members wrongly pruned / re-JOINs | 1 / 7 | 0 / 0 |

With a leader crash every 40 rounds and 20 % loss, both versions detect all 4
failures with no false alarms; detection goes from 10.4 s (K = 2) to 15.4 s
(K = 3), slot success from 92.2 % to 100 %, lost samples from 2 to 0. Cooja
loses frames independently, while Wi-Fi bursts are correlated, so the second
beacon copy helps less on hardware than here.

On the motes (2 TelosB, 80 cm apart on a desk next to a laptop, channel 20,
10 min each). The share of member noise samples that hit a burst above
−75 dBm was 35 % in the first two runs and 42–50 % in the last two:

| | Before (1 beacon, K = 2) | 2 copies, ackmap NACK, K = 3 | + backup fix | + 2 RESULT copies, backlog 6 |
|---|---|---|---|---|
| Slot success | 46.8 % | 75.0 % | 82.6 % | 85.6 % |
| Delivery | 88.3 % | 95.0 % | 98.9 % | 100 % |
| False failovers | 5 | 0 | 0 | 0 |
| Rounds with two leaders | 15 | 0 | 0 | 0 |
| Orphan leaders | 17 | 3 | 1 | 0 |
| Rounds without a leader | many | 4 | 2 | 0 |
| Failed handovers (`HANDOVER fail`) | 5 | 0 | 3 | 0 |
| Rounds in which a member heard nothing (`MISS`) | 62 | 24 | 13 | 7 |
| Missed beacons recovered via ackmap + retry | 0 | 18 | 13 | 12 |
| Radio on per round, leader / member (median) | 256 / 84 ms | 256 / 84.5 ms | 153 / 57 ms | 153 / 48 ms |

All 13 samples lost in the second run came from two outages of about
3 rounds in both directions. Each time the last beacon before the outage
named no backup, because the leader built the list only from members it had
heard in that round. The member detected the failure but could not take over,
the leader gave up as an orphan, and the pair took 25–40 s to re-form, longer
than the 3-reading backlog. The list now keeps every member that is still in
the slot table. In a Cooja run with the same loss pattern (one round lost, one
good, 25 s of total loss) this turned 10 lost samples and a 26 s re-formation
into a takeover on the old schedule with no lost samples.

The third run had one such outage (3 rounds in which the leader got nothing
from the member). The member took over after K = 3 rounds on the old
schedule, the old leader joined it 70 ms later, and there were 2 rounds
without a leader. The 3 lost samples came from this outage: about 5 rounds of
readings had to wait, more than the backlog of 3 at the time, so the backlog is
now 6 (§8). In the same run the winner of the start-up election missed the
single RESULT three rounds in a row, so the old leader kept the role and
repeated the election (`HANDOVER fail`). No data was lost, but the RESULT is
now sent twice as well. In Cooja (5 motes, 30 min, 30 % broadcast loss) the
two changes together took handover fallbacks from 6 to 1 and lost samples
from 11 to 0; the RESULT copy alone gave 2 lost samples.

The fourth run, with both changes, delivered every sample (247 of 247). All
5 rotations handed over at the first attempt, and once a member delivered
4 buffered readings in one packet, which a backlog of 3 would have cut. It
had no long outage, so the takeover path was not exercised; the third run
shows it.

## 10. Multiple clusters

`LE_NODE_CLUSTERS` assigns each node to a cluster (`DEFINES=LE_TWO_CLUSTERS=1`
gives {0,1,2} and {3,4}). Nodes missing from the end of the table (for
example nodes 6–8 with the default 5-entry table) are in cluster 0. Every cluster runs the whole protocol independently:
its own bootstrap coordinator, leader, rotation and backups. Packets of other
clusters are dropped by the cluster id in the header. By default both clusters
share a channel and CSMA separates their frames. With
`LE_CLUSTER_OWN_CHANNEL=1` each cluster uses its own channel.

## 11. Energy model

Energy is simulated (`le-energy.c`), so that "low energy" and battery death
happen within an experiment:

| Event | Cost (units) |
|---|---|
| message sent / received | 2 / 1 |
| round as member (+ election round) | 1 (+2) |
| round as leader | 10 |
| initial energy | 2000, spread by 0–10 % between nodes |
| low threshold `LE_E_LOW` | 800 |

The real radio-on time is measured with Energest and logged in every `STAT`
line (`radio_ms`), so the simulated model can be compared with real duty
cycles.

## 12. How each metric is measured

`scripts/analyze.py` computes everything from the serial logs. Durations always
use the clock of one mote; events of different motes are matched by round and
term, which travel in the beacons.

| Metric | Source |
|---|---|
| Election time, planned | `ELECT dur_ms` at the leader (ELECT beacon → RESULT), and the members' `NEWLDR gap_ms` (last beacon of the old leader → first of the new one). |
| Election time, failure | `FAILDET since_ms` at the backup (detection) and the members' `NEWLDR gap_ms` (recovery). |
| Control messages | Per-node counters in `STAT`, summed over the group per round: normal round versus election round. |
| Communication overhead | Payload bytes per message type from `STAT`, plus an estimate of 802.15.4 headers and ACK frames. Reported as control share of messages, payload bytes and on-air bytes. |
| Memory | `make TARGET=sky size` (flash, .data + .bss) and the `BOOT` line (`ram_static`, `state_bytes`); peak stack (`STAT stack=`) from Contiki-NG's stack checker. |
| Disruption | `RD` lines at the leader (one per delivered reading, with the sample sequence number): lost samples, samples delivered late, rounds without a leader. |
| Energy balance | Energy left, rounds as leader per node, Jain fairness, round of the first death. |

## 13. Results in Cooja (simulation, before hardware)

Results from Cooja simulations: 5 sky motes in UDGM placed 5–35 m apart,
default parameters unless noted. They are expected values to compare with the
hardware measurements, not hardware results.

**Scenario `sim/leader-election.csc`** (10 min, the leader crashes at 60 s and comes back 40 s later; at 150 s the leader's energy is drained). Measured before the interference changes of §9, i.e. with one beacon per round and K = 2; with them a normal round has one more beacon, detection takes one period longer (K = 3) and the firmware is 0.7 KB larger:

| Metric | Result |
|---|---|
| Bootstrap | Index 0 starts the network; the first election (0.5 s) hands over to index 1 (most energy). |
| Election time, planned (rotation / low energy) | Decision 0.42–0.51 s at the leader. Members' beacon gap = period + 8 ms, so no round without a leader. |
| Election time, failure (K = 2) | Detection 10.35 s at the backup; members without a leader for 10.36 s (1 round); election of the new leader 0.43 s. |
| Control messages | Normal round: 2 (beacon + ackmap) + one DATA per member. Election round: beacon + ~4 REPORT + ackmap + RESULT, i.e. **+4.8 per election**, all in one round. |
| Communication overhead | 37.6 % of messages, 37.9 % of payload bytes, 30.2 % of estimated on-air bytes are control (6 elections in 10 min). Most of it is the beacon + ackmap that every round needs, not the elections. |
| Disruption | 583 samples, **0 lost**. During the failover 5 samples arrived one round late (backlog); a planned election delays 1 sample. |
| Memory | Flash 34.3 KB of 48 KB; RAM .data + .bss 3.06 KB of 10 KB (protocol state 641 B); peak stack 426 B. |
| Radio duty cycle (Energest) | Leader 277 ms per round (5.5 %), member 40 ms (0.8 %). |

**Rotation and lifetime** (45 min runs without failures until batteries die):

| Variant | First node dies at round | Planned handovers |
|---|---|---|
| Periodic rotation (24 rounds) + low-energy relinquish (default) | 231 | 12 |
| Low-energy relinquish only (`LE_ROTATION_ROUNDS=0`) | 236 | 7 |
| Fixed leader (`LE_ROTATION_ROUNDS=0,LE_E_LOW=0`) | 111 (the leader) | 1 |

Moving the role doubles the time to the first death. Relinquishing on low
energy alone gets the same lifetime with fewer elections. Periodic rotation
spreads the load continuously: the gap between the fullest and the emptiest
battery stays at 250–400 units (rounds 50–200), against 450–880 with
low-energy relinquish only and up to 1340 with a fixed leader. The price is
more elections.

**Failure-detection timeout K** (30 min each, the leader crashes every 30 rounds for 40 s, 10 % broadcast loss), with the firmware of §9 (two beacon copies, ackmap NACK, "missed" = nothing heard from the leader):

| K | Failovers | False failovers | Detection | Members without leader | Samples lost | Rounds with two leaders |
|---|---|---|---|---|---|---|
| 1 | 11 | 0 | 5.8 s | 5.3 s | 0 / 1193 | 0 |
| 2 | 11 | 0 | 10.3 s | 10.3 s | 1 / 1192 | 0 |
| 3 | 12 | 0 | 15.3 s | 15.3 s | 0 / 1242 | 0 |
| 4 | 10 | 0 | 20.3 s | 20.3 s | 34 / 1269 | 0 |

Detection time grows linearly (K · 5 s + 0.3 s). With the old firmware (one
beacon, missed beacon = missed round) the same runs gave 2 false failovers and
3 rounds with two leaders at K = 1, and 3, 5, 11 and 46 lost samples for
K = 1–4. With K = 4 the outage (4 rounds) is longer than the backlog
(3 readings in these runs), so samples are lost: the backlog should be at
least K. It is now 6 (§8).

In Cooja even K = 1 now has no false alarms, but Cooja loses frames
independently. On the motes, losses came in bursts of 2–3 rounds and K = 2
gave false failovers. **K = 3 is the default**: it still fits the backlog, and
costs 5 s of detection time over K = 2. The hardware runs should repeat this
table to confirm the choice.

**Two clusters** (`LE_TWO_CLUSTERS=1`, 10 min): both clusters elect and rotate
their own leader on the shared channel; 100 % delivery in both, no
interference.

## 14. Answer to the main question

*How can a WSN maintain coordination when the leader fails or becomes energy
constrained, while keeping election overhead low?*

* **Make leadership temporary and energy-driven.** Periodic rotation with an
  energy-dominated LEACH-C score spreads the cost of being leader, so no
  single node becomes energy constrained early.
* **Compute the next leaders in advance.** The leader keeps a ranked backup
  list up to date from data it already receives, and puts it in every beacon.
  A failover then needs no election messages: backup 0 just starts beaconing
  after K silent rounds. The cost is a few bytes per beacon.
* **Piggyback the election on normal traffic.** A planned election replaces
  the DATA of one round with REPORTs, which carry the readings, and adds only
  an ackmap and a RESULT. That is about 4–5 extra messages per election, and
  no data round is lost.
* **Trade detection speed against false alarms with K.** Recovery time grows
  as about K · period. A small K reacts fast but turns lost beacons into
  unnecessary takeovers.
* **Expect interference and recover locally.** Sending the beacon twice, using
  the ackmap as a NACK list with one retry round, and declaring a failure only
  after rounds with nothing at all from the leader keep coordination on a
  noisy channel. In normal operation this costs one extra 26-byte beacon per
  round (§9).
* **Buffer at the members.** A small backlog of unconfirmed readings turns the
  leaderless gap into delay instead of loss.

## 15. Limitations

* The energy model is simulated. Energest gives the real radio-on time but not
  the remaining battery charge.
* Single hop only. A member that cannot hear any candidate is lost until it can.
* Node IDs are hard-coded (`LE_NODE_IDS`). On the TelosB the ID is taken from
  the first two bytes of the link address, which come from the DS2411
  serial-number chip; it is printed at boot as `id=`. Contiki's own `node_id`
  cannot be used: with NullNet it is built from the last two bytes, the fixed
  vendor prefix, and is 4608 on every mote. If two motes show the same ID,
  use `DEFINES=LE_FORCE_INDEX=<i>`.
* Cooja's loss models (`loss <pct>` and the UDGM success ratio) lose frames
  independently. Real interference is bursty, so the gain from the second
  beacon copy must be measured on the motes.
* The simulated packet loss (`loss <pct>`) applies to broadcast frames only,
  because unicast frames have already been ACKed by the radio when the
  software sees them. For loss on unicast frames, use distance or a lower
  `LE_TX_POWER_DBM` on the motes, or the RX success ratio in Cooja.
