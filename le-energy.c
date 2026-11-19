#include "le-energy.h"
#include "le-conf.h"

static uint16_t energy;
static uint16_t energy0;

/*---------------------------------------------------------------------------*/
void
le_energy_init(uint8_t idx)
{
  /* deterministic spread so that nodes do not all start equal:
   * k = 0..4 picks 0, 25, 50, 75 or 100 % of the spread */
  uint8_t k = (uint8_t)((idx * 3 + 2) % 5);
  uint32_t spread = (uint32_t)LE_E_INIT * LE_E_INIT_SPREAD_PCT / 100;
  energy0 = (uint16_t)(LE_E_INIT - spread * k / 4);
  energy = energy0;
}
/*---------------------------------------------------------------------------*/
uint16_t
le_energy(void)
{
  return energy;
}
/*---------------------------------------------------------------------------*/
uint16_t
le_energy_initial(void)
{
  return energy0;
}
/*---------------------------------------------------------------------------*/
void
le_energy_use(uint16_t units)
{
  energy = units >= energy ? 0 : energy - units;
}
