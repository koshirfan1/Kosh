#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "chunk.h" /* for KhanClosure — VM closure capture, see value_copy/value_free */

/* ---------------------------------------------------------------------
 * Cached string length (SDS-style hidden header)
 * ---------------------------------------------------------------------
 * VAL_STRING stores a bare `const char *` (see interpreter.h) that every
 * read site across the codebase (85 direct `.as.string` reads, checked)
 * treats as an ordinary null-terminated C string — printf("%s"), strcmp,
 * indexing, all of it. Changing that field's type would touch dozens of
 * files for no real gain, since none of those sites need the length.
 *
 * The one place that DOES need the length repeatedly is OP_ADD's string
 * branch in vm.c, which used to call strlen() on the *accumulated*
 * string on every "s = s + x" — O(length) per call, O(N^2) total across
 * a loop of N concatenations (see benchmarks/RESULTS.md's string_concat
 * writeup for the measured 80x blowup at 100k iterations).
 *
 * Fix: every string is allocated with a hidden `size_t` length header
 * placed immediately before the bytes the returned pointer points at
 * (the classic "SDS" trick). `v.as.string` still points straight at the
 * string data — every existing read site keeps working unmodified — but
 * str_length() can now recover the length in O(1) instead of scanning.
 * Construction funnels through exactly two functions (value_string,
 * value_copy) and destruction through exactly one (value_free) — all
 * three are in this file, verified by grepping the whole codebase for
 * any other `.as.string =` assignment or `free(...as.string...)` call
 * (there are none), so this is a fully self-contained change.
 * ------------------------------------------------------------------- */

static char *str_alloc(const char *s, size_t len) {
    char *base = malloc(sizeof(size_t) + len + 1);
    *(size_t *)base = len;
    char *data = base + sizeof(size_t);
    memcpy(data, s, len);
    data[len] = '\0';
    return data;
}

size_t str_length(const char *s) {
    return ((const size_t *)s)[-1];
}

/* Every GC_AUTO_INTERVAL array/map allocations, run one cycle-collector
 * pass automatically — see gc_collect_cycles()'s own comment for why
 * this is safe to call unconditionally, including when there's
 * nothing to collect. 4096 is a starting point, not a carefully tuned
 * constant — fine to revisit if a workload profile ever calls for it. */
#define GC_AUTO_INTERVAL 4096
static long gc_alloc_counter = 0;

static Obj *obj_new(ValueType type) {
    Obj *obj = malloc(sizeof(Obj));
    obj->type = type;
    obj->ref_count = 1;
    obj->color = 0;     /* GC_COLOR_BLACK — see cycle collector section below */
    obj->buffered = 0;
    obj->class_name = NULL;
    obj->class_ref = NULL;
    obj->super_ref = NULL;
    if (++gc_alloc_counter >= GC_AUTO_INTERVAL) {
        gc_alloc_counter = 0;
        gc_collect_cycles();
    }
    return obj;
}

Value value_number(double n) {
    Value v; v.type = VAL_NUMBER; v.as.number = n; return v;
}

Value value_bool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b; return v;
}

Value value_nil(void) {
    Value v; v.type = VAL_NIL; v.as.number = 0; return v;
}

Value value_string(const char *s) {
    Value v;
    v.type = VAL_STRING;
    v.as.string = str_alloc(s, strlen(s));
    return v;
}

Value value_string_concat(const char *a, size_t la, const char *b, size_t lb) {
    Value v;
    v.type = VAL_STRING;
    char *base = malloc(sizeof(size_t) + la + lb + 1);
    *(size_t *)base = la + lb;
    char *data = base + sizeof(size_t);
    memcpy(data, a, la);
    memcpy(data + la, b, lb);
    data[la + lb] = '\0';
    v.as.string = data;
    return v;
}

Value value_array(Value *items, int count) {
    Value v;
    v.type = VAL_ARRAY;
    v.as.obj = obj_new(VAL_ARRAY);
    v.as.obj->as.array.items = items;
    v.as.obj->as.array.count = count;
    v.as.obj->as.array.capacity = count;
    return v;
}

Value value_map_empty(void) {
    Value v;
    v.type = VAL_MAP;
    v.as.obj = obj_new(VAL_MAP);
    v.as.obj->as.map.entries = NULL;
    v.as.obj->as.map.count = 0;
    v.as.obj->as.map.capacity = 0;
    v.as.obj->as.map.hash_index = NULL;
    v.as.obj->as.map.hash_capacity = 0;
    return v;
}

Value value_class_new(const char *name) {
    Value v;
    v.type = VAL_CLASS;
    v.as.obj = obj_new(VAL_CLASS);
    v.as.obj->class_name = strdup(name);
    v.as.obj->as.map.entries = NULL;
    v.as.obj->as.map.count = 0;
    v.as.obj->as.map.capacity = 0;
    v.as.obj->as.map.hash_index = NULL;
    v.as.obj->as.map.hash_capacity = 0;
    return v;
}

Value value_instance_new(Value klass) {
    Value v;
    v.type = VAL_INSTANCE;
    v.as.obj = obj_new(VAL_INSTANCE);
    v.as.obj->class_ref = klass.as.obj;
    klass.as.obj->ref_count++;   /* instance keeps its class alive */
    v.as.obj->as.map.entries = NULL;
    v.as.obj->as.map.count = 0;
    v.as.obj->as.map.capacity = 0;
    v.as.obj->as.map.hash_index = NULL;
    v.as.obj->as.map.hash_capacity = 0;
    return v;
}

Value value_native(const char *name, NativeFn fn) {
    Value v;
    v.type = VAL_NATIVE;
    v.as.native.name = strdup(name);
    v.as.native.function = fn;
    return v;
}

Value value_function(const char *name, Environment *closure,
                     AstNode *body, AstNodeList *params) {
    Value v;
    v.type = VAL_FUNCTION;
    v.as.function.name = strdup(name);
    v.as.function.closure = closure;
    v.as.function.body = body;
    v.as.function.params = params;
    return v;
}

Value value_copy(Value v) {
    if (v.type == VAL_ARRAY || v.type == VAL_MAP ||
        v.type == VAL_CLASS || v.type == VAL_INSTANCE) {
        if (v.as.obj) v.as.obj->ref_count++;
        return v;
    }
    if (v.type == VAL_STRING) {
        Value copy = v;
        copy.as.string = str_alloc(v.as.string, str_length(v.as.string));
        return copy;
    }
    if (v.type == VAL_FUNCTION) {
        Value copy = v;
        copy.as.function.name = strdup(v.as.function.name);
        if (copy.as.function.closure) {
            khanclosure_retain((KhanClosure *)copy.as.function.closure);
        }
        return copy;
    }
    if (v.type == VAL_NATIVE) {
        Value copy = v;
        copy.as.native.name = strdup(v.as.native.name);
        return copy;
    }
    return v;
}

/* ═══════════════════════════════════════════════════════════════════
 * Cycle collector — VAL_ARRAY / VAL_MAP only
 * ═══════════════════════════════════════════════════════════════════
 * Arrays and maps (see the ref_count fields above) are the only heap
 * values in Khan that are genuinely SHARED (value_copy increments
 * ref_count rather than deep-copying — see value_copy()) rather than
 * always independently owned. That sharing is exactly what makes a
 * cycle possible: `a = []; a[0] = a` (direct self-reference — note
 * push() specifically does NOT create this, since it builds a brand
 * new array rather than mutating in place; genuine in-place mutation
 * needs index-assignment) or an indirect cycle through two or more
 * objects leaves ref_count that never reaches zero, even once every
 * *external* reference has let go. Plain refcounting can never free
 * that — nothing ever decrements the last ref, because the cycle is
 * decrementing itself. Strings, numbers, bools, nil can't do this
 * (never shared); functions/closures use a separate refcount in
 * `chunk.c`'s `KhanClosure` and CAN in principle cycle too (a closure
 * capturing a variable that indirectly holds the same closure) but
 * that's a much rarer pattern in practice and is a documented
 * follow-up, not covered here.
 *
 * This implements Bacon & Rajan's synchronous trial-deletion cycle
 * collector (the same algorithm CPython's `gc` module uses for its
 * container types) rather than a full stop-the-world tracing GC. The
 * key advantage for this codebase specifically: trial deletion only
 * needs to walk the ARRAY/MAP OBJECT GRAPH itself (each object's own
 * children) — it never needs to know about the VM's stack, call
 * frames, globals table, upvalues, or the try/catch handler stack.
 * Those are the same values a normal `value_free()` already visits
 * via ordinary refcount decrements; nothing extra to enumerate as
 * "roots" in the traditional GC sense, no new integration surface
 * across vm.c/compiler.c/every native library. A full tracing
 * collector would need all of that enumerated correctly (and kept
 * correct as the VM changes) to avoid freeing something still live —
 * trial deletion sidesteps the whole problem by only ever considering
 * objects that a real decrement already touched.
 *
 * The algorithm in one paragraph: every time a decrement leaves an
 * object's ref_count above zero (so it wasn't freed the normal way),
 * that object *might* now be part of an unreachable cycle — it's
 * added to a small "possible roots" buffer instead of being examined
 * immediately (immediate per-decrement checking would be far too
 * expensive to do on every single decrement). `gc_collect_cycles()`
 * processes that whole buffer at once, in three passes: (1) MarkGray
 * — trial-decrement every object reachable from a possible root, as
 * if that root's own references didn't count, so anything still
 * "purely" cyclic ends up with a trial count of zero; (2) Scan — for
 * each object touched, if its trial count is still positive it has a
 * real external reference and everything reachable from it is
 * restored to black/live (undoing the trial decrements); anything
 * left at zero is colored white — a confirmed cycle member with no
 * outside references; (3) CollectWhite — actually free every white
 * object. Objects that survive (colored black) are completely
 * unaffected — their real ref_counts are exactly what they were
 * before collection started, this is what makes "trial" the right
 * word: every trial decrement on a survivor gets undone in Scan.
 * ------------------------------------------------------------------- */

#define GC_COLOR_BLACK  0  /* live (default/steady state) */
#define GC_COLOR_GRAY   1  /* currently being trial-traced */
#define GC_COLOR_WHITE  2  /* confirmed cycle garbage, pending free */
#define GC_COLOR_PURPLE 3  /* possible root, sitting in the buffer */

static Obj **gc_roots = NULL;
static int   gc_roots_count = 0;
static int   gc_roots_cap = 0;

/* Called from value_free() whenever a decrement leaves ref_count > 0
 * — this object *might* be cyclic garbage now, so remember it for the
 * next collection pass instead of deciding right away. `buffered`
 * guards against the same object being added twice if it gets
 * decremented-without-reaching-zero more than once before the next
 * gc_collect_cycles() call. */
static void gc_possible_root(Obj *o) {
    o->color = GC_COLOR_PURPLE;
    if (o->buffered) return;
    o->buffered = 1;
    if (gc_roots_count >= gc_roots_cap) {
        gc_roots_cap = gc_roots_cap ? gc_roots_cap * 2 : 64;
        gc_roots = realloc(gc_roots, sizeof(Obj *) * gc_roots_cap);
    }
    gc_roots[gc_roots_count++] = o;
}

/* Called from value_free() right before an object is genuinely freed
 * (ref_count reached 0 the normal way). If it's currently sitting in
 * the roots buffer, that slot must be neutralized — otherwise a later
 * gc_collect_cycles() pass would dereference a dangling pointer.
 * Scanning the whole buffer on every real free sounds expensive, but
 * the buffer only ever holds objects that survived a decrement above
 * zero — in practice a small minority of all frees — and gets fully
 * drained every collection pass, so it never grows unbounded between
 * collections. NULL slots left behind here are skipped by every pass
 * below. */
static void gc_unbuffer(Obj *o) {
    if (!o->buffered) return;
    for (int i = 0; i < gc_roots_count; i++) {
        if (gc_roots[i] == o) { gc_roots[i] = NULL; break; }
    }
    o->buffered = 0;
}

/* Applies `fn` to every VAL_ARRAY/VAL_MAP child of `o`. Scalars,
 * strings, functions etc. are skipped — they can't be part of a
 * reference cycle (see the section header). */
static void gc_for_each_child(Obj *o, void (*fn)(Obj *)) {
    if (o->type == VAL_ARRAY) {
        for (int i = 0; i < o->as.array.count; i++) {
            Value *child = &o->as.array.items[i];
            if ((child->type == VAL_ARRAY || child->type == VAL_MAP) && child->as.obj) {
                fn(child->as.obj);
            }
        }
    } else { /* VAL_MAP */
        for (int i = 0; i < o->as.map.count; i++) {
            Value *child = o->as.map.entries[i].value;
            if ((child->type == VAL_ARRAY || child->type == VAL_MAP) && child->as.obj) {
                fn(child->as.obj);
            }
        }
    }
}

static void gc_trial_decrement(Obj *o) { o->ref_count--; }

static void gc_mark_gray(Obj *o) {
    if (o->color == GC_COLOR_GRAY) return;
    o->color = GC_COLOR_GRAY;
    gc_for_each_child(o, gc_trial_decrement);
    /* Recurse into every child unconditionally (not just ones whose
       trial count just hit zero) — a child might have other live
       incoming edges from outside this trace and still need its own
       ref_count trial-decremented once per edge from within it. The
       gray check above is what stops this from looping forever on a
       cycle (including a direct self-reference, where a child IS o
       itself — the very first line of this function catches that on
       the recursive re-entry). */
    if (o->type == VAL_ARRAY) {
        for (int i = 0; i < o->as.array.count; i++) {
            Value *c = &o->as.array.items[i];
            if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) gc_mark_gray(c->as.obj);
        }
    } else {
        for (int i = 0; i < o->as.map.count; i++) {
            Value *c = o->as.map.entries[i].value;
            if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) gc_mark_gray(c->as.obj);
        }
    }
}

static void gc_scan_black(Obj *o) {
    o->color = GC_COLOR_BLACK;
    if (o->type == VAL_ARRAY) {
        for (int i = 0; i < o->as.array.count; i++) {
            Value *c = &o->as.array.items[i];
            if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) {
                c->as.obj->ref_count++;
                if (c->as.obj->color != GC_COLOR_BLACK) gc_scan_black(c->as.obj);
            }
        }
    } else {
        for (int i = 0; i < o->as.map.count; i++) {
            Value *c = o->as.map.entries[i].value;
            if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) {
                c->as.obj->ref_count++;
                if (c->as.obj->color != GC_COLOR_BLACK) gc_scan_black(c->as.obj);
            }
        }
    }
}

static void gc_scan(Obj *o) {
    if (o->color != GC_COLOR_GRAY) return;
    if (o->ref_count > 0) {
        gc_scan_black(o);
    } else {
        o->color = GC_COLOR_WHITE;
        if (o->type == VAL_ARRAY) {
            for (int i = 0; i < o->as.array.count; i++) {
                Value *c = &o->as.array.items[i];
                if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) gc_scan(c->as.obj);
            }
        } else {
            for (int i = 0; i < o->as.map.count; i++) {
                Value *c = o->as.map.entries[i].value;
                if ((c->type == VAL_ARRAY || c->type == VAL_MAP) && c->as.obj) gc_scan(c->as.obj);
            }
        }
    }
}

/* Frees the container structure itself (items array / map entries +
 * hash index, and the Obj). Every CHILD edge is resolved one of two
 * ways, and getting this distinction right is the crux of the whole
 * algorithm's safety:
 *
 * - If a child is ALSO white (Scan's verdict — see below for why this
 *   check stays trustworthy here), it's part of the SAME garbage
 *   batch: recurse into gc_collect_white() for it, guarded against
 *   revisiting via `buffered` (see next paragraph). No ref_count
 *   decrement — gc_mark_gray() already trial-decremented that exact
 *   edge and Scan already confirmed the result was zero.
 * - Otherwise (child is black — Scan proved it has a real reference
 *   from *outside* this cycle, e.g. a still-live global variable) this
 *   object being destroyed is still giving up one genuine reference to
 *   it, so that edge needs a REAL value_free()/decrement — the child
 *   survives, it just loses this one incoming ref.
 *
 * `buffered` (not `color`) is the reentrancy guard against visiting
 * the same object twice within this free pass — critical for a direct
 * self-reference (`a[0] = a`) or any multi-object cycle, where an
 * object is reachable from more than one edge in the same batch.
 * `color` is deliberately left untouched by this whole function so
 * the white/black check above stays accurate for every remaining edge
 * no matter what order objects are freed in. An earlier version of
 * this function used `color` itself as the reentrancy guard (flipping
 * it to black on entry) — that's what `buffered` is for instead now:
 * reusing `color` collided with the "is this child black-and-live"
 * check above, so a self-referencing array crashed with a stack
 * overflow (value_free recursing into itself indefinitely) the first
 * time this was tested against a real cycle, because the self-edge
 * got misread as "escaped to a live object outside the cycle" and
 * value_free() was called on the very object still being torn down.
 *
 * One more hazard the `buffered` guard alone does NOT cover: it only
 * protects re-entry *through the object graph* (following Value
 * pointers between still-live Objs). It does nothing for the RAW
 * pointer sitting in gc_collect_cycles()'s own `gc_roots` array — if
 * two different roots in the same batch turn out to be reachable from
 * each other (exactly the two-object-cycle case: `a[0]=b; b[0]=a;`),
 * processing the FIRST root can recursively free the SECOND one
 * (since it's just another white node reachable from the first), and
 * gc_roots_cycles()'s driving loop would then call gc_collect_white()
 * on that second root's now-dangling pointer when it reaches that
 * slot — straight into a use-after-free, since by that point the
 * memory doesn't exist to have a color or a buffered flag to check.
 * That's exactly what happened the first time this was tested against
 * a real two-object cycle (single self-reference alone doesn't trigger
 * it — it takes at least two roots in the same buffer where one's
 * subtree reaches the other). The fix mirrors what value_free() already
 * does for the ordinary-free path: gc_unbuffer() below scans gc_roots
 * for a matching pointer and nulls it out — called here too, right
 * before every actual free(), so gc_collect_cycles()'s own array never
 * holds a stale pointer past the point where it's freed, regardless of
 * whether that free happened via the driving loop directly or via a
 * cascade from an earlier root in the same pass. */
static void gc_collect_white(Obj *o) {
    if (o->color != GC_COLOR_WHITE) return;
    if (o->buffered) return; /* already being torn down elsewhere in this same pass */
    o->buffered = 1;
    if (o->type == VAL_ARRAY) {
        for (int i = 0; i < o->as.array.count; i++) {
            Value c = o->as.array.items[i];
            if ((c.type == VAL_ARRAY || c.type == VAL_MAP) && c.as.obj) {
                if (c.as.obj->color == GC_COLOR_WHITE) gc_collect_white(c.as.obj);
                else value_free(c);
            } else {
                value_free(c);
            }
        }
        free(o->as.array.items);
    } else {
        for (int i = 0; i < o->as.map.count; i++) {
            free((void *)o->as.map.entries[i].key);
            Value c = *o->as.map.entries[i].value;
            if ((c.type == VAL_ARRAY || c.type == VAL_MAP) && c.as.obj) {
                if (c.as.obj->color == GC_COLOR_WHITE) gc_collect_white(c.as.obj);
                else value_free(c);
            } else {
                value_free(c);
            }
            free(o->as.map.entries[i].value);
        }
        free(o->as.map.entries);
        free(o->as.map.hash_index);
    }
    gc_unbuffer(o); /* null out any gc_roots slot pointing at o before it's gone */
    free(o);
}

/* Runs one full collection pass over every object currently in the
 * possible-roots buffer. Safe to call at any time — including "never"
 * (a program with no cycles simply keeps behaving exactly as pure
 * refcounting always did; nothing in this section changes normal
 * free() timing for acyclic data). Exposed to Khan scripts as the
 * native `gc_collect()` (see khan_stdlib.c) and also called
 * automatically every GC_AUTO_INTERVAL array/map allocations — see
 * obj_new()'s counter — so long-running programs (a `webi` server
 * handling many requests, each building and discarding request-scoped
 * structures) don't need to call it manually to stay flat. */
void gc_collect_cycles(void) {
    for (int i = 0; i < gc_roots_count; i++) {
        if (gc_roots[i] && gc_roots[i]->color == GC_COLOR_PURPLE) {
            gc_mark_gray(gc_roots[i]);
        }
    }
    for (int i = 0; i < gc_roots_count; i++) {
        if (gc_roots[i]) gc_scan(gc_roots[i]);
    }
    /* CollectRoots: unbuffer everything before freeing any of it — the
       roots-buffer meaning of `buffered` ("queued") must be cleared
       before gc_collect_white() starts reusing the same field with
       its own, unrelated meaning ("currently being torn down"). */
    for (int i = 0; i < gc_roots_count; i++) {
        if (gc_roots[i]) gc_roots[i]->buffered = 0;
    }
    for (int i = 0; i < gc_roots_count; i++) {
        if (gc_roots[i]) gc_collect_white(gc_roots[i]);
    }
    gc_roots_count = 0;
}

void value_free(Value v) {
    if (v.type == VAL_ARRAY || v.type == VAL_MAP) {
        if (!v.as.obj) return;
        v.as.obj->ref_count--;
        if (v.as.obj->ref_count <= 0) {
            gc_unbuffer(v.as.obj); /* neutralize any stale roots-buffer slot before freeing */
            if (v.type == VAL_ARRAY) {
                for (int i = 0; i < v.as.obj->as.array.count; i++) {
                    value_free(v.as.obj->as.array.items[i]);
                }
                free(v.as.obj->as.array.items);
            } else {
                for (int i = 0; i < v.as.obj->as.map.count; i++) {
                    free((void*)v.as.obj->as.map.entries[i].key);
                    value_free(*v.as.obj->as.map.entries[i].value);
                    free(v.as.obj->as.map.entries[i].value);
                }
                free(v.as.obj->as.map.entries);
                free(v.as.obj->as.map.hash_index);
            }
            free(v.as.obj);
        } else {
            /* Didn't reach zero — might now be part of an unreachable
               cycle with no external references left. Remember it for
               the next gc_collect_cycles() pass instead of deciding
               right now (see the cycle-collector section above). */
            gc_possible_root(v.as.obj);
        }
    } else if (v.type == VAL_CLASS || v.type == VAL_INSTANCE) {
        /* Plain refcounting only — deliberately NOT registered with the
           cycle collector (no gc_possible_root), same documented gap as
           closures. A cycle through instance fields (a.friend = b;
           b.friend = a) will leak; everything acyclic is reclaimed. */
        if (!v.as.obj) return;
        v.as.obj->ref_count--;
        if (v.as.obj->ref_count <= 0) {
            for (int i = 0; i < v.as.obj->as.map.count; i++) {
                free((void*)v.as.obj->as.map.entries[i].key);
                value_free(*v.as.obj->as.map.entries[i].value);
                free(v.as.obj->as.map.entries[i].value);
            }
            free(v.as.obj->as.map.entries);
            free(v.as.obj->as.map.hash_index);
            if (v.type == VAL_CLASS) {
                free(v.as.obj->class_name);
                if (v.as.obj->super_ref) {
                    Value sv;
                    sv.type = VAL_CLASS;
                    sv.as.obj = v.as.obj->super_ref;
                    value_free(sv);   /* release this class's hold on its superclass */
                }
            }
            if (v.type == VAL_INSTANCE && v.as.obj->class_ref) {
                Value kv;
                kv.type = VAL_CLASS;
                kv.as.obj = v.as.obj->class_ref;
                value_free(kv);   /* release the instance's hold on its class */
            }
            free(v.as.obj);
        }
    } else if (v.type == VAL_STRING) {
        free((void*)(v.as.string - sizeof(size_t)));
    } else if (v.type == VAL_FUNCTION) {
        free((void*)v.as.function.name);
        if (v.as.function.closure) {
            khanclosure_release((KhanClosure *)v.as.function.closure);
        }
    } else if (v.type == VAL_NATIVE) {
        free((void*)v.as.native.name);
    }
}

/* ---------------------------------------------------------------------
 * Map hash index — open-addressing (linear probing) side table that maps
 * key -> slot in the dense `entries[]` array. `entries[]` itself stays in
 * insertion order (unchanged), so every existing caller that walks
 * AS_MAP_ENTRIES()/AS_MAP_COUNT() in a for-loop (JSON encoding, keys(),
 * header iteration, etc.) keeps working byte-for-byte the same as before.
 * This only replaces the O(n) linear scan that map_set/map_get used to do.
 *
 * No deletion exists anywhere in the codebase for maps (checked), so no
 * tombstones are needed — every index slot is either -1 (empty) or a
 * valid entries[] index.
 * ------------------------------------------------------------------- */

static unsigned long map_hash_key(const char *key) {
    /* FNV-1a */
    unsigned long h = 2166136261UL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 16777619UL;
    }
    return h;
}

static void map_hash_grow(Obj *o) {
    int old_cap = o->as.map.hash_capacity;
    int new_cap = old_cap ? old_cap * 2 : 8;
    int *new_index = malloc(sizeof(int) * new_cap);
    for (int i = 0; i < new_cap; i++) new_index[i] = -1;

    /* Rehash every existing entry (in insertion order) into the new table. */
    unsigned long mask = (unsigned long)(new_cap - 1);
    for (int i = 0; i < o->as.map.count; i++) {
        unsigned long h = map_hash_key(o->as.map.entries[i].key) & mask;
        while (new_index[h] != -1) {
            h = (h + 1) & mask;
        }
        new_index[h] = i;
    }

    free(o->as.map.hash_index);
    o->as.map.hash_index = new_index;
    o->as.map.hash_capacity = new_cap;
}

/* Returns the entries[] slot index for `key`, or -1 if absent.
 * If `out_probe` is non-NULL, also writes the empty slot where `key`
 * should be inserted (only meaningful when the return value is -1). */
static int map_hash_find(Obj *o, const char *key, unsigned long *out_probe) {
    if (o->as.map.hash_capacity == 0) {
        if (out_probe) *out_probe = 0;
        return -1;
    }
    unsigned long mask = (unsigned long)(o->as.map.hash_capacity - 1);
    unsigned long h = map_hash_key(key) & mask;
    while (o->as.map.hash_index[h] != -1) {
        int slot = o->as.map.hash_index[h];
        if (strcmp(o->as.map.entries[slot].key, key) == 0) {
            return slot;
        }
        h = (h + 1) & mask;
    }
    if (out_probe) *out_probe = h;
    return -1;
}

void map_set(Value *map, const char *key, Value value) {
    /* VAL_CLASS (method table) and VAL_INSTANCE (fields) store their
       entries in as.map with the exact same layout as a VAL_MAP. */
    if ((map->type != VAL_MAP && map->type != VAL_CLASS &&
         map->type != VAL_INSTANCE) || !map->as.obj) return;
    Obj *o = map->as.obj;

    /* Grow the hash table before probing if we're at/over ~70% load,
     * so map_hash_find always has room to find an empty slot. */
    if (o->as.map.hash_capacity == 0 ||
        (o->as.map.count + 1) * 10 >= o->as.map.hash_capacity * 7) {
        map_hash_grow(o);
    }

    unsigned long probe;
    int existing = map_hash_find(o, key, &probe);
    if (existing != -1) {
        value_free(*o->as.map.entries[existing].value);
        *o->as.map.entries[existing].value = value;
        return;
    }

    if (o->as.map.count >= o->as.map.capacity) {
        o->as.map.capacity = o->as.map.capacity ? o->as.map.capacity * 2 : 4;
        o->as.map.entries = realloc(o->as.map.entries, sizeof(MapEntry) * o->as.map.capacity);
    }
    int new_slot = o->as.map.count;
    o->as.map.entries[new_slot].key = strdup(key);
    o->as.map.entries[new_slot].value = malloc(sizeof(Value));
    *o->as.map.entries[new_slot].value = value;
    o->as.map.count++;

    /* `probe` was computed just above by map_hash_find on the current
     * (just-possibly-grown) table, so it's still a valid empty slot. */
    o->as.map.hash_index[probe] = new_slot;
}

Value *map_get(Value *map, const char *key) {
    if ((map->type != VAL_MAP && map->type != VAL_CLASS &&
         map->type != VAL_INSTANCE) || !map->as.obj) return NULL;
    Obj *o = map->as.obj;
    int slot = map_hash_find(o, key, NULL);
    if (slot == -1) return NULL;
    return o->as.map.entries[slot].value;
}

void value_print(Value v) {
    switch (v.type) {
        case VAL_NUMBER: {
            double n = v.as.number;
            if (n == (long long)n) printf("%lld", (long long)n);
            else                   printf("%g", n);
            break;
        }
        case VAL_STRING:  printf("%s", v.as.string); break;
        case VAL_BOOL:    printf("%s", v.as.boolean ? "true" : "false"); break;
        case VAL_NIL:     printf("nil"); break;
        case VAL_ARRAY: {
            printf("[");
            if (v.as.obj) {
                for (int i = 0; i < v.as.obj->as.array.count; i++) {
                    if (i > 0) printf(", ");
                    value_print(v.as.obj->as.array.items[i]);
                }
            }
            printf("]");
            break;
        }
        case VAL_MAP: {
            printf("{");
            if (v.as.obj) {
                for (int i = 0; i < v.as.obj->as.map.count; i++) {
                    if (i > 0) printf(", ");
                    printf("\"%s\": ", v.as.obj->as.map.entries[i].key);
                    value_print(*v.as.obj->as.map.entries[i].value);
                }
            }
            printf("}");
            break;
        }
        case VAL_FUNCTION: printf("<fn %s>", v.as.function.name); break;
        case VAL_NATIVE:   printf("<native %s>", v.as.native.name); break;
        case VAL_CLASS:
            printf("<class %s>", (v.as.obj && v.as.obj->class_name) ? v.as.obj->class_name : "?");
            break;
        case VAL_INSTANCE:
            printf("<%s instance>",
                   (v.as.obj && v.as.obj->class_ref && v.as.obj->class_ref->class_name)
                       ? v.as.obj->class_ref->class_name : "?");
            break;
    }
}

int vm_is_truthy(Value v) {
    if (v.type == VAL_NIL) return 0;
    if (v.type == VAL_BOOL) return v.as.boolean;
    if (v.type == VAL_NUMBER) return v.as.number != 0;
    if (v.type == VAL_STRING) return v.as.string && v.as.string[0] != '\0';
    return 1;
}

int vm_values_equal(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_BOOL:   return a.as.boolean == b.as.boolean;
        case VAL_NUMBER: return a.as.number == b.as.number;
        case VAL_STRING: return strcmp(a.as.string, b.as.string) == 0;
        case VAL_NIL:    return 1;
        case VAL_ARRAY:  return a.as.obj == b.as.obj;
        case VAL_MAP:    return a.as.obj == b.as.obj;
        case VAL_CLASS:
        case VAL_INSTANCE: return a.as.obj == b.as.obj;   /* identity */
        default: return 0;
    }
}

void vm_print_value(Value v) {
    value_print(v);
}
