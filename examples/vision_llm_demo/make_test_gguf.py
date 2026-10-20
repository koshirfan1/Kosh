import sys
sys.path.insert(0, '/home/claude/llama.cpp/gguf-py')
import numpy as np
import gguf

n_vocab  = 259   # 256 byte tokens + unk + bos + eos
n_embd   = 32
n_head   = 2
n_head_kv= 2
n_layer  = 1
n_ff     = 64
head_dim = n_embd // n_head

rng = np.random.default_rng(42)
def rnd(*shape):
    return (rng.standard_normal(shape) * 0.02).astype(np.float32)

w = gguf.GGUFWriter("/home/claude/tiny.gguf", "llama")

# --- metadata ---
w.add_name("tiny-test")
w.add_context_length(128)
w.add_embedding_length(n_embd)
w.add_block_count(n_layer)
w.add_feed_forward_length(n_ff)
w.add_head_count(n_head)
w.add_head_count_kv(n_head_kv)
w.add_layer_norm_rms_eps(1e-5)
w.add_rope_dimension_count(head_dim)
w.add_file_type(gguf.LlamaFileType.ALL_F32)

# --- vocab: 256 raw byte tokens + 3 specials ---
tokens = []
scores = []
toktypes = []
for i in range(256):
    tokens.append(f"<0x{i:02X}>")
    scores.append(0.0)
    toktypes.append(gguf.TokenType.BYTE)
tokens += ["<unk>", "<s>", "</s>"]
scores += [0.0, 0.0, 0.0]
toktypes += [gguf.TokenType.UNKNOWN, gguf.TokenType.CONTROL, gguf.TokenType.CONTROL]

w.add_tokenizer_model("llama")
w.add_token_list(tokens)
w.add_token_scores(scores)
w.add_token_types(toktypes)
w.add_unk_token_id(256)
w.add_bos_token_id(257)
w.add_eos_token_id(258)

# --- tensors ---
w.add_tensor("token_embd.weight", rnd(n_vocab, n_embd))
w.add_tensor("output_norm.weight", np.ones(n_embd, dtype=np.float32))
w.add_tensor("output.weight", rnd(n_vocab, n_embd))

for i in range(n_layer):
    p = f"blk.{i}."
    w.add_tensor(p+"attn_norm.weight", np.ones(n_embd, dtype=np.float32))
    w.add_tensor(p+"attn_q.weight", rnd(n_embd, n_embd))
    w.add_tensor(p+"attn_k.weight", rnd(n_embd, n_embd))
    w.add_tensor(p+"attn_v.weight", rnd(n_embd, n_embd))
    w.add_tensor(p+"attn_output.weight", rnd(n_embd, n_embd))
    w.add_tensor(p+"ffn_norm.weight", np.ones(n_embd, dtype=np.float32))
    w.add_tensor(p+"ffn_gate.weight", rnd(n_ff, n_embd))
    w.add_tensor(p+"ffn_up.weight", rnd(n_ff, n_embd))
    w.add_tensor(p+"ffn_down.weight", rnd(n_embd, n_ff))

w.write_header_to_file()
w.write_kv_data_to_file()
w.write_tensors_to_file()
w.close()
print("wrote /home/claude/tiny.gguf")
