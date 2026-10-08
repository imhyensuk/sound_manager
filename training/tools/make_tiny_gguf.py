"""Builds a tiny random-weight GGUF language model for tests.

The weights are random (the output is meaningless), but the file is a real llama.cpp model
with the Qwen2 tokenizer and chat template, so loading, chat formatting, grammar-constrained
sampling, KV-cache reuse and state save/restore can be tested offline without downloading
anything.

    python make_tiny_gguf.py <llama.cpp source dir> <out.gguf>
"""
import os
import sys

import numpy as np


def main() -> None:
    llama_dir, out_path = sys.argv[1], sys.argv[2]
    sys.path.insert(0, os.path.join(llama_dir, "gguf-py"))
    import gguf  # noqa: E402  (llama.cpp's own writer, numpy only)

    vocab = gguf.GGUFReader(os.path.join(llama_dir, "models", "ggml-vocab-qwen2.gguf"))

    def field(name):
        f = vocab.fields[name]
        return f.contents()

    tokens = field("tokenizer.ggml.tokens")
    n_vocab, n_embd, n_head, n_head_kv, n_ff, n_layer = len(tokens), 64, 4, 2, 128, 2
    head_dim = n_embd // n_head

    w = gguf.GGUFWriter(out_path, "qwen2")
    w.add_name("smix-tiny-test")
    w.add_context_length(4096)
    w.add_embedding_length(n_embd)
    w.add_feed_forward_length(n_ff)
    w.add_block_count(n_layer)
    w.add_head_count(n_head)
    w.add_head_count_kv(n_head_kv)
    w.add_rope_freq_base(10000.0)
    w.add_layer_norm_rms_eps(1e-6)
    w.add_file_type(gguf.LlamaFileType.MOSTLY_F16)

    w.add_tokenizer_model(field("tokenizer.ggml.model"))
    w.add_tokenizer_pre(field("tokenizer.ggml.pre"))
    w.add_token_list(tokens)
    w.add_token_types(field("tokenizer.ggml.token_type"))
    w.add_token_merges(field("tokenizer.ggml.merges"))
    w.add_eos_token_id(int(field("tokenizer.ggml.eos_token_id")))
    w.add_bos_token_id(int(field("tokenizer.ggml.bos_token_id")))
    w.add_pad_token_id(int(field("tokenizer.ggml.padding_token_id")))
    w.add_chat_template(field("tokenizer.chat_template"))

    rng = np.random.default_rng(1)

    def mat(rows, cols, scale=0.05):
        return (rng.standard_normal((rows, cols)) * scale).astype(np.float16)

    w.add_tensor("token_embd.weight", mat(n_vocab, n_embd))
    w.add_tensor("output_norm.weight", np.ones(n_embd, dtype=np.float32))
    for i in range(n_layer):
        p = f"blk.{i}."
        w.add_tensor(p + "attn_norm.weight", np.ones(n_embd, dtype=np.float32))
        w.add_tensor(p + "attn_q.weight", mat(n_embd, n_embd))
        w.add_tensor(p + "attn_q.bias", np.zeros(n_embd, dtype=np.float32))
        w.add_tensor(p + "attn_k.weight", mat(n_head_kv * head_dim, n_embd))
        w.add_tensor(p + "attn_k.bias", np.zeros(n_head_kv * head_dim, dtype=np.float32))
        w.add_tensor(p + "attn_v.weight", mat(n_head_kv * head_dim, n_embd))
        w.add_tensor(p + "attn_v.bias", np.zeros(n_head_kv * head_dim, dtype=np.float32))
        w.add_tensor(p + "attn_output.weight", mat(n_embd, n_embd))
        w.add_tensor(p + "ffn_norm.weight", np.ones(n_embd, dtype=np.float32))
        w.add_tensor(p + "ffn_gate.weight", mat(n_ff, n_embd))
        w.add_tensor(p + "ffn_up.weight", mat(n_ff, n_embd))
        w.add_tensor(p + "ffn_down.weight", mat(n_embd, n_ff))

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {out_path} ({os.path.getsize(out_path) / 1e6:.1f} MB, vocab {n_vocab})")


if __name__ == "__main__":
    main()
