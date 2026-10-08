"""Fine-tunes the reasoning module on a Colab T4 (15 GB) and produces the GGUF the plugin loads.

    1. LoRA training with memopro.finetune: the 16-bit base weights stream from their files within
       a memory budget, so a 3B model trains on a T4 (falls back to plain PEFT with --no-memopro).
    2. Merge the adapter into the base model.
    3. Convert to GGUF and quantise (Q4_K_M) with llama.cpp.
    4. Copy smix-intent.gguf into the plugin's models folder (SoundManagerAI/models), or set
       "로컬 언어 모델" in the plugin's 모듈·설정 tab.

Usage (Colab):
    !pip install "memopro[llm]" transformers peft datasets sentencepiece
    !python make_dataset.py --out data --n 8000
    !python finetune_t4.py --base Qwen/Qwen2.5-1.5B-Instruct --data data --out out --budget 4GiB
"""
import argparse
import json
import os
import subprocess
import sys


def load_examples(path):
    with open(path, encoding="utf-8") as f:
        return [json.loads(line)["messages"] for line in f if line.strip()]


def to_texts(examples, tokenizer):
    # The exact chat format llama.cpp applies at inference (the model's own template).
    return [tokenizer.apply_chat_template(m, tokenize=False) for m in examples]


def train_memopro(args, texts):
    import memopro

    r = memopro.finetune(args.base, texts, budget=args.budget, epochs=args.epochs, seq_len=args.seq_len,
                         rank=args.rank, alpha=2 * args.rank, lr=args.lr)
    print("loss first/last: %.3f / %.3f, %.1f tokens/s" % (r.losses[0], r.losses[-1], r.tokens / max(r.seconds, 1e-9)))
    r.adapter.save_pretrained(os.path.join(args.out, "adapter"))


def train_peft(args, texts, tokenizer):
    import torch
    from peft import LoraConfig, get_peft_model
    from transformers import AutoModelForCausalLM

    model = AutoModelForCausalLM.from_pretrained(args.base, torch_dtype=torch.float16, device_map="auto")
    model.gradient_checkpointing_enable()
    model = get_peft_model(model, LoraConfig(r=args.rank, lora_alpha=2 * args.rank,
                                             target_modules=["q_proj", "k_proj", "v_proj", "o_proj"], task_type="CAUSAL_LM"))
    opt = torch.optim.AdamW([p for p in model.parameters() if p.requires_grad], lr=args.lr)
    model.train()
    for epoch in range(args.epochs):
        for i, text in enumerate(texts):
            ids = tokenizer(text, return_tensors="pt", truncation=True, max_length=args.seq_len).input_ids.to(model.device)
            loss = model(input_ids=ids, labels=ids).loss
            loss.backward()
            opt.step()
            opt.zero_grad()
            if i % 200 == 0:
                print("epoch %d step %d loss %.3f" % (epoch, i, loss.item()))
    model.save_pretrained(os.path.join(args.out, "adapter"))


def merge(args):
    import torch
    from peft import PeftModel
    from transformers import AutoModelForCausalLM, AutoTokenizer

    base = AutoModelForCausalLM.from_pretrained(args.base, torch_dtype=torch.float16)
    merged = PeftModel.from_pretrained(base, os.path.join(args.out, "adapter")).merge_and_unload()
    merged.save_pretrained(os.path.join(args.out, "merged"))
    AutoTokenizer.from_pretrained(args.base).save_pretrained(os.path.join(args.out, "merged"))


def to_gguf(args):
    llama = os.path.join(args.out, "llama.cpp")
    if not os.path.isdir(llama):
        subprocess.check_call(["git", "clone", "--depth", "1", "--branch", args.llama_tag, "https://github.com/ggml-org/llama.cpp", llama])
        subprocess.check_call([sys.executable, "-m", "pip", "install", "-q", "-r", os.path.join(llama, "requirements", "requirements-convert_hf_to_gguf.txt")])
    f16 = os.path.join(args.out, "smix-intent-f16.gguf")
    subprocess.check_call([sys.executable, os.path.join(llama, "convert_hf_to_gguf.py"), os.path.join(args.out, "merged"),
                           "--outtype", "f16", "--outfile", f16])
    build = os.path.join(llama, "build")
    subprocess.check_call(["cmake", "-S", llama, "-B", build, "-DGGML_NATIVE=OFF", "-DLLAMA_OPENSSL=OFF"])
    subprocess.check_call(["cmake", "--build", build, "--target", "llama-quantize", "-j", "4"])
    out = os.path.join(args.out, "smix-intent.gguf")
    subprocess.check_call([os.path.join(build, "bin", "llama-quantize"), f16, out, args.quant])
    print("done:", out, "%.0f MB" % (os.path.getsize(out) / 1e6))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="Qwen/Qwen2.5-1.5B-Instruct", help="any HF chat model llama.cpp can convert")
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="out")
    ap.add_argument("--budget", default="4GiB", help="memopro weight budget (T4: 4GiB leaves room for activations)")
    ap.add_argument("--epochs", type=int, default=2)
    ap.add_argument("--seq-len", type=int, default=1024)
    ap.add_argument("--rank", type=int, default=16)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--quant", default="Q4_K_M")
    ap.add_argument("--llama-tag", default="b11121", help="same llama.cpp release as the plugin")
    ap.add_argument("--no-memopro", action="store_true")
    ap.add_argument("--skip-train", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(args.base)
    texts = to_texts(load_examples(os.path.join(args.data, "train.jsonl")), tokenizer)
    print("%d training texts" % len(texts))
    if not args.skip_train:
        if args.no_memopro:
            train_peft(args, texts, tokenizer)
        else:
            train_memopro(args, texts)
    merge(args)
    to_gguf(args)


if __name__ == "__main__":
    main()
