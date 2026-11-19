#include <string.h>
#include "le-election.h"
#include "le-msg.h"

#define BIT(i) ((uint16_t)1 << (i))

/*---------------------------------------------------------------------------*/
/* Combined RSSI of the link i<->j: mean of both directions if both known. */
static int16_t
pair_rssi(const le_link_row_t *mx, uint8_t i, uint8_t j)
{
  int8_t a = mx[i][j];
  int8_t b = mx[j][i];
  if(a != LE_RSSI_NONE && b != LE_RSSI_NONE) {
    return ((int16_t)a + b) / 2;
  }
  if(a != LE_RSSI_NONE) {
    return a;
  }
  return b; /* may be LE_RSSI_NONE */
}
/*---------------------------------------------------------------------------*/
static uint16_t
norm(int32_t v, int32_t lo, int32_t hi)
{
  if(v <= lo) {
    return 0;
  }
  if(v >= hi) {
    return 1000;
  }
  return (uint16_t)((v - lo) * 1000 / (hi - lo));
}
/*---------------------------------------------------------------------------*/
void
le_election_run(const le_elect_in_t *in, le_elect_out_t *out)
{
  uint8_t c, x, k, level;
  uint32_t esum = 0;
  uint8_t cnt = 0;
  uint16_t eavg;

  memset(out, 0, sizeof(*out));
  out->winner = LE_NONE;

  for(c = 0; c < in->n; c++) {
    if(in->alive & BIT(c)) {
      esum += in->energy[c];
      cnt++;
    }
  }
  if(cnt == 0) {
    return;
  }
  eavg = (uint16_t)(esum / cnt);

  /* Link quality and score of every participant */
  for(c = 0; c < in->n; c++) {
    int32_t sum = 0;
    uint8_t known = 0;
    int16_t mn = 127;
    uint16_t e_norm, l_norm;

    out->link_avg[c] = LE_RSSI_NONE;
    out->link_min[c] = LE_RSSI_NONE;
    if(!(in->alive & BIT(c))) {
      continue;
    }
    for(x = 0; x < in->n; x++) {
      int16_t r;
      if(x == c || !(in->alive & BIT(x))) {
        continue;
      }
      r = pair_rssi(in->mx, c, x);
      if(r == LE_RSSI_NONE) {
        continue;
      }
      sum += r;
      known++;
      if(r < mn) {
        mn = r;
      }
    }
    if(known) {
      out->link_avg[c] = (int8_t)(sum / known);
      out->link_min[c] = (int8_t)mn;
      l_norm = norm(out->link_avg[c], LE_RSSI_FLOOR, LE_RSSI_CEIL);
    } else {
      l_norm = 500; /* no information: neutral */
    }
    e_norm = norm(in->energy[c], 0, LE_E_INIT);
    out->score[c] = (uint16_t)(((uint32_t)LE_W_ENERGY * e_norm +
                                (uint32_t)LE_W_LINK * l_norm) /
                               (LE_W_ENERGY + LE_W_LINK));
  }

  /* Eligibility, relaxing the rules until somebody qualifies:
   * level 0: avg-energy + min-link + E_LOW, 1: min-link + E_LOW,
   * 2: E_LOW, 3: any energy > 0 */
  for(level = 0; level < 4; level++) {
    out->eligible = 0;
    for(c = 0; c < in->n; c++) {
      if(!(in->alive & BIT(c)) || c == in->exclude || in->energy[c] == 0) {
        continue;
      }
      if(level < 1 && LE_LEACHC_ABOVE_AVG && in->energy[c] < eavg) {
        continue;
      }
      if(level < 2 && out->link_min[c] != LE_RSSI_NONE &&
         out->link_min[c] < LE_RSSI_MIN_LINK) {
        continue;
      }
      if(level < 3 && in->energy[c] < LE_E_LOW) {
        continue;
      }
      out->eligible |= BIT(c);
    }
    if(out->eligible) {
      break;
    }
  }
  out->relaxed = level;

  /* Ranking: eligible first, then by score, then by lower index */
  for(c = 0; c < in->n; c++) {
    if((in->alive & BIT(c)) && c != in->exclude) {
      out->rank[out->n_rank++] = c;
    }
  }
  for(k = 1; k < out->n_rank; k++) {
    uint8_t v = out->rank[k];
    int8_t j = (int8_t)k - 1;
    while(j >= 0) {
      uint8_t u = out->rank[j];
      uint8_t ev = (out->eligible & BIT(v)) != 0, eu = (out->eligible & BIT(u)) != 0;
      uint8_t better = (ev > eu) ||
        (ev == eu && (out->score[v] > out->score[u] ||
                      (out->score[v] == out->score[u] && v < u)));
      if(!better) {
        break;
      }
      out->rank[j + 1] = u;
      j--;
    }
    out->rank[j + 1] = v;
  }

  if(out->n_rank > 0 && (out->eligible & BIT(out->rank[0]))) {
    out->winner = out->rank[0];
  }
}
/*---------------------------------------------------------------------------*/
uint8_t
le_election_succession(const le_elect_in_t *in, const le_elect_out_t *out,
                       uint8_t leader, uint8_t *succ, uint8_t max)
{
  uint8_t k, cnt = 0;
  for(k = 0; k < out->n_rank && cnt < max; k++) {
    uint8_t c = out->rank[k];
    if(c == leader || in->energy[c] == 0) {
      continue;
    }
    succ[cnt++] = c;
  }
  return cnt;
}
