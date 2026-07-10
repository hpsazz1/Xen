#ifndef PROVIDER_BENCHMARK_H
#define PROVIDER_BENCHMARK_H

// 推理后端性能基准测试命名空间
namespace benchmarks
{
// 检查命令行参数是否包含基准测试请求
bool IsProviderBenchmarkRequested(int argc, char** argv);
// 运行命令行界面的推理后端基准测试
int RunProviderBenchmarkCli(int argc, char** argv);
}

#endif // PROVIDER_BENCHMARK_H
