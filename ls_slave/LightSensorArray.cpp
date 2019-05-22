#include "LightSensorArray.h"
#define MUX_EN 2
#define MUX_A0 3
#define MUX_A1 4
#define MUX_A2 5
#define MUX_A3 6
#define MUX_A4 7
#define MUX_WR 8
#define LS0 A0
#define LS1 A1

#define LS_CALIBRATION_COUNT 10
#define LS_CALIBRATION_BUFFER 300
#define LS_ES_DEFAULT 69
#define NO_LINE_ANGLE 0xBAD
#define NO_LINE_SIZE 0xBAD
#define LS_NUM_MULTIPLIER 7.5 // 360 / LS_NUM

#define DEG_RAD 0.017453292519943295 // multiply to convert degrees to radians
#define RAD_DEG 57.29577951308232 // multiply to convert radians to degrees

static int32_t mod(int32_t x, int32_t m){
    int32_t r = x % m;
    return r < 0 ? r + m : r;
}

static double angleBetween(double angleCounterClockwise, double angleClockwise){
    return mod(angleClockwise - angleCounterClockwise, 360);
}

static double smallestAngleBetween(double angle1, double angle2){
    double ang = angleBetween(angle1, angle2);
    return fminf(ang, 360 - ang);
}

static double midAngleBetween(double angleCounterClockwise, double angleClockwise){
    return mod(angleCounterClockwise + angleBetween(angleCounterClockwise, angleClockwise) / 2.0, 360);
}

void LightSensorArray::init() {
    pinMode(MUX_EN, OUTPUT);
    pinMode(MUX_WR, OUTPUT);
    pinMode(MUX_A0, OUTPUT);
    pinMode(MUX_A1, OUTPUT);
    pinMode(MUX_A2, OUTPUT);
    pinMode(MUX_A3, OUTPUT);
    pinMode(MUX_A4, OUTPUT);

    pinMode(LS0, INPUT);
    pinMode(LS1, INPUT);

    digitalWrite(MUX_EN, LOW);
}

void LightSensorArray::changeMUXChannel(uint8_t channel) {
    // Change the multiplexer channel
    digitalWrite(MUX_WR, LOW);

    digitalWrite(MUX_A0, channel & 0x1);
    digitalWrite(MUX_A1, (channel >> 1) & 0x1);
    digitalWrite(MUX_A2, (channel >> 2) & 0x1);
    digitalWrite(MUX_A3, (channel >> 3) & 0x1);
    digitalWrite(MUX_A4, (channel >> 4) & 0x1);

    digitalWrite(MUX_WR, HIGH);
}

void LightSensorArray::calibrate() {
    // Average a number of reads of each sensor, and add a buffer

    for (int i = 0; i < LS_NUM; i++) {
        int defaultValue = 0;

        for (int j = 0; j < LS_CALIBRATION_COUNT; j++) {
            defaultValue += readSensor(i);
        }

        thresholds[i] = round(((double)defaultValue / LS_CALIBRATION_COUNT) + LS_CALIBRATION_BUFFER);
    }
}

int LightSensorArray::readSensor(int sensor) {
    // Unlike LJ, we always read off the mux
    ////////////////// TODO ask Yellando why it's - 4 - 1
    changeMUXChannel(muxChannels[sensor - 4] - 1);
    ////////////////// TODO this will either be A0 or A1
    return analogRead(A0);
}

void LightSensorArray::read() {
    // Read all 48 sensors
    for (int i = 0; i < LS_NUM; i++) {
        data[i] = readSensor(i) > thresholds[i];
    }
}

void LightSensorArray::calculateClusters(bool doneFillInSensors) {
    // Pick the on/off data array based on whether "filling in" has been done
    bool *lightData = !doneFillInSensors ? data : filledInData;

    // Clear start and end arrays
    resetStartEnds();

    // Sensor index
    int index = 0;
    
    // Previous sensor on/off value
    bool previousValue = false;

    // Loop through the sensors to find clusters
    for (int i = 0; i < LS_NUM; i++) {
        // Cluster start if sensors go off->on
        if (lightData[i] && !previousValue) {
            starts[index] = i;
        }

        // Cluster end if sensors go on->off
        if (!lightData[i] && previousValue) {
            ends[index] = i - 1;
            index++;

            if (index > 3) {
                // Too many clusters
                if (!doneFillInSensors) {
                    // "Fill in" sensors
                    fillInSensors();
                } else {
                    // Unrecognisable line
                    resetStartEnds();
                    numClusters = 0;
                }

                return;
            }
        }

        previousValue = lightData[i];
    }

    // Number of completed clusters
    int tempNumClusters = (int)(starts[0] != LS_ES_DEFAULT) + (int)(starts[1] != LS_ES_DEFAULT) + (int)(starts[2] != LS_ES_DEFAULT) + (int)(starts[3] != LS_ES_DEFAULT);

    if (tempNumClusters != index) {
        // If the final cluster didn't end, index will be one less than tempNumClusters

        if (starts[0] == 0) {
            // If the first cluster starts at 0, then merge the first and last
            starts[0] = starts[index];
        } else {
            // Otherwise, end the last cluster
            ends[index] = LS_NUM - 1;
            index++;

            if (index > 3) {
                // Too many clusters
                if (!doneFillInSensors) {
                    // "Fill in" sensors
                    fillInSensors();
                } else {
                    // Unrecognisable line
                    resetStartEnds();
                    numClusters = 0;
                }

                return;
            }
        }
    }

    numClusters = index;
}

void LightSensorArray::fillInSensors() {
    // "Filling in sensors" if an off sensor has two adjacent on sensors, it will be turned on

    for (int i = 0; i < LS_NUM; i++) {
        filledInData[i] = data[i];

        if (!data[i] && data[mod(i - 1, LS_NUM)] && data[mod(i + 1, LS_NUM)]) {
            filledInData[i] = true;
        }
    }

    calculateClusters(true);
}

void LightSensorArray::calculateLine() {
    if (numClusters == 0) {
        // No clusters, no line
        angle = NO_LINE_ANGLE;
        size = NO_LINE_SIZE;
    } else {
        // Angle of each cluster
        double cluster1Angle = midAngleBetween(starts[0] * LS_NUM_MULTIPLIER, ends[0] * LS_NUM_MULTIPLIER);
        double cluster2Angle = midAngleBetween(starts[1] * LS_NUM_MULTIPLIER, ends[1] * LS_NUM_MULTIPLIER);
        double cluster3Angle = midAngleBetween(starts[2] * LS_NUM_MULTIPLIER, ends[2] * LS_NUM_MULTIPLIER);

        if (numClusters == 1) {
            // 1 cluster, the line angle is the angle of the cluster

            angle = cluster1Angle;
            size = 1 - cos(DEG_RAD * (angleBetween(starts[0] * LS_NUM_MULTIPLIER, ends[0] * LS_NUM_MULTIPLIER) / 2.0));
        } else if (numClusters == 2) {
            // 2 clusters, angle is the midpoint of the two

            angle = angleBetween(cluster1Angle, cluster2Angle) <= 180 ? midAngleBetween(cluster1Angle, cluster2Angle) : midAngleBetween(cluster2Angle, cluster1Angle);
            size = 1 - cos(DEG_RAD * (angleBetween(cluster1Angle, cluster2Angle) <= 180 ? angleBetween(cluster1Angle, cluster2Angle) / 2.0 : angleBetween(cluster2Angle, cluster1Angle) / 2.0));
        } else {
            // 3 clusters, angle is the midpoint of the arc connecting the two furthest apart clusters through the third cluster.

            double angleDiff12 = angleBetween(cluster1Angle, cluster2Angle);
            double angleDiff23 = angleBetween(cluster2Angle, cluster3Angle);
            double angleDiff31 = angleBetween(cluster3Angle, cluster1Angle);

            double biggestAngle = max(angleDiff12, max(angleDiff23, angleDiff31));

            if (angleDiff12 == biggestAngle) {
                angle = midAngleBetween(cluster2Angle, cluster1Angle);
                size = angleBetween(cluster2Angle, cluster1Angle) <= 180 ? 1 - cos(DEG_RAD * (angleBetween(cluster2Angle, cluster1Angle) / 2.0)) : 1;
            } else if (angleDiff23 == biggestAngle) {
                angle = midAngleBetween(cluster3Angle, cluster2Angle);
                size = angleBetween(cluster3Angle, cluster2Angle) <= 180 ? 1 - cos(DEG_RAD * (angleBetween(cluster3Angle, cluster2Angle) / 2.0)) : 1;
            } else {
                angle = midAngleBetween(cluster1Angle, cluster3Angle);
                size = angleBetween(cluster1Angle, cluster3Angle) <= 180 ? 1 - cos(DEG_RAD * (angleBetween(cluster1Angle, cluster3Angle) / 2.0)) : 1;
            }
        }
    }
}

void LightSensorArray::resetStartEnds() {
    for (int i = 0; i < 4; i++) {
        starts[i] = LS_ES_DEFAULT;
        ends[i] = LS_ES_DEFAULT;
    }
}

double LightSensorArray::getLineAngle() {
    return angle;
}

double LightSensorArray::getLineSize() {
    return size;
}

void LightSensorArray::dump() {
    for(int i = 0; i < 48; i++) {
        print(data[i]);
        print("\t");
    }
    println();
}
