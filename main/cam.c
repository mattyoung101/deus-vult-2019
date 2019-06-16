#include "cam.h"

// This file implements communication with the OpenMV M7 using a custom protocol over UART

SemaphoreHandle_t goalDataSem = NULL;
cam_goal goalBlue = {0};
cam_goal goalYellow = {0};
int16_t robotX = 0;
int16_t robotY = 0;
static float k = 92.5f; // distance of goal to centre in cm, measured on the field

static void cam_receive_task(void *pvParameter){
    static const char *TAG = "CamReceiveTask";
    goalDataSem = xSemaphoreCreateMutex();
    xSemaphoreGive(goalDataSem);
    
    uint8_t *buffer = calloc(CAM_BUF_SIZE, sizeof(uint8_t));
    ESP_LOGI(TAG, "Cam receive task init OK");
    esp_task_wdt_add(NULL);

    while (true){
        memset(buffer, 0, CAM_BUF_SIZE);
        esp_task_wdt_reset();

        // wait slightly shorter than the watchdog timer for our bytes to come in
        uart_read_bytes(UART_NUM_2, buffer, CAM_BUF_SIZE, pdMS_TO_TICKS(4096)); 

        if (buffer[0] == CAM_BEGIN_BYTE){
            // ESP_LOGW(TAG, "Found start byte %d", buffer[0]);
            if (xSemaphoreTake(goalDataSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT))){
                // first byte is begin byte so skip that
                goalBlue.exists = buffer[1];
                goalBlue.x = buffer[2] - CAM_OFFSET_X;
                goalBlue.y = buffer[3] - CAM_OFFSET_Y;

                goalYellow.exists = buffer[4];
                goalYellow.x = buffer[5] - CAM_OFFSET_X;
                goalYellow.y = buffer[6] - CAM_OFFSET_Y;

                cam_calc();
                xSemaphoreGive(goalDataSem);
            } else {
                ESP_LOGW(TAG, "Unable to acquire semaphore in time!");
            }
        } else {
            ESP_LOGW(TAG, "Invalid buffer, first byte is: 0x%X, expected: 0x%X", buffer[0], CAM_BEGIN_BYTE);
            uart_flush_input(UART_NUM_2);
        }

        esp_task_wdt_reset();
        // uart_flush_input(UART_NUM_2);
    }
}

static const char *TAG = "Camera";

void cam_init(void){
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, CAM_UART_TX, CAM_UART_RX, -1, -1));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 256, 256, 8, NULL, 0));

    xTaskCreate(cam_receive_task, "CamReceiveTask", 4096, NULL, configMAX_PRIORITIES - 2, NULL);

    ESP_LOGI(TAG, "Camera init OK");
}

/** 
 * Converts from pixel distance to real distance in cm
 * Model: f(x) = 3E-07x^4.8552
 **/
static inline float cam_pixel_to_cm(int16_t measurement){
    return powf(3E-07, 4.8552);
}

/** calculates the position vector for a goal */
static inline hmm_vec2 cam_goal_calc(float angle, float distance){
    float theta = fmodf(90.0f - angle, 360.0f);
    float r = distance;
    return HMM_Vec2(-r * cosfd(theta), k + r * sinfd(theta));
}

void cam_calc(void){
    goalBlue.angle = floatMod(450 - roundf(RAD_DEG * atan2f(goalBlue.y, goalBlue.x)), 360.0f);
    goalBlue.length = sqrtf(sq(goalBlue.x) + sq(goalBlue.y));

    goalYellow.angle = floatMod(450 - roundf(RAD_DEG * atan2f(goalYellow.y, goalYellow.x)), 360.0f);
    goalYellow.length = sqrtf(sq(goalYellow.x) + sq(goalYellow.y));

    goalYellow.distance = cam_pixel_to_cm(goalYellow.length);
    goalBlue.distance = cam_pixel_to_cm(goalBlue.distance);

    if (!goalBlue.exists && !goalYellow.exists){
        robotX = CAM_NO_VALUE;
        robotY = CAM_NO_VALUE;
    } else {
        hmm_vec2 yellowPos = cam_goal_calc(goalYellow.angle, goalYellow.distance);
        hmm_vec2 bluePos = cam_goal_calc(goalBlue.angle, goalBlue.distance);

        if (goalYellow.exists && !goalBlue.exists){
            // only yellow goal visible
            robotX = yellowPos.X;
            robotY = yellowPos.Y;
        } else if (goalBlue.exists && !goalYellow.exists){
            // only blue goal visible
            robotX = bluePos.X;
            robotY = bluePos.Y;
        } else {
            // both goals visible
            if (goalYellow.distance < goalBlue.distance){
                // yellow goal is closer, use it
                robotX = yellowPos.X;
                robotY = yellowPos.Y;
            } else {
                // blue goal is closer, use it
                robotX = bluePos.X;
                robotY = bluePos.Y;
            }
        }
    }
}