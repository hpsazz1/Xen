"""通过实际标注生成脚本核对中心 ROI 与 C++ 非负整数除法一致。"""
import csv
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
class RoiTemplateTests(unittest.TestCase):
    def test_center_roi_all_remainders(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            video = root / 'scene.mp4'
            video.write_bytes(b'offline fixture')
            report = root / 'benchmark.csv'
            for dx in range(4):
                for dy in range(4):
                    with report.open('w', newline='', encoding='utf-8') as stream:
                        fields = ['scene','status','failed_frames','frames','source_width','source_height','evaluated_width','evaluated_height','model_input_width','model_input_height']
                        writer = csv.DictWriter(stream, fieldnames=fields)
                        writer.writeheader()
                        writer.writerow(dict(scene='scene',status='SUCCESS',failed_frames=0,frames=1,source_width=1920+dx,source_height=1080+dy,evaluated_width=640,evaluated_height=640,model_input_width=640,model_input_height=640))
                    for script, suffix in [('new_aim_ground_truth_annotations.ps1','aim'),('new_video_visibility_annotations.ps1','visibility')]:
                        output = root / f'{suffix}-{dx}-{dy}'
                        result = subprocess.run([shutil.which('pwsh') or shutil.which('powershell'), '-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'scripts'/script),'-VideoDirectory',str(root),'-BenchmarkReport',str(report),'-OutputDirectory',str(output)],capture_output=True,timeout=30)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        data = json.loads((output / f'scene.mp4.{suffix}.json').read_text(encoding='utf-8-sig'))
                        with self.subTest(script=script,dx=dx,dy=dy):
                            self.assertEqual(data['roi_x'], (1920+dx-640)//2)
                            self.assertEqual(data['roi_y'], (1080+dy-640)//2)
if __name__ == '__main__':
    unittest.main()
