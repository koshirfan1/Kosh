#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llama.h"
#include "llm_lib.h"

/* ===========================================================================
 * Model slots
 *
 * Same pattern as ocr_lib.c's engine table: a small fixed-size array of
 * native handles, addressed from Khan by index. Each Khan-visible "model"
 * map just carries its slot number back to us.
 * ========================================================================= */

#define LLM_MAX_HANDLES 4

typedef struct {
    struct llama_model   *model;
    struct llama_context  *ctx;
} LlmHandle;

static LlmHandle g_llm_handles[LLM_MAX_HANDLES];
static int        g_llm_backend_ready = 0;

static int llm_alloc_slot(void) {
    for (int i = 0; i < LLM_MAX_HANDLES; i++) if (!g_llm_handles[i].model) return i;
    return -1;
}

/* Resolve a Khan-level model map back to its native slot, or -1 if the
 * argument isn't a live model (wrong type, freed already, corrupt id). */
static int llm_resolve_slot(Value *m) {
    if (m->type != VAL_MAP) return -1;
    Value *idv = map_get(m, "__llm_id");
    if (!idv || idv->type != VAL_NUMBER) return -1;
    int slot = (int)idv->as.number;
    if (slot < 0 || slot >= LLM_MAX_HANDLES || !g_llm_handles[slot].model) return -1;
    return slot;
}

/* ===========================================================================
 * Native functions
 * ========================================================================= */

void fn_llm_load(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_STRING) return;
    const char *path = args[0].as.string;

    int n_ctx = 2048;
    if (argc >= 2) {
        if (args[1].type != VAL_NUMBER) return;
        n_ctx = (int)args[1].as.number;
    }

    int slot = llm_alloc_slot();
    if (slot < 0) {
        fprintf(stderr,
            "Runtime error: llm_load() - too many open models (max %d); "
            "call llm_free() on ones you're done with\n", LLM_MAX_HANDLES);
        return;
    }

    if (!g_llm_backend_ready) {
        llama_backend_init();
        g_llm_backend_ready = 1;
    }

    struct llama_model_params mparams = llama_model_default_params();
    struct llama_model *model = llama_model_load_from_file(path, mparams);
    if (!model) {
        fprintf(stderr,
            "Runtime error: llm_load(\"%s\") - failed to load GGUF file "
            "(bad path or not a valid GGUF)\n", path);
        return;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = (uint32_t)n_ctx;
    struct llama_context *ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        fprintf(stderr,
            "Runtime error: llm_load(\"%s\") - failed to create context "
            "(n_ctx=%d; not enough RAM for the KV cache?)\n", path, n_ctx);
        llama_model_free(model);
        return;
    }

    g_llm_handles[slot].model = model;
    g_llm_handles[slot].ctx   = ctx;

    Value m = value_map_empty();
    map_set(&m, "__llm_id", value_number(slot));
    *result = m;
}

void fn_llm_complete(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[1].type != VAL_STRING) return;

    int slot = llm_resolve_slot(&args[0]);
    if (slot < 0) return;

    int max_tokens = 256;
    if (argc >= 3) {
        if (args[2].type != VAL_NUMBER) return;
        max_tokens = (int)args[2].as.number;
    }

    struct llama_model         *model = g_llm_handles[slot].model;
    struct llama_context       *ctx   = g_llm_handles[slot].ctx;
    const struct llama_vocab   *vocab = llama_model_get_vocab(model);

    const char *prompt     = args[1].as.string;
    int         prompt_len = (int)strlen(prompt);

    /* Ask for the required token count first (n_tokens_max=0 -> returns
     * -needed on a would-be-truncated / empty-buffer call), then tokenize
     * for real into a buffer of exactly that size. */
    int n_needed = -llama_tokenize(vocab, prompt, prompt_len, NULL, 0, true, true);
    if (n_needed <= 0) {
        fprintf(stderr, "Runtime error: llm_complete() - tokenization failed\n");
        return;
    }

    llama_token *tokens = malloc(sizeof(llama_token) * (size_t)n_needed);
    if (!tokens) return;

    int n_tokens = llama_tokenize(vocab, prompt, prompt_len, tokens, n_needed, true, true);
    if (n_tokens < 0) {
        fprintf(stderr, "Runtime error: llm_complete() - tokenization failed\n");
        free(tokens);
        return;
    }

    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr,
            "Runtime error: llm_complete() - initial decode failed "
            "(prompt too long for n_ctx?)\n");
        free(tokens);
        return;
    }

    /* Greedy-only sampler for this first cut - deterministic, no
     * temperature/top-k/top-p yet. See docs/llm.md. */
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    size_t out_cap = 4096, out_len = 0;
    char  *out = malloc(out_cap);
    if (!out) { free(tokens); llama_sampler_free(sampler); return; }
    out[0] = '\0';

    for (int i = 0; i < max_tokens; i++) {
        llama_token next_tok = llama_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, next_tok)) break;

        char piece[256];
        int piece_len = llama_token_to_piece(vocab, next_tok, piece, sizeof(piece), 0, false);
        if (piece_len < 0) break; /* token text longer than 256B - not handled in this cut */

        if (out_len + (size_t)piece_len + 1 > out_cap) {
            out_cap *= 2;
            char *grown = realloc(out, out_cap);
            if (!grown) break;
            out = grown;
        }
        memcpy(out + out_len, piece, (size_t)piece_len);
        out_len += (size_t)piece_len;
        out[out_len] = '\0';

        llama_sampler_accept(sampler, next_tok);

        struct llama_batch next_batch = llama_batch_get_one(&next_tok, 1);
        if (llama_decode(ctx, next_batch) != 0) {
            fprintf(stderr,
                "Runtime error: llm_complete() - decode failed mid-generation "
                "(ran out of context)\n");
            break;
        }
    }

    llama_sampler_free(sampler);
    free(tokens);

    *result = value_string(out); /* value_string() strdup()s - safe to free below */
    free(out);
}

void fn_llm_free(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;

    int slot = llm_resolve_slot(&args[0]);
    if (slot < 0) return;

    llama_free(g_llm_handles[slot].ctx);
    llama_model_free(g_llm_handles[slot].model);
    g_llm_handles[slot].model = NULL;
    g_llm_handles[slot].ctx   = NULL;
}

void llm_register_all(Environment *env) {
    env_define(env, "llm_load",     value_native("llm_load",     fn_llm_load));
    env_define(env, "llm_complete", value_native("llm_complete", fn_llm_complete));
    env_define(env, "llm_free",     value_native("llm_free",     fn_llm_free));
}

void llm_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "llm_load",     fn_llm_load);
    vm_global_set_native(vm, "llm_complete", fn_llm_complete);
    vm_global_set_native(vm, "llm_free",     fn_llm_free);
}
