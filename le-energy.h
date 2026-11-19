/*
 * le-energy.h - simulated battery. Remaining energy is decreased per sent and
 * received message and per round, with an extra cost for the leader.
 */
#ifndef LE_ENERGY_H_
#define LE_ENERGY_H_

#include <stdint.h>

void le_energy_init(uint8_t idx);
uint16_t le_energy(void);
void le_energy_use(uint16_t units);
uint16_t le_energy_initial(void);

#endif /* LE_ENERGY_H_ */
