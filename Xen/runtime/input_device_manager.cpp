#include "Xen.h"

#include <iostream>
#include <utility>

#include "runtime/startup_helpers.h"

void createInputDevices()
{
    if (globalMouseThread)
        globalMouseThread->setMouseInput(nullptr);

    Config configSnapshot;
    {
        std::lock_guard<std::mutex> configLock(configMutex);
        configSnapshot = config;
    }

    auto newOwner = CreateMouseInputDevice(configSnapshot);
    IMouseInput* newInput = newOwner.get();

    GhubMouse* newGhub = newInput ? newInput->ghub() : nullptr;
    RzctlMouse* newRazer = newInput ? newInput->razer() : nullptr;
    KmboxNetConnection* newKmboxNet = newInput ? newInput->kmboxNet() : nullptr;
    KmboxAConnection* newKmboxA = newInput ? newInput->kmboxA() : nullptr;
    MakcuConnection* newMakcu = newInput ? newInput->makcu() : nullptr;

    std::string message = std::string("[鼠标] 使用 ")
        + (newInput ? newInput->name() : "未知") + " 输入。";
    if (!newInput || !newInput->isOpen())
        message += " 设备未连接；该输入方式可用前将保持禁用。";

    std::unique_ptr<IMouseInput> oldOwner;
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        oldOwner = std::move(activeMouseInputOwner);
        activeMouseInputOwner = std::move(newOwner);
        gHub = newGhub;
        razerControl = newRazer;
        kmboxNetSerial = newKmboxNet;
        kmboxASerial = newKmboxA;
        makcuSerial = newMakcu;
    }

    WriteConsoleLine(newInput && newInput->isOpen() ? ConsoleTone::Success : ConsoleTone::Warning, message);
}

void assignInputDevices()
{
    if (globalMouseThread)
        globalMouseThread->setMouseInput(activeMouseInputOwner.get());
}
