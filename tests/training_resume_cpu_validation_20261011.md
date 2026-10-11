# Actual CPU resume after cancellation

`training_resume_cpu_tests.py` closes the resume gap left by the cancellation
tests. Run it explicitly with the existing pinned training interpreter:

```text
<PINNED_PYTHON> -B -X utf8 tests/training_resume_cpu_tests.py --output-root <RETAINED_FIXTURE_DIRECTORY>
```

The test creates its own reviewed synthetic images and random tiny detection
model. It cancels actual Ultralytics training during validation and after saving,
then resumes each epoch-0 checkpoint into a new directory. No downloaded or
user-provided PT file is deserialized.

Both cases passed on the unchanged production baseline `249e995` in 18.75 seconds:

- The cancelled job runs only epoch 0 and retains the optimizer and matching hashes.
- The production resume path restores nonempty optimizer state, EMA weights and
  update count, best fitness, and the historical best checkpoint exactly.
- Actual resumed batches run in epochs 1 and 2, with CPU, zero loader workers,
  and the new output and working dataset paths.
- Every original job and frozen dataset file retains its hash.
- Exported ONNX remains `CANDIDATE_NOT_ACTIVATED`; test evaluation and physical
  acceptance remain `NOT_EXECUTED`.

Only public callbacks observe the real trainer; training, checkpoint loading,
state restoration, and export are not replaced. The test requires the existing
Ultralytics 8.3.203 / Torch 2.8.0+cu128 / ONNX 1.19.0 environment and explicitly
uses CPU with CUDA hidden, offline mode, and automatic installation disabled.
It is not registered in the default suite, which does not require training
dependencies. JSON results and generated fixtures are retained under the caller's
output root. This proves neither model quality nor real-device performance.
