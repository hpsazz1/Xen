# 本地压枪导入入口

此目录只提供固定版本的17份源CSV哈希与转换元数据，不分发来源权利链尚未闭合的社区CSV。原作者仓库为ArtanisInc/Artanis-RCS固定提交7387d399aa11fec5b26b8cd63953b4cde37f8987；本地审计来源E:/Dev/Xen固定0861455384f5b63dc3e7263aa2e9f6312c8e87db。源仓库声明MIT，但原测量者、数据许可链和游戏适用条件仍未确认；不把仓库许可声明扩展为数据权利已验证。

用户持有该组文件时可运行：

```powershell
python scripts/import_recoil_profiles.py --source-directory E:/Dev/Xen/Xen/recoil/patterns --output-directory <新的本地候选目录> --reference-sensitivity <明确参考灵敏度>
```

转换严格校验清单SHA256。三列按增量X/Y与delay_ms消费，使用旧经验比例2.45/参考灵敏度并反转Y，累计为counts；每行时长为multiple*(delay/sleep_divider-sleep_suber)。保留全部行、不跳过首子步、不随机化，不把该周期当游戏射速。实际单位/输入响应尚未标定，转换不能双重套入Aim counts_per_pixel或DPI系数。

输出始终为IMPORTED，phase_tolerance/recovery未知且校准证据为空，不能进入活动压枪执行。导入报告记录总量、时长和旧参数差异；不覆盖已存在版本。项目软件测试只使用自制合成曲线，未将这些源候选当实机已验收数据。
