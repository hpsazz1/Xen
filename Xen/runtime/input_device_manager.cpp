#include "Xen.h"

#include <iostream>
#include <utility>

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

    std::string message = std::string("[Mouse] Using ")
        + (newInput ? newInput->name() : "unknown") + " input.";
    if (!newInput || !newInput->isOpen())
        message += " Device not connected; input disabled until the method becomes available.";

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

    std::cout << message << std::endl;
}

void assignInputDevices()
{
    if (globalMouseThread)
        globalMouseThread->setMouseInput(activeMouseInputOwner.get());
}

