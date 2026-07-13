#ifndef XEN_H
#define XEN_H

// Xen 项目主头文件，包含所有核心模块的头文件和全局变量声明

#include "config.h"
#ifdef USE_CUDA
#include "trt_detector.h"
#else
#include "dml_detector.h"
#endif
#include "mouse.h"
#include "MouseInput.h"
#include "detection_buffer.h"
#include "KmboxNetConnection.h"
#include "KmboxAConnection.h"
#include "Makcu.h"
#include "rzctl.h"
#include <memory>
#include <mutex>

extern Config config;
// 全局检测器实例（根据编译配置选择 TensorRT 或 DirectML）
#ifdef USE_CUDA
extern TrtDetector trt_detector;
#else
extern std::unique_ptr<DirectMLDetector> dml_detector;
#endif
// 检测结果环形缓冲区
extern DetectionBuffer detectionBuffer;
// 全局鼠标线程指针
extern MouseThread* globalMouseThread;
// 各鼠标输入设备实例
extern GhubMouse* gHub;
extern RzctlMouse* razerControl;
extern KmboxNetConnection* kmboxNetSerial;
extern KmboxAConnection* kmboxASerial;
extern MakcuConnection* makcuSerial;
// 当前激活的输入设备（unique_ptr 管理生命周期）
extern std::unique_ptr<IMouseInput> activeMouseInputOwner;
// 输入设备切换标志
extern std::atomic<bool> input_method_changed;
// 自瞄状态
extern std::atomic<bool> aiming;
// 开火状态
extern std::atomic<bool> shooting;
// 缩放状态
extern std::atomic<bool> zooming;
// 配置访问互斥锁
extern std::mutex configMutex;
// 输入设备列表互斥锁
extern std::mutex inputDevicesMutex;

void createInputDevices();
void assignInputDevices();

#endif // XEN_H
