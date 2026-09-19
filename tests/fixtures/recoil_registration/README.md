# 真实背景配准回归图像

源：用户本地RECOIL-WORKFLOW-001实际采集，诊断版2138c32。

- reference-a.png / shot-a.png：debug-17898242729669805-6，frames/frame-0.png / registration-failure.png。
- reference.png / shot-b.png：debug-17898242953091647-10，同上两类文件。

图像保持采集原始320×320 BGR PNG；不是合成射击或设备动作真值。原ROI(40,40,80,80)在同坐标相位相关时response约0.37，位移仍在ROI30%内。用于生产接口配准和假设备回放，不证明真实压枪效果或实际逐发时刻。中心/右下动态内容不作为配准模板。

shot-before-a.png：第一组Run的frames/frame-3.png，用于亚像素残差回归。
