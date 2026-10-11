"""只用标准库生成有外置initializer的自有Detector身份回归模型。

wire字段依据ONNX v1.18.0公开格式；复用同目录既有最小编码函数。
"""
from pathlib import Path
import hashlib
import struct
from generate_detector_typeinfo_fixture import integer, message, value_info, node


def build_fixture():
    keepdims = message(1, "keepdims") + integer(3, 0) + integer(20, 2)
    weight = (b"".join(integer(1, dim) for dim in [1, 5, 2]) + integer(2, 1)
              + message(8, "weight") + integer(14, 1)
              + message(13, message(1, "location") + message(2, "detector_identity_weights.bin")))
    graph = (message(1, node("ReduceMean", ["images"], "mean", message(5, keepdims)))
             + message(1, node("Mul", ["mean", "weight"], "prediction"))
             + message(2, "detector_identity_external") + message(5, weight)
             + message(11, value_info("images", [1, 3, 4, 4]))
             + message(12, value_info("prediction", [1, 5, 2])))
    model = integer(1, 8) + message(2, "Xen owned identity fixture") + message(7, graph) + message(8, integer(2, 13))
    return model, struct.pack("<10f", 2, 0, 2, 0, 2, 0, 2, 0, .9, 0)


if __name__ == "__main__":
    for name, payload in zip(["detector_identity_external.onnx", "detector_identity_weights.bin"], build_fixture()):
        destination = Path(__file__).with_name(name)
        destination.write_bytes(payload)
        print(name, len(payload), hashlib.sha256(payload).hexdigest())
