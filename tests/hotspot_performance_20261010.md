# 2026-10-10 有界热点性能验证

本轮只优化有实测收益且精确等价的两个模块；运动账本保留原实现。全部数据来自确定性合成 CPU 夹具，不能外推为真实游戏、设备、Provider 或端到端性能。前轮审计见[固定验证快照](https://github.com/hpsazz1/Xen/blob/0c26580edd869c0343e03a7d26655c363a74205b/tests/audit_validation_20261010.md)。

环境：Windows、AMD Ryzen7 9800X3D（8核16线程）、约31GiB内存、Python3.12.10、VS2026/MSVC19.51 Release。主要基准串行运行；不保证系统没有其他负载。完整分位数、分配、输入及源码/EXE/原始结果摘要见[精简原始数据](hotspot_performance_20261010.json)。

## 精确近重复检查

RGB16×16，平均绝对差≤2的原阈值不变；仅在非负整数累计值严格超过门槛后早退。外层仍为 O(n²)，比较顺序、SHA优先和错误文案不变；非原生容器/整数保留原式回退。没有近似索引或漏检容忍。

| 分布（1200样本） | 原oracle P50秒 | 最终实现 P50秒 | 加速 |
|---|---:|---:|---:|
| random | 11.730795 | 0.325547 | 36.03× |
| low-contrast | 11.403966 | 2.211752 | 5.16× |
| late-difference | 10.454235 | 4.016369 | 2.60× |

每组244800次跨分组比较，原oracle188006400次通道比较；预热1次、计时3次、失败0。最终源码复测仅current，复用冻结原oracle基线；按相同生成器/seed重建fixture摘要核对顺序、分组、身份、RGB。原oracle函数和生成器未改变，旧计时执行源码身份单独保留，未冒充最终源码。120样本另有11次复测，P50加速11.54/3.91/2.21倍；独立tracemalloc峰值2320→10992字节，额外8672字节为O(n)结构合法性缓存，无缩略图副本。

回归：32项pipeline测试、审核与集成消费者；含1535/1536/1537阈值、随机原oracle差分、SHA/近重复错误先后及自定义类型兼容。

## 相位窗口扫描

超过64点的已验证曲线建一次极值树，闭窗口用lower_bound/upper_bound精确定位。保留插值、浮点累计、相等值优先顺序（含正负零）、报告及轨迹身份；小曲线不分配树。

| 点数 | 原实现P50 ms | 优化P50 ms | 原/后累计new字节 |
|---:|---:|---:|---:|
| 3 | 0.0117 | 0.0123 | 8968 / 8968 |
| 1000 | 1.4336 | 0.6066 | 133825 / 199400 |
| 10000 | 123.5314 | 7.9463 | 1363315 / 2411930 |
| 30000 | 3975.9361 | 23.2489 | 4399000 / 6496191 |

每组5个trial、每trial10个ACK，预热2次/计时11次，失败0；30000点P50约提升171.0倍。小曲线略慢约0.0006ms，不宣称所有规模提升。30000点增加一次约2MiB树分配；100000点上界约8MiB。统计只含普通C++new/new[]，不含OpenCV malloc/aligned。计时包含完整optimize和索引构建，未用局部查询时间冒充总收益。

首轮探索未留baseline EXE摘要，未将其当正式二进制身份。正式结果补用审计终点的原实现重编译测量，前后EXE哈希均已绑定；随后精确恢复优化源码并重建应用/CLI/测试。四组完整报告逐字段、类型、顺序及double位对照原实现通过；窗口测试含64/65切换、闭端点、nextafter、空窗口、非单调极值、正负零、NaN及重复时点拒绝。

## 运动账本：保留原实现

| 事件数 | 生产mean μs | reserve原型mean μs | 生产分配字节/次 |
|---:|---:|---:|---:|
| 0 | 0.0286 | 0.0622 | 0 |
| 64 | 0.0675 | 0.0869 | 1536 |
| 256 | 0.1588 | 0.4019 | 6183 |
| 1024 | 0.5583 | 0.6414 | 24615 |
| 4096 | 2.1442 | 2.0952 | 98343 |

三轮，每场景每轮20000次、预热1000次；满4096历史复制98304字节，生产P99各轮2.1–3.1μs、max42.2μs。reserve原型仅省约0.049μs（2.3%），小历史与空历史更慢，空快照从无分配变为每次约98KiB分配，因此不采用。MSVC大分配统计包含39字节对齐开销。分位数只能报告各轮范围，未伪造合并分布；微秒计时有量化限制。

实际派发路径还持有外层arbiter guard，非派发路径可能并发；本轮没有测实际锁等待或整条控制链。copy-only是预分配理想下界，不能视为可直接采用的生产方案。

## 可复跑入口

速度阈值不进入CTest。固定seed合成benchmark显式运行；输出目标已存在时拒覆盖。Python与tuner JSON先关闭暂存再原子新目标发布，前后源码/EXE身份记录与结果摘要保留。

```powershell
python -B -X utf8 tests/model_data_leakage_benchmark.py --sizes 1200 --repeats 3 --warmup 1 --skip-memory --output <新JSON>
cmake --build <build> --config Release --target recoil_tuner_phase_benchmark motion_ledger_benchmark
<build>/Release/recoil_tuner_phase_benchmark.exe <新JSON>
python -B -X utf8 tests/recoil_tuner_phase_compare.py <原实现JSON> <优化JSON>
python -B -X utf8 tests/recoil_tuner_phase_compare.py --self-test
<build>/Release/motion_ledger_benchmark.exe 20000 1000
```

调参基线使用同一benchmark源码链接审计终点2d66fd4的原tuner实现，优化结果链接当前实现；完整报告比较不只检查success或汇总。账本工具输出CSV到stdout，采集者须在成功结束后原子发布。

## 依据与未执行范围

采用Python[原生整数精确算术](https://docs.python.org/3.12/library/stdtypes.html#numeric-types-int-float-complex)及非负累计下界；极值索引参考[segment tree monoid](https://atcoder.github.io/ac-library/production/document_en/segtree.html)机制，按[C++ min/max首值规则](https://eel.is/c++draft/alg.min.max)和[lower_bound](https://eel.is/c++draft/lower.bound)/[upper_bound](https://eel.is/c++draft/upper.bound)保持顺序与闭界。本实现独立编写，未复制外部库；参考AC Library许可CC0。没有把L1缩略图改成Hamming索引。

应用、调参工具和相关Release目标构建通过；正式5项CTest通过，补测恢复后2项调参回归再次通过，新增逐位比较器自检CTest通过。未执行全量CTest、多Provider矩阵、真实设备/游戏、部署或合并。少量repeat的P95/P99是观察样本统计，不能估计生产尾延迟。后续仅在有真实输入指标、稳定热点和等价oracle时另立优化，不继续无依据泛化重写。

优化独立提交：近重复 `5737af2`，相位索引 `af5801c`；本报告所列最终源码哈希对应这两项提交内容。
