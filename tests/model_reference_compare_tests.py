"""参考对照公开入口的离线数值回归，不加载模型或访问设备。"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import numpy as np

ROOT = Path(__file__).resolve().parents[1]

class Tensor:
    def __init__(self, values): self.values = np.asarray(values, dtype=float)
    def cpu(self): return self
    def numpy(self): return self.values
    def float(self): return self
    def __getitem__(self, item): return Tensor(self.values[item])

class ReferenceTests(unittest.TestCase):
    def run_comparison(self, task, mutation=None, value=float("nan"), threshold=None):
        spec = importlib.util.spec_from_file_location(task, ROOT / "scripts" / f"compare_{task}_reference.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        instance = dict(class_id=0, x1=1., y1=2., x2=4., y2=6., confidence=.9,
                        center_x=2., center_y=3., width=2., height=4., angle_radians=.1,
                        mask_file="mask.png", keypoints=[dict(x=2., y=3., confidence=.9)])
        summary = dict(instances=[instance], keypoints_per_detection=1, keypoint_dimensions=3)
        reference = types.SimpleNamespace(
            boxes=types.SimpleNamespace(xyxy=Tensor([[1,2,4,6]]), conf=Tensor([.9]), cls=Tensor([0])),
            keypoints=types.SimpleNamespace(data=Tensor([[[2,3,.9]]])),
            obb=types.SimpleNamespace(data=Tensor([[2,3,2,4,.1,.9,0]])),
            masks=types.SimpleNamespace(data=Tensor([[[1,1],[1,1]]])), orig_shape=(2,2))
        if mutation:
            side, field = mutation.split(":")
            if side == "actual":
                if field.startswith("keypoint_"): instance["keypoints"][0][field[9:]] = value
                else: instance[field] = value
            elif side == "reference":
                tensor = {"box":reference.boxes.xyxy, "confidence":reference.boxes.conf,
                          "class":reference.boxes.cls, "keypoint":reference.keypoints.data,
                          "obb":reference.obb.data,"mask":reference.masks.data}[field]
                tensor.values.flat[0] = value
        fake = types.ModuleType("ultralytics")
        fake.YOLO = lambda *a, **k: types.SimpleNamespace(predict=lambda *a, **k:[reference])
        ops = types.ModuleType("ultralytics.utils.ops")
        ops.scale_masks = lambda tensor, shape:tensor
        cv = types.ModuleType("cv2"); cv.IMREAD_GRAYSCALE=0; cv.imread=lambda *a:np.ones((2,2),dtype=np.uint8)
        modules = {"ultralytics":fake, "ultralytics.utils.ops":ops, "onnxruntime":types.ModuleType("onnxruntime"), "cv2":cv}
        with tempfile.TemporaryDirectory() as text:
            root=Path(text); (root/"ultralytics").mkdir(); (root/"model.onnx").touch(); (root/"image.png").touch()
            (root/"summary.json").write_text(json.dumps(summary),encoding="utf-8")
            argv=["compare", "--model",str(root/"model.onnx"),"--image",str(root/"image.png"),"--xen-result",str(root),"--ultralytics-root",str(root)]
            if threshold: argv += [threshold,str(value)]
            with patch.dict(sys.modules,modules),patch.object(sys,"argv",argv),patch.object(sys,"path",list(sys.path)),contextlib.redirect_stdout(io.StringIO()):
                return module.main()

    def test_finite_normal_results_pass(self):
        for task in ("pose","segmentation","obb"):
            with self.subTest(task=task): self.assertEqual(self.run_comparison(task),0)

    def test_nonfinite_actual_results_fail(self):
        fields={"pose":["x1","confidence","keypoint_x","keypoint_y","keypoint_confidence"],
                "segmentation":["x1","confidence"],"obb":["center_x","angle_radians","confidence"]}
        for task, names in fields.items():
            for field in names:
                for value in (float("nan"),float("inf"),float("-inf"),"NaN"):
                    with self.subTest(task=task,field=field,value=value),self.assertRaises(RuntimeError):
                        self.run_comparison(task,"actual:"+field,value)

    def test_nonfinite_reference_results_fail(self):
        fields={"pose":["box","confidence","class","keypoint"],"segmentation":["box","confidence","class","mask"],"obb":["obb"]}
        for task,names in fields.items():
            for field in names:
                for value in (float("nan"),float("inf"),float("-inf")):
                    with self.subTest(task=task,field=field,value=value),self.assertRaises(RuntimeError):
                        self.run_comparison(task,"reference:"+field,value)

    def test_nonfinite_thresholds_fail(self):
        flags={"pose":["--maximum-box-delta","--maximum-detection-confidence-delta","--maximum-keypoint-coordinate-delta","--maximum-keypoint-confidence-delta"],
               "segmentation":["--minimum-mask-iou","--maximum-box-delta","--maximum-confidence-delta"],
               "obb":["--maximum-geometry-delta","--maximum-angle-delta","--maximum-confidence-delta"]}
        for task,names in flags.items():
            for flag in names:
                for value in (float("nan"),float("inf")):
                    with self.subTest(task=task,flag=flag,value=value),self.assertRaises(RuntimeError):
                        self.run_comparison(task,value=value,threshold=flag)

if __name__ == "__main__": unittest.main()
