/*
 * le-link.h - link estimation without a link-estimation layer (NullNet).
 *
 * - RSSI per neighbour: EWMA of the RSSI of every packet we receive
 *   (packetbuf_attr(PACKETBUF_ATTR_RSSI), already in dBm on the CC2420).
 * - Link matrix: mx[i][j] = RSSI at which node i hears node j. Our own row
 *   comes from our measurements; the other rows come from the RSSI tables
 *   that members piggyback in REPORT messages (which everybody overhears).
 * - WMEWMA packet reception ratio for links with a known expected packet
 *   rate (beacons at members, DATA at the leader).
 */
#ifndef LE_LINK_H_
#define LE_LINK_H_

#include <stdint.h>
#include "le-conf.h"

typedef struct {
  uint8_t expected;   /* packets expected in the current window */
  uint8_t received;   /* packets received in the current window */
  uint8_t prr;        /* estimate, percent */
  uint8_t valid;      /* at least one window completed */
} le_prr_t;

void le_prr_init(le_prr_t *p);
/* One expected packet: got = 1 if it arrived. */
void le_prr_update(le_prr_t *p, uint8_t got);
/* Current estimate in percent (100 before the first window completes). */
uint8_t le_prr_get(const le_prr_t *p);

void le_link_init(uint8_t me, uint8_t n_nodes);
/* Record the RSSI of a packet received from node 'from'. */
void le_link_rx(uint8_t from, int16_t rssi);
/* Our smoothed RSSI of node 'from' (LE_RSSI_NONE if never heard). */
int8_t le_link_rssi(uint8_t from);
/* Copy our RSSI table (one entry per node) into row. */
void le_link_my_row(int8_t *row);
/* Store the RSSI table reported by node i. */
void le_link_set_row(uint8_t i, const int8_t *row);
int8_t le_link_get(uint8_t i, uint8_t j);
/* Number of nodes we have heard (= neighbours). */
uint8_t le_link_neighbors(void);
/* The whole matrix (read-only), for the election. */
typedef int8_t le_link_row_t[LE_MAX_NODES];
const le_link_row_t *le_link_matrix(void);

#endif /* LE_LINK_H_ */
