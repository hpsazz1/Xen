# 日常更改的构建工作流

在设置 backend 或更改依赖项时，请使用完整构建器：

```powershell
.\BUILDER.bat
```

在 backend 已经构建完成且你只更改了应用代码时，使用无选项构建器：

```powershell
.\build_no-options.bat
```

无选项构建器只询问 DML 或 CUDA，然后先清理并完整重编译现有的应用目标。这样即使中文MSVC的头文件依赖提示未被Ninja正确识别，也不会沿用陈旧对象；它不会下载、恢复、更新或重新构建外部OpenCV。

项目构建应始终通过提供的批处理包装器进行：

```powershell
.\build_dml.bat
.\build_cuda.bat
```

相关文档：

- [从源码构建](../build.md)
- [常见方案](recipes.md)
