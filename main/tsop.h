#pragma once
#include "driver/gpio.h"
#include "esp_err.h"
#include <math.h>
#include "defines.h"
#include "utils.h"
#include "mplexer.h"
#include "esp_timer.h"
#include <inttypes.h>

float tsopAngle;
float tsopStrength;

/** Initialises the TSOP pins and multiplexer **/
void tsop_init(void);
/** Reads all the TSOP values into the temp array **/
void tsop_update(void *args);
/** Sorts and processes the temp array **/
void tsop_process(void);
/** Calculates tsopAngle and tsopStrength for the best n values **/
void tsop_calc(uint8_t n);
/** Dumps the temp array to UART **/
void tsop_dump(void);