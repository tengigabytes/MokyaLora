#pragma once

// Aggregate header — pulls in all power-bus sub-module headers
#include "bringup_lm27965.h"
#include "bringup_bq25622.h"
#include "bringup_bq27441.h"

// Bus B lifecycle (shared i2c1, GPIO 6/7)
void bus_b_init(void);
void bus_b_deinit(void);

// Bus B diagnostics
void dump_bus_b(void);
void scan_bus_b(void);

// Vibration motor (PWM on MTR_PWM_PIN)
void motor_breathe(void);
