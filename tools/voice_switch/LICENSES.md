# 语音工具第三方来源

只复用依赖 API 和中文拼音转换机制，不复制完整第三方示例程序。安装所得 wheel 保留各自 dist-info 许可证；不得从发布环境剥离这些文件。

| 组件 | 固定版本 | 许可与一手来源 |
|---|---|---|
| sherpa-onnx / sherpa-onnx-core | 1.13.8 | [Apache-2.0](https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE)；core 包含的原生依赖以 wheel 的第三方声明为准 |
| NumPy | 2.2.6 | [BSD-3-Clause 及随 wheel 的第三方声明](https://github.com/numpy/numpy/blob/v2.2.6/LICENSE.txt) |
| sounddevice | 0.5.3 | [MIT](https://github.com/spatialaudio/python-sounddevice/blob/0.5.3/LICENSE)；Windows wheel 附 PortAudio，保留其许可 |
| pypinyin | 0.55.0 | [MIT](https://github.com/mozillazg/python-pinyin/blob/v0.55.0/LICENSE.txt) |
| cffi | 2.1.1 | [MIT](https://cffi.readthedocs.io/en/latest/license.html) |
| pycparser | 3.11 | [BSD-3-Clause](https://github.com/eliben/pycparser/blob/main/LICENSE) |

中文模型：`sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01`，只安装 epoch-12-avg-2-chunk-16-left-64 的 int8 encoder/decoder/joiner、tokens.txt 和模型原始 README.md。

- [官方模型发布](https://github.com/k2-fsa/sherpa-onnx/releases/tag/kws-models)
- [作者 ModelScope 模型卡](https://modelscope.cn/models/pkufool/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01)
- [模型卡原文](https://modelscope.cn/api/v1/models/pkufool/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01/repo?Revision=master&FilePath=README.md)：明示 Apache License 2.0，模型来自 WenetSpeech L 训练；安装脚本保留官方包内同一 README，不复制训练数据。
- [官方关键词接口与示例](https://k2-fsa.github.io/sherpa/onnx/kws/index.html)

上述声明为本次固定依赖及模型的来源记录，不把引擎许可证外推至任意其他模型。
