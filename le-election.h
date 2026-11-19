/*
 * le-election.h - LEACH-C style leader selection, run on the current leader.
 *
 * Criterion (justification in README):
 *   score = (W_E * energy_norm + W_L * link_norm) / (W_E + W_L)
 *     energy_norm = remaining energy / LE_E_INIT          (0..1000)
 *     link_norm   = average RSSI between the candidate and the other
 *                   participants, mapped LE_RSSI_FLOOR..CEIL -> 0..1000
 * Eligibility (relaxed step by step if nobody qualifies):
 *   1. energy >= average energy of the participants (LEACH-C rule)
 *   2. worst known link >= LE_RSSI_MIN_LINK (can reach everybody)
 *   3. energy >= LE_E_LOW
 *   and the 'exclude' node (the current leader on a rotation or low-energy
 *   election) never wins.
 * Ties: lower node index wins.
 *
 * Pure function of its inputs, so it can be unit-tested on the PC.
 */
#ifndef LE_ELECTION_H_
#define LE_ELECTION_H_

#include <stdint.h>
#include "le-conf.h"
#include "le-link.h"

typedef struct {
  uint8_t n;                    /* number of nodes */
  uint16_t alive;               /* bitmap of participants */
  uint8_t exclude;              /* may not win (LE_NONE = nobody) */
  const uint16_t *energy;       /* energy[i] */
  const le_link_row_t *mx;      /* mx[i][j]: RSSI at i of j */
} le_elect_in_t;

typedef struct {
  uint8_t winner;               /* LE_NONE if no participant */
  uint16_t score[LE_MAX_NODES];
  int8_t link_avg[LE_MAX_NODES];
  int8_t link_min[LE_MAX_NODES];
  uint16_t eligible;            /* passed the (possibly relaxed) rules */
  uint8_t relaxed;              /* 0 = strict, 1..3 = rules dropped */
  uint8_t rank[LE_MAX_NODES];   /* participants except 'exclude', best first */
  uint8_t n_rank;
} le_elect_out_t;

void le_election_run(const le_elect_in_t *in, le_elect_out_t *out);

/* Fill succ[] with up to max backups from the ranking, skipping 'leader'
 * and nodes without energy. Returns the number written. */
uint8_t le_election_succession(const le_elect_in_t *in, const le_elect_out_t *out,
                               uint8_t leader, uint8_t *succ, uint8_t max);

#endif /* LE_ELECTION_H_ */
