#ifndef __POKEYDEVICE_H
#define __POKEYDEVICE_H

#include "PoKeysLib.h"
#include "common/simhubdeviceplugin.h"
#include <assert.h>
#include <iostream>
#include <map>
#include <stdlib.h>
#include <thread>
#include <unistd.h>
#include <uv.h>

#define DEBUG 1

#ifdef DEBUG
#define eprintf(...) printf(__VA_ARGS__)
#endif

class PokeyDevice
{
private:
    static void DigitalIOTimerCallback(uv_timer_t *timer, int status);

protected:
    uint8_t _index;
    std::string _serialNumber;
    uint8_t _userId;
    uint8_t _firwareVersionMajorMajor;
    uint8_t _firwareVersionMajor;
    uint8_t _firwareVersionMinor;
    uint8_t _ipAddress[4];
    uint8_t _hardwareType;
    uint8_t _dhcp;
    std::map<std::string, int> _pinMap;
    sPoKeysDevice *_pokey;

    std::shared_ptr<std::thread> _pollThread;
    uv_loop_t *_pollLoop;
    uv_timer_t _pollTimer;

    int pinFromName(std::string targetName);
    void pollCallback(uv_timer_t *timer, int status);

public:
    PokeyDevice(sPoKeysNetworkDeviceSummary, uint8_t);
    virtual ~PokeyDevice(void);

    bool validatePinCapability(int, std::string);
    void mapNameToPin(std::string name, int pin);
    uint32_t targetValue(std::string targetName, bool value);
    uint32_t inputPin(uint8_t pin);
    uint32_t outputPin(uint8_t pin);
    int32_t name(std::string name);

    std::string serialNumber() { return _serialNumber; };
    uint8_t userId() { return _userId; };
    uint8_t firmwareMajorMajorVersion() { return _firwareVersionMajorMajor; };
    uint8_t firmwareMajorVersion() { return _firwareVersionMajor; };
    uint8_t firmwareMinorVersion() { return _firwareVersionMinor; };
    uint8_t hardwareType() { return _hardwareType; }
    std::string hardwareTypeString();
    uint8_t dhcp() { return _dhcp; }
    uint8_t index() { return _index; }
    uint8_t *ipAddress() { return _ipAddress; }
    sPoKeysDevice *pokey() { return _pokey; }
    sPoKeysDevice_Info info() { return _pokey->info; }
    sPoKeysDevice_Data deviceData() { return _pokey->DeviceData; }
    uint8_t loadPinConfiguration() { return PK_PinConfigurationGet(_pokey); }
    bool isPinDigitalOutput(uint8_t pin);
    bool isPinDigitalInput(uint8_t pin);
    std::string name()
    {
        std::string tmp((char *)deviceData().DeviceName);
        return tmp;
    }
};

#endif