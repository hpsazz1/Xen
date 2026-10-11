# Native Workspace CPU evaluation and file-path boundary

Baseline: `3f01c6fd1c62a0a3938b447593c6b3b40e09b5ba`.

The explicit integration host uses a real `Workspace` with no Collector or Runtime.
It launches the production Python evaluator under the normal owned JobObject,
with CPU, zero workers, offline mode, and the existing pinned dependencies.
The ONNX and reviewed images/labels are generated locally; no PT file is loaded.

```text
<PINNED_PYTHON> -B -X utf8 tests/model_workspace_cpu_integration_tests.py --host <RELEASE_HOST> --output-root <RETAINED_FIXTURE_DIRECTORY>
```

The original long-directory run failed before launching Python: the job path was
247 characters, `job.json` 256, and its pending file 264. The same binary passed
all 14 integration checks in a shorter fresh directory. The normal Workspace
test now also creates a 195-character data root: the process working directory
stays below 260 while the pending record and, depending on PID length, status
file reach the Windows file-path limit.

Diagnosis also confirmed the directory-creation boundary and a successful worker
status at a 260-character path that the original existence check did not see.
The production fix uses the existing extended-path helper at file I/O boundaries,
including reparse attribute checks, job directories, records, logs, state reads,
hash reads, and ordinary ONNX publication. Logical paths remain ordinary absolute
paths. No registry, permission, credential, or process-working-directory limit is
changed. This is not a retry of the separate deep-directory compiler limitation.

After the fix, the normal Workspace test and the original long-directory real
CPU chain pass. Integration checks cover missing evaluation, duplicate action,
actual worker cancellation and recovery, known TP/FP/FN counts, no automatic
import, changed model/manifest/image/label rejection, restoration and import,
matching model/sidecar identities, no model switch, and duplicate protection.
Every generated input file has the same final hash as before the run.

The first diagnostic fixture tried to inspect a snapshot not yet published after
launch failure; that ancillary assertion was corrected to inspect the actual job
directory. Its failure is not a separate production defect. Logs retain every
failed attempt. These tests prove neither model quality nor real-device behavior;
physical acceptance remains `NOT_EXECUTED`. The native CPU host is explicit-only
because the default suite does not require the training environment.
