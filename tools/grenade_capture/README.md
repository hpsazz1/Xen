# OBS 道具采集内部作业

日常入口只有 OBS 插件：游戏中试投成功后，恢复原站位及正确瞄角，按 F10 自动采集、按方案短名打包并同步辅机。插件负责热键和作业生命周期；本目录为随插件安装的内部 Python 作业，不需要单独启动工具、安装语音模型或运行常驻 Python 监听器。全部依赖为 Python 标准库。

`worker.py` 只执行一次采集与发送，由插件传入辅机地址、本地资源根、目标共享目录、触发时的游戏窗口/进程和作业状态文件；结果写入状态文件后退出。它复用原有 `capture_once` / `transfer_capture` 流程，不发送游戏输入。F10 本身承载人工试投确认，不增加二次确认。

Mirage T1～T5 使用用户提供的 `standing/mirage-t-spawns.png`。其他站位图片仍位于本地资源根 `standing/<方案文件stem>.png`。成品保存在 `references/<方案stem>/`，短名 ZIP 保存在 `packages/<地图>/`；旧资源备份与中断恢复记录保持原格式。发送只更新本次资源，不覆盖原投掷方案。

状态中的 `pending` 表示本地资源存在但发送尚未确认；成功需 `state=sent`、`terminal=true` 且进程退出码为0。运行中的进度或异常进程退出不能被当成发送成功。OBS 插件负责同一时刻仅一个作业、超时结束和状态展示。

离线验证为 `tests/grenade_reference_tests.py` 与 `tests/grenade_capture_workflow_tests.py`，使用合成 PNG、本地临时目录和回环 HTTP；不采真实屏幕、不连接辅机、不触发游戏动作。
