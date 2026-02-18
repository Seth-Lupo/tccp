#include "templates.hpp"

ProjectTemplate make_qwen_template() {
    ProjectTemplate t;
    t.name = "qwen";
    t.project_type = "python-pytorch";
    t.description = "Qwen Chat Project";

    t.files = {
        // tccp.yaml
        {"tccp.yaml",
            "name: {{PROJECT_NAME}}\n"
            "type: python-pytorch\n"
            "\n"
            "env: .env\n"
            "\n"
            "output: ./output\n"
            "\n"
            "cache: ./cache\n"
            "\n"
            "jobs:\n"
            "  chat:\n"
            "    script: chat.py\n"
            "    args: \"\"\n"
            "    time: \"2:00:00\"\n"
            "    slurm:\n"
            "      gpu_type: a100\n"
            "      gpu_count: 1\n"
            "      memory: \"32G\"\n"
            "      cpus_per_task: 4\n"
        },

        // chat.py
        {"chat.py",
            R"PY(import os
import torch
from dotenv import load_dotenv
from transformers import AutoModelForCausalLM, AutoTokenizer

load_dotenv()

MODEL_ID = "Qwen/Qwen3-4B"
HF_TOKEN = os.environ.get("HF_TOKEN", "").strip()

if not HF_TOKEN:
    print("ERROR: HF_TOKEN not set. Add your Hugging Face token to .env:")
    print('  HF_TOKEN=hf_...')
    raise SystemExit(1)

print(f"Loading {MODEL_ID}...")
print(f"  Device: cuda ({torch.cuda.get_device_name(0)})")
print(f"  PyTorch: {torch.__version__}")
print()

tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, token=HF_TOKEN)
model = AutoModelForCausalLM.from_pretrained(
    MODEL_ID,
    torch_dtype=torch.bfloat16,
    device_map="auto",
    token=HF_TOKEN,
)

print("Model loaded. Type 'quit' to exit, 'clear' to reset conversation.")
print()

messages = []

while True:
    try:
        user_input = input("You: ")
    except (EOFError, KeyboardInterrupt):
        print("\nBye!")
        break

    if not user_input.strip():
        continue
    if user_input.strip().lower() == "quit":
        print("Bye!")
        break
    if user_input.strip().lower() == "clear":
        messages = []
        print("-- conversation cleared --")
        print()
        continue

    messages.append({"role": "user", "content": user_input})

    text = tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=True,
        enable_thinking=False,
    )
    inputs = tokenizer(text, return_tensors="pt").to(model.device)

    with torch.no_grad():
        output_ids = model.generate(
            **inputs,
            max_new_tokens=2048,
            do_sample=True,
            temperature=0.7,
            top_p=0.9,
            top_k=20,
        )

    # Decode only the new tokens (skip the prompt)
    new_tokens = output_ids[0][inputs["input_ids"].shape[1]:]
    response = tokenizer.decode(new_tokens, skip_special_tokens=True)

    print(f"\nQwen: {response}\n")
    messages.append({"role": "assistant", "content": response})
)PY"
        },

        // requirements.txt
        {"requirements.txt",
            "transformers\n"
            "accelerate\n"
            "safetensors\n"
            "sentencepiece\n"
            "python-dotenv\n"
        },

        // .env
        {".env",
            "# Hugging Face access token (https://huggingface.co/settings/tokens)\n"
            "HF_TOKEN=\n"
        },

        // .gitignore
        {".gitignore",
            "__pycache__/\n"
            "*.pyc\n"
            ".venv/\n"
            "*.egg-info/\n"
            ".env\n"
            "output/\n"
            "cache/\n"
        },
    };

    t.directories = {"output"};

    t.next_steps = {
        "Add your Hugging Face token to .env",
        "Run 'tccp' to connect",
        "Run 'run chat' to start chatting",
    };

    return t;
}
