/*
 * le-conf.h - all hard-coded parameters of the leader-election protocol.
 *
 * Every value can be overridden at build time without editing this file:
 *   make TARGET=sky DEFINES=LE_FAILURE_TIMEOUT_K=3,LE_ROTATION_ROUNDS=0
 * (run "make TARGET=sky clean" first: the build system does not notice
 * changed DEFINES).
 */
#ifndef LE_CONF_H_
#define LE_CONF_H_

/*===========================================================================*/
/* Nodes and clusters                                                        */
/*===========================================================================*/

/* Upper bound for array sizes. Must be <= 16 (bitmaps are 16 bit). */
#ifndef LE_MAX_NODES
#define LE_MAX_NODES            8
#endif

/* Node IDs in index order (index 0 = first entry, it starts the network).
 * Real TelosB: use the "id=" number the mote prints at boot (from its
 * serial-number chip). Do NOT use Contiki's "Node ID" line: with NullNet it
 * is 4608 on every mote. Cooja: the mote IDs 1, 2, 3, ... also match. */
#ifndef LE_NODE_IDS
#define LE_NODE_IDS             { 45524, 57609 }  /* our 2 motes; Cooja: { 1, 2, 3, 4, 5 } */
#endif

/* If two motes happen to show the same id, flash them one at a time
 * with DEFINES=LE_FORCE_INDEX=<index> to bypass the lookup. */
/* #define LE_FORCE_INDEX 0 */

/* Cluster of each node, same order as LE_NODE_IDS. Every cluster elects its
 * own leader; packets of other clusters are ignored.
 * DEFINES=LE_TWO_CLUSTERS=1 switches to two clusters {0,1,2} and {3,4}. */
#ifndef LE_TWO_CLUSTERS
#define LE_TWO_CLUSTERS         0
#endif
#ifndef LE_NODE_CLUSTERS
#if LE_TWO_CLUSTERS
#define LE_NODE_CLUSTERS        { 0, 0, 0, 1, 1 }
#else
#define LE_NODE_CLUSTERS        { 0, 0, 0, 0, 0 }
#endif
#endif

/* 1: each cluster uses its own radio channel (LE_RF_CHANNEL - cluster), so
 * clusters do not interfere at all. 0: all clusters share one channel and
 * rely on CSMA + the cluster id in every header. */
#ifndef LE_CLUSTER_OWN_CHANNEL
#define LE_CLUSTER_OWN_CHANNEL  0
#endif

/*===========================================================================*/
/* Radio                                                                     */
/*===========================================================================*/
#ifndef LE_RF_CHANNEL
#define LE_RF_CHANNEL           26
#endif
/* CC2420 output power in dBm (-25 .. 0). Lower power spreads the RSSI values
 * of motes that sit close together, which gives the link metric more to
 * work with. */
#ifndef LE_TX_POWER_DBM
#define LE_TX_POWER_DBM         0
#endif

/* Clear-channel assessment threshold in dBm: the radio only transmits when
 * the channel is quieter than this. Contiki's CC2420 default is -90 dBm
 * (CC2420_CONF_CCA_THRESH -45 is a register value, plus the RSSI offset of
 * -45 dB). That is below the background noise in many rooms (our dorm scan:
 * -78 to -87 dBm), so the radio thinks the channel is always busy and hardly
 * ever sends. -75 dBm is the 802.15.4 energy-detection level. */
#ifndef LE_CCA_THRESH_DBM
#define LE_CCA_THRESH_DBM       (-75)
#endif
/* 1: check the channel (CCA) before every frame, as plain CSMA does.
 * 0: only before JOIN, the one frame sent in a shared contention slot. All
 * other frames have their own TDMA slot, so a busy channel can only be
 * noise, and waiting for it to clear just drops the frame. On our motes one
 * of them found the channel busy for ~1/3 of its frames even at -75 dBm. */
#ifndef LE_SLOT_CCA
#define LE_SLOT_CCA             0
#endif

/*===========================================================================*/
/* Frame timing (beacon-synchronised TDMA)                                   */
/*===========================================================================*/
/* Beacon period = one round. */
#ifndef LE_PERIOD_MS
#define LE_PERIOD_MS            5000
#endif
/* Members wake up this long before an expected beacon/ackmap/result.
 * The sky clock ticks every 7.8 ms (CLOCK_SECOND = 128) and both ends round
 * to a tick, so the guard must cover ~2 ticks: 10 ms is too tight. */
#ifndef LE_GUARD_MS
#define LE_GUARD_MS             30
#endif
/* Offset of slot 0 after the beacon, and slot length. A slot holds one
 * unicast frame with up to LE_DATA_TX_ATTEMPTS attempts (~4 ms each). */
#ifndef LE_FIRST_SLOT_MS
#define LE_FIRST_SLOT_MS        40
#endif
#ifndef LE_SLOT_MS
#define LE_SLOT_MS              40
#endif
/* How long members listen after the expected beacon time. Must be larger
 * than LE_HANDOVER_WAIT_MS. */
#ifndef LE_BEACON_WAIT_MS
#define LE_BEACON_WAIT_MS       120
#endif
/* Time diversity against burst interference (Lecture 10A/10B: errors come
 * in bursts, e.g. Wi-Fi frames): the leader sends every beacon
 * LE_BEACON_COPIES times, LE_BEACON_GAP_MS apart. Each copy carries its
 * number, so a member that hears only a later copy still knows when the
 * round started. 1 = a single beacon per round. All copies must be sent
 * before slot 0. */
#ifndef LE_BEACON_COPIES
#define LE_BEACON_COPIES        2
#endif
#ifndef LE_BEACON_GAP_MS
#define LE_BEACON_GAP_MS        15
#endif
/* The same for the RESULT of an election: if the winner misses it, the
 * handover fails and the election is repeated in the next round. Copies
 * are LE_BEACON_GAP_MS apart and must fit in LE_RX_WINDOW_MS. */
#ifndef LE_RESULT_COPIES
#define LE_RESULT_COPIES        2
#endif
/* How long members listen after an expected ackmap/result. */
#ifndef LE_RX_WINDOW_MS
#define LE_RX_WINDOW_MS         60
#endif
/* MAC transmission attempts for a DATA frame inside its slot. */
#ifndef LE_DATA_TX_ATTEMPTS
#define LE_DATA_TX_ATTEMPTS     3
#endif
/* Readings kept by a member until the leader confirms them (link-layer ACK
 * or ackmap bit). Readings sampled while there is no leader are delivered
 * once a leader is back. The worst outage seen on the motes: the leader
 * prunes a silent member after LE_MAX_MISSED_REPORTS (3) rounds, then the
 * member takes over after K (3) more, so 6. */
#ifndef LE_BACKLOG
#define LE_BACKLOG              6
#endif
/* 1: a member that got a link-layer ACK for its DATA does not wake up for
 * the ackmap (it already knows the leader has it). */
#ifndef LE_SKIP_ACKMAP_ON_LLACK
#define LE_SKIP_ACKMAP_ON_LLACK 1
#endif

/*===========================================================================*/
/* Bootstrap, failure detection and succession                               */
/*===========================================================================*/
/* At boot every node listens with the radio on. If no beacon is heard within
 * LE_BOOT_WAIT_MS * (1 + rank of the node inside its cluster), the node
 * starts the cluster itself. So the lowest index starts first ("lowest ID
 * starts as leader and runs the first election"), the next one only if the
 * first is missing, and so on. Must be longer than one period. */
#ifndef LE_BOOT_WAIT_MS
#define LE_BOOT_WAIT_MS         (LE_PERIOD_MS + 1000)
#endif
/* Failure-detection timeout: the leader is declared failed after K
 * consecutive rounds in which a member heard nothing from it: no beacon
 * copy and no ackmap (passive estimation by overhearing, Lecture 10B).
 * K = 2 gave false failovers on real motes with Wi-Fi nearby. */
#ifndef LE_FAILURE_TIMEOUT_K
#define LE_FAILURE_TIMEOUT_K    3
#endif
/* Length of the succession (backup) list that the leader keeps up to date
 * and puts in every beacon. Backup j takes over after
 * K + j * LE_BACKUP_STAGGER silent rounds (nothing heard from the leader). */
#ifndef LE_SUCC_LEN
#define LE_SUCC_LEN             3
#endif
#ifndef LE_BACKUP_STAGGER
#define LE_BACKUP_STAGGER       1
#endif
/* A backup does not take over as soon as the beacon is missing: it keeps
 * listening until the leader's ackmap was due (hearing it proves the leader
 * is alive) and sends its own beacon this long after that window. Members
 * that suspect a failure stay awake for that window too. */
#ifndef LE_TAKEOVER_DELAY_MS
#define LE_TAKEOVER_DELAY_MS    20
#endif
/* Backup j takes over j * LE_BACKUP_GAP_MS later than backup 0 within the
 * same window (in case it counted one missed beacon more), so it hears
 * backup 0's beacon first and follows it instead. */
#ifndef LE_BACKUP_GAP_MS
#define LE_BACKUP_GAP_MS        80
#endif
/* After a planned handover the old leader listens this long for the new
 * leader's first beacon; if it does not come, the old leader resumes. */
#ifndef LE_HANDOVER_WAIT_MS
#define LE_HANDOVER_WAIT_MS     60
#endif
/* The leader drops a member from the slot table after this many
 * consecutive rounds without its packet (it can come back with JOIN). */
#ifndef LE_MAX_MISSED_REPORTS
#define LE_MAX_MISSED_REPORTS   3
#endif

/*===========================================================================*/
/* Election (LEACH-C, centralised on the leader)                             */
/*===========================================================================*/
/* Periodic rotation: start an election every N rounds as leader (0 = off). */
#ifndef LE_ROTATION_ROUNDS
#define LE_ROTATION_ROUNDS      24
#endif
/* Score = (W_E * energy_norm + W_L * link_norm) / (W_E + W_L), each in
 * 0..1000. */
#ifndef LE_W_ENERGY
#define LE_W_ENERGY             70
#endif
#ifndef LE_W_LINK
#define LE_W_LINK               30
#endif
/* RSSI range mapped linearly to link_norm 0..1000 (dBm). */
#ifndef LE_RSSI_FLOOR
#define LE_RSSI_FLOOR           (-95)
#endif
#ifndef LE_RSSI_CEIL
#define LE_RSSI_CEIL            (-30)
#endif
/* A candidate whose worst known link is below this cannot be elected. */
#ifndef LE_RSSI_MIN_LINK
#define LE_RSSI_MIN_LINK        (-90)
#endif
/* LEACH-C rule: only nodes with at least the average energy are candidates. */
#ifndef LE_LEACHC_ABOVE_AVG
#define LE_LEACHC_ABOVE_AVG     1
#endif

/*===========================================================================*/
/* Link estimation                                                           */
/*===========================================================================*/
/* RSSI EWMA: avg += (sample - avg) / 2^SHIFT */
#ifndef LE_RSSI_EWMA_SHIFT
#define LE_RSSI_EWMA_SHIFT      2
#endif
/* WMEWMA packet reception ratio (Woo & Culler): every T expected packets
 * prr = alpha * prr + (1 - alpha) * received/T. */
#ifndef LE_PRR_WINDOW_T
#define LE_PRR_WINDOW_T         4
#endif
#ifndef LE_PRR_ALPHA_PCT
#define LE_PRR_ALPHA_PCT        60
#endif

/*===========================================================================*/
/* Simulated energy model (units are arbitrary "energy units")               */
/*===========================================================================*/
#ifndef LE_E_INIT
#define LE_E_INIT               2000
#endif
/* Spread of the initial energy between nodes, in percent of LE_E_INIT
 * (deterministic per index). 0 = everybody starts equal. */
#ifndef LE_E_INIT_SPREAD_PCT
#define LE_E_INIT_SPREAD_PCT    10
#endif
#ifndef LE_E_TX
#define LE_E_TX                 2   /* per transmitted message */
#endif
#ifndef LE_E_RX
#define LE_E_RX                 1   /* per received message */
#endif
#ifndef LE_E_ROUND_MEMBER
#define LE_E_ROUND_MEMBER       1   /* per round as member (wake-ups) */
#endif
#ifndef LE_E_ROUND_ELECT
#define LE_E_ROUND_ELECT        2   /* extra for a member in an election round */
#endif
#ifndef LE_E_ROUND_LEADER
#define LE_E_ROUND_LEADER       10  /* per round as leader (radio on for the frame) */
#endif
/* Below this level the leader relinquishes its role and is not eligible. */
#ifndef LE_E_LOW
#define LE_E_LOW                800
#endif
/* 1: a node whose energy reaches 0 dies (stops for good). */
#ifndef LE_E_DEATH
#define LE_E_DEATH              1
#endif

/*===========================================================================*/
/* Experiments                                                               */
/*===========================================================================*/
/* Automatic leader failure: the node that is leader at round
 * LE_FAIL_AT_ROUND crashes (0 = off). With LE_FAIL_EVERY_ROUNDS > 0 the
 * current leader also crashes every N rounds after that. */
#ifndef LE_FAIL_AT_ROUND
#define LE_FAIL_AT_ROUND        0
#endif
#ifndef LE_FAIL_EVERY_ROUNDS
#define LE_FAIL_EVERY_ROUNDS    0
#endif
/* How long a simulated failure lasts, seconds (0 = until reset). After it
 * the node rejoins as a member. Used by automatic failures and the button. */
#ifndef LE_FAIL_DURATION_S
#define LE_FAIL_DURATION_S      60
#endif
/* Simulated packet loss at every receiver, percent (also settable at run
 * time with the serial command "loss <pct>"). Applies to broadcast frames
 * only (beacon, report, ackmap, result): unicast frames are acknowledged by
 * the CC2420 before the application sees them, so dropping them in software
 * would not be a realistic loss. */
#ifndef LE_RX_LOSS_PCT
#define LE_RX_LOSS_PCT          0
#endif
/* 1: read temperature from the SHT11 (blocks ~0.2 s, done outside frames).
 * 0: light only. */
#ifndef LE_USE_SHT11
#define LE_USE_SHT11            1
#endif
/* Print a STAT line every N rounds. */
#ifndef LE_STAT_EVERY
#define LE_STAT_EVERY           1
#endif

#endif /* LE_CONF_H_ */
