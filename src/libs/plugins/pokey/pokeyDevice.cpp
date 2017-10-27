#include <chrono>
#include <string.h>
#include <thread>

#include "elements/attributes/attribute.h"
#include "pokeyDevice.h"

using namespace std::chrono_literals;

std::mutex PokeyDevice::_BigPokeyLock;

PokeyDevice::PokeyDevice(sPoKeysNetworkDeviceSummary deviceSummary, uint8_t index)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    _callbackArg = NULL;
    _enqueueCallback = NULL;

    _pokey = PK_ConnectToNetworkDevice(&deviceSummary);

    if (!_pokey) {
        throw std::exception();
    }

    _index = index;
    _userId = deviceSummary.UserID;
    _serialNumber = std::to_string(deviceSummary.SerialNumber);
    _firwareVersionMajorMajor = (deviceSummary.FirmwareVersionMajor >> 4) + 1;
    _firwareVersionMajor = deviceSummary.FirmwareVersionMajor & 0x0F;
    _firwareVersionMinor = deviceSummary.FirmwareVersionMinor;
    memcpy(&_ipAddress, &deviceSummary.IPaddress, 4);
    _hardwareType = deviceSummary.HWtype;
    _dhcp = deviceSummary.DHCP;

    _intToDisplayRow[0] = 0b11111100;
    _intToDisplayRow[1] = 0b01100000;
    _intToDisplayRow[2] = 0b11011010;
    _intToDisplayRow[3] = 0b11110010;
    _intToDisplayRow[4] = 0b01100110;
    _intToDisplayRow[5] = 0b10110110;
    _intToDisplayRow[6] = 0b10111110;
    _intToDisplayRow[7] = 0b11100000;
    _intToDisplayRow[8] = 0b11111110;
    _intToDisplayRow[9] = 0b11100110;

    loadPinConfiguration();
    _pollTimer.data = this;
    _pollLoop = uv_loop_new();
    uv_timer_init(_pollLoop, &_pollTimer);

    int ret = uv_timer_start(&_pollTimer, (uv_timer_cb)&PokeyDevice::DigitalIOTimerCallback, DEVICE_START_DELAY, DEVICE_READ_INTERVAL);

    if (ret == 0) {
        _pollThread.reset(new std::thread([=] { uv_run(_pollLoop, UV_RUN_DEFAULT); }));
    }
}

void PokeyDevice::setCallbackInfo(EnqueueEventHandler enqueueCallback, void *callbackArg, SPHANDLE pluginInstance)
{
    _enqueueCallback = enqueueCallback;
    _callbackArg = callbackArg;
    _pluginInstance = pluginInstance;
}

void PokeyDevice::DigitalIOTimerCallback(uv_timer_t *timer, int status)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    PokeyDevice *self = static_cast<PokeyDevice *>(timer->data);

    int encoderRetValue = PK_EncoderValuesGet(self->_pokey);
    if (encoderRetValue == PK_OK) {
        GenericTLV el;
        for (int i = 0; i < self->_encoderMap.size(); i++) {

            uint32_t step = self->_encoders[i].step;
            uint32_t newEncoderValue = self->_pokey->Encoders[i].encoderValue;
            uint32_t previousEncoderValue = self->_encoders[i].previousEncoderValue;

            uint32_t currentValue = self->_encoders[i].value;
            uint32_t min = self->_encoders[i].min;
            uint32_t max = self->_encoders[i].max;

            if (previousEncoderValue != newEncoderValue) {

                if (newEncoderValue < previousEncoderValue) {
                    // values are decreasing
                    if (currentValue <= min) {
                        self->_encoders[i].previousValue = min;
                        self->_encoders[i].value = min;
                    }
                    else {
                        self->_encoders[i].value = currentValue - step;
                    }
                }
                else {
                    // values are increasing
                    if (currentValue >= max) {
                        self->_encoders[i].previousValue = max;
                        self->_encoders[i].value = max;
                    }
                    else {
                        self->_encoders[i].value = currentValue + step;
                    }
                }

                el.ownerPlugin = self->_pluginInstance;
                el.type = CONFIG_INT;
                el.value.int_value = (int)self->_encoders[i].value;
                el.length = sizeof(uint32_t);
                el.name = (char *)self->_encoders[i].name.c_str();
                el.description = (char *)self->_encoders[i].description.c_str();
                el.units = (char *)self->_encoders[i].units.c_str();

                // enqueue the element
                self->_enqueueCallback(self, (void *)&el, self->_callbackArg);
                // set previous to equal new
                self->_encoders[i].previousEncoderValue = newEncoderValue;
            }
        }
    }

    int ret = PK_DigitalIOGet(self->_pokey);
    if (ret == PK_OK) {
        GenericTLV el;

        for (int i = 0; i < self->_pokey->info.iPinCount; i++) {

            if (self->_pins[i].type == "DIGITAL_INPUT") {
                if (self->_pins[i].value != self->_pokey->Pins[i].DigitalValueGet) {
                    // data has changed so send it ofr for processing
                    self->_pins[i].previousValue = self->_pins[i].value;
                    self->_pins[i].value = self->_pokey->Pins[i].DigitalValueGet;
                    el.ownerPlugin = self->_pluginInstance;
                    el.type = CONFIG_BOOL;
                    el.value.bool_value = self->_pins[i].value;
                    el.length = sizeof(uint8_t);
                    el.name = (char *)self->_pins[i].pinName.c_str();
                    el.description = (char *)self->_pins[i].description.c_str();
                    el.units = (char *)self->_encoders[i].units.c_str();
                    self->_enqueueCallback(self, (void *)&el, self->_callbackArg);
                }
            }
        }
    }
}

void PokeyDevice::addPin(std::string pinName, int pinNumber, std::string pinType, int defaultValue, std::string description)
{
    if (pinType == "DIGITAL_OUTPUT") {
        outputPin(pinNumber);
    }
    else if (pinType == "DIGITAL_INPUT") {
        inputPin(pinNumber);
    }

    mapNameToPin(pinName.c_str(), pinNumber);

    int portNumber = pinNumber - 1;

    _pins[portNumber].pinName = pinName;
    _pins[portNumber].type = pinType.c_str();
    _pins[portNumber].pinNumber = pinNumber;
    _pins[portNumber].defaultValue = defaultValue;
    _pins[portNumber].value = defaultValue;
    _pins[portNumber].description = description;
}

void PokeyDevice::addPWM(
    uint8_t channel, 
    std::string name, 
    std::string description, 
    std::string units, 
    uint32_t leftDutyCycle, 
    uint32_t rightDutyCycle, 
    uint32_t period)
{
    _pwmChannels[channel] = true;

	mapNameToPWM(name.c_str(), channel);

    _pwm[channel].name = name;
    _pwm[channel].description = description;
    _pwm[channel].units = units;
    _pwm[channel].leftDutyCycle = leftDutyCycle;
    _pwm[channel].rightDutyCycle = rightDutyCycle;
    _pwm[channel].period = period;

    PK_PWMConfigurationGet(_pokey);
    _pokey->PWM.PWMperiod = _pwm[channel].period;
    _pokey->PWM.PWMenabledChannels[channel] = true;
    int ret = PK_PWMConfigurationSet(_pokey);
}

void PokeyDevice::startPolling()
{
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}

void PokeyDevice::stopPolling()
{
    assert(_pollLoop);
    uv_stop(_pollLoop);

    if (_pollThread->joinable())
        _pollThread->join();
}

/**
 *   @brief  Default  destructor for PokeyDevice
 *
 *   @return nothing
 */
PokeyDevice::~PokeyDevice()
{
    stopPolling();

    if (_pollThread) {
        if (_pollThread->joinable()) {
            _pollThread->join();
        }
    }

    PK_DisconnectDevice(_pokey);
}

std::string PokeyDevice::hardwareTypeString()
{
    if (_hardwareType == 31) {
        return "Pokey 57E";
    }

    return "Unknown";
}

bool PokeyDevice::validatePinCapability(int pin, std::string type)
{
    assert(_pokey);
    bool retVal = false;

    if (type == "DIGITAL_OUTPUT") {
        retVal = isPinDigitalOutput(pin - 1);
    }
    else if (type == "DIGITAL_INPUT") {
        retVal = isPinDigitalInput(pin - 1);
    }
    return retVal;
}

bool PokeyDevice::validateEncoder(int encoderNumber)
{
    assert(_pokey);
    bool retVal = false;

    if (encoderNumber == ENCODER_1) {
        //! TODO: Check pins 1 and 2 are not allocated already
        retVal = isEncoderCapable(1) && isEncoderCapable(2);
    }
    else if (encoderNumber == ENCODER_2) {
        //! TODO: Check pins 5 and 6 are not allocated already
        retVal = isEncoderCapable(5) && isEncoderCapable(6);
    }
    else if (encoderNumber == ENCODER_3) {
        //! TODO: Check pins 15 and 16 are not allocated already
        retVal = isEncoderCapable(15) && isEncoderCapable(16);
    }

    return retVal;
}

bool PokeyDevice::isEncoderCapable(int pin)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    switch (pin) {
    case 1:
        return (bool)PK_CheckPinCapability(_pokey, 0, PK_AllPinCap_fastEncoder1A);
    case 2:
        return (bool)PK_CheckPinCapability(_pokey, 1, PK_AllPinCap_fastEncoder1B);
    case 5:
        return (bool)PK_CheckPinCapability(_pokey, 4, PK_AllPinCap_fastEncoder2A);
    case 6:
        return (bool)PK_CheckPinCapability(_pokey, 5, PK_AllPinCap_fastEncoder2B);
    case 15:
        return (bool)PK_CheckPinCapability(_pokey, 14, PK_AllPinCap_fastEncoder3A);
    case 16:
        return (bool)PK_CheckPinCapability(_pokey, 14, PK_AllPinCap_fastEncoder3B);
    default:
        return false;
    }

    return false;
}

void PokeyDevice::addEncoder(
    int encoderNumber, 
    uint32_t defaultValue, 
    std::string name, 
    std::string description, 
    int min, 
    int max, 
    int step, 
    int invertDirection, 
    std::string units)
{
    assert(encoderNumber >= 1);

    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    PK_EncoderConfigurationGet(_pokey);
    int encoderIndex = encoderNumber - 1;

    _pokey->Encoders[encoderIndex].encoderValue = defaultValue;
    _pokey->Encoders[encoderIndex].encoderOptions = 0b11;

    if (encoderNumber == 1) {
        if (invertDirection) {
            _pokey->Encoders[encoderIndex].channelApin = 1;
            _pokey->Encoders[encoderIndex].channelBpin = 0;
        }
        else {
            _pokey->Encoders[encoderIndex].channelApin = 1;
            _pokey->Encoders[encoderIndex].channelBpin = 0;
        }
    }
    else if (encoderNumber == 2) {
        if (invertDirection) {
            _pokey->Encoders[encoderIndex].channelApin = 5;
            _pokey->Encoders[encoderIndex].channelBpin = 4;
        }
        else {
            _pokey->Encoders[encoderIndex].channelApin = 4;
            _pokey->Encoders[encoderIndex].channelBpin = 5;
        }
    }
    else if (encoderNumber == 3) {
        if (invertDirection) {
            _pokey->Encoders[encoderIndex].channelApin = 15;
            _pokey->Encoders[encoderIndex].channelBpin = 14;
        }
        else {
            _pokey->Encoders[encoderIndex].channelApin = 14;
            _pokey->Encoders[encoderIndex].channelBpin = 15;
        }
    }

    _encoders[encoderIndex].name = name;
    _encoders[encoderIndex].number = encoderNumber;
    _encoders[encoderIndex].defaultValue = defaultValue;
    _encoders[encoderIndex].value = defaultValue;
    _encoders[encoderIndex].previousValue = defaultValue;
    _encoders[encoderIndex].previousEncoderValue = defaultValue;
    _encoders[encoderIndex].min = min;
    _encoders[encoderIndex].max = max;
    _encoders[encoderIndex].step = step;
    _encoders[encoderIndex].units = units;
    _encoders[encoderIndex].description = description;

    int val = PK_EncoderConfigurationSet(_pokey);
    if (val == PK_OK) {
        PK_EncoderValuesSet(_pokey);
        mapNameToEncoder(name.c_str(), encoderNumber);
    }
    else {
        // throw exception
    }
}

void PokeyDevice::addMatrixLED(int id, std::string name, std::string type)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    PK_MatrixLEDConfigurationGet(_pokey);
    _matrixLED[id].name = name;
    _matrixLED[id].type = type;

    mapNameToMatrixLED(name, id);
}

void PokeyDevice::addGroupToMatrixLED(int id, int displayId, std::string name, int digits, int position)
{
    _matrixLED[displayId].group[position].name = name;
    _matrixLED[displayId].group[position].position = position;
    _matrixLED[displayId].group[position].length = digits;
    _matrixLED[displayId].group[position].value = 0;
}

void PokeyDevice::configMatrixLED(int id, int rows, int cols, int enabled)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    _pokey->MatrixLED[id].rows = rows;
    _pokey->MatrixLED[id].columns = cols;
    _pokey->MatrixLED[id].displayEnabled = enabled;
    _pokey->MatrixLED[id].RefreshFlag = 1;
    _pokey->MatrixLED[id].data[0] = 0;
    _pokey->MatrixLED[id].data[1] = 0;
    _pokey->MatrixLED[id].data[2] = 0;
    _pokey->MatrixLED[id].data[3] = 0;
    _pokey->MatrixLED[id].data[4] = 0;
    _pokey->MatrixLED[id].data[5] = 0;
    _pokey->MatrixLED[id].data[6] = 0;
    _pokey->MatrixLED[id].data[7] = 0;

    int32_t ret = PK_MatrixLEDConfigurationSet(_pokey);
    PK_MatrixLEDUpdate(_pokey);
}

uint32_t PokeyDevice::targetValue(std::string targetName, int value)
{
    uint8_t displayNum = displayFromName(targetName);
    displayNumber(displayNum, targetName, value);
    return 0;
}

using namespace std::chrono_literals;

uint32_t PokeyDevice::targetValue(std::string targetName, float value)
{
    uint8_t channel = PWMFromName(targetName);

    // value is percent, so calculate cycles back from left cycle count

    uint32_t duty = _pwm[channel].leftDutyCycle - ((_pwm[channel].leftDutyCycle - _pwm[channel].rightDutyCycle) * value);
    PK_SL_PWM_SetDuty(_pokey, channel, duty);
    PK_PWMUpdate(_pokey);
    std::this_thread::sleep_for(750ms);
    PK_SL_PWM_SetDuty(_pokey, channel, 0);
    PK_PWMUpdate(_pokey);

    return 0;
}

uint32_t PokeyDevice::targetValue(std::string targetName, bool value)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    uint32_t retValue = -1;
    uint8_t pin = pinFromName(targetName) - 1;

    if (pin >= 0) {
        retValue = PK_DigitalIOSetSingle(_pokey, pin, value);
    }

    if (retValue == PK_ERR_TRANSFER) {
        printf("----> PK_ERR_TRANSFER pin %d --> %d %d\n\n", pin, (uint8_t)value, retValue);
    }
    else if (retValue == PK_ERR_GENERIC) {
        printf("----> PK_ERR_GENERIC pin %d --> %d %d\n\n", pin, (uint8_t)value, retValue);
    }
    else if (retValue == PK_ERR_PARAMETER) {
        printf("----> PK_ERR_PARAMETER pin %d --> %d %d\n\n", pin, (uint8_t)value, retValue);
    }

    return retValue;
}

uint8_t PokeyDevice::displayNumber(uint8_t displayNumber, std::string targetName, int value)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);

    int groupIndex = 0;

    for (int i = 0; i < MAX_MATRIX_LED_GROUPS; i++) {
        if (_matrixLED[displayNumber].group[i].name == targetName) {
            groupIndex = i;
        }
    }

    // we should only display +ve values
    if (value < -1) {
        value = value * -1;
    }

    std::string charString = std::to_string(value);
    int numberOfChars = charString.length();
    int groupLength = _matrixLED[displayNumber].group[groupIndex].length;

    if (value == 0) {
        int position = _matrixLED[displayNumber].group[groupIndex].position;
        for (int i = position; i < (groupLength + position); i++) {
            _pokey->MatrixLED[displayNumber].data[i] = 0b00000000;
        }
        _pokey->MatrixLED[displayNumber].data[(position + groupLength) - 1] = _intToDisplayRow[0];
    }

    if (numberOfChars <= groupLength) {
        for (int i = 0; i < numberOfChars; i++) {
            int displayOffset = (int)charString.at(i) - 48;
            int convertedValue = _intToDisplayRow[displayOffset];
            int position = groupIndex + i;

            if (value > 0) {
                _matrixLED[displayNumber].group[groupIndex].value = convertedValue;
                _pokey->MatrixLED[displayNumber].data[position] = convertedValue;
            }
            else if (value == -1) {
                for (int i = groupIndex; i < groupLength + groupIndex; i++) {
                    _pokey->MatrixLED[displayNumber].data[i] = 0b00000000;
                }
            }
        }
    }

    _pokey->MatrixLED[displayNumber].RefreshFlag = 1;

    if (PK_MatrixLEDUpdate(_pokey) != PK_OK) {
        printf("---> could not update Maxtix LED \n");
        return -1;
    }

    return 0;
}

uint32_t PokeyDevice::outputPin(uint8_t pin)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);
    _pokey->Pins[--pin].PinFunction = PK_PinCap_digitalOutput | PK_PinCap_invertPin;
    return PK_PinConfigurationSet(_pokey);
}

uint32_t PokeyDevice::inputPin(uint8_t pin)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);
    _pokey->Pins[--pin].PinFunction = PK_PinCap_digitalInput;
    return PK_PinConfigurationSet(_pokey);
}

int32_t PokeyDevice::name(std::string name)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);
    strncpy((char *)_pokey->DeviceData.DeviceName, name.c_str(), 30);
    return PK_DeviceNameSet(_pokey);
}

uint8_t PokeyDevice::displayFromName(std::string targetName)
{
    std::map<std::string, int>::iterator it;
    it = _displayMap.find(targetName);

    if (it != _displayMap.end()) {
        return it->second;
    }
    else {
        printf("---> cant find display\n");
        return -1;
    }
}

int PokeyDevice::pinFromName(std::string targetName)
{
    std::map<std::string, int>::iterator it;
    it = _pinMap.find(targetName);

    if (it != _pinMap.end()) {
        return it->second;
    }
    else
        return -1;
}

uint8_t PokeyDevice::PWMFromName(std::string targetName)
{
    std::map<std::string, int>::iterator it = _pwmMap.find(targetName);

    if (it != _pwmMap.end()) {
        return it->second;
    }
    else
        return -1;
}

void PokeyDevice::mapNameToPin(std::string name, int pin)
{
    _pinMap.emplace(name, pin);
}

void PokeyDevice::mapNameToPWM(std::string name, int pin)
{
    _pwmMap.emplace(name, pin);
}

void PokeyDevice::mapNameToEncoder(std::string name, int encoderNumber)
{
    _encoderMap.emplace(name, encoderNumber);
}

void PokeyDevice::mapNameToMatrixLED(std::string name, int id)
{
    _displayMap.emplace(name, id);
}

bool PokeyDevice::isPinDigitalOutput(uint8_t pin)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);
    return (bool)PK_CheckPinCapability(_pokey, pin, PK_AllPinCap_digitalOutput);
}

bool PokeyDevice::isPinDigitalInput(uint8_t pin)
{
    std::lock_guard<std::mutex> pokeyLock(_BigPokeyLock);
    return (bool)PK_CheckPinCapability(_pokey, pin, PK_AllPinCap_digitalInput);
}
