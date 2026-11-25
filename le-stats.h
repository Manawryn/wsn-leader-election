/*
 * le-stats.h - counters for the evaluation (messages and bytes per type,
 * MAC attempts, radio-on time from Energest, stack usage).
 */
#ifndef LE_STATS_H_
#define LE_STATS_H_

#include <stdint.h>
#include "le-msg.h"

typedef struct {
  uint16_t tx[LE_MSG_NUM];      /* messages sent, per type */
  uint32_t tx_bytes[LE_MSG_NUM];/* application payload bytes sent */
  uint16_t rx[LE_MSG_NUM];      /* messages received (accepted) */
  uint32_t mac_tx;              /* MAC transmission attempts (incl. retries) */
  uint16_t mac_coll;            /* frames dropped: channel busy (CCA) */
  uint16_t rx_dropped;          /* dropped by simulated packet loss */
} le_stats_t;

extern le_stats_t le_stats;

void le_stats_tx(uint8_t type, uint16_t len);
void le_stats_rx(uint8_t type);
/* control messages sent so far (everything except DATA) */
uint32_t le_stats_ctrl_msgs(void);
/* ms since boot */
uint32_t le_now_ms(void);
/* radio listen / transmit time from Energest, ms */
uint32_t le_radio_listen_ms(void);
uint32_t le_radio_tx_ms(void);
/* peak stack usage in bytes (-1 if unknown) */
long le_stack_peak(void);
/* RAM used by .data + .bss (sky only, 0 otherwise) */
unsigned le_static_ram(void);

#endif /* LE_STATS_H_ */
