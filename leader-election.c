/*
 * leader-election.c - leader election and leader rotation in a single-hop
 * TelosB group (Contiki-NG, NullNet over CSMA).
 *
 * One round = one beacon period (LE_PERIOD_MS). Frame of a round, all
 * offsets relative to the end of the beacon (t0):
 *
 *   t0          beacon (DATA or ELECT round, slot table, backups)
 *   +FIRST+i*SLOT   slot i: member slot[i] sends DATA (unicast, link ACK)
 *                   or, in an ELECT round, REPORT (broadcast, overheard)
 *   +FIRST+n*SLOT   contention slot: JOIN from nodes without a slot
 *   +B          ackmap: bitmap of what the leader received       (tb)
 *   tb+FIRST+i*SLOT retry slot i, only for members whose bit is 0
 *   tb+R        ELECT rounds only: RESULT (new leader, term, backups)
 *   ...         everybody sleeps (radio off) until t0 + PERIOD - GUARD
 *
 * Election: LEACH-C, computed by the leader from the REPORTs (energy +
 * link table) - see le-election.c.
 * Failure: members count missed beacons; after K misses backup 0 from the
 * succession list in the last beacon takes over, backup j after K + j.
 * Conflicts (two leaders): higher term wins, then lower index.
 */
#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "net/packetbuf.h"
#include "net/mac/mac.h"
#include "net/linkaddr.h"
#include "sys/node-id.h"
#include "sys/ctimer.h"
#include "dev/leds.h"
#include "dev/serial-line.h"
#include "lib/random.h"
#ifdef CONTIKI_TARGET_SKY
#include "dev/button-sensor.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "le-conf.h"
#include "le-msg.h"
#include "le-election.h"
#include "le-link.h"
#include "le-energy.h"
#include "le-stats.h"
#include "le-sensor.h"

/*===========================================================================*/
/* Configuration tables                                                      */
/*===========================================================================*/
static const uint16_t node_ids[] = LE_NODE_IDS;
static const uint8_t node_cluster[] = LE_NODE_CLUSTERS;
#define N_NODES ((uint8_t)(sizeof(node_ids) / sizeof(node_ids[0])))

#define BIT(i)  ((uint16_t)1 << (i))

/*===========================================================================*/
/* Time helpers                                                              */
/*===========================================================================*/
#define MS2T(ms) ((clock_time_t)(((uint32_t)(ms) * CLOCK_SECOND + 500) / 1000))
#define T2MS(t)  ((uint32_t)(t) * 1000 / CLOCK_SECOND)
#define HALF_RANGE (((clock_time_t)~(clock_time_t)0) >> 1)

static clock_time_t
until(clock_time_t t)
{
  clock_time_t d = t - clock_time();
  return d > HALF_RANGE ? 0 : d;
}

/* frame offsets in ms, n = number of slots */
#define SLOT_OFF(i)   (LE_FIRST_SLOT_MS + (uint32_t)(i) * LE_SLOT_MS)
#define JOIN_OFF(n)   SLOT_OFF(n)
#define ACK_OFF(n)    (SLOT_OFF((n) + 1) + LE_GUARD_MS)
#define RETRY_LEN(n)  (SLOT_OFF(n) + LE_GUARD_MS)
/* a backup takes over only after the leader's ackmap was due as well */
#define TAKEOVER_OFF(n) (ACK_OFF(n) + LE_RX_WINDOW_MS + LE_TAKEOVER_DELAY_MS)
/* backup j waits j gaps longer, so it hears backup 0's beacon first */
#define BACKUP_OFF(n, j) (TAKEOVER_OFF(n) + (uint32_t)(j) * LE_BACKUP_GAP_MS)
#define SUSPECT_END(n)  (BACKUP_OFF(n, LE_SUCC_LEN - 1) + LE_BEACON_WAIT_MS)

/*===========================================================================*/
/* State                                                                     */
/*===========================================================================*/
typedef enum { ST_LISTEN, ST_MEMBER, ST_LEADER, ST_FAILED, ST_DEAD } state_t;
static const char *const state_name[] = { "LISTEN", "MEMBER", "LEADER", "FAILED", "DEAD" };
static const char *const reason_name[] = {
  "none", "bootstrap", "rotation", "low_energy", "failover", "manual", "retry"
};

/* events recorded during the frame, printed afterwards */
typedef enum {
  EV_NEWLDR = 1, EV_FAILDET, EV_TAKEOVER, EV_HANDOVER_OK, EV_HANDOVER_FAIL,
  EV_STEPDOWN, EV_RESULT_RX, EV_PRUNE, EV_JOIN, EV_MISS, EV_ALIVE
} ev_type_t;

typedef struct {
  uint8_t type;
  uint8_t a, b, d;
  uint16_t c;
  uint32_t t_ms;
  uint32_t v;
} ev_t;
#define EV_QUEUE 8

typedef struct {
  uint8_t src;
  le_reading_t r;
} rd_t;
#define RD_MAX (LE_MAX_NODES * LE_BACKLOG)

static struct {
  state_t state;
  uint8_t me, cluster, cluster_rank, cluster_size;
  uint16_t cluster_mask;
  uint16_t term, round;
  uint8_t leader;               /* current leader, LE_NONE if unknown */
  clock_time_t t0;              /* end of this round's beacon (or predicted) */
  clock_time_t tb;              /* end of this round's ackmap (or predicted) */
  clock_time_t last_beacon;     /* last beacon actually received */
  uint8_t missed;               /* consecutive missed beacons */
  uint8_t fail_detected;
  uint8_t succ[LE_SUCC_LEN], n_succ;

  /* current round, as announced in the beacon */
  uint8_t elect, reason, n_slots, slot[LE_MAX_NODES], my_slot;
  uint8_t n_last;               /* slots in the last beacon actually received */
  uint16_t expected;            /* bitmap of slot owners */
  uint8_t acked;                /* my packet of this round is confirmed */

  /* member side */
  uint8_t pending_leader;       /* RESULT named me: lead from next round */
  uint16_t pending_term, pending_alive;
  uint8_t handover_wait;        /* I handed over: wait for the new leader */
  le_prr_t beacon_prr;
  le_reading_t backlog[LE_BACKLOG];
  uint8_t n_backlog;
  uint16_t seq;
  uint16_t dropped;

  /* leader side */
  uint16_t active;              /* members that get a slot */
  uint16_t join_req;
  uint16_t rx_map;              /* DATA/REPORT received this round */
  uint8_t miss_cnt[LE_MAX_NODES];
  uint16_t energy[LE_MAX_NODES];/* last known energy of every node */
  le_prr_t prr[LE_MAX_NODES];   /* uplink PRR as seen by the leader */
  uint16_t rounds_as_leader;
  uint8_t elect_next, elect_reason_next;
  uint16_t low_energy_cooldown;
  clock_time_t elect_start;
  uint32_t elect_dur_ms;        /* ELECT beacon -> RESULT sent */
  uint32_t elect_ctrl_start;
  uint8_t result_winner;
  uint16_t result_term;
  le_elect_out_t eout;          /* last election (for the log) */
  uint8_t elect_logged;
  rd_t rd[RD_MAX];
  uint8_t n_rd;

  uint8_t loss_pct;
  uint8_t tx_state;             /* role when the pending frame was queued */
  clock_time_t listen_since;
  ev_t ev[EV_QUEUE];
  uint8_t n_ev;
} s;

static linkaddr_t node_addr[LE_MAX_NODES];   /* learned from received frames */
static struct ctimer seq_timer;              /* frame sequencer */
static struct ctimer fail_timer;
static uint8_t txbuf[sizeof(le_report_t) + 8];

PROCESS(le_process, "Leader election");
AUTOSTART_PROCESSES(&le_process);

static void m_wake_beacon(void *ptr);
static void m_round_end(void *ptr);
static void l_send_beacon(void);
static void l_round_end(void *ptr);
static void enter_listen(uint32_t wait_ms);
static void fail_start(uint16_t seconds);
static void accept_beacon(const le_beacon_t *b);

/*===========================================================================*/
/* Small helpers                                                             */
/*===========================================================================*/
static void
at(clock_time_t t, void (*fn)(void *))
{
  ctimer_set(&seq_timer, until(t), fn, NULL);
}

static void
radio_on(void)
{
  NETSTACK_RADIO.on();
}

static void
radio_off(void)
{
  NETSTACK_RADIO.off();
}

static void
set_leds(void)
{
  leds_off(LEDS_ALL);
  switch(s.state) {
  case ST_LEADER: leds_on(LEDS_BLUE); break;
  case ST_MEMBER: leds_on(LEDS_GREEN); break;
  case ST_FAILED: case ST_DEAD: leds_on(LEDS_RED); break;
  default: break;
  }
}

#define LOG_PREFIX() printf("LE %u %lu ", s.me, (unsigned long)le_now_ms())

static void
ev_push(uint8_t type, uint8_t a, uint8_t b, uint16_t c, uint8_t d, uint32_t v)
{
  if(s.n_ev < EV_QUEUE) {
    ev_t *e = &s.ev[s.n_ev++];
    e->type = type;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    e->v = v;
    e->t_ms = le_now_ms();
  }
}

static void
log_role(void)
{
  LOG_PREFIX();
  printf("ROLE role=%s term=%u leader=%d round=%u\n", state_name[s.state],
         s.term, s.leader == LE_NONE ? -1 : s.leader, s.round);
  set_leds();
}

static int8_t
slot_of(uint8_t idx)
{
  uint8_t i;
  for(i = 0; i < s.n_slots; i++) {
    if(s.slot[i] == idx) {
      return (int8_t)i;
    }
  }
  return -1;
}

static int8_t
succ_rank(uint8_t idx)
{
  uint8_t i;
  for(i = 0; i < s.n_succ; i++) {
    if(s.succ[i] == idx) {
      return (int8_t)i;
    }
  }
  return -1;
}

/* 16-bit ID from the unique part of the link address. On the TelosB the
 * link address comes from the DS2411 serial-number chip; with NullNet its
 * LAST two bytes are the fixed prefix 0x12 0x00, so Contiki's node_id is the
 * same (4608) on every mote. The FIRST two bytes are serial-number bytes:
 * this is the number the same mote shows as "Node ID" in an IPv6 build. */
static uint16_t
serial_id(void)
{
  return ((uint16_t)linkaddr_node_addr.u8[1] << 8) | linkaddr_node_addr.u8[0];
}

static void
hdr(le_hdr_t *h, uint8_t type)
{
  h->magic = LE_MAGIC;
  h->type = type;
  h->cluster = s.cluster;
  h->src = s.me;
  h->term = s.term;
  h->round = s.round;
}

/*===========================================================================*/
/* Sending                                                                   */
/*===========================================================================*/
typedef void (*tx_done_t)(int status);

static void
mac_sent(void *ptr, int status, int num_tx)
{
  le_stats.mac_tx += num_tx;
  if(s.state == ST_FAILED || s.state == ST_DEAD || s.state != s.tx_state) {
    return;   /* crashed or changed role while the frame was queued */
  }
  if(ptr != NULL) {
    ((tx_done_t)ptr)(status);
  }
}

/* dest = LE_NONE for broadcast */
static void
send_msg(uint16_t len, uint8_t dest, uint8_t attempts, tx_done_t done)
{
  le_hdr_t *h = (le_hdr_t *)txbuf;
  packetbuf_clear();
  packetbuf_copyfrom(txbuf, len);
  if(dest == LE_NONE) {
    packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &linkaddr_null);
  } else {
    packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &node_addr[dest]);
  }
  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &linkaddr_node_addr);
  packetbuf_set_attr(PACKETBUF_ATTR_MAX_MAC_TRANSMISSIONS, attempts);
  le_stats_tx(h->type, len);
  le_energy_use(LE_E_TX);
  s.tx_state = s.state;
  NETSTACK_MAC.send(mac_sent, (void *)done);
}

/*===========================================================================*/
/* Readings and backlog (member)                                             */
/*===========================================================================*/
static void
sample(void)
{
  le_reading_t r;
  le_sensor_read(&r);
  r.seq = s.seq++;
  if(s.state == ST_LEADER) {
    /* the leader's own reading goes straight to the log */
    if(s.n_rd < RD_MAX) {
      s.rd[s.n_rd].src = s.me;
      s.rd[s.n_rd].r = r;
      s.n_rd++;
    }
    return;
  }
  if(s.n_backlog == LE_BACKLOG) {
    memmove(&s.backlog[0], &s.backlog[1], sizeof(le_reading_t) * (LE_BACKLOG - 1));
    s.n_backlog--;
    s.dropped++;
  }
  s.backlog[s.n_backlog++] = r;
}

static void
confirm_backlog(void)
{
  s.n_backlog = 0;
  s.acked = 1;
}

/*===========================================================================*/
/* Logging (only called outside the frame)                                   */
/*===========================================================================*/
static void
print_events(void)
{
  uint8_t i;
  for(i = 0; i < s.n_ev; i++) {
    ev_t *e = &s.ev[i];
    printf("LE %u %lu ", s.me, (unsigned long)e->t_ms);
    switch(e->type) {
    case EV_NEWLDR:
      printf("NEWLDR from=%d to=%u term=%u gap_ms=%lu missed=%u\n",
             e->a == LE_NONE ? -1 : e->a, e->b, e->c, (unsigned long)e->v, e->d);
      break;
    case EV_FAILDET:
      printf("FAILDET leader=%u since_ms=%lu missed=%u k=%u\n",
             e->a, (unsigned long)e->v, e->d, LE_FAILURE_TIMEOUT_K);
      break;
    case EV_TAKEOVER:
      printf("TAKEOVER prev=%u rank=%u term=%u missed=%u r=%lu\n", e->a, e->b, e->c, e->d,
             (unsigned long)e->v);
      break;
    case EV_HANDOVER_OK:
      printf("HANDOVER ok new=%u term=%u\n", e->a, e->c);
      break;
    case EV_HANDOVER_FAIL:
      printf("HANDOVER fail new=%u\n", e->a);
      break;
    case EV_STEPDOWN:
      printf("STEPDOWN other=%u term=%u\n", e->a, e->c);
      break;
    case EV_RESULT_RX:
      printf("RESULT new=%u term=%u\n", e->a, e->c);
      break;
    case EV_PRUNE:
      printf("PRUNE node=%u\n", e->a);
      break;
    case EV_JOIN:
      printf("JOIN node=%u\n", e->a);
      break;
    case EV_MISS:
      printf("MISS n=%u\n", e->d);
      break;
    case EV_ALIVE:
      printf("ALIVE leader=%u missed_beacon=%u\n", e->a, e->d);
      break;
    }
  }
  s.n_ev = 0;
}

static void
print_stat(void)
{
  if(LE_STAT_EVERY == 0 || (s.round % LE_STAT_EVERY) != 0) {
    return;
  }
  LOG_PREFIX();
  printf("STAT r=%u role=%s e=%u tx=%u,%u,%u,%u,%u,%u txB=%lu,%lu,%lu,%lu,%lu,%lu "
         "rx=%u,%u,%u,%u,%u,%u mac=%lu radio_ms=%lu txon_ms=%lu drop=%u nbr=%u stack=%ld\n",
         s.round, state_name[s.state], le_energy(),
         le_stats.tx[1], le_stats.tx[2], le_stats.tx[3],
         le_stats.tx[4], le_stats.tx[5], le_stats.tx[6],
         (unsigned long)le_stats.tx_bytes[1], (unsigned long)le_stats.tx_bytes[2],
         (unsigned long)le_stats.tx_bytes[3], (unsigned long)le_stats.tx_bytes[4],
         (unsigned long)le_stats.tx_bytes[5], (unsigned long)le_stats.tx_bytes[6],
         le_stats.rx[1], le_stats.rx[2], le_stats.rx[3],
         le_stats.rx[4], le_stats.rx[5], le_stats.rx[6],
         (unsigned long)le_stats.mac_tx,
         (unsigned long)(le_radio_listen_ms() + le_radio_tx_ms()),
         (unsigned long)le_radio_tx_ms(), s.dropped, le_link_neighbors(),
         le_stack_peak());
}

static void
print_election(void)
{
  uint8_t i;
  if(!s.elect_logged) {
    return;
  }
  s.elect_logged = 0;
  LOG_PREFIX();
  printf("ELECT r=%u reason=%s winner=%u term=%u dur_ms=%lu ctrl_local=%lu relaxed=%u cand=",
         s.round, reason_name[s.reason], s.result_winner, s.result_term,
         (unsigned long)s.elect_dur_ms,
         (unsigned long)(le_stats_ctrl_msgs() - s.elect_ctrl_start), s.eout.relaxed);
  for(i = 0; i < s.eout.n_rank; i++) {
    uint8_t c = s.eout.rank[i];
    printf("%s%u:%u:%u:%d%s", i ? "," : "", c, s.eout.score[c], s.energy[c],
           s.eout.link_avg[c], (s.eout.eligible & BIT(c)) ? "" : "x");
  }
  printf(" succ=");
  for(i = 0; i < s.n_succ; i++) {
    printf("%s%u", i ? "," : "", s.succ[i]);
  }
  printf("\n");
}

/*===========================================================================*/
/* Becoming leader                                                           */
/*===========================================================================*/
static void
become_leader(uint16_t term, uint16_t active, uint8_t elect, uint8_t reason)
{
  uint8_t i;
  s.state = ST_LEADER;
  s.leader = s.me;
  s.term = term;
  s.active = active & s.cluster_mask & ~BIT(s.me);
  s.join_req = 0;
  s.rounds_as_leader = 0;
  s.elect_next = elect;
  s.elect_reason_next = reason;
  s.handover_wait = 0;
  s.pending_leader = 0;
  s.missed = 0;
  s.fail_detected = 0;
  for(i = 0; i < N_NODES; i++) {
    s.miss_cnt[i] = 0;
    le_prr_init(&s.prr[i]);
  }
  /* readings still in the backlog are now the leader's own */
  for(i = 0; i < s.n_backlog && s.n_rd < RD_MAX; i++) {
    s.rd[s.n_rd].src = s.me;
    s.rd[s.n_rd].r = s.backlog[i];
    s.n_rd++;
  }
  s.n_backlog = 0;
  log_role();
}

/*===========================================================================*/
/* Member: frame                                                             */
/*===========================================================================*/
static void m_wake_result(void *ptr);
static void m_retry(void *ptr);

/* after the ackmap (received or not) */
static void
m_after_ack(void)
{
  if(s.elect) {
    radio_off();
    at(s.tb + MS2T(RETRY_LEN(s.n_slots)) - MS2T(LE_GUARD_MS), m_wake_result);
  } else {
    m_round_end(NULL);
  }
}

static void
m_retry_done(int status)
{
  if(!s.elect && status == MAC_TX_OK) {
    confirm_backlog();
  }
  radio_off();
  m_after_ack();
}

static void
m_ack_timeout(void *ptr)
{
  radio_off();
  s.tb = s.t0 + MS2T(ACK_OFF(s.n_slots));     /* predicted */
  if(!s.acked && s.my_slot != LE_NONE) {
    at(s.tb + MS2T(SLOT_OFF(s.my_slot)), m_retry);
  } else {
    m_after_ack();
  }
}

static void
m_wake_ack(void *ptr)
{
  radio_on();
  at(s.t0 + MS2T(ACK_OFF(s.n_slots) + LE_RX_WINDOW_MS), m_ack_timeout);
}

static uint16_t
build_uplink(void)
{
  if(s.elect) {
    le_report_t *m = (le_report_t *)txbuf;
    hdr(&m->h, LE_MSG_REPORT);
    m->energy = le_energy();
    m->beacon_rssi = le_link_rssi(s.leader);
    m->beacon_prr = le_prr_get(&s.beacon_prr);
    le_link_my_row(m->rssi);
    m->n_readings = s.n_backlog;
    memcpy(m->r, s.backlog, s.n_backlog * sizeof(le_reading_t));
    return LE_REPORT_LEN(s.n_backlog);
  } else {
    le_data_t *m = (le_data_t *)txbuf;
    hdr(&m->h, LE_MSG_DATA);
    m->energy = le_energy();
    m->beacon_rssi = le_link_rssi(s.leader);
    m->beacon_prr = le_prr_get(&s.beacon_prr);
    m->n_readings = s.n_backlog;
    memcpy(m->r, s.backlog, s.n_backlog * sizeof(le_reading_t));
    return LE_DATA_LEN(s.n_backlog);
  }
}

static void
m_tx_done(int status)
{
  if(!s.elect && status == MAC_TX_OK) {
    confirm_backlog();
    if(LE_SKIP_ACKMAP_ON_LLACK) {
      radio_off();
      m_round_end(NULL);
      return;
    }
  }
  if(!s.elect) {
    radio_off();   /* in ELECT rounds keep listening to the other REPORTs */
  }
  at(s.t0 + MS2T(ACK_OFF(s.n_slots)) - MS2T(LE_GUARD_MS), m_wake_ack);
}

static void
m_tx(void *ptr)
{
  uint16_t len;
  radio_on();
  len = build_uplink();
  if(s.elect) {
    send_msg(len, LE_NONE, 1, m_tx_done);
  } else {
    send_msg(len, s.leader, LE_DATA_TX_ATTEMPTS, m_tx_done);
  }
  leds_toggle(LEDS_GREEN);
}

static void
m_retry(void *ptr)
{
  uint16_t len;
  radio_on();
  len = build_uplink();
  if(s.elect) {
    send_msg(len, LE_NONE, 1, m_retry_done);
  } else {
    send_msg(len, s.leader, LE_DATA_TX_ATTEMPTS, m_retry_done);
  }
}

static void
m_join_done(int status)
{
  radio_off();
  at(s.t0 + MS2T(ACK_OFF(s.n_slots) + LE_RX_WINDOW_MS), m_round_end);
}

static void
m_join(void *ptr)
{
  le_join_t *m = (le_join_t *)txbuf;
  radio_on();
  hdr(&m->h, LE_MSG_JOIN);
  m->energy = le_energy();
  send_msg(sizeof(*m), s.leader, 2, m_join_done);
}

static void
m_result_timeout(void *ptr)
{
  m_round_end(NULL);
}

static void
m_wake_result(void *ptr)
{
  radio_on();
  at(s.tb + MS2T(RETRY_LEN(s.n_slots) + LE_RX_WINDOW_MS), m_result_timeout);
}

/* plan the rest of the round after a beacon was accepted */
static void
member_plan_round(void)
{
  if(s.my_slot != LE_NONE) {
    clock_time_t tx = s.t0 + MS2T(SLOT_OFF(s.my_slot));
    if(!s.elect && until(tx) > 2) {
      radio_off();
    }
    at(tx, m_tx);
  } else {
    /* not in the slot table: ask for a slot in the contention slot */
    clock_time_t tj = s.t0 + MS2T(JOIN_OFF(s.n_slots)) +
      (random_rand() % (MS2T(LE_SLOT_MS / 2) + 1));
    if(!s.elect) {
      radio_off();
    }
    at(tj, m_join);
  }
}

static void
m_takeover(void *ptr)
{
  int8_t rank = succ_rank(s.me);
  uint8_t prev = s.leader;
  /* this window counts as missed too */
  s.missed++;
  s.t0 += MS2T(LE_PERIOD_MS);
  s.round++;
  le_prr_update(&s.beacon_prr, 0);
  ev_push(EV_FAILDET, prev, 0, 0, s.missed, T2MS(clock_time() - s.last_beacon));
  ev_push(EV_TAKEOVER, prev, (uint8_t)rank, s.term + 1, s.missed, s.round);
  become_leader(s.term + 1,
                s.cluster_mask & ~BIT(s.me) & (prev == LE_NONE ? 0xFFFF : ~BIT(prev)),
                1, LE_REASON_FAILOVER);
  l_send_beacon();
}

static void
m_handover_fallback(void *ptr)
{
  /* the new leader did not show up: take the role back */
  ev_push(EV_HANDOVER_FAIL, s.leader, 0, 0, 0, 0);
  s.t0 += MS2T(LE_PERIOD_MS);
  s.round++;
  become_leader(s.term, s.pending_alive, 1, LE_REASON_RETRY);
  l_send_beacon();
}

static void
m_beacon_timeout(void *ptr)
{
  radio_off();
  s.missed++;
  s.t0 += MS2T(LE_PERIOD_MS);
  s.round++;
  s.n_slots = 0;
  s.elect = 0;
  s.my_slot = LE_NONE;
  le_prr_update(&s.beacon_prr, 0);
  ev_push(EV_MISS, 0, 0, 0, s.missed, 0);
  if(s.missed >= LE_FAILURE_TIMEOUT_K && !s.fail_detected) {
    s.fail_detected = 1;
    ev_push(EV_FAILDET, s.leader, 0, 0, s.missed,
            T2MS(clock_time() - s.last_beacon));
  }
  if(s.missed > LE_FAILURE_TIMEOUT_K + LE_SUCC_LEN * LE_BACKUP_STAGGER + 1) {
    /* nobody took over: start from scratch */
    print_events();
    enter_listen((uint32_t)LE_BOOT_WAIT_MS * (1 + s.cluster_rank));
    return;
  }
  m_round_end(NULL);
}

static void
m_wake_beacon(void *ptr)
{
  int8_t rank = succ_rank(s.me);
  radio_on();
  s.acked = 0;
  if(s.handover_wait) {
    at(s.t0 + MS2T(LE_PERIOD_MS + LE_HANDOVER_WAIT_MS), m_handover_fallback);
  } else if(rank >= 0 &&
            s.missed + 1 >= LE_FAILURE_TIMEOUT_K + rank * LE_BACKUP_STAGGER) {
    /* my turn to take over if the leader stays silent for the whole
     * beacon + ackmap window */
    at(s.t0 + MS2T(LE_PERIOD_MS + BACKUP_OFF(s.n_last, rank)), m_takeover);
  } else if(s.missed + 1 >= LE_FAILURE_TIMEOUT_K) {
    /* failure suspected: stay awake long enough to hear a takeover beacon
     * (or the leader's ackmap, which proves it is alive) */
    at(s.t0 + MS2T(LE_PERIOD_MS + SUSPECT_END(s.n_last)), m_beacon_timeout);
  } else {
    at(s.t0 + MS2T(LE_PERIOD_MS + LE_BEACON_WAIT_MS), m_beacon_timeout);
  }
}

static void m_start_leading(void *ptr);

static void
m_round_end(void *ptr)
{
  ctimer_stop(&seq_timer);
  radio_off();
  if(s.state != ST_MEMBER) {
    return;
  }
  sample();
  le_energy_use(LE_E_ROUND_MEMBER + (s.elect ? LE_E_ROUND_ELECT : 0));
  if(s.n_rd) {
    /* readings collected while I was still leader (stepped down) */
    uint8_t i;
    for(i = 0; i < s.n_rd; i++) {
      LOG_PREFIX();
      printf("RD r=%u src=%u seq=%u temp=%d light=%u\n", s.round, s.rd[i].src,
             s.rd[i].r.seq, s.rd[i].r.temp, s.rd[i].r.light);
    }
    s.n_rd = 0;
  }
  print_events();
  print_stat();
  if(LE_E_DEATH && le_energy() == 0) {
    s.state = ST_DEAD;
    log_role();
    return;
  }
  if(s.pending_leader) {
    /* lead from the next period; same schedule as the old leader */
    at(s.t0 + MS2T(LE_PERIOD_MS), m_start_leading);
    return;
  }
  at(s.t0 + MS2T(LE_PERIOD_MS) - MS2T(LE_GUARD_MS), m_wake_beacon);
}

/*===========================================================================*/
/* Leader: frame                                                             */
/*===========================================================================*/
static void l_result(void *ptr);

static void
l_result_sent(int status)
{
  s.elect_dur_ms = T2MS(clock_time() - s.elect_start);
  radio_off();
  if(s.result_winner != s.me) {
    /* hand over: become a member that waits for the new leader's beacon */
    s.state = ST_MEMBER;
    s.handover_wait = 1;
    s.pending_alive = s.rx_map | BIT(s.me);
    s.leader = s.result_winner;
    s.last_beacon = s.t0;
    s.missed = 0;
    s.my_slot = LE_NONE;
    log_role();
  }
  l_round_end(NULL);
}

static void
l_result(void *ptr)
{
  le_elect_in_t in;
  le_result_t *m = (le_result_t *)txbuf;
  uint8_t exclude = LE_NONE;

  if(s.reason == LE_REASON_ROTATION || s.reason == LE_REASON_LOW_ENERGY) {
    exclude = s.me;
  }
  s.energy[s.me] = le_energy();
  in.n = N_NODES;
  in.alive = s.rx_map | BIT(s.me);
  in.exclude = exclude;
  in.energy = s.energy;
  in.mx = le_link_matrix();
  le_election_run(&in, &s.eout);

  s.result_winner = s.eout.winner == LE_NONE ? s.me : s.eout.winner;
  if(s.reason == LE_REASON_ROTATION && s.eout.relaxed >= 3 && le_energy() >= LE_E_LOW) {
    /* only nodes below LE_E_LOW are left: rotating would hand the role to a
     * weaker node, so the healthy current leader keeps it */
    s.result_winner = s.me;
  }
  s.result_term = s.result_winner == s.me ? s.term : s.term + 1;
  if(s.result_winner == s.me && s.reason == LE_REASON_LOW_ENERGY) {
    /* nobody better: do not retry every round */
    s.low_energy_cooldown = LE_ROTATION_ROUNDS ? LE_ROTATION_ROUNDS : 24;
  }
  s.n_succ = le_election_succession(&in, &s.eout, s.result_winner, s.succ, LE_SUCC_LEN);

  hdr(&m->h, LE_MSG_RESULT);
  m->new_leader = s.result_winner;
  m->reason = s.reason;
  m->new_term = s.result_term;
  m->alive = in.alive;
  m->score = s.eout.score[s.result_winner];
  m->n_succ = s.n_succ;
  memcpy(m->succ, s.succ, sizeof(m->succ));
  s.elect_logged = 1;
  radio_on();
  send_msg(sizeof(*m), LE_NONE, 1, l_result_sent);
}

static void
l_retry_end(void *ptr)
{
  if(s.elect) {
    l_result(NULL);
  } else {
    l_round_end(NULL);
  }
}

static void
l_ackmap_sent(int status)
{
  s.tb = clock_time();
  if(s.expected & ~s.rx_map) {
    /* stay on for the retry slots */
    at(s.tb + MS2T(RETRY_LEN(s.n_slots)), l_retry_end);
  } else {
    radio_off();
    if(s.elect) {
      at(s.tb + MS2T(RETRY_LEN(s.n_slots)), l_result);
    } else {
      l_round_end(NULL);
    }
  }
}

static void
l_ackmap(void *ptr)
{
  le_ackmap_t *m = (le_ackmap_t *)txbuf;
  hdr(&m->h, LE_MSG_ACKMAP);
  m->received = s.rx_map;
  m->expected = s.expected;
  send_msg(sizeof(*m), LE_NONE, 1, l_ackmap_sent);
}

static void
l_beacon_sent(int status)
{
  s.t0 = clock_time();
  at(s.t0 + MS2T(ACK_OFF(s.n_slots)), l_ackmap);
}

/* build and send the beacon of a new round (radio on for the whole frame) */
static void
l_send_beacon(void)
{
  le_beacon_t *b = (le_beacon_t *)txbuf;
  uint8_t i;
  uint16_t members;

  radio_on();
  s.elect = s.elect_next;
  s.reason = s.elect ? s.elect_reason_next : LE_REASON_NONE;
  s.elect_next = 0;

  members = s.active | s.join_req;
  s.active = members;
  s.join_req = 0;
  s.n_slots = 0;
  s.expected = 0;
  for(i = 0; i < N_NODES; i++) {
    if(members & BIT(i)) {
      s.slot[s.n_slots++] = i;
      s.expected |= BIT(i);
    }
  }
  s.my_slot = LE_NONE;
  s.rx_map = 0;
  if(s.elect) {
    s.elect_start = clock_time();
    s.elect_ctrl_start = le_stats_ctrl_msgs();
  }

  hdr(&b->h, LE_MSG_BEACON);
  b->flags = s.elect ? LE_BF_ELECT : 0;
  b->reason = s.reason;
  b->energy = le_energy();
  b->n_slots = s.n_slots;
  b->n_succ = s.n_succ;
  memset(b->slot, LE_NONE, sizeof(b->slot));
  memcpy(b->slot, s.slot, s.n_slots);
  memcpy(b->succ, s.succ, sizeof(b->succ));
  send_msg(sizeof(*b), LE_NONE, 1, l_beacon_sent);
}

static void
l_next_round(void *ptr)
{
  s.round++;
  l_send_beacon();
}

/* a RESULT named this node: first beacon on the old leader's schedule */
static void
m_start_leading(void *ptr)
{
  if(s.state != ST_MEMBER || !s.pending_leader) {
    return;
  }
  s.round++;
  become_leader(s.pending_term, s.pending_alive, 0, LE_REASON_NONE);
  l_send_beacon();
}

/* refresh the succession list from what the leader knows now */
static void
l_update_succession(void)
{
  le_elect_in_t in;
  le_elect_out_t out;
  uint8_t i;
  uint16_t alive = BIT(s.me);
  for(i = 0; i < N_NODES; i++) {
    if((s.active & BIT(i)) && s.miss_cnt[i] == 0) {
      alive |= BIT(i);
    }
  }
  s.energy[s.me] = le_energy();
  in.n = N_NODES;
  in.alive = alive;
  in.exclude = s.me;
  in.energy = s.energy;
  in.mx = le_link_matrix();
  le_election_run(&in, &out);
  s.n_succ = le_election_succession(&in, &out, s.me, s.succ, LE_SUCC_LEN);
}

static uint8_t
auto_fail_now(void)
{
#if LE_FAIL_AT_ROUND > 0
  if(s.round == LE_FAIL_AT_ROUND) {
    return 1;
  }
#if LE_FAIL_EVERY_ROUNDS > 0
  if(s.round > LE_FAIL_AT_ROUND &&
     (s.round - LE_FAIL_AT_ROUND) % LE_FAIL_EVERY_ROUNDS == 0) {
    return 1;
  }
#endif
#endif
  return 0;
}

static void
l_round_end(void *ptr)
{
  uint8_t i, got = 0;
  uint8_t was_leader = s.state == ST_LEADER || s.handover_wait;

  ctimer_stop(&seq_timer);
  radio_off();
  if(!was_leader) {
    return;
  }

  /* bookkeeping of members */
  for(i = 0; i < s.n_slots; i++) {
    uint8_t m = s.slot[i];
    uint8_t ok = (s.rx_map & BIT(m)) != 0;
    le_prr_update(&s.prr[m], ok);
    if(ok) {
      s.miss_cnt[m] = 0;
      got++;
    } else if(++s.miss_cnt[m] >= LE_MAX_MISSED_REPORTS && s.state == ST_LEADER) {
      s.active &= ~BIT(m);
      ev_push(EV_PRUNE, m, 0, 0, 0, 0);
    }
  }

  if(s.state == ST_LEADER) {
    s.rounds_as_leader++;
    if(s.low_energy_cooldown) {
      s.low_energy_cooldown--;
    }
  }
  sample();
  le_energy_use(LE_E_ROUND_LEADER);

  /* log the round */
  LOG_PREFIX();
  printf("RND r=%u term=%u type=%s reason=%s slots=%u rx=%u ackmap=0x%x\n",
         s.round, s.term, s.elect ? "ELECT" : "DATA", reason_name[s.reason],
         s.n_slots, got, s.rx_map);
  for(i = 0; i < s.n_rd; i++) {
    LOG_PREFIX();
    printf("RD r=%u src=%u seq=%u temp=%d light=%u\n", s.round, s.rd[i].src,
           s.rd[i].r.seq, s.rd[i].r.temp, s.rd[i].r.light);
  }
  s.n_rd = 0;
  print_election();
  print_events();
  print_stat();

  if(LE_E_DEATH && le_energy() == 0) {
    s.state = ST_DEAD;
    log_role();
    return;
  }

  if(s.state == ST_LEADER && s.active == 0 && s.n_slots > 0 && s.cluster_size > 1) {
    /* every member was pruned: most likely they follow another leader whose
     * frames do not overlap with ours. Listen continuously to find it. */
    LOG_PREFIX();
    printf("ORPHAN r=%u\n", s.round);
    enter_listen((uint32_t)LE_BOOT_WAIT_MS * (1 + s.cluster_size));
    return;
  }

  if(s.state == ST_LEADER) {
    if(!s.elect) {
      l_update_succession();
    }
    /* triggers for the next round */
    if(LE_ROTATION_ROUNDS > 0 && s.rounds_as_leader >= LE_ROTATION_ROUNDS) {
      s.elect_next = 1;
      s.elect_reason_next = LE_REASON_ROTATION;
      s.rounds_as_leader = 0;
    } else if(le_energy() < LE_E_LOW && s.low_energy_cooldown == 0 && !s.elect) {
      /* relinquish only if somebody can do better; when everybody is low,
       * periodic rotation keeps spreading the load */
      for(i = 0; i < N_NODES; i++) {
        if((s.active & BIT(i)) && s.miss_cnt[i] == 0 && s.energy[i] >= LE_E_LOW) {
          s.elect_next = 1;
          s.elect_reason_next = LE_REASON_LOW_ENERGY;
          break;
        }
      }
    }
    if(auto_fail_now()) {
      fail_start(LE_FAIL_DURATION_S);
      return;
    }
    at(s.t0 + MS2T(LE_PERIOD_MS), l_next_round);
  } else {
    /* handed over: listen for the new leader like a member */
    at(s.t0 + MS2T(LE_PERIOD_MS) - MS2T(LE_GUARD_MS), m_wake_beacon);
  }
}

/*===========================================================================*/
/* Receiving                                                                 */
/*===========================================================================*/
static uint8_t
beacon_is_better(const le_hdr_t *h, uint16_t term, uint8_t leader)
{
  if(h->term != term) {
    return h->term > term;
  }
  return leader == LE_NONE || h->src <= leader;
}

static void
accept_beacon(const le_beacon_t *b)
{
  clock_time_t now = clock_time();
  uint8_t i;
  uint8_t prev = s.leader;

  if(s.state == ST_LISTEN) {
    le_energy_use((uint16_t)(T2MS(now - s.listen_since) / LE_PERIOD_MS) * LE_E_ROUND_LEADER);
  }
  if(prev != b->h.src || s.term != b->h.term || s.state == ST_LISTEN) {
    if(s.handover_wait && b->h.src == s.leader) {
      ev_push(EV_HANDOVER_OK, b->h.src, 0, b->h.term, 0, 0);
    }
    ev_push(EV_NEWLDR, s.handover_wait ? s.me : prev, b->h.src, b->h.term,
            s.missed, s.state == ST_MEMBER ? T2MS(now - s.last_beacon) : 0);
  }
  ctimer_stop(&seq_timer);
  if(s.state != ST_MEMBER) {
    s.state = ST_MEMBER;
    s.leader = b->h.src;
    s.term = b->h.term;
    s.round = b->h.round;
    log_role();
  }
  s.handover_wait = 0;
  if(s.pending_leader && b->h.src != s.me) {
    s.pending_leader = 0;
  }
  s.leader = b->h.src;
  s.term = b->h.term;
  s.round = b->h.round;
  s.t0 = now;
  s.last_beacon = now;
  s.missed = 0;
  s.fail_detected = 0;
  s.acked = 0;
  le_prr_update(&s.beacon_prr, 1);

  s.elect = (b->flags & LE_BF_ELECT) != 0;
  s.reason = b->reason;
  s.n_slots = b->n_slots > LE_MAX_NODES ? LE_MAX_NODES : b->n_slots;
  s.n_last = s.n_slots;
  memcpy(s.slot, b->slot, s.n_slots);
  s.expected = 0;
  for(i = 0; i < s.n_slots; i++) {
    if(s.slot[i] < N_NODES) {
      s.expected |= BIT(s.slot[i]);
    }
  }
  s.n_succ = b->n_succ > LE_SUCC_LEN ? LE_SUCC_LEN : b->n_succ;
  memcpy(s.succ, b->succ, s.n_succ);
  {
    int8_t sl = slot_of(s.me);
    s.my_slot = sl < 0 ? LE_NONE : (uint8_t)sl;
  }
  s.energy[b->h.src] = b->energy;
  member_plan_round();
}

static void
rx_beacon(const le_hdr_t *h, const void *data, uint16_t len)
{
  le_beacon_t b;
  memset(&b, 0, sizeof(b));
  memcpy(&b, data, len < sizeof(b) ? len : sizeof(b));

  switch(s.state) {
  case ST_LISTEN:
    accept_beacon(&b);
    break;
  case ST_MEMBER:
    if(s.pending_leader && h->term < s.pending_term) {
      break;  /* I am about to lead with a newer term */
    }
    if(s.handover_wait ? h->term > s.term
                       : beacon_is_better(h, s.term, s.leader)) {
      accept_beacon(&b);
    }
    break;
  case ST_LEADER:
    if(beacon_is_better(h, s.term, s.me)) {
      ev_push(EV_STEPDOWN, h->src, 0, h->term, 0, 0);
      accept_beacon(&b);
    }
    break;
  default:
    break;
  }
}

static void
store_readings(uint8_t src, uint8_t n, const le_reading_t *r)
{
  uint8_t i;
  for(i = 0; i < n && i < LE_BACKLOG && s.n_rd < RD_MAX; i++) {
    s.rd[s.n_rd].src = src;
    memcpy(&s.rd[s.n_rd].r, &r[i], sizeof(le_reading_t));
    s.n_rd++;
  }
}

static void
input_cb(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
  le_hdr_t h;
  int16_t rssi;

  if(len < sizeof(h) || s.state == ST_FAILED || s.state == ST_DEAD) {
    return;
  }
  memcpy(&h, data, sizeof(h));
  if(h.magic != LE_MAGIC || h.src >= N_NODES || h.src == s.me ||
     h.cluster != s.cluster || node_cluster[h.src] != s.cluster ||
     h.type == 0 || h.type >= LE_MSG_NUM) {
    return;
  }
  /* simulated loss only for broadcast frames: a unicast frame has already
   * been acknowledged by the radio when it gets here */
  if(s.loss_pct && linkaddr_cmp(dest, &linkaddr_null) &&
     (random_rand() % 100) < s.loss_pct) {
    le_stats.rx_dropped++;
    return;
  }
  /* RSSI belongs to this packet only: read it now */
  rssi = (int16_t)packetbuf_attr(PACKETBUF_ATTR_RSSI);
  linkaddr_copy(&node_addr[h.src], src);
  le_link_rx(h.src, rssi);
  le_stats_rx(h.type);
  le_energy_use(LE_E_RX);

  switch(h.type) {
  case LE_MSG_BEACON:
    rx_beacon(&h, data, len);
    break;

  case LE_MSG_DATA:
    if(s.state == ST_LEADER && h.term == s.term) {
      le_data_t m;
      memset(&m, 0, sizeof(m));
      memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
      if(h.round == s.round) {
        s.rx_map |= BIT(h.src);
      }
      s.energy[h.src] = m.energy;
      store_readings(h.src, m.n_readings, m.r);
    }
    break;

  case LE_MSG_REPORT: {
    le_report_t m;
    memset(&m, 0, sizeof(m));
    memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
    /* everybody keeps the link table and energy (a future leader needs them) */
    le_link_set_row(h.src, m.rssi);
    s.energy[h.src] = m.energy;
    if(s.state == ST_LEADER && h.term == s.term) {
      if(h.round == s.round) {
        s.rx_map |= BIT(h.src);
      }
      store_readings(h.src, m.n_readings, m.r);
    }
    break;
  }

  case LE_MSG_ACKMAP:
    if(s.state == ST_MEMBER && !s.handover_wait && h.src == s.leader &&
       h.term == s.term && h.round == (uint16_t)(s.round + 1)) {
      /* we missed this round's beacon, but the leader is alive: resync on
       * its ackmap and skip this round (the reading stays in the backlog) */
      le_ackmap_t m;
      uint8_t i, n = 0;
      memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
      for(i = 0; i < N_NODES; i++) {
        n += (m.expected & BIT(i)) != 0;
      }
      ctimer_stop(&seq_timer);
      le_prr_update(&s.beacon_prr, 0);
      ev_push(EV_ALIVE, h.src, 0, h.term, s.missed + 1, 0);
      s.round = h.round;
      s.t0 = clock_time() - MS2T(ACK_OFF(n));
      s.n_slots = 0;
      s.elect = 0;
      s.my_slot = LE_NONE;
      s.missed = 0;
      s.fail_detected = 0;
      m_round_end(NULL);
    } else if(s.state == ST_MEMBER && h.src == s.leader && h.term == s.term &&
       h.round == s.round) {
      le_ackmap_t m;
      memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
      ctimer_stop(&seq_timer);
      s.tb = clock_time();
      if(m.received & BIT(s.me)) {
        confirm_backlog();
      }
      radio_off();
      if(!s.acked && s.my_slot != LE_NONE && (m.expected & BIT(s.me))) {
        at(s.tb + MS2T(SLOT_OFF(s.my_slot)), m_retry);
      } else {
        m_after_ack();
      }
    }
    break;

  case LE_MSG_RESULT:
    if(s.state == ST_MEMBER && h.src == s.leader && h.term == s.term) {
      le_result_t m;
      memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
      s.n_succ = m.n_succ > LE_SUCC_LEN ? LE_SUCC_LEN : m.n_succ;
      memcpy(s.succ, m.succ, s.n_succ);
      ev_push(EV_RESULT_RX, m.new_leader, 0, m.new_term, 0, 0);
      if(m.new_leader == s.me && m.new_term > s.term) {
        s.pending_leader = 1;
        s.pending_term = m.new_term;
        s.pending_alive = m.alive;
      }
      m_round_end(NULL);
    }
    break;

  case LE_MSG_JOIN:
    if(s.state == ST_LEADER) {
      le_join_t m;
      memcpy(&m, data, len < sizeof(m) ? len : sizeof(m));
      s.join_req |= BIT(h.src);
      s.miss_cnt[h.src] = 0;
      s.energy[h.src] = m.energy;
      ev_push(EV_JOIN, h.src, 0, 0, 0, 0);
    }
    break;
  }
}

/*===========================================================================*/
/* Listening (boot / recovery) and becoming leader at a scheduled time       */
/*===========================================================================*/
static void
listen_timeout(void *ptr)
{
  /* nobody is leading this cluster: start it */
  le_energy_use((uint16_t)(T2MS(clock_time() - s.listen_since) / LE_PERIOD_MS) * LE_E_ROUND_LEADER);
  s.round++;
  become_leader(s.term + 1, s.cluster_mask, 1, LE_REASON_BOOTSTRAP);
  l_send_beacon();
}

static void
enter_listen(uint32_t wait_ms)
{
  ctimer_stop(&seq_timer);
  s.state = ST_LISTEN;
  s.leader = LE_NONE;
  s.missed = 0;
  s.fail_detected = 0;
  s.handover_wait = 0;
  s.pending_leader = 0;
  s.n_succ = 0;
  s.listen_since = clock_time();
  log_role();
  radio_on();
  ctimer_set(&seq_timer, MS2T(wait_ms), listen_timeout, NULL);
}

/*===========================================================================*/
/* Simulated failure                                                         */
/*===========================================================================*/
static void
fail_end(void *ptr)
{
  LOG_PREFIX();
  printf("RECOVER\n");
  s.n_backlog = 0;
  enter_listen((uint32_t)LE_BOOT_WAIT_MS * (1 + s.cluster_size));
}

static void
fail_start(uint16_t seconds)
{
  if(s.state == ST_FAILED || s.state == ST_DEAD) {
    return;
  }
  LOG_PREFIX();
  printf("FAILSIM role=%s dur_s=%u r=%u\n", state_name[s.state], seconds, s.round);
  ctimer_stop(&seq_timer);
  radio_off();
  s.state = ST_FAILED;
  set_leds();
  if(seconds > 0) {
    ctimer_set(&fail_timer, (clock_time_t)seconds * CLOCK_SECOND, fail_end, NULL);
  }
}

/*===========================================================================*/
/* Serial commands                                                           */
/*===========================================================================*/
static void
command(const char *line)
{
  if(strncmp(line, "fail", 4) == 0) {
    int sec = line[4] ? atoi(line + 5) : LE_FAIL_DURATION_S;
    fail_start((uint16_t)sec);
  } else if(strncmp(line, "drain ", 6) == 0) {
    le_energy_use((uint16_t)atoi(line + 6));
    LOG_PREFIX();
    printf("DRAIN e=%u\n", le_energy());
  } else if(strncmp(line, "elect", 5) == 0) {
    if(s.state == ST_LEADER) {
      s.elect_next = 1;
      s.elect_reason_next = LE_REASON_MANUAL;
    }
  } else if(strncmp(line, "loss ", 5) == 0) {
    s.loss_pct = (uint8_t)atoi(line + 5);
    LOG_PREFIX();
    printf("LOSS pct=%u\n", s.loss_pct);
  } else if(strncmp(line, "status", 6) == 0) {
    LOG_PREFIX();
    printf("STATUS role=%s term=%u leader=%d round=%u e=%u missed=%u\n",
           state_name[s.state], s.term, s.leader == LE_NONE ? -1 : s.leader,
           s.round, le_energy(), s.missed);
  }
}

/*===========================================================================*/
/* Process                                                                   */
/*===========================================================================*/
PROCESS_THREAD(le_process, ev, data)
{
  uint8_t i;

  PROCESS_BEGIN();

  memset(&s, 0, sizeof(s));
  s.me = LE_NONE;
#ifdef LE_FORCE_INDEX
  s.me = LE_FORCE_INDEX;
#else
  /* match the serial-number ID (real motes) or node_id (Cooja) */
  for(i = 0; i < N_NODES; i++) {
    if(node_ids[i] == serial_id() || node_ids[i] == node_id) {
      s.me = i;
    }
  }
#endif
  if(s.me == LE_NONE || N_NODES > LE_MAX_NODES) {
    printf("LE ? 0 ERROR id=%u is not in LE_NODE_IDS (le-conf.h)\n", serial_id());
    radio_off();
    leds_on(LEDS_RED);
    PROCESS_EXIT();
  }
  s.cluster = node_cluster[s.me];
  for(i = 0; i < N_NODES; i++) {
    if(node_cluster[i] == s.cluster) {
      s.cluster_mask |= BIT(i);
      s.cluster_size++;
      if(i < s.me) {
        s.cluster_rank++;
      }
    }
  }
  s.leader = LE_NONE;
  s.my_slot = LE_NONE;
  s.loss_pct = LE_RX_LOSS_PCT;
  le_prr_init(&s.beacon_prr);
  le_link_init(s.me, N_NODES);
  le_energy_init(s.me);
  for(i = 0; i < N_NODES; i++) {
    s.energy[i] = LE_E_INIT;
  }
  le_sensor_init();

  NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL,
                           LE_RF_CHANNEL - (LE_CLUSTER_OWN_CHANNEL ? s.cluster : 0));
  NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, LE_TX_POWER_DBM);
  nullnet_set_input_callback(input_cb);
#ifdef CONTIKI_TARGET_SKY
  SENSORS_ACTIVATE(button_sensor);
#endif

  LOG_PREFIX();
  printf("BOOT id=%u node_id=%u cluster=%u rank=%u n=%u e0=%u addr=", serial_id(), node_id, s.cluster,
         s.cluster_rank, N_NODES, le_energy());
  for(i = 0; i < LINKADDR_SIZE; i++) {
    printf("%02x", linkaddr_node_addr.u8[i]);
  }
  printf(" ram_static=%u state_bytes=%u k=%u period_ms=%u rot=%u\n", le_static_ram(),
         (unsigned)(sizeof(s) + sizeof(node_addr) + sizeof(txbuf)),
         LE_FAILURE_TIMEOUT_K, LE_PERIOD_MS, LE_ROTATION_ROUNDS);

  enter_listen((uint32_t)LE_BOOT_WAIT_MS * (1 + s.cluster_rank));

  while(1) {
    PROCESS_WAIT_EVENT();
    if(ev == serial_line_event_message && data != NULL) {
      command((const char *)data);
#ifdef CONTIKI_TARGET_SKY
    } else if(ev == sensors_event && data == &button_sensor) {
      fail_start(LE_FAIL_DURATION_S);
#endif
    }
  }

  PROCESS_END();
}
