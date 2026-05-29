#ifndef ESP32_MOTOR_H
#define ESP32_MOTOR_H

#include "motor.h"

class Esp32Motor : public Motor {

public:
    Esp32Motor();
    ~Esp32Motor();

    virtual void MoveForward(int spd, int dur);

    virtual void MoveBackward(int spd, int dur);

    virtual void AccelerateForward(int target, int acc_t, int hold_t);

    virtual void Stop();
  
};

#endif // ESP32_MOTOR_H
