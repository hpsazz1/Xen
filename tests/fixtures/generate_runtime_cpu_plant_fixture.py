"""生成像素驱动检测框的自有CPU链夹具；沿用ONNX公开wire编码，无模型下载。"""
from pathlib import Path
import hashlib
from generate_detector_typeinfo_fixture import integer, message, value_info, tensor, node


def build_fixture():
    keepdims = message(1, "keepdims") + integer(3, 0) + integer(20, 2)
    nodes = [node("ReduceMean", ["images"], "mean", message(5, keepdims)),
             node("Mul", ["mean", "weight"], "offset"),
             node("Add", ["offset", "bias"], "prediction")]
    graph = (b"".join(message(1, item) for item in nodes)
             + message(2, "runtime_cpu_plant")
             + message(5, tensor("weight", [1, 5, 2], [24, 0, 0, 0, 0, 0, 0, 0, 0, 0]))
             + message(5, tensor("bias", [1, 5, 2], [4, 0, 18.1, 0, 8, 0, 14, 0, .95, 0]))
             + message(11, value_info("images", [1, 3, 32, 32]))
             + message(12, value_info("prediction", [1, 5, 2])))
    return integer(1, 8) + message(2, "Xen owned CPU plant fixture") + message(7, graph) + message(8, integer(2, 13))


if __name__ == "__main__":
    data = build_fixture()
    Path(__file__).with_name("runtime_cpu_plant.onnx").write_bytes(data)
    print(len(data), hashlib.sha256(data).hexdigest())
