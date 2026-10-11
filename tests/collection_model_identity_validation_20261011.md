# 加载模型与采集会话的字节身份

基线56af478e1b99f74286b163ac9541f9f53778d090，分支fix/collection-model-identity-20261011。
实际CPU链已复现：Runtime加载A后路径被改为合法B（仅producer_name不同），
App→Workspace接受B类别确认，Collector保存真实推理PNG并记录B的SHA，
Runtime仍是A且代次不变。两个失败断言代表一个身份缺陷，不声称此例框或权重不同。

现在Detector对自有模型字节同时计算SHA并调用ORT内存构造；加载期间只读共享，
完成释放字节和句柄。Runtime按代次存放加载SHA；Workspace传入类别确认SHA，
Runtime与Collector依次核对后才创建会话。旧模型拒绝B，明确重载B后恢复采集；
类别检查之后文件再次变化也拒绝，恢复相同字节可重试。没有每帧读盘或哈希。
独立Collector空expected保留原用法。新增一次加载期模型容器大小的内存副本及SHA
遍历，不宣称峰值或加载性能改善；实测输入模型10459617字节。

使用公开ORT外置initializer目录选项保留相对权重目录；自有274字节ONNX+40字节
权重fixture在Unicode目录真实CPU得到已知检测框，SHA与Python hashlib结果一致。
SHA明确只描述模型容器，外置权重本身身份未纳入，不是完整供应链指纹。

额外发现既有ORT边界：Unicode目录权重缺失返回0xC0000409，原路径构造同样终止，
ASCII正常，当前线程UTF8字符环境对照正常。生产只在ORT构造期间启用线程局部
UTF8 LC_CTYPE并恢复原locale和模式；没有修改系统/全局区域设置、无依赖升级。
默认字符环境、ASCII路径、原本启用线程UTF8三种用例均保留失败旧模型、恢复后
重新加载、reset清身份，检查调用线程locale未泄漏。未获得SDK内部栈，不宣称
已定位到具体内部函数。初次辅助断言编译失败和所有异常退出日志均保留。

回归还涵盖已知SHA向量、Unicode文件读取、写/重命名共享冲突、空文件失败后
资源释放、候选加载失败保留原模型及摘要，真实Runtime停止/重启/重载/异步采集
提交边界与原Detector故障恢复。固定SHA命令、JUnit、身份及远端/CI见本地
`docs/evidence/XEN-ROUND1-CLOSEOUT-20261011-001/COLLECTION_MODEL_RESULT.md`。
仅CPU、合成像素、回环UDP与禁输出适配器；不涉及真实设备、采集、原生UI或GPU。
