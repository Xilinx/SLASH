# Building SLASH on an LSF cluster

A worked example of the `SLASH_TOOL_LAUNCHER` contract (see
`docs/howto/offload-builds-to-a-cluster.rst`) for IBM Spectrum LSF. It runs the
heavy tool invocations of a SLASH build — Vivado, bootgen, the AVED firmware
build, HLS — on compute nodes instead of the local machine.

**Nothing SLASH-specific is installed on the execution nodes.** Every input is
staged onto shared storage by the local Python before the job is submitted, so
a node needs only the vendor toolchain and a mount of that storage. Keep it
that way: if you find yourself wanting to install something on a node, the
staging is what needs fixing.

## Setup

```bash
cp scripts/lsf/site.conf.example scripts/lsf/site.conf
$EDITOR scripts/lsf/site.conf          # queue, settings script, sizing
export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
```

`site.conf` is gitignored, because everything in it — queue names, OS
selectors, tool paths — describes one particular cluster and has no business in
a public repository. Point `SLASH_LSF_SITE_CONF` at a copy on shared storage to
share one configuration across a team.

Then check that a node can actually do the work, before spending queue time
finding out it cannot:

```bash
scripts/lsf/preflight.sh          # the Vivado steps
scripts/lsf/preflight.sh aved     # the firmware build, which needs the most
```

## The pieces

| File | Runs on | Job |
| --- | --- | --- |
| `lsf-run.sh` | submit host | Turns an argv into a job script, submits it with `bsub -K`, blocks, propagates the exit status |
| `tool-run.sh` | execution host | Sources the settings script, restores the working directory, cleans up the inherited environment, `exec`s the command |
| `site-conf.sh` | both | Loads `site.conf` and resolves per-task values |
| `preflight.sh`, `preflight-probe.sh` | both | One short job reporting what a node has |
| `smoke/run_dummy.py` | submit host | Two-minute throwaway synthesis through the real code path |

## Two ways to use it

### Per tool invocation

`slashkit` prefixes `$SLASH_TOOL_LAUNCHER` to every offloadable tool command it
builds, so each one becomes its own job while the Python orchestration stays
local:

```bash
export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh

cd linker
python3 -m slashkit install --shell-type compute --jobs 16 \
    --build-dir install.prj.compute --out-dir slashkit/resources/
```

This is the finer-grained option: each step holds only the reservation it
needs, and a one-core bootgen run does not sit in a 70 GB slot. The local
machine needs no tools at all — pass `--vivado vivado` and the bare name is
resolved on the execution host.

### Whole build as one job

One reservation covers everything, at the cost of sizing it for the largest
step:

```bash
SLASH_BUILD_TASK=static_shell scripts/lsf/lsf-run.sh \
    python3 -m slashkit install --shell-type compute --jobs 16 \
    --build-dir install.prj.compute --out-dir slashkit/resources/
```

`lsf-run.sh` notices `LSB_JOBID` and runs the command directly when it is
already inside a job, so leaving `SLASH_TOOL_LAUNCHER` exported does not make
the build try to submit jobs from a compute node.

## HLS kernels

`build_hls()` is driven by `make`, not by slashkit, so it reads the launcher at
CMake configure time:

```bash
cmake -B build -DSLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
cmake --build build
```

It is a cache entry, so it sticks across re-configures; use
`cmake -B build -USLASH_TOOL_LAUNCHER` to go back to building locally. With a
launcher set, `v++` and `vitis-run` are not looked for on the local machine.

The base IP cores that `slashkit install` needs are built by plain makefiles
rather than by CMake, and read the same variable from the environment:

```bash
export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
make -C linker/slashkit/resources/base/common/iprepo -j2
```

Both cores are tagged `hls`, and `-j2` puts them in the queue together.

## Task kinds and sizing

Each invocation is tagged with `SLASH_BUILD_TASK`, and `site.conf` can give any
of them their own queue, memory, cores and wall time — `SLASH_LSF_MEM_MB_bootgen`
overrides `SLASH_LSF_MEM_MB`, and so on. The kinds are `static_shell`,
`static_shell_compute`, `rm_slash`, `rm_service_layer`, `bootgen`, `aved`,
`hls`, and `preflight`.

There are no built-in defaults for queue, memory, cores or wall time: a
reservation that is wrong by default either wastes the cluster's capacity or
gets a twelve-hour implementation run OOM-killed in hour three. An unset value
aborts before submission, naming what to set and where.

`aved` covers both firmware builds — the AMC one and RP1, which `build_all.sh`
invokes — so it wants the more generous wall time of the two, and a node with
`ninja`, `sdtgen` and both R5 cross compilers. `preflight.sh aved` checks
exactly that.

For a starting point, the SLURM job in `.gitlab-ci.yml` reserves 120 GB, 16
cores and 12 hours for a static-shell build. Replace the guess with a
measurement as soon as you have one — `bjobs -l <job_id>` reports `MAX MEM` and
`AVG MEM` for a finished job. Over-reserving costs scheduling priority.

Note that `-n` slots are scheduled but not enforced on the host, so a tool will
happily use every core on the machine. Keep `--jobs` and `SLASH_LSF_CORES` in
step. Check too whether `rusage[mem=...]` is per-job or per-slot at your site
before trusting a number; `bjobs -l` shows the effective reservation.

An exported value beats the per-task configuration, so a one-off override works
as expected and is the way to probe an expensive task cheaply:

```bash
SLASH_LSF_MEM_MB=120000 python3 -m slashkit link ...
SLASH_LSF_MEM_MB=1000 SLASH_LSF_QUEUE=<short> scripts/lsf/preflight.sh aved
```

## Smoke test

`smoke/run_dummy.py` synthesises a throwaway design for the V80 part through
the same `launcher.run_tool` call a real build uses:

```bash
export SLASH_TOOL_LAUNCHER=$PWD/scripts/lsf/lsf-run.sh
python3 scripts/lsf/smoke/run_dummy.py
```

Roughly two minutes and 3 GB peak. It prints `OK` and the path to the
checkpoint it wrote, and fails if the tool reports success without producing
one. Unset `SLASH_TOOL_LAUNCHER` to run the same thing locally as an A/B.

## Watching a build

`bsub -K` writes the job's stdout and stderr to `$SLASH_LSF_LOG_DIR` only when
the job ends, but the tool's own log file lives in the build directory on
shared storage and updates live, so that is the thing to tail:

```bash
tail -f install.prj.compute/vivado_compute.log
bjobs -w                 # is it still running
bpeek <job_id>           # peek at job stdout
bjobs -l <job_id>        # includes MAX MEM / AVG MEM, useful for sizing
bkill -J 'slash-*'       # kill by name, globbing works
```

`SLASH_LSF_DRY_RUN=1` writes the job script and prints the `bsub` command
without submitting anything, which is the cheapest way to check a
configuration change.

## Gotchas that cost time

**Do not build a `bsub ... vivado ...` command string by hand.** `bsub`
flattens its argv into a single command string that the shell on the execution
host parses *again*. Arguments containing spaces, quotes, `$`, or brackets get
mangled. `lsf-run.sh` avoids this by writing the argv into a job script with
`printf %q` and submitting just that script's path.

**Use `settings64.sh`, not `settings64.csh`.** LSF runs the job as a plain
non-interactive script under the submitting user's login shell, typically bash.
Sourcing the csh flavour dies with `setenv: command not found` and then a
syntax error on the first `if`/`else`, and because the usual idiom is
`source ... && vivado ...`, the `&&` short-circuits and the tool never runs. A
prefix like `echo $0 &&` does not change this — `$0` is expanded by the *local*
shell before `bsub` ever sees it, so it only adds a harmless `echo`. If such a
command line appears to work, the tool is being picked up from the submission
environment that LSF copied over, not from the settings script. `tool-run.sh`
treats an unreadable settings script as fatal for exactly that reason.

**The AVED step compiles with the node's own C compiler, not only with the
toolchain's.** The AMC firmware is cross-compiled for the ARM R5, but its
`CMakeLists.txt` calls `project()` before selecting the cross compiler, so CMake
checks the *host* compiler first — and seeds `CMAKE_C_FLAGS` from `CFLAGS` while
doing it. An execution host with an older compiler than the machine you develop
on will then fail at configure time over a flag the firmware compiler is
perfectly happy with, and the error names a trivial test program rather than
anything in the build. Keep `CFLAGS` portable, and prefer `-Wno-<warning>` to
`-Wno-error=<warning>`: an unknown `-Wno-` is ignored, while an unknown
`-Wno-error=` is fatal.

**`LD_PRELOAD` does not travel.** slashkit sets it to local library paths as a
container workaround, and LSF copies the environment to an execution host where
those files do not exist. Every process the tool spawns then prints an `ld.so`
error. `tool-run.sh` drops the entries that are not present on the node.

**The working directory is not guaranteed.** Some tools resolve their inputs
against it — bootgen against the paths inside a `.bif`, `v++` against
`--work_dir .` — and a scheduler reproduces the submission directory only as a
courtesy, sometimes falling back to a temporary directory. slashkit exports
`SLASH_TOOL_CWD` and `tool-run.sh` `cd`s there, failing loudly if it cannot.

**Everything must be on shared storage** — the checkout, the build directory,
the Python interpreter, the log directory, and the tool installation.
