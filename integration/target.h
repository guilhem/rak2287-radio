#pragma once

#include <Dispatcher.h>
#include <LittleFS.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>

class MonitorBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override { return 0; }
    const char* getManufacturerName() const override { return "Linux external radio"; }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
    void reboot() override {}
};

class ExternalRadio : public mesh::Radio {
protected:
    float rssi = 0, snr = 0;
    uint32_t received = 0, rejected = 0, sent = 0;
public:
    uint32_t getEstAirtimeFor(int len) override;
    float packetScore(float snr, int len) override;
    float getLastRSSI() const override { return rssi; }
    float getLastSNR() const override { return snr; }
    uint32_t getPacketsRecv() const { return received; }
    uint32_t getPacketsSent() const { return sent; }
    uint32_t getPacketsRecvErrors() const { return rejected; }
    void reject() { ++rejected; }
    void resetStats() { received = rejected = sent = 0; }
};

class KernelRandom : public mesh::RNG {
public:
    void random(uint8_t* p, size_t n) override;
};

bool validEnvelope(const uint8_t* data, int len);
extern ExternalRadio& radio_driver;
extern MonitorBoard board;
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;
// Both external targets use a fixed EU Narrow profile.
inline void radio_set_params(float, float, uint8_t, uint8_t) {}
inline void radio_set_tx_power(int8_t) {}
