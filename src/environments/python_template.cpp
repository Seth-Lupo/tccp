#include "templates.hpp"

ProjectTemplate make_python_template() {
    ProjectTemplate t;
    t.name = "python";
    t.project_type = "python";
    t.description = "Python Project";

    t.files = {
        // tccp.yaml
        {"tccp.yaml",
            "name: {{PROJECT_NAME}}\n"
            "type: python\n"
            "\n"
            "rodata:\n"
            "  - ./data\n"
            "\n"
            "output: ./output\n"
            "\n"
            "jobs:\n"
            "  main:\n"
            "    script: main.py\n"
            "    args: \"\"\n"
        },

        // main.py
        {"main.py",
            "import numpy as np\n"
            "import os\n"
            "import time\n"
            "\n"
            "# ── Step 1: Load data ──\n"
            "print(\"[step 1] Loading data...\")\n"
            "time.sleep(1)\n"
            "data = np.loadtxt(\"data/input.csv\", delimiter=\",\")\n"
            "print(f\"  Loaded {data.shape[0]} rows, {data.shape[1]} cols\")\n"
            "print(f\"  Column means: {data.mean(axis=0)}\")\n"
            "print(f\"  Column ranges: {data.min(axis=0)} -> {data.max(axis=0)}\")\n"
            "print()\n"
            "\n"
            "# ── Step 2: Ask for confirmation ──\n"
            "response = input(\"[step 2] Normalize and write output? (yes/no): \")\n"
            "if response.strip().lower() != \"yes\":\n"
            "    print(\"Aborted.\")\n"
            "    exit(0)\n"
            "print()\n"
            "\n"
            "# ── Step 3: Process and write ──\n"
            "print(\"[step 3] Normalizing...\")\n"
            "time.sleep(1)\n"
            "mins = data.min(axis=0)\n"
            "maxs = data.max(axis=0)\n"
            "normalized = (data - mins) / (maxs - mins + 1e-8)\n"
            "print(f\"  Column means after: {normalized.mean(axis=0)}\")\n"
            "\n"
            "os.makedirs(\"output\", exist_ok=True)\n"
            "np.savetxt(\"output/normalized.csv\", normalized, delimiter=\",\", fmt=\"%.6f\")\n"
            "print(f\"  Wrote output/normalized.csv ({normalized.shape[0]} rows)\")\n"
            "print()\n"
            "print(\"Done.\")\n"
        },

        // data/input.csv
        {"data/input.csv",
            "1.0,10.0,100.0\n"
            "2.0,20.0,200.0\n"
            "3.0,30.0,300.0\n"
            "4.0,40.0,400.0\n"
            "5.0,50.0,500.0\n"
        },

        // requirements.txt
        {"requirements.txt",
            "numpy\n"
        },

        // .gitignore
        {".gitignore",
            "__pycache__/\n"
            "*.pyc\n"
            ".venv/\n"
            "*.egg-info/\n"
            ".env\n"
            "output/\n"
            "tccp-output/\n"
        },
    };

    t.directories = {"output"};

    t.next_steps = {
        "Run 'tccp' to connect",
        "Run 'run main' to submit your job",
    };

    return t;
}
