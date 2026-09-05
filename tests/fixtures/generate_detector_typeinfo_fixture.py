"""重建同目录极小 ONNX fixture；仅使用 Python 标准库，不参与构建或测试运行。

格式字段来自 ONNX v1.18.0 onnx.proto（Apache-2.0）：
https://github.com/onnx/onnx/blob/v1.18.0/onnx/onnx.proto
本文件只按公开 wire format 编码所需字段，不复制 ONNX 实现或依赖私有模型。
"""

from pathlib import Path
import hashlib
import struct


def varint(value: int) -> bytes:
    encoded = bytearray()
    while value > 127:
        encoded.append((value & 127) | 128)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def integer(field: int, value: int) -> bytes:
    return varint(field << 3) + varint(value)


def message(field: int, value: bytes | str) -> bytes:
    if isinstance(value, str):
        value = value.encode("utf-8")
    return varint((field << 3) | 2) + varint(len(value)) + value


def value_info(name: str, dimensions: list[int]) -> bytes:
    shape = b"".join(message(1, integer(1, dim)) for dim in dimensions)
    tensor_type = integer(1, 1) + message(2, shape)  # FLOAT
    return message(1, name) + message(2, message(1, tensor_type))


def tensor(name: str, dimensions: list[int], values: list[float]) -> bytes:
    return (
        b"".join(integer(1, dim) for dim in dimensions)
        + integer(2, 1)  # FLOAT
        + message(8, name)
        + message(9, struct.pack(f"<{len(values)}f", *values))
    )


def node(kind: str, inputs: list[str], output: str, attributes: bytes = b"") -> bytes:
    return (
        b"".join(message(1, name) for name in inputs)
        + message(2, output)
        + message(4, kind)
        + attributes
    )


def build_fixture() -> bytes:
    # 白图经 BGR→RGB/CHW/归一化后 mean=1，黑图 mean=0。
    # 两个 Mul 让 prediction 与 prototype 都依赖真实模型输入。
    keepdims = message(1, "keepdims") + integer(3, 0) + integer(20, 2)
    nodes = [
        node("ReduceMean", ["images"], "mean", message(5, keepdims)),
        node("Mul", ["mean", "prediction_weight"], "prediction"),
        node("Mul", ["mean", "prototype_weight"], "prototype"),
    ]
    # 4 坐标 + 1 类 + 2 掩码系数，共 7 平面、2 个 anchor。
    # 白图第一框 cx=cy=w=h=2、置信度=0.9，第二框置信度=0。
    prediction = [2, 0, 2, 0, 2, 0, 2, 0, 0.9, 0, 1, 0, 0, 0]
    prototype = [10, 10, 10, 10, -10, -10, -10, -10]
    graph = (
        b"".join(message(1, value) for value in nodes)
        + message(2, "detector_typeinfo_lifetime")
        + message(5, tensor("prediction_weight", [1, 7, 2], prediction))
        + message(5, tensor("prototype_weight", [1, 2, 2, 2], prototype))
        + message(11, value_info("images", [1, 3, 4, 4]))
        + message(12, value_info("prediction", [1, 7, 2]))
        + message(12, value_info("prototype", [1, 2, 2, 2]))
    )
    return (
        integer(1, 8)  # IR 8
        + message(2, "Xen owned test fixture")
        + message(7, graph)
        + message(8, integer(2, 13))  # 默认域 opset 13
        + message(14, message(1, "task") + message(2, "segment"))
    )


if __name__ == "__main__":
    payload = build_fixture()
    destination = Path(__file__).with_name("detector_typeinfo_segmentation.onnx")
    destination.write_bytes(payload)
    print(f"{destination.name}: {len(payload)} bytes, SHA-256={hashlib.sha256(payload).hexdigest()}")
