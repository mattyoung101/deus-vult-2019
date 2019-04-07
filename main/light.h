#pragma once
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_adc_cal.h"
#include "driver/adc.h"
#include "utils.h"
#include "defines.h"
#include "math.h"

// Converted from LightSensor.cpp

typedef struct {
    int32_t thresholdValue;
    gpio_num_t pin;
} light_sensor;

/** Initialises the ADC for light sensor usage **/
void ls_init_adc(void);
/** Initialises a single light sensor struct **/
void ls_init(light_sensor *ls);
/** Reads the value of a light sensor **/
uint16_t ls_read(light_sensor *ls);
/** Returns true if the light sensor is on the line, else false **/
bool ls_on_white(light_sensor *ls);

typedef struct {
    float centre;
    int16_t length;
    int16_t leftSensor;
    int16_t rightSensor;
} ls_cluster;

void cluster_update_left_right(ls_cluster *cluster);
void cluster_update_length_centre(ls_cluster *cluster);
/** Adds two clusters, storing the result in cluster1 **/
void cluster_add_cluster(ls_cluster *cluster1, ls_cluster *cluster2);
void cluster_reset(ls_cluster *cluster);
void cluster_add_clockwise(ls_cluster *cluster);
float cluster_get_angle(ls_cluster *cluster);
float cluster_get_left_angle(ls_cluster *cluster);
float cluster_get_right_angle(ls_cluster *cluster);

/** Initialises all light sensors in the light sensor array **/
void lsarray_init(void);
/** Reads all light sensors **/
void lsarray_read(void);
void lsarray_calc_clusters(void);
void lsarray_fill_in_sensors(void);
void lsarray_calc_line(void);

uint8_t numClusters = 0;
float lineAngle = 0.0f;
float lineSize = 0.0f;