#include "contiki.h"
#include "sys/energest.h"
#include "sys/stack-check.h"
#include "le-stats.h"

le_stats_t le_stats;

/*---------------------------------------------------------------------------*/
void
le_stats_tx(uint8_t type, uint16_t len)
{
  if(type < LE_MSG_NUM) {
    le_stats.tx[type]++;
    le_stats.tx_bytes[type] += len;
  }
}
/*---------------------------------------------------------------------------*/
void
le_stats_rx(uint8_t type)
{
  if(type < LE_MSG_NUM) {
    le_stats.rx[type]++;
  }
}
/*---------------------------------------------------------------------------*/
uint32_t
le_stats_ctrl_msgs(void)
{
  uint8_t t;
  uint32_t c = 0;
  for(t = 1; t < LE_MSG_NUM; t++) {
    if(t != LE_MSG_DATA) {
      c += le_stats.tx[t];
    }
  }
  return c;
}
/*---------------------------------------------------------------------------*/
uint32_t
le_now_ms(void)
{
  clock_time_t t = clock_time();
  return (uint32_t)(t / CLOCK_SECOND) * 1000 +
         (uint32_t)(t % CLOCK_SECOND) * 1000 / CLOCK_SECOND;
}
/*---------------------------------------------------------------------------*/
static uint32_t
energest_ms(uint64_t ticks)
{
#if ENERGEST_SECOND == 32768
  uint32_t x = (uint32_t)(ticks >> 5);   /* 1/1024 s */
  return x - ((3 * x) >> 7);             /* * 125 / 128 */
#else
  return (uint32_t)(ticks * 1000 / ENERGEST_SECOND);
#endif
}
/*---------------------------------------------------------------------------*/
uint32_t
le_radio_listen_ms(void)
{
  energest_flush();
  return energest_ms(energest_type_time(ENERGEST_TYPE_LISTEN));
}
/*---------------------------------------------------------------------------*/
uint32_t
le_radio_tx_ms(void)
{
  energest_flush();
  return energest_ms(energest_type_time(ENERGEST_TYPE_TRANSMIT));
}
/*---------------------------------------------------------------------------*/
long
le_stack_peak(void)
{
#if STACK_CHECK_ENABLED && defined(STACK_CONF_ORIGIN)
  return (long)stack_check_get_usage();
#else
  return -1;
#endif
}
/*---------------------------------------------------------------------------*/
unsigned
le_static_ram(void)
{
#ifdef CONTIKI_TARGET_SKY
  extern uint8_t _stack;  /* end of .data + .bss + .noinit */
  return (unsigned)((uintptr_t)&_stack - 0x1100);
#else
  return 0;
#endif
}
