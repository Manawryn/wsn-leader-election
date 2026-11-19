/*
 * Contiki-NG stack configuration for the leader-election project.
 * Application parameters (timing, election weights, energy model, ...)
 * are in le-conf.h.
 */
#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* ---- MAC: CSMA (default with NullNet) tuned for slotted access ---------- */
/* CSMA normally waits a random 0..(2^3-1) clock ticks (up to ~47 ms on the
 * sky, CLOCK_SECOND = 128) before the FIRST attempt. Our slots are already
 * collision-free, so send immediately; after a busy channel the backoff
 * window grows as usual. */
#define CSMA_CONF_MIN_BE                 0
#define CSMA_CONF_MAX_BE                 3
#define CSMA_CONF_MAX_BACKOFF            3
/* Default retries for unicast; the application overrides per packet
 * (PACKETBUF_ATTR_MAX_MAC_TRANSMISSIONS). */
#define CSMA_CONF_MAX_FRAME_RETRIES      2

#define QUEUEBUF_CONF_NUM                4

/* ---- Energest: measure real radio-on time --------------------------------*/
#define ENERGEST_CONF_ON                 1

/* ---- Stack usage measurement (memory requirements) ---------------------- */
#define STACK_CHECK_CONF_ENABLED         1
#define STACK_CHECK_CONF_PERIODIC_CHECKS 0
/* (the sky platform defines STACK_CONF_ORIGIN = top of RAM, 0x3900) */

/* ---- Logging: keep the stack quiet, the app prints its own lines ------- */
#define LOG_CONF_LEVEL_MAC               LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_FRAMER            LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_NULLNET           LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAIN              LOG_LEVEL_INFO

#endif /* PROJECT_CONF_H_ */
