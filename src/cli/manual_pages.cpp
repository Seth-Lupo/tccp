#include "tccp_cli.hpp"
#include "theme.hpp"
#include <iostream>

static const char* MANUAL_OVERVIEW = R"(
TCCP - Tufts Cluster Command Prompt
====================================

Run your code on the Tufts HPC cluster without thinking about the cluster.
Write a script, point tccp at it, and get results back.

QUICK START
-----------

  1. tccp setup          Store your Tufts credentials (once)
  2. tccp register       Set up your project (creates tccp.yaml)
  3. tccp                Connect and enter the interactive prompt

HOW IT WORKS
------------

  your laptop                              cluster
  +-------------+      tccp connect       +----------------+
  | tccp.yaml   | ----------------------> | your code runs |
  | train.py    |     files synced        | deps installed |
  | data/       |     deps installed      | GPU allocated  |
  +-------------+     job launched        +-------+--------+
                                                  |
                  <-------------------------------+
                    output synced back when done

  You stay in the tccp prompt. Your job runs on the cluster. When it
  finishes, output is automatically downloaded to your machine.

  If you disconnect (close laptop, lose wifi), your job keeps running.
  Reconnect with 'tccp' and pick up where you left off.

COMMANDS
--------

  run [job]         Run a job (default: "main" or the only defined job)
  view [job]        Watch a running job's live output
  jobs              List all tracked jobs and their status
  cancel <job>      Cancel a running job
  restart <job>     Cancel and re-run a job
  return <job>      Download output from a completed job

  logs [job]        Print full job output to terminal
  tail [job] [N]    Print last N lines of output (default: 30)
  output [job]      View full job output in vim
  initlogs [job]    Print job initialization logs
  info <job>        Show detailed job status (node, times, ports)

  config            Show resolved project configuration
  status            Show connection and project status
  allocs            Show your active compute allocations
  gpus              Show available GPUs on the cluster

  ssh <job> [cmd]   Run command on job's compute node
  open              Open an interactive remote shell
  shell             Open a shell on the login node
  exec <cmd>        Run a one-off command on the login node
  dealloc [id]      Release a compute allocation
  help              List all commands

KEYBOARD SHORTCUTS (while viewing a job)
-----------------------------------------

  Ctrl+C            Cancel job (during setup) or detach (while running)
  Ctrl+\            Detach and return to prompt
  ESC ESC           Emergency detach

PROJECT CONFIG (tccp.yaml)
--------------------------

  Minimal:

    script: train.py

  With a GPU:

    script: train.py
    gpu: a100

  Full form:

    name: myproject
    type: python-pytorch
    script: train.py
    gpu: a100
    output: output
    cache: .model_cache
    rodata:
      - data
    env: .env
    jobs:
      train:
        script: train.py
        args: --epochs 100
        time: "8:00:00"
      eval: eval.py

  name       Project name (default: directory name)
  type       "python" or "python-pytorch" (default: python)
  script     Main script to run (default: main.py)
  gpu        Request a GPU: a100, v100, a10
  output     Directory downloaded back after each job
  cache      Directory that persists across jobs (model weights, etc.)
  rodata     Read-only data directories (uploaded once, not every run)
  env        Dotfile to upload (default: .env — not blocked by .gitignore)
  jobs       Named jobs (shorthand: "train: train.py" or full config)

  An empty tccp.yaml runs main.py with no GPU. The project name
  comes from the directory name.

FILE SYNC
---------

  tccp respects your .gitignore when uploading files. If you need
  different rules for the cluster, create a .tccpignore instead.
  Common large files (.git, __pycache__, .venv, etc.) are always
  skipped automatically.

GETTING STARTED
---------------

  tccp setup              Store your Tufts credentials (once)
  tccp register           Set up your project (creates tccp.yaml)
  tccp manual <topic>     Detailed guide (python, python-pytorch)
)";

static const char* MANUAL_PYTHON = R"(
TCCP MANUAL: Python Projects
==============================

For CPU-based Python work: data processing, scripting, CPU training.
Uses Python 3.11. No GPU.

GETTING STARTED
---------------

  mkdir myproject && cd myproject

  # Option A: interactive setup
  tccp register

  # Option B: by hand
  echo "script: main.py" > tccp.yaml

  You need a .gitignore so tccp knows what files to upload.

PROJECT LAYOUT
--------------

  myproject/
    tccp.yaml
    main.py               your script
    requirements.txt      pip dependencies (optional)
    .gitignore
    data/                 input data (optional)
    output/               results (optional, downloaded after job)

EXAMPLES
--------

  Simplest case (runs main.py):

    script: main.py

  With output and data:

    script: process.py
    output: results
    rodata:
      - data

  Multiple jobs:

    jobs:
      preprocess: preprocess.py
      train: train.py
      eval: eval.py

DEPENDENCIES
------------

  Put your pip dependencies in requirements.txt. They are installed
  automatically before your script runs. tccp caches the install by
  hash — if requirements.txt hasn't changed, it skips installation.

  Only the Python standard library is available by default. Everything
  else must go in requirements.txt.

WORKFLOW
--------

  $ tccp                          connect to the cluster
  tccp:myproject@cluster> run     submit the job
                                  (watch live output, or Ctrl+\ to detach)
  tccp:myproject@cluster> jobs    check status
  tccp:myproject@cluster> quit    disconnect (job keeps running)

  $ tccp                          reconnect later
  tccp:myproject@cluster> return  download output

TIPS
----

  - Edit requirements.txt to force a dependency reinstall next run.
  - Set "output" in tccp.yaml to auto-download results when jobs finish.
  - Use "rodata" for large input data — it's uploaded once per allocation,
    not every time you run.
)";

static const char* MANUAL_PYTHON_PYTORCH = R"(
TCCP MANUAL: Python + PyTorch Projects
========================================

For GPU training and inference. Comes with PyTorch 2.6, CUDA 12.4,
and cuDNN 9 pre-installed. Just add "gpu: a100" to your config.

GETTING STARTED
---------------

  mkdir myproject && cd myproject

  # Option A: interactive setup
  tccp register

  # Option B: by hand
  echo -e "type: python-pytorch\nscript: train.py\ngpu: a100" > tccp.yaml

  You need a .gitignore so tccp knows what files to upload.

PROJECT LAYOUT
--------------

  myproject/
    tccp.yaml
    train.py              your training script
    requirements.txt      extra deps (torch is already provided)
    .gitignore
    .env                  secrets like HF_TOKEN (optional)
    data/                 datasets (optional)
    output/               results (optional, downloaded after job)

EXAMPLES
--------

  Minimal GPU training:

    type: python-pytorch
    script: train.py
    gpu: a100

  Full project:

    type: python-pytorch
    script: train.py
    gpu: a100
    output: checkpoints
    cache: .hf_cache
    rodata:
      - data
    env: .env
    jobs:
      train:
        script: train.py
        args: --epochs 50 --lr 0.001
        time: "8:00:00"

  Multiple jobs:

    type: python-pytorch
    gpu: a100
    output: results
    jobs:
      train: train.py
      eval: eval.py

GPU OPTIONS
-----------

  gpu: a100        NVIDIA A100 (40 or 80 GB, auto-selected)
  gpu: a100-80gb   Explicit 80 GB variant
  gpu: a100-40gb   Explicit 40 GB variant
  gpu: v100        NVIDIA V100 (32 GB)
  gpu: a10         NVIDIA A10 (24 GB)

  For multiple GPUs, use the full slurm block:

    slurm:
      gpu_type: a100
      gpu_count: 2
      memory: 64G
      cpus_per_task: 8

DEPENDENCIES
------------

  PyTorch, torchvision, torchaudio, and CUDA are already available.
  You do NOT need to install them. If they appear in your
  requirements.txt, tccp automatically skips them to avoid conflicts.

  Put any additional dependencies (transformers, numpy, etc.) in
  requirements.txt. They are installed automatically before your
  script runs, and cached by hash so unchanged files skip install.

  torch.cuda.is_available() will return True. Use it normally.

HUGGING FACE MODELS
-------------------

  To use gated models (Llama, Qwen, etc.):

    1. Create a .env file:       HF_TOKEN=hf_xxxxx
    2. Add to tccp.yaml:         env: .env
    3. Add a cache directory:    cache: .hf_cache

  The cache persists across jobs, so model weights download once.

WORKFLOW
--------

  $ tccp                          connect to the cluster
  tccp:myproject@cluster> run     submit the job
                                  (watch live output, or Ctrl+\ to detach)
  tccp:myproject@cluster> jobs    check status
  tccp:myproject@cluster> quit    disconnect (job keeps running)

  $ tccp                          reconnect later
  tccp:myproject@cluster> return  download output

  The first run takes a few extra minutes while the environment is
  prepared. Subsequent runs start much faster.

TIPS
----

  - Don't install torch via requirements.txt. It's already there and
    tccp will skip it automatically, but you'll save time by not
    listing it at all.
  - Use "cache" for model weights so they aren't re-downloaded.
  - Use "rodata" for large datasets — uploaded once, not every run.
  - Set "output" to auto-download results when jobs finish.
  - For multi-GPU, use the slurm block instead of the gpu shorthand.
)";

void TCCPCLI::run_manual(const std::string& topic) {
    if (topic.empty()) {
        std::cout << MANUAL_OVERVIEW;
    } else if (topic == "python") {
        std::cout << MANUAL_PYTHON;
    } else if (topic == "python-pytorch") {
        std::cout << MANUAL_PYTHON_PYTORCH;
    } else {
        std::cout << theme::fail("Unknown manual topic: " + topic);
        std::cout << theme::step("Available topics: python, python-pytorch");
        std::cout << theme::step("Run 'tccp manual' for the general overview.");
    }
}
