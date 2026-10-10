"""精确跨集近重复检查的确定性离线基准；不代表真实采集或Provider性能。"""
from __future__ import annotations
import argparse
from collections import Counter
import gc
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import platform
import sys
import statistics
import time
import tracemalloc

ROOT = Path(__file__).resolve().parents[1]
PIPELINE_PATH = ROOT / "scripts" / "model_data_pipeline.py"
BENCHMARK_PATH = Path(__file__).resolve()
# 在加载被测实现前冻结磁盘身份；成功发布前再次核对。
PIPELINE_SHA256 = hashlib.sha256(PIPELINE_PATH.read_bytes()).hexdigest()
BENCHMARK_SHA256 = hashlib.sha256(BENCHMARK_PATH.read_bytes()).hexdigest()
PUBLICATION_SPEC = importlib.util.spec_from_file_location(
    "leakage_evidence_publication", ROOT / "scripts" / "evidence_publication.py")
PUBLICATION = importlib.util.module_from_spec(PUBLICATION_SPEC)
PUBLICATION_SPEC.loader.exec_module(PUBLICATION)
SPEC = importlib.util.spec_from_file_location("leakage_pipeline", ROOT / "scripts" / "model_data_pipeline.py")
PIPELINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PIPELINE)


def original_check_leakage(entries):
    """优化前生产实现的独立oracle，保留循环和中文错误顺序。"""
    for index, left in enumerate(entries):
        for right in entries[index + 1:]:
            if left["split"] == right["split"]:
                continue
            if left["sample"]["image_sha256"] == right["sample"]["image_sha256"]:
                raise PIPELINE.PipelineError("相同图片跨 train/val/test 泄漏，请调整 session 分组")
            a, b = left["sample"]["_thumb"], right["sample"]["_thumb"]
            distance = sum(abs(x-y) for p, q in zip(a, b) for x, y in zip(p, q)) / (len(a)*3)
            if distance <= 2:
                raise PIPELINE.PipelineError("近重复图片跨 train/val/test；请将关联 session 放同组或排除重复")


def early_channel_check_leakage(entries):
    for index, left in enumerate(entries):
        for right in entries[index + 1:]:
            if left["split"] == right["split"]: continue
            if left["sample"]["image_sha256"] == right["sample"]["image_sha256"]:
                raise PIPELINE.PipelineError("相同图片跨 train/val/test 泄漏，请调整 session 分组")
            a,b=left["sample"]["_thumb"],right["sample"]["_thumb"]
            difference=0; limit=len(a)*6
            for p,q in zip(a,b):
                for x,y in zip(p,q):
                    difference+=abs(x-y)
                    if difference>limit: break
                if difference>limit: break
            if difference/(len(a)*3)<=2:
                raise PIPELINE.PipelineError("近重复图片跨 train/val/test；请将关联 session 放同组或排除重复")


def early_pixel_check_leakage(entries):
    for index,left in enumerate(entries):
        for right in entries[index+1:]:
            if left["split"]==right["split"]: continue
            if left["sample"]["image_sha256"]==right["sample"]["image_sha256"]:
                raise PIPELINE.PipelineError("相同图片跨 train/val/test 泄漏，请调整 session 分组")
            a,b=left["sample"]["_thumb"],right["sample"]["_thumb"]
            difference=0;limit=len(a)*6
            for p,q in zip(a,b):
                difference+=abs(p[0]-q[0])+abs(p[1]-q[1])+abs(p[2]-q[2])
                if difference>limit: break
            if difference/(len(a)*3)<=2:
                raise PIPELINE.PipelineError("近重复图片跨 train/val/test；请将关联 session 放同组或排除重复")

def compatible_pixel_check_leakage(entries):
    rgb_cache={}
    def rgb_thumbnail(thumb):
        identity=id(thumb)
        if identity not in rgb_cache:
            rgb_cache[identity]=type(thumb) in (tuple,list) and all(
                type(pixel) in (tuple,list) and len(pixel)==3 and
                all(type(value) is int and 0<=value<=255 for value in pixel)
                for pixel in thumb)
        return rgb_cache[identity]
    for index,left in enumerate(entries):
        for right in entries[index+1:]:
            if left["split"]==right["split"]: continue
            if left["sample"]["image_sha256"]==right["sample"]["image_sha256"]:
                raise PIPELINE.PipelineError("相同图片跨 train/val/test 泄漏，请调整 session 分组")
            a,b=left["sample"]["_thumb"],right["sample"]["_thumb"]
            difference=0;limit=len(a)*6
            if rgb_thumbnail(a) and rgb_thumbnail(b):
                for p,q in zip(a,b):
                    difference+=abs(p[0]-q[0])+abs(p[1]-q[1])+abs(p[2]-q[2])
                    if difference>limit: break
            else:
                difference=sum(abs(x-y) for p,q in zip(a,b) for x,y in zip(p,q))
            if difference/(len(a)*3)<=2:
                raise PIPELINE.PipelineError("近重复图片跨 train/val/test；请将关联 session 放同组或排除重复")



def make_fixture(count, profile="random", seed=20261010):
    """固定16x16 RGB形状；80/10/10分组；所有样本身份唯一。"""
    rng = random.Random(seed)
    entries = []
    for index in range(count):
        if profile == "random":
            raw = rng.randbytes(768)
        elif profile == "low-contrast":
            raw = bytes(rng.randrange(16) for _ in range(768))
        elif profile == "late-difference":
            raw = bytes(699) + rng.randbytes(69)
        else:
            raise ValueError(profile)
        pixels = tuple(tuple(raw[offset:offset+3]) for offset in range(0, 768, 3))
        entries.append(dict(split="train" if index % 10 < 8 else "val" if index % 10 == 8 else "test",
                            sample=dict(image_sha256=hashlib.sha256(index.to_bytes(8,"little")+raw).hexdigest(), _thumb=pixels)))
    return entries


def fixture_digest(entries):
    """按原pair顺序绑定split、图片身份和每个RGB通道。"""
    snapshot = [(entry["split"], entry["sample"]["image_sha256"],
                 entry["sample"]["_thumb"]) for entry in entries]
    payload = json.dumps(snapshot, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def verify_source_identity():
    if (hashlib.sha256(PIPELINE_PATH.read_bytes()).hexdigest() != PIPELINE_SHA256 or
            hashlib.sha256(BENCHMARK_PATH.read_bytes()).hexdigest() != BENCHMARK_SHA256):
        raise RuntimeError("基准期间源码身份已变化，拒绝发布结果")


def percentile(values, q):
    ordered=sorted(values)
    position=(len(ordered)-1)*q
    lower=int(position);upper=min(lower+1,len(ordered)-1)
    return ordered[lower]+(ordered[upper]-ordered[lower])*(position-lower)


def measure(check, entries, repeats, warmup, memory=True):
    for _ in range(warmup): check(entries)
    durations=[];failures=[]
    for _ in range(repeats):
        gc.collect()
        start=time.perf_counter()
        try: check(entries)
        except Exception as error: failures.append(dict(type=type(error).__name__,message=str(error)))
        durations.append(time.perf_counter()-start)
    peak=None
    if memory:
        gc.collect();tracemalloc.start()
        try: check(entries)
        except Exception: pass
        _,peak=tracemalloc.get_traced_memory();tracemalloc.stop()
    return dict(seconds=durations,mean_seconds=statistics.mean(durations),p50_seconds=percentile(durations,.5),
                p95_seconds=percentile(durations,.95),p99_seconds=percentile(durations,.99),max_seconds=max(durations),
                peak_python_bytes=peak,failures=len(failures),failure_details=failures)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes",default="120,360,720")
    parser.add_argument("--profiles",default="random,low-contrast,late-difference")
    parser.add_argument("--repeats",type=int,default=7)
    parser.add_argument("--warmup",type=int,default=1)
    parser.add_argument("--implementations",default="oracle,current")
    parser.add_argument("--skip-memory",action="store_true")
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    records=[]
    for profile in args.profiles.split(","):
        for size in map(int,args.sizes.split(",")):
            entries=make_fixture(size,profile)
            splits=Counter(entry["split"] for entry in entries)
            pairs=sum(left*right for i,left in enumerate(splits.values()) for right in list(splits.values())[i+1:])
            implementations={"oracle":original_check_leakage,"current":PIPELINE.check_leakage,
                             "early-channel":early_channel_check_leakage,"early-pixel":early_pixel_check_leakage,"compatible-pixel":compatible_pixel_check_leakage}
            timings={name:measure(implementations[name],entries,args.repeats,args.warmup,not args.skip_memory)
                     for name in args.implementations.split(",")}
            baseline=timings.get("oracle")
            record=dict(profile=profile,samples=size,fixture_sha256=fixture_digest(entries),cross_split_pairs=pairs,baseline_full_channel_comparisons=pairs*768,
                        warmup=args.warmup,repeats=args.repeats,measurements=timings,
                        speedups={name:baseline["p50_seconds"]/value["p50_seconds"] for name,value in timings.items()} if baseline else {})
            records.append(record)
            print(json.dumps(record),flush=True)
    report=dict(seed=20261010,thumbnail="16x16 RGB",scope="synthetic CPU Python leakage only",
                python_version=sys.version,platform=platform.platform(),
                pipeline_sha256=PIPELINE_SHA256,
                benchmark_sha256=BENCHMARK_SHA256,
                percentile_method="linear interpolation over observed durations; small repeat counts do not estimate production tail latency",
                memory_method="separate tracemalloc run after uninstrumented timing; fixtures allocated before trace",records=records)
    if any(value["failures"] for record in records for value in record["measurements"].values()):
        raise SystemExit(1)
    verify_source_identity()
    PUBLICATION.publish_json_new(args.output, report, newline="\n")

if __name__ == "__main__": main()
