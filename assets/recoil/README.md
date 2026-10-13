# 本地压枪导入入口

此目录只提供固定版本的17份源CSV哈希与转换元数据，不分发来源权利链尚未闭合的社区CSV。来源身份见 [legacy_manifest.json](legacy_manifest.json)：原作者仓库为ArtanisInc/Artanis-RCS固定提交7387d399aa11fec5b26b8cd63953b4cde37f8987；本地审计来源E:/Dev/Xen固定0861455384f5b63dc3e7263aa2e9f6312c8e87db。源仓库声明MIT，但原测量者、数据许可链和游戏适用条件仍未确认；不把仓库许可声明扩展为数据权利已验证。

用户持有该组文件时，在统一程序目录运行以下命令；先将尖括号占位符替换为实际路径与明确的参考灵敏度，含空格的路径加引号：

```powershell
py -3 -X utf8 .\tools\recoil\import_recoil_profiles.py --source-directory <用户持有的CSV目录> --output-directory .\cache\recoil\profiles --reference-sensitivity <明确参考灵敏度>
```

源码工作区将脚本路径替换为 `scripts/import_recoil_profiles.py`。需要已安装的 Python 3，转换脚本仅使用标准库；也可用解释器完整路径替换 `py -3`。工具均从自身位置定位本目录的清单，支持程序路径包含中文/空格，也可从其他工作目录用脚本绝对路径启动；相对输入/输出路径仍以当前工作目录为准。输出目录里的已有版本不会覆盖；再次导入请选择新的目录。只随程序提供清单和转换工具，原CSV需用户自行提供。

转换严格校验清单SHA256。三列按增量X/Y与delay_ms消费，使用旧经验比例2.45/参考灵敏度并反转Y，累计为counts；每行时长为multiple*(delay/sleep_divider-sleep_suber)。保留全部行、不跳过首子步、不随机化，不把该周期当游戏射速。实际单位/输入响应尚未标定，转换不能双重套入Aim counts_per_pixel或DPI系数。

默认累计格式的输出始终为IMPORTED，phase_tolerance/recovery未知且校准证据为空，不能直接进入活动压枪执行；下文离散迁移格式也不自动确认或启用。导入报告记录总量、时长和旧参数差异；不覆盖已存在版本。项目软件测试只使用自制合成曲线，未将这些源候选当实机已验收数据。用户核对与启用流程见 [控制指南](../guide/controls.md#弹道配置与活动版本)。

武器内部身份与统一显示名以 `assets/weapon_catalog.inc` 为唯一目录，GSI、共享点射资料和导入器使用同表。保存的 `weapon_id` 保持现有 canonical ID，例如 `m4a1_s`；界面统一显示 `M4A1-S`。旧曲线的 `id`、文件名和来源哈希保留，旧清单 `m4a1.csv` 明确对应 `m4a1_s`，不可由裸 `m4a1` 猜测武器；GSI `weapon_m4a1` 明确对应 `m4a4`。G3SG1 可识别但没有已验证点射资料，不由名称目录补造时序。

仅 `IMPORTED` 与 `SCHEMA_VALID` 候选允许自动归一明确别名。`CALIBRATED` / `ACCEPTED` 若使用需要转换的别名则明确拒绝加载、验证和保存，须以规范 ID 建立新候选并重新校准；不能通过改名沿用旧环境指纹与语义哈希。原规范 ID 及未知自定义 ID 保持原身份，不改写历史文件。

编辑器的 schema2 累计曲线与 schema3 离散曲线保留已有人工确认：旧文件 `verified=true` 且缺少摘要时兼容读取，不要求重复确认。新保存的文件写入
`confirmation_sha256`，确认绑定版本、武器、灵敏度及节点/事件。已有摘要但内容变化时
仍可作为候选编辑，不能直接采用或沿用旧确认；需核对实际内容后重新确认。保存候选与载入候选是两步，
存在待用测试组导致载入失败时，已保存文件仍保留，当前选择与待用组不会被清除；按界面提示处理后重试。

## 已确认旧曲线的离散迁移

`import_recoil_profiles.py --discrete-legacy` 输出未确认schema3事件：按旧length裁剪、delay归一到0.1ms、multiple细分和尾步余量、首子步仅等待、绝对计划时刻。默认导入及新采集候选继续保持原累计语义；新导入不会自动确认或启用。

`migrate_legacy_recoil_profiles.py prepare` 仅为已确认且与审计源一致的17条活动 schema2 曲线生成迁移计划；与生产读取一致，旧文件 `verified=true` 且缺少摘要时直接保留已有确认，不要求补摘要或重复确认；已有摘要失配时仍需核对实际修改。原版本数据与灵敏度不改写，新离散版本沿用原灵敏度并写入摘要。计划目录须为新的独立目录，不能放入活动曲线目录。`apply` 重新校验原活动曲线、待发布文件及活动索引，保存索引备份和 `previous`、另存离散版本再切换。使用前运行以下只读帮助查看明确输入：

```powershell
py -3 -X utf8 .\tools\recoil\migrate_legacy_recoil_profiles.py --help
```

源码工作区使用 `scripts/migrate_legacy_recoil_profiles.py`。`prepare` 需要 `--source-directory`、
`--source-profiles`、`--profiles-directory` 和 `--plan-directory`；`apply` 使用后两项。
原CSV不进入发布包，其他候选、历史Run和配置不改写。普通离散弹道保持原始比例，不能用累计节点编辑或强度参数悄悄改变其执行语义；实际游戏效果仍须人工对照。
