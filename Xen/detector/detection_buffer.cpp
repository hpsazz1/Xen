#include "detection_buffer.h"

// 全局检测结果缓冲区实例，用于存储 AI 模型推理后的目标检测结果
// 供渲染线程、鼠标控制线程等跨线程安全读取
DetectionBuffer detectionBuffer;