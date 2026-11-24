#pragma once
// Host-preview stand-in: only the power snapshot types the UI reads.
#include <cstdint>
class ChargeStatus {
public:
    enum class State : uint8_t { kNoPower = 0, kPrecharge, kConstantCurrent, kConstantVoltage, kFull, kNoBattery, kFault };
    struct Snapshot {
        State state; bool power_present; bool charging; bool full; bool no_battery;
        bool precharge; bool constant_current; bool constant_voltage; bool fault;
    };
};
struct ZectrixPowerSnapshot {
    bool battery_valid = false;
    uint16_t battery_mv = 0;
    uint8_t battery_percent = 0;
    ChargeStatus::Snapshot charge = {};
};
