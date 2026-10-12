# 真实背景配准回归图像

源：用户本地RECOIL-WORKFLOW-001实际采集，诊断版2138c32。

这是历史回归输入；下列来源提交和测量数值描述原采集时点。当前使用者是
[recoil_wall_analysis_tests.cpp](../../recoil_wall_analysis_tests.cpp) 与
[wall_capture_run_tests.cpp](../../wall_capture_run_tests.cpp)：前者直接检查生产配准接口，后者用记录图像
和假设备验证采集、报告及释放流程。已有对应 Release 测试程序时，可从仓库根目录运行：

```powershell
ctest --test-dir build/nvidia -C Release --output-on-failure -R '^(recoil_wall_analysis_tests|wall_capture_run_tests)$'
```

其他后端按实际构建目录替换。文档刷新不代表重新采集或执行新的物理验收。

- reference-a.png / shot-a.png：debug-17898242729669805-6，frames/frame-0.png / registration-failure.png。
- reference.png / shot-b.png：debug-17898242953091647-10，同上两类文件。

图像保持采集原始320×320 BGR PNG；不是合成射击或设备动作真值。原ROI(40,40,80,80)在同坐标相位相关时response约0.37，位移仍在ROI30%内。用于生产接口配准和假设备回放，不证明真实压枪效果或实际逐发时刻。中心/右下动态内容不作为配准模板。

shot-before-a.png：第一组Run的frames/frame-3.png，用于亚像素残差回归。

reference-stage.png / shot-stage.png：debug-17898257553581723-7 的基线与失败帧，原亚像素网格残差8.0073，验证保持阈值8的细化精度。

reference-text.png / shot-text.png：debug-17898280289641842-17 的基线与真实失败帧；原始文字边缘残差8.046875。用于采样稳健残差回归，并从同图生成局部遮挡负例；不把配准通过当作真实压枪验收。

reference-range-0/1/2.png 与 shot-range-0/1/2.png：分别来自 debug-17898291654516275-6、debug-17898294046523600-2、debug-17898294276079669-4 的基线与失败帧，覆盖5发和两次3发范围拒绝。真实文字对应约Y27.27/26.35/27.65像素，仍有完整图像支撑；验证移除ROI30%默认截断，不修改可靠性判据。
