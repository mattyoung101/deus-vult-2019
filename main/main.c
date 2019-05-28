#define HANDMADE_MATH_IMPLEMENTATION
#define HANDMADE_MATH_NO_SSE
#include "HandmadeMath.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "defines.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include <math.h>
#include "motor.h"
#include "light.h"
#include "tsop.h"
#include <str.h>
#include "fsm.h"
#include "cam.h"
#include "states.h"
#include "soc/efuse_reg.h"
#include "comms_i2c.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "simple_imu.h"
#include "pid.h"
#include "vl53l0x_api.h"
#include "comms_bluetooth.h"
#include "pb_encode.h"
#include "i2c.pb.h"

#if ENEMY_GOAL == GOAL_YELLOW
    #define AWAY_GOAL goalYellow
    #define HOME_GOAL goalBlue
#elif ENEMY_GOAL == GOAL_BLUE
    #define AWAY_GOAL goalBlue
    #define HOME_GOAL goalYellow
#endif

static uint8_t mode = AUTOMODE_ILLEGAL;

// Task which runs on the master. Receives sensor data from slave and handles complex routines
// like moving, finite state machines, Bluetooth, etc
void master_task(void *pvParameter){
    static const char *TAG = "MasterTask";
    uint8_t robotId = 69;

    // Initialise comms and hardware
    motor_init();
    comms_i2c_init_slave();
    cam_init();
    ESP_LOGI(TAG, "Master hardware init OK");

    // read robot ID from NVS and init Bluetooth
    nvs_get_u8_graceful("RobotSettings", "RobotID", &robotId);
    if (robotId == 0){
        comms_bt_init_master();
    } else {
        comms_bt_init_slave();
    }

    // Initialise FSM
    state_machine_t fsm = fsm_new(&stateAttackPursue);

    esp_task_wdt_add(NULL);

    while (true){
        // update cam
        cam_calc();

        // update values for FSM, mutexes are used to prevent race conditions
        if (xSemaphoreTake(robotStateSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT)) && 
            xSemaphoreTake(pbSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT)) && 
            xSemaphoreTake(goalDataSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT))){
                // reset out values
                robotState.outShouldBrake = false;
                robotState.outOrientation = 0;
                robotState.outDirection = 0;

                // update
                robotState.inBallAngle = (lastSensorUpdate.tsopAngle + TSOP_CORRECTION) % 360;
                robotState.inBallStrength = lastSensorUpdate.tsopStrength;

                robotState.inGoalVisible = robotState.outIsAttack ? AWAY_GOAL.exists : HOME_GOAL.exists;
                robotState.inGoalAngle = robotState.outIsAttack ? AWAY_GOAL.angle + CAM_ANGLE_OFFSET : HOME_GOAL.angle 
                                        + CAM_ANGLE_OFFSET;
                robotState.inGoalLength = robotState.outIsAttack ? (int16_t) AWAY_GOAL.length : HOME_GOAL.length;
                robotState.inGoalDistance = robotState.outIsAttack ? AWAY_GOAL.distance : HOME_GOAL.distance;
                // hack to convert to IMU data to float by multiplying it by 100 before sending then diving it
                robotState.inHeading = lastSensorUpdate.heading;
                robotState.inX = robotX;
                robotState.inY = robotY;

                // unlock semaphores
                xSemaphoreGive(robotStateSem);
                xSemaphoreGive(pbSem);
                xSemaphoreGive(goalDataSem);
        } else {
            ESP_LOGE(TAG, "Failed to acquire semaphores, cannot update FSM data.");
        }

        // update the actual FSM
        fsm_update(&fsm);

        // update_line(&robotState);

        // robotState.outSpeed = 0;
        // print_ball_data(&robotState);

        // run motors
        motor_calc(robotState.outDirection, robotState.outOrientation, robotState.outSpeed);
        motor_move(robotState.outShouldBrake);

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10)); // Random delay at of loop to allow motors to spin
    }
}

// Task which runs on the slave. Reads and calculates sensor data, then sends to master.
void slave_task(void *pvParameter){
    static const char *TAG = "SlaveTask";
    static uint8_t pbBuf[PROTOBUF_SIZE] = {0};

    // Initialise comms
    comms_i2c_init_master(I2C_NUM_0);
    i2c_scanner();

    // Initialise hardware
    tsop_init();
    ls_init();
    // simu_init();
    // simu_calibrate();

    ESP_LOGI(TAG, "Slave hardware init OK");
    esp_task_wdt_add(NULL);
    
    while (true) {
        // update TSOPs
        for (int i = 0; i < 255; i++){
            tsop_update(NULL);
        }
        tsop_calc();
        
        // update IMU
        // simu_calc();

        // setup protobuf byte stream - as far as I can tell, msg and stream will be disposed of after the loop ends
        memset(pbBuf, 0, PROTOBUF_SIZE);
        SensorUpdate msg = SensorUpdate_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(pbBuf, PROTOBUF_SIZE);
        
        // set the message's values
        msg.heading = 36.0f;//heading;
        // if (xSemaphoreTake(nanoDataSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT))){
            msg.lastAngle = 17.8f;//nanoData.lastAngle;
            msg.lineAngle = 16.9f;//nanoData.lineAngle;
            msg.lineOver = true;//nanoData.isLineOver;
            msg.lineSize = 42.69f;//nanoData.lineSize;
            msg.onLine = false;//nanoData.isOnLine;
            // xSemaphoreGive(nanoDataSem);
        // } else {
            // ESP_LOGW(TAG, "Failed to unlock nano data semaphore!");
        // }
        msg.temperature = 24;
        // TODO make these a float
        msg.tsopAngle = 69;//tsopAngle;
        msg.tsopStrength = 32;//tsopStrength;
        msg.voltage = 12.8f;

        // encode and send it
        if (pb_encode(&stream, SensorUpdate_fields, &msg)){
            comms_i2c_write_protobuf(pbBuf, stream.bytes_written, MSG_SENSORUPDATE_ID);
            // PERF_TIMER_STOP;
        } else {
            ESP_LOGE(TAG, "Failed to encode SensorUpdate message: %s", PB_GET_ERROR(&stream));
        }

        esp_task_wdt_reset();
    }
}

void app_main(){
    puts("===================================================================================");
    puts(" * This ESP32 belongs to a robot from Team Deus Vult at Brisbane Boys' College.");
    puts(" * Software copyright (c) 2019 Team Deus Vult. All rights reserved.");
    puts("====================================================================================\n");

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI("AppMain", "Reflashing NVS");
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // AutoMode: automatically assign to slave/master depending on a value set in NVS
    nvs_handle storageHandle;
    ESP_ERROR_CHECK(nvs_open("RobotSettings", NVS_READWRITE, &storageHandle));

    // write master/slave/robot ID to NVS if configured
    #if defined NVS_WRITE_MASTER
        ESP_ERROR_CHECK(nvs_set_u8(storageHandle, "Mode", AUTOMODE_MASTER));
        ESP_LOGE("AutoMode", "Successfully wrote Master to NVS.");
    #elif defined NVS_WRITE_SLAVE
        ESP_ERROR_CHECK(nvs_set_u8(storageHandle, "Mode", AUTOMODE_SLAVE));
        ESP_LOGE("AutoMode", "Successfully wrote Slave to NVS.");
    #elif defined NVS_WRITE_ROBOTNUM
        ESP_ERROR_CHECK(nvs_set_u8(storageHandle, "RobotID", NVS_WRITE_ROBOTNUM));
        ESP_LOGE("RobotID", "Successfully wrote robot number to NVS.");
    #endif

    #if defined NVS_WRITE_MASTER || defined NVS_WRITE_SLAVE || defined NVS_WRITE_ROBOTNUM
        ESP_ERROR_CHECK(nvs_commit(storageHandle));
    #endif

    // we don't use nvs_get_u8_graceful, because its different here as we have the storage handle already open
    err = nvs_get_u8(storageHandle, "Mode", &mode);
    nvs_close(storageHandle);

    if (err == ESP_ERR_NVS_NOT_FOUND){
        ESP_LOGE("AutoMode", "Mode key not found! Please identify this device, see main.c for help. Cannot continue.");
        abort();
    } else if (err != ESP_OK) {
        ESP_LOGE("AutoMode", "Unexpected error reading key: %s. Cannot continue.", esp_err_to_name(err));
        abort();
    }
    fflush(stdout);

    // we use xTaskCreatePinnedToCore() because its necessary to get hardware accelerated floating point maths
    // see: https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/freertos-smp.html#floating-point-aritmetic
    // also according to a forum post, going against the FreeRTOS norm, stack size is in fact in bytes, NOT words!
    // source: https://esp32.com/viewtopic.php?t=900#p3879
    if (mode == AUTOMODE_MASTER){
        ESP_LOGI("AppMain", "Running as master");
        xTaskCreatePinnedToCore(master_task, "MasterTask", 12048, NULL, configMAX_PRIORITIES, NULL, APP_CPU_NUM);
    } else {
        ESP_LOGI("AppMain", "Running as slave");
        xTaskCreatePinnedToCore(slave_task, "SlaveTask", 12048, NULL, configMAX_PRIORITIES, NULL, APP_CPU_NUM);  
    }
}