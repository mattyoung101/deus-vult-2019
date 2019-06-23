#include "states.h"
#include "simple_imu.h"
#include "utils.h"

// static pid_config_t lrfPID = {LRF_KP, LRF_KI, LRF_KD, LRF_MAX};
static pid_config_t interceptPID = {INTERCEPT_KP, INTERCEPT_KI, INTERCEPT_KD, INTERCEPT_MAX, 0};

fsm_state_t stateDefenceReverse = {&state_nothing_enter, &state_nothing_exit, &state_defence_reverse_update, "DefenceReverse"};
fsm_state_t stateDefenceIdle = {&state_nothing_enter, &state_nothing_exit, &state_defence_idle_update, "DefenceIdle"};
fsm_state_t stateDefenceDefend = {&state_defence_defend_enter, &state_nothing_exit, &state_defence_defend_update, "DefenceDefend"};
fsm_state_t stateDefenceSurge = {&state_nothing_enter, &state_nothing_exit, &state_defence_surge_update, "DefenceSurge"};

static dv_timer_t surgeTimer = {NULL, false};

// shortcut lol
#define rs robotState

// Reverse
void state_defence_reverse_update(state_machine_t *fsm){
    static const char *TAG = "DefendReverseState";

    rs.outIsAttack = false;
    imu_correction(&robotState);

    if (rs.inGoalVisible){
        LOG_ONCE(TAG, "Goal is visible, switching to idle");
        FSM_CHANGE_STATE_DEFENCE(Idle);
    }

    if (rs.inBallStrength > 0.0f){ 
        // Check if ball is behind
        if (!is_angle_between(rs.inBallAngle, 90.0f, 270.0f)){
            LOG_ONCE(TAG, "Ball is behind, orbiting");
            orbit(&robotState);
        } else {
            // TODO is ball in front? should we request switch?
            float distanceMovement = REVERSE_SPEED;
            float sidewaysMovement = pid_update(&sidePID, fmodf(rs.inBallAngle + 180.0f, 360.0f) - 180, 0.0f, 0.0f);

            rs.outDirection = fmodf(RAD_DEG * (atan2f(sidewaysMovement, distanceMovement)) - rs.inHeading, 360.0f);
            rs.outSpeed = get_magnitude(sidewaysMovement, distanceMovement);
        }
    } else {
        rs.outDirection = 180;
        rs.outSpeed = REVERSE_SPEED;
    }
}

// Idle
void state_defence_idle_update(state_machine_t *fsm){
    static const char *TAG = "DefendIdleState";

    rs.outIsAttack = false;
    imu_correction(&robotState);

    if (!rs.inGoalVisible){
        // LOG_ONCE(TAG, "Goal not visible, switching to reverse");
        LOG_ONCE(TAG, "Cancelling state change to reverse"); // TODO change when we get LRFs
        // FSM_CHANGE_STATE_DEFENCE(Reverse);
    } else if (rs.inBallStrength > 0.0f){
        LOG_ONCE(TAG, "Ball is visible, switching to defend");
        FSM_CHANGE_STATE_DEFENCE(Defend);
    }

    position(&robotState, DEFEND_DISTANCE, 0.0f, rs.inGoalAngle, rs.inGoalLength, true);
}

// Defend
static void surge_timer_callback(TimerHandle_t timer){
    static const char *TAG = "SurgeTimerCallback";
    ESP_LOGI(TAG, "Surge timer has gone off, switching to surge state");

    state_machine_t *fsm = (state_machine_t*) pvTimerGetTimerID(timer);
    dv_timer_stop(&surgeTimer);
    FSM_CHANGE_STATE_DEFENCE(Surge);
}

void state_defence_defend_enter(state_machine_t *fsm){
    dv_timer_check_create(&surgeTimer, "SurgeTimer", SURGE_TIMEOUT, (void*) fsm, surge_timer_callback);
 }

 void state_defence_defend_update(state_machine_t *fsm){
    static const char *TAG = "DefendDefendState";

    rs.outIsAttack = false;

    // if (is_angle_between(rs.inBallAngle, DEFEND_MIN_ANGLE, DEFEND_MAX_ANGLE)) goal_correction(&robotState);
    // else imu_correction(&robotState); // Face the back of the robot to the goal
    goal_correction(&robotState);

    // check the timer conditions first, so that we don't get switched out of before we can check
    if (is_angle_between(rs.inBallAngle, IN_FRONT_MIN_ANGLE + 40, IN_FRONT_MAX_ANGLE - 40) 
        && rs.inBallStrength >= SURGE_STRENGTH && rs.inGoalLength < SURGE_DISTANCE){
        LOG_ONCE(TAG, "Ball is in capture zone and goal is nearby, starting surge timer");
        RS_SEM_LOCK;
        rs.outSwitchOk = true;
        RS_SEM_UNLOCK;
        dv_timer_start(&surgeTimer);
    } else {
        LOG_ONCE(TAG, "Stopping surge timer as criteria is no longer satisfied");
        dv_timer_stop(&surgeTimer);
    }

    // Check criteria: goal visible and ball visible
    if (!rs.inGoalVisible){
        LOG_ONCE(TAG, "Goal not visible, switching to reverse"); // NOTE: should reverse using LRFs but we dono't have those yet
        FSM_CHANGE_STATE_DEFENCE(Reverse);
    } else if (rs.inBallStrength <= 0.0f){
        LOG_ONCE(TAG, "Ball not visible, switching to idle");
        FSM_CHANGE_STATE_DEFENCE(Idle);
    } 

    if (!is_angle_between(rs.inBallAngle, DEFEND_MIN_ANGLE, DEFEND_MAX_ANGLE)){
        // Ball is behind, orbit so we don't score an own goal
        orbit(&robotState);
    } else {
        float tempAngle = robotState.inBallAngle > 180 ? robotState.inBallAngle - 360 : robotState.inBallAngle;
        float goalAngle = robotState.inGoalAngle < 0.0f ? robotState.inGoalAngle + 360.0f : robotState.inGoalAngle; // Convert to 0 - 360 range
        float goalAngle_ = fmodf(goalAngle + robotState.inHeading, 360.0f);

        float verticalDistance = fabsf(robotState.inGoalLength * cosf(DEG_RAD * goalAngle_));
        float distanceMovement = pid_update(&forwardPID, verticalDistance, DEFEND_DISTANCE, 0.0f); // Stay a fixed distance from the goal
        
        float sidewaysMovement = -pid_update(&interceptPID, tempAngle, 0.0f, 0.0f); // Centre on the ball
        if(fabsf(sidewaysMovement) < INTERCEPT_MIN) sidewaysMovement = 0;

        rs.outDirection = fmodf(RAD_DEG * (atan2f(sidewaysMovement, distanceMovement)), 360.0f);
        rs.outSpeed = get_magnitude(sidewaysMovement, distanceMovement);
        // printf("goalAngle_: %f, verticleDistance: %f, distanceMovement: %f, sidewaysMovement: %f\n", goalAngle_, verticalDistance, distanceMovement, sidewaysMovement);
    }
}

// Surge
void state_defence_surge_update(state_machine_t *fsm){
    static const char *TAG = "DefendSurgeState";
    imu_correction(&robotState);

    rs.outIsAttack = false;
    RS_SEM_LOCK;
    rs.outSwitchOk = true;
    RS_SEM_UNLOCK;

    if (rs.inGoalLength > SURGE_DISTANCE || !rs.inGoalVisible){
        LOG_ONCE(TAG, "Too far from goal, switching to defend");
        FSM_CHANGE_STATE_DEFENCE(Defend);
    } else if (!is_angle_between(rs.inBallAngle, IN_FRONT_MIN_ANGLE + 20, IN_FRONT_MAX_ANGLE - 20) || rs.inBallStrength < SURGE_STRENGTH - 30){
        LOG_ONCE(TAG, "Ball is not in capture zone, switching to defend");
        FSM_CHANGE_STATE_DEFENCE(Defend);
    }

    // LOG_ONCE(TAG, "Yeet");
    rs.outDirection = rs.inBallAngle;
    rs.outSpeed = SURGE_SPEED;
}