/*
 * PC unit tests for the election and link-estimation logic.
 *   cc -I.. -o test-election test-election.c ../le-election.c ../le-link.c && ./test-election
 */
#include <stdio.h>
#include <string.h>
#include "le-election.h"
#include "le-link.h"
#include "le-msg.h"

static int failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while(0)

static le_link_row_t mx[LE_MAX_NODES];

static void
fill(int8_t v)
{
  memset(mx, LE_RSSI_NONE, sizeof(mx));
  for(int i = 0; i < 5; i++) for(int j = 0; j < 5; j++) if(i != j) mx[i][j] = v;
}

static le_elect_out_t
run(uint16_t alive, uint8_t exclude, const uint16_t *e)
{
  le_elect_in_t in = { 5, alive, exclude, e, (const le_link_row_t *)mx };
  le_elect_out_t out;
  le_election_run(&in, &out);
  return out;
}

int
main(void)
{
  le_elect_out_t o;

  /* 1. equal links: highest energy wins */
  fill(-60);
  { uint16_t e[5] = { 1500, 1900, 1700, 1800, 1600 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner == 1);
    CHECK(o.relaxed == 0);
    CHECK(o.rank[0] == 1 && o.rank[1] == 3); }

  /* 2. rotation: current leader excluded even if it is the best */
  { uint16_t e[5] = { 1500, 1900, 1700, 1800, 1600 };
    o = run(0x1f, 1, e);
    CHECK(o.winner == 3);
    for(int k = 0; k < o.n_rank; k++) CHECK(o.rank[k] != 1); }

  /* 3. LEACH-C: below-average energy is not eligible */
  { uint16_t e[5] = { 1000, 1000, 1000, 1000, 1900 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner == 4);
    CHECK((o.eligible & 0x0f) == 0); }

  /* 4. node with a bad worst link loses although it has more energy */
  fill(-60);
  mx[2][4] = -93; mx[4][2] = -93;      /* link 2<->4 is bad */
  { uint16_t e[5] = { 1700, 1700, 1800, 1700, 1700 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner != 2 && o.winner != 4);
    CHECK(!(o.eligible & (1 << 2))); }

  /* 5. better links win when energy is equal */
  fill(-80);
  for(int j = 0; j < 5; j++) if(j != 3) { mx[3][j] = -50; mx[j][3] = -50; }
  { uint16_t e[5] = { 1800, 1800, 1800, 1800, 1800 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner == 3); }

  /* 6. everybody below E_LOW: rules are relaxed, best of them still wins */
  fill(-60);
  { uint16_t e[5] = { 300, 500, 400, 200, 100 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner == 1);
    CHECK(o.relaxed == 3); }

  /* 7. dead node never wins, ties go to the lower index */
  { uint16_t e[5] = { 1800, 1800, 0, 1800, 1800 };
    o = run(0x1f, LE_NONE, e);
    CHECK(o.winner == 0);
    uint8_t succ[3];
    le_elect_in_t in = { 5, 0x1f, LE_NONE, e, (const le_link_row_t *)mx };
    uint8_t n = le_election_succession(&in, &o, o.winner, succ, 3);
    CHECK(n == 3 && succ[0] == 1 && succ[1] == 3 && succ[2] == 4); }

  /* 8. only participants are considered */
  { uint16_t e[5] = { 1000, 1900, 1800, 1000, 1000 };
    o = run(0x1d /* node 1 missing */, LE_NONE, e);
    CHECK(o.winner == 2); }

  /* 9. WMEWMA PRR */
  { le_prr_t p; le_prr_init(&p);
    CHECK(le_prr_get(&p) == 100);
    for(int i = 0; i < LE_PRR_WINDOW_T; i++) le_prr_update(&p, i % 2);
    CHECK(le_prr_get(&p) == 50);
    for(int i = 0; i < LE_PRR_WINDOW_T; i++) le_prr_update(&p, 1);
    CHECK(le_prr_get(&p) == (LE_PRR_ALPHA_PCT * 50 + (100 - LE_PRR_ALPHA_PCT) * 100) / 100); }

  /* 10. RSSI EWMA */
  le_link_init(0, 5);
  le_link_rx(1, -60);
  CHECK(le_link_rssi(1) == -60);
  le_link_rx(1, -80);
  CHECK(le_link_rssi(1) == -65);
  CHECK(le_link_rssi(2) == LE_RSSI_NONE);
  CHECK(le_link_neighbors() == 1);

  printf("%s (%d failures)\n", failures ? "FAILED" : "all tests passed", failures);
  return failures != 0;
}
