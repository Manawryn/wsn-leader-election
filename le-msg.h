/*
 * le-msg.h - over-the-air message formats.
 *
 * All messages start with le_hdr_t. Structures are packed; received bytes
 * are always memcpy'd into a local struct before use (the MSP430 cannot do
 * unaligned 16-bit accesses).
 */
#ifndef LE_MSG_H_
#define LE_MSG_H_

#include <stdint.h>
#include "le-conf.h"

#define LE_MAGIC        0xE7   /* filters out other groups' NullNet traffic */
#define LE_NONE         0xFF   /* "no node" */
#define LE_RSSI_NONE    (-128) /* unknown RSSI */

typedef enum {
  LE_MSG_BEACON = 1, /* leader -> all: sync + slot table (+ election start)  */
  LE_MSG_DATA,       /* member -> leader (unicast): readings                 */
  LE_MSG_REPORT,     /* member -> all (broadcast, election rounds): readings */
                     /*   + energy + link table                              */
  LE_MSG_ACKMAP,     /* leader -> all: bitmap of received DATA/REPORT        */
  LE_MSG_RESULT,     /* leader -> all: election result                       */
  LE_MSG_JOIN,       /* unscheduled node -> leader, in the contention slot   */
  LE_MSG_NUM
} le_msg_type_t;

/* Why an election was started */
typedef enum {
  LE_REASON_NONE = 0,
  LE_REASON_BOOTSTRAP,   /* first election after start-up               */
  LE_REASON_ROTATION,    /* periodic rotation                            */
  LE_REASON_LOW_ENERGY,  /* leader energy below LE_E_LOW                 */
  LE_REASON_FAILOVER,    /* backup took over after a leader failure      */
  LE_REASON_MANUAL,      /* serial command "elect"                       */
  LE_REASON_RETRY,       /* previous handover did not complete           */
  LE_REASON_NUM
} le_reason_t;

typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t type;       /* le_msg_type_t */
  uint8_t cluster;
  uint8_t src;        /* sender's node index */
  uint16_t term;      /* leadership term, +1 on every change of leader */
  uint16_t round;     /* round number (one round = one beacon period) */
} le_hdr_t;

#define LE_BF_ELECT     0x01   /* this round is an election round */

typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint8_t flags;
  uint8_t copy;                 /* 0..LE_BEACON_COPIES-1; copy c is sent
                                 * c * LE_BEACON_GAP_MS after copy 0 */
  uint8_t reason;               /* le_reason_t when LE_BF_ELECT */
  uint16_t energy;              /* leader's remaining energy */
  uint8_t n_slots;
  uint8_t n_succ;
  uint8_t slot[LE_MAX_NODES];   /* slot i belongs to node slot[i] */
  uint8_t succ[LE_SUCC_LEN];    /* backups, best first */
} le_beacon_t;

typedef struct __attribute__((packed)) {
  uint16_t seq;                 /* sample number at the member */
  int16_t temp;                 /* centi-degC (INT16_MIN = n/a) */
  uint16_t light;               /* raw ADC */
} le_reading_t;

/* DATA: sent with only n_readings entries of r[] */
typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint16_t energy;
  int8_t beacon_rssi;           /* how well this member hears the leader */
  uint8_t beacon_prr;           /* WMEWMA PRR of beacons, percent */
  uint8_t n_readings;
  le_reading_t r[LE_BACKLOG];
} le_data_t;

/* REPORT: DATA + the sender's RSSI table (overheard by everybody) */
typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint16_t energy;
  int8_t beacon_rssi;
  uint8_t beacon_prr;
  int8_t rssi[LE_MAX_NODES];    /* RSSI at which the sender hears node i */
  uint8_t n_readings;
  le_reading_t r[LE_BACKLOG];
} le_report_t;

/* ACKMAP: cumulative ACK of the round. expected & ~received works as a NACK
 * list: those nodes retry in the retry slots. It also carries enough of the
 * beacon (slot owners, election flag) for a member that missed the beacon to
 * use its retry slot. Slots are given in increasing node index, so a node's
 * slot is the number of expected bits below its own. */
typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint16_t received;            /* bit i: packet of node i received */
  uint16_t expected;            /* bit i: node i had a slot */
  uint8_t flags;                /* LE_BF_ELECT, as in the beacon */
} le_ackmap_t;

typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint8_t new_leader;
  uint8_t reason;
  uint16_t new_term;
  uint16_t alive;               /* nodes that took part (new slot table) */
  uint16_t score;               /* winner's score, for the logs */
  uint8_t n_succ;
  uint8_t succ[LE_SUCC_LEN];
} le_result_t;

typedef struct __attribute__((packed)) {
  le_hdr_t h;
  uint16_t energy;
} le_join_t;

#define LE_DATA_LEN(n)   (sizeof(le_data_t) - (LE_BACKLOG - (n)) * sizeof(le_reading_t))
#define LE_REPORT_LEN(n) (sizeof(le_report_t) - (LE_BACKLOG - (n)) * sizeof(le_reading_t))

#endif /* LE_MSG_H_ */
