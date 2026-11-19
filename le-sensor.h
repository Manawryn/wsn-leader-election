/* le-sensor.h - temperature (SHT11) and light readings of the TelosB */
#ifndef LE_SENSOR_H_
#define LE_SENSOR_H_

#include "le-msg.h"

void le_sensor_init(void);
/* Fill temp and light (not seq). May block ~0.2 s for the SHT11, so it is
 * only called outside the TDMA frame. */
void le_sensor_read(le_reading_t *r);

#endif /* LE_SENSOR_H_ */
