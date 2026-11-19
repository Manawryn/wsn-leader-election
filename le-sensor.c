#include "contiki.h"
#include "le-sensor.h"
#include "le-conf.h"

#ifdef CONTIKI_TARGET_SKY
#include "dev/light-sensor.h"
#include "sht11-sensor.h"
#endif

/*---------------------------------------------------------------------------*/
void
le_sensor_init(void)
{
#ifdef CONTIKI_TARGET_SKY
  SENSORS_ACTIVATE(light_sensor);
#if LE_USE_SHT11
  SENSORS_ACTIVATE(sht11_sensor);
#endif
#endif
}
/*---------------------------------------------------------------------------*/
void
le_sensor_read(le_reading_t *r)
{
  r->temp = INT16_MIN;
  r->light = 0;
#ifdef CONTIKI_TARGET_SKY
  r->light = (uint16_t)light_sensor.value(LIGHT_SENSOR_TOTAL_SOLAR);
#if LE_USE_SHT11
  {
    int raw = sht11_sensor.value(SHT11_SENSOR_TEMP);
    if(raw != -1) {
      /* SHT11 datasheet, 14 bit, 3 V: T = -39.60 + 0.01 * raw  [degC] */
      r->temp = (int16_t)(raw - 3960);
    }
  }
#endif
#endif
}
