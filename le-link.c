#include <string.h>
#include "le-link.h"
#include "le-msg.h"

static uint8_t my_idx;
static uint8_t n;
static le_link_row_t mx[LE_MAX_NODES];
/* our own row in fixed point (dBm * 16) for the EWMA */
static int16_t my_fp[LE_MAX_NODES];

/*---------------------------------------------------------------------------*/
void
le_prr_init(le_prr_t *p)
{
  memset(p, 0, sizeof(*p));
  p->prr = 100;
}
/*---------------------------------------------------------------------------*/
void
le_prr_update(le_prr_t *p, uint8_t got)
{
  p->expected++;
  if(got) {
    p->received++;
  }
  if(p->expected >= LE_PRR_WINDOW_T) {
    uint8_t window = (uint8_t)((uint16_t)p->received * 100 / p->expected);
    if(!p->valid) {
      p->prr = window;
      p->valid = 1;
    } else {
      p->prr = (uint8_t)(((uint16_t)LE_PRR_ALPHA_PCT * p->prr +
                          (uint16_t)(100 - LE_PRR_ALPHA_PCT) * window) / 100);
    }
    p->expected = 0;
    p->received = 0;
  }
}
/*---------------------------------------------------------------------------*/
uint8_t
le_prr_get(const le_prr_t *p)
{
  return p->prr;
}
/*---------------------------------------------------------------------------*/
void
le_link_init(uint8_t me, uint8_t n_nodes)
{
  my_idx = me;
  n = n_nodes;
  memset(mx, LE_RSSI_NONE, sizeof(mx));
  memset(my_fp, 0, sizeof(my_fp));
}
/*---------------------------------------------------------------------------*/
void
le_link_rx(uint8_t from, int16_t rssi)
{
  if(from >= n || from == my_idx) {
    return;
  }
  if(rssi < -127) {
    rssi = -127;
  } else if(rssi > 127) {
    rssi = 127;
  }
  if(mx[my_idx][from] == LE_RSSI_NONE) {
    my_fp[from] = rssi * 16;
  } else {
    my_fp[from] += (rssi * 16 - my_fp[from]) >> LE_RSSI_EWMA_SHIFT;
  }
  /* round to nearest dBm */
  mx[my_idx][from] = (int8_t)((my_fp[from] + (my_fp[from] >= 0 ? 8 : -8)) / 16);
  if(mx[my_idx][from] == LE_RSSI_NONE) {
    mx[my_idx][from] = LE_RSSI_NONE + 1;
  }
}
/*---------------------------------------------------------------------------*/
int8_t
le_link_rssi(uint8_t from)
{
  return from < n ? mx[my_idx][from] : LE_RSSI_NONE;
}
/*---------------------------------------------------------------------------*/
void
le_link_my_row(int8_t *row)
{
  memcpy(row, mx[my_idx], LE_MAX_NODES);
}
/*---------------------------------------------------------------------------*/
void
le_link_set_row(uint8_t i, const int8_t *row)
{
  if(i < n && i != my_idx) {
    memcpy(mx[i], row, LE_MAX_NODES);
  }
}
/*---------------------------------------------------------------------------*/
int8_t
le_link_get(uint8_t i, uint8_t j)
{
  return (i < n && j < n) ? mx[i][j] : LE_RSSI_NONE;
}
/*---------------------------------------------------------------------------*/
uint8_t
le_link_neighbors(void)
{
  uint8_t i, c = 0;
  for(i = 0; i < n; i++) {
    if(i != my_idx && mx[my_idx][i] != LE_RSSI_NONE) {
      c++;
    }
  }
  return c;
}
/*---------------------------------------------------------------------------*/
const le_link_row_t *
le_link_matrix(void)
{
  return (const le_link_row_t *)mx;
}
