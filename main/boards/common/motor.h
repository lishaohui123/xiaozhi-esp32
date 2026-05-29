#ifndef MOTOR_H
#define MOTOR_H

#include <string>

class Motor {
public:
    virtual ~Motor() = default;  // 添加虚析构函数

    virtual void MoveForward(int spd, int dur) = 0;

    virtual void MoveBackward(int spd, int dur) = 0;

    virtual void AccelerateForward(int target, int acc_t, int hold_t) = 0;

    virtual void Stop() = 0;
};
 
#endif // MOTOR_H
