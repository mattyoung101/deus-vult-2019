#include "comms_i2c.h"
#include "HandmadeMath.h"
#include "states.h"
#include "pb_decode.h"

i2c_data_t receivedData = {0};
nano_data_t nanoData = {0};
SensorUpdate lastSensorUpdate = SensorUpdate_init_zero;
SemaphoreHandle_t pbSem = NULL;

static const char *TAG = "CommsI2C";

// stupid hack to assign field given message ID as it's a fuckin const so we can't use an if statement
static const pb_field_t* get_pb_fields(uint8_t msgId){
    switch (msgId){
        case MSG_SENSORUPDATE_ID:
            return SensorUpdate_fields;
        default:
            ESP_LOGE(TAG, "Unknown message ID: %d", msgId);
            return NULL;
    }
}

static void comms_i2c_receive_task(void *pvParameters){
    static const char *TAG = "I2CReceiveTask";
    uint8_t *buf = calloc(PROTOBUF_SIZE, sizeof(uint8_t));
    pbSem = xSemaphoreCreateMutex();
    xSemaphoreGive(pbSem);

    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "Slave I2C task init OK");

    while (true){
        memset(buf, 0, PROTOBUF_SIZE);
        esp_task_wdt_reset();

        if (!xSemaphoreTake(pbSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT))){
            ESP_LOGE(TAG, "Failed to unlock Protobuf semaphore!");
        }
        
        // first, try to read in the header
        i2c_slave_read_buffer(I2C_NUM_0, buf, 5, pdMS_TO_TICKS(4096));

        // if it's a valid header it'll start with "HED" (0x48, 0x45, 0x44)
        if (buf[0] == 0x48 && buf[0] == 0x45 && buf[2] == 0x44){
            uint8_t msgId = buf[3];
            uint8_t msgSize = buf[4];

            memset(buf, 0, PROTOBUF_SIZE);
            ESP_LOGD(TAG, "Got header, msg id: %d, msg size: %d", msgId, msgSize);

            // now try to read in the actual protobuf byte stream and deserialise it
            esp_task_wdt_reset();
            i2c_slave_read_buffer(I2C_NUM_0, buf, msgSize, pdMS_TO_TICKS(4096));

            pb_istream_t stream = pb_istream_from_buffer(buf, PROTOBUF_SIZE);
            void *dest = NULL;

            // assign destination struct based on message ID
            switch (msgId){
                case MSG_SENSORUPDATE_ID:
                    dest = (void*) &lastSensorUpdate;
                default:
                    ESP_LOGE(TAG, "Unknown message ID: %d", msgId);
                    return NULL;
            }

            // decode the stream
            if (!pb_decode(&stream, get_pb_fields(msgId), dest)){
                ESP_LOGE(TAG, "Protobuf decode error for message ID %d: %s", msgId, PB_GET_ERROR(&stream));
            }

            xSemaphoreGive(pbSem);
        } else {
            ESP_LOGW(TAG, "Invalid header.");
            ESP_LOG_BUFFER_HEX(TAG, buf, PROTOBUF_SIZE);
        }

        esp_task_wdt_reset();
    }
}

/** sends/receives data from the Arduino Nano LS slave **/
static void nano_comms_task(void *pvParameters){
    static const char *TAG = "NanoCommsTask";
    uint8_t buf[9] = {0};
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "Nano comms task init OK");

    while (true){
        /*
        float inLineAngle: 2 bytes
        float inLineSize: 2 bytes
        bool inOnLine: 1 byte
        bool inLineOver: 1 byte
        float inLastAngle: 2 bytes
        = 8 bytes + 1 start byte 
        = 9 bytes total
        */
        memset(buf, 0, NANO_PACKET_SIZE);
        nano_read(I2C_NANO_SLAVE_ADDR, NANO_PACKET_SIZE, buf);

        if (buf[0] == I2C_BEGIN_DEFAULT){
            if (xSemaphoreTake(robotStateSem, pdMS_TO_TICKS(SEMAPHORE_UNLOCK_TIMEOUT))){
                // buf[0] is the begin byte, so start from buf[1]
                nanoData.lineAngle = UNPACK_16(buf[1], buf[2]) / IMU_MULTIPLIER;
                nanoData.lineSize = UNPACK_16(buf[3], buf[4]) / IMU_MULTIPLIER;
                nanoData.isOnLine = (bool) buf[5];
                nanoData.isLineOver = (bool) buf[6];
                nanoData.lastAngle = UNPACK_16(buf[7], buf[8]) / IMU_MULTIPLIER;
                xSemaphoreGive(robotStateSem);
            } else {
                ESP_LOGW(TAG, "Failed to acquire robot state semaphore in time!");
            }
        } else {
            ESP_LOGW(TAG, "Invalid buffer, first byte is: 0x%X", buf[0]);
        }

        esp_task_wdt_reset();
    }
}

void comms_i2c_init_master(i2c_port_t port){
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = 21,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = 22,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        // 0.5 MHz, max is 1 MHz, unit is Hz
        // NOTE: 1MHz tends to break the i2c packets - use with caution!!
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(port, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(port, conf.mode, 0, 0, 0));

    xTaskCreate(nano_comms_task, "NanoCommsTask", 3096, NULL, configMAX_PRIORITIES - 1, NULL);

    ESP_LOGI("CommsI2C_M", "I2C init OK as master (RL slave) on bus %d", port);
}

void comms_i2c_init_slave(void){
    i2c_config_t conf = {
        .mode = I2C_MODE_SLAVE,
        .sda_io_num = 21,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = 22,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .slave.addr_10bit_en = 0,
        .slave.slave_addr = I2C_ESP_SLAVE_ADDR
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    // min size is of i2c fifo buffer is 100 bytes, so we use a 32 * 9 byte buffer
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, conf.mode, 288, 288, 0));
    xTaskCreate(comms_i2c_receive_task, "I2CReceiveTask", 8192, NULL, configMAX_PRIORITIES - 1, NULL);

    ESP_LOGI("CommsI2C_S", "I2C init OK as slave (RL master)");
}

static int write_buffer(uint8_t *buf, size_t size){
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    ESP_ERROR_CHECK(i2c_master_start(cmd));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd, (I2C_ESP_SLAVE_ADDR << 1) | I2C_MASTER_WRITE, I2C_ACK_MODE));

    ESP_ERROR_CHECK(i2c_master_write(cmd, buf, size, I2C_ACK_MODE));
    ESP_ERROR_CHECK(i2c_master_stop(cmd));
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(I2C_TIMEOUT));
    I2C_ERR_CHECK(err);

    i2c_cmd_link_delete(cmd);
    return ESP_OK;
}

int comms_i2c_write_protobuf(uint8_t *buf, size_t msgSize, uint8_t msgId){
    // First, write header. The first three bytes are "HED" in ASCII so we can see that it's not a protocol buffer.
    // Then we write out the msg's ID and size.
    uint8_t header[] = {0x48, 0x45, 0x44, msgId, msgSize};
    int status = write_buffer(header, 5);

    // Now write out the actual Protobuf data stream.
    status += write_buffer(buf, msgSize);

    return status;
}