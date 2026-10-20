#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tesseract/capi.h>
#include "vision_lib.h"
#include "ocr_lib.h"

/* ===========================================================================
 * Engine slots
 *
 * Same pattern as vision_cascade.c's HaarCascade table: a small fixed-size
 * array of native handles, addressed from Khan by index. Each Khan-visible
 * "engine" map just carries its slot number back to us.
 * ========================================================================= */

#define OCR_MAX_HANDLES 8

static TessBaseAPI    *g_ocr_handles[OCR_MAX_HANDLES];
static int              g_ocr_last_conf[OCR_MAX_HANDLES];
static TessPageSegMode  g_ocr_psm[OCR_MAX_HANDLES]; /* last PSM the *caller*
    asked for, tracked so ocr_detect_orientation() can put it back the way
    it found it instead of permanently leaving the engine in PSM_AUTO_OSD */

static int ocr_alloc_slot(void) {
    for (int i = 0; i < OCR_MAX_HANDLES; i++) if (!g_ocr_handles[i]) return i;
    return -1;
}

/* Resolve a Khan-level engine map back to its native slot, or -1 if the
 * argument isn't a live engine (wrong type, freed already, corrupt id). */
static int ocr_resolve_slot(Value *engine) {
    if (engine->type != VAL_MAP) return -1;
    Value *idv = map_get(engine, "__ocr_id");
    if (!idv || idv->type != VAL_NUMBER) return -1;
    int slot = (int)idv->as.number;
    if (slot < 0 || slot >= OCR_MAX_HANDLES || !g_ocr_handles[slot]) return -1;
    return slot;
}

/* Tesseract's own behavior when you pass a NULL datapath isn't consistent
 * across how different packagers build libtesseract: on Ubuntu's apt
 * build it correctly falls back to /usr/share/tesseract-ocr/.../tessdata;
 * on MSYS2's mingw-w64 build (verified) it just tries "./" relative to
 * the current directory and gives up. So rather than lean on that, we
 * do our own best-effort search across the common install locations
 * before ever calling TessBaseAPIInit3, and only hand it NULL (falling
 * through to whatever Tesseract does on its own) if none of them pan
 * out either.
 *
 * Returns a pointer to a directory (valid until the next call to this
 * function - copy it if you need it to outlive that), or NULL. */
static const char *ocr_guess_datapath(const char *lang) {
    static char dir[512];
    char probe[768];

    const char *env = getenv("TESSDATA_PREFIX");
    if (env && *env) return env;

    static const char *candidates[] = {
#ifdef _WIN32
        "C:/msys64/mingw64/share/tessdata",
        "C:/Program Files/Tesseract-OCR/tessdata",
#elif defined(__APPLE__)
        "/opt/homebrew/share/tessdata",
        "/usr/local/share/tessdata",
#else
        "/usr/share/tesseract-ocr/5/tessdata",
        "/usr/share/tesseract-ocr/4.00/tessdata",
        "/usr/share/tessdata",
#endif
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        snprintf(probe, sizeof(probe), "%s/%s.traineddata", candidates[i], lang);
        FILE *f = fopen(probe, "rb");
        if (f) {
            fclose(f);
            snprintf(dir, sizeof(dir), "%s", candidates[i]);
            return dir;
        }
    }
    return NULL;
}

/* ===========================================================================
 * Native functions
 * ========================================================================= */

void fn_ocr_init(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();

    const char *lang = "eng";
    if (argc >= 1) {
        if (args[0].type != VAL_STRING) return;
        lang = args[0].as.string;
    }
    const char *datapath = NULL;
    if (argc >= 2) {
        if (args[1].type != VAL_STRING) return;
        datapath = args[1].as.string;
    } else {
        datapath = ocr_guess_datapath(lang);
    }

    int slot = ocr_alloc_slot();
    if (slot < 0) {
        fprintf(stderr,
            "Runtime error: ocr_init() - too many open OCR engines (max %d); "
            "call ocr_free() on ones you're done with\n", OCR_MAX_HANDLES);
        return;
    }

    TessBaseAPI *handle = TessBaseAPICreate();
    if (!handle) return;

    if (TessBaseAPIInit3(handle, datapath, lang) != 0) {
        fprintf(stderr,
            "Runtime error: ocr_init(\"%s\") failed - couldn't find trained "
            "language data. Pass an explicit datapath as the 2nd argument, "
            "or install the language pack (e.g. tesseract-ocr-%s). "
            "See docs/ocr.md.\n", lang, lang);
        TessBaseAPIDelete(handle);
        return;
    }

    g_ocr_handles[slot]   = handle;
    g_ocr_last_conf[slot] = -1;
    g_ocr_psm[slot]       = PSM_AUTO; /* Tesseract's own real default */

    Value m = value_map_empty();
    map_set(&m, "__ocr_id", value_number(slot));
    map_set(&m, "lang", value_string(lang));
    *result = m;
}

void fn_ocr_recognize(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    int img_slot = vision_internal_get_slot(args[1]);
    if (img_slot < 0) return;

    unsigned char *data = vision_internal_data(img_slot);
    int w = vision_internal_width(img_slot);
    int h = vision_internal_height(img_slot);
    int c = vision_internal_channels(img_slot);
    if (!data || w <= 0 || h <= 0) return;
    if (c != 1 && c != 3 && c != 4) {
        fprintf(stderr,
            "Runtime error: ocr_recognize() - unsupported channel count %d "
            "(need 1 [grayscale], 3 [RGB], or 4 [RGBA]); try "
            "vision_grayscale(image) first\n", c);
        return;
    }

    TessBaseAPI *handle = g_ocr_handles[slot];
    TessBaseAPISetImage(handle, data, w, h, c, w * c);

    char *text = TessBaseAPIGetUTF8Text(handle);
    g_ocr_last_conf[slot] = TessBaseAPIMeanTextConf(handle);
    if (!text) return;

    *result = value_string(text);
    TessDeleteText(text);
}

void fn_ocr_confidence(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;
    if (g_ocr_last_conf[slot] < 0) return; /* recognize() hasn't run yet */
    *result = value_number(g_ocr_last_conf[slot]);
}

void fn_ocr_set_page_seg_mode(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[1].type != VAL_NUMBER) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    int mode = (int)args[1].as.number;
    if (mode < 0 || mode > 13) return; /* PSM_OSD_ONLY..PSM_COUNT-1 */
    TessBaseAPISetPageSegMode(g_ocr_handles[slot], (TessPageSegMode)mode);
    g_ocr_psm[slot] = (TessPageSegMode)mode;
}

void fn_ocr_free(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    TessBaseAPIEnd(g_ocr_handles[slot]);
    TessBaseAPIDelete(g_ocr_handles[slot]);
    g_ocr_handles[slot]   = NULL;
    g_ocr_last_conf[slot] = -1;
}

/* ocr_recognize_words(engine, image) -> array of {"text","confidence",
 * "x","y","width","height"} maps, one per recognized word - same
 * SetImage input as ocr_recognize(), but reads Tesseract's per-word
 * iterator instead of the single flattened text blob. */
void fn_ocr_recognize_words(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_array(NULL, 0);
    if (argc < 2) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    int img_slot = vision_internal_get_slot(args[1]);
    if (img_slot < 0) return;

    unsigned char *data = vision_internal_data(img_slot);
    int w = vision_internal_width(img_slot);
    int h = vision_internal_height(img_slot);
    int c = vision_internal_channels(img_slot);
    if (!data || w <= 0 || h <= 0) return;
    if (c != 1 && c != 3 && c != 4) {
        fprintf(stderr,
            "Runtime error: ocr_recognize_words() - unsupported channel "
            "count %d (need 1, 3, or 4); try vision_grayscale(image) first\n", c);
        return;
    }

    TessBaseAPI *handle = g_ocr_handles[slot];
    TessBaseAPISetImage(handle, data, w, h, c, w * c);
    TessBaseAPIRecognize(handle, NULL);
    g_ocr_last_conf[slot] = TessBaseAPIMeanTextConf(handle);

    TessResultIterator *ri = TessBaseAPIGetIterator(handle);
    if (!ri) return; /* no text found at all - empty array is correct */
    TessPageIterator *pi = TessResultIteratorGetPageIterator(ri);

    int capacity = 16, count = 0;
    Value *items = malloc(sizeof(Value) * (size_t)capacity);
    if (!items) { TessResultIteratorDelete(ri); return; }

    do {
        char *word = TessResultIteratorGetUTF8Text(ri, RIL_WORD);
        if (!word) continue;

        float conf = TessResultIteratorConfidence(ri, RIL_WORD);
        int left = 0, top = 0, right = 0, bottom = 0;
        TessPageIteratorBoundingBox(pi, RIL_WORD, &left, &top, &right, &bottom);

        if (count >= capacity) {
            capacity *= 2;
            Value *grown = realloc(items, sizeof(Value) * (size_t)capacity);
            if (!grown) { TessDeleteText(word); break; }
            items = grown;
        }

        Value m = value_map_empty();
        map_set(&m, "text", value_string(word));
        map_set(&m, "confidence", value_number(conf));
        map_set(&m, "x", value_number(left));
        map_set(&m, "y", value_number(top));
        map_set(&m, "width", value_number(right - left));
        map_set(&m, "height", value_number(bottom - top));
        items[count++] = m;

        TessDeleteText(word);
    } while (TessPageIteratorNext(pi, RIL_WORD));

    TessResultIteratorDelete(ri); /* also releases its page-iterator view -
                                   * don't separately delete pi here */

    Value arr = value_array(NULL, 0);
    arr.as.obj->as.array.items    = items;
    arr.as.obj->as.array.count    = count;
    arr.as.obj->as.array.capacity = capacity;
    *result = arr;
}

/* ocr_detect_orientation(engine, image) -> {"orientation","correction_degrees"}
 * or nil. correction_degrees is in vision_rotate()'s clockwise-positive
 * convention - pass it straight through: vision_rotate(image,
 * result["correction_degrees"]). (History: vision_rotate() briefly had
 * a sign bug where its actual behavior didn't match that documented
 * convention for 90-degree corrections; this mapping was calibrated
 * against the bug at the time, then recalibrated back to the documented
 * convention once vision_cv.c's fn_vision_rotate itself was fixed.
 * Verified against real rotated images, all four orientations, both
 * before and after that fix.)
 *
 * Needs a real block of text to be confident (a few words in a corner
 * won't do it) - returns nil rather than guessing when there isn't enough.
 * Uses the *modern* LSTM-compatible path (PSM_AUTO_OSD + AnalyseLayout);
 * deliberately not TessBaseAPIDetectOrientationScript, which depends on
 * the legacy engine's data and segfaults against LSTM-only trained data
 * (confirmed against a real build - not a hypothetical). */
void fn_ocr_detect_orientation(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    int img_slot = vision_internal_get_slot(args[1]);
    if (img_slot < 0) return;

    unsigned char *data = vision_internal_data(img_slot);
    int w = vision_internal_width(img_slot);
    int h = vision_internal_height(img_slot);
    int c = vision_internal_channels(img_slot);
    if (!data || w <= 0 || h <= 0) return;
    if (c != 1 && c != 3 && c != 4) return;

    TessBaseAPI *handle = g_ocr_handles[slot];
    TessPageSegMode saved_psm = g_ocr_psm[slot];
    TessBaseAPISetPageSegMode(handle, PSM_AUTO_OSD);
    TessBaseAPISetImage(handle, data, w, h, c, w * c);
    TessBaseAPIRecognize(handle, NULL);

    TessPageIterator *pi = TessBaseAPIAnalyseLayout(handle);
    if (!pi) {
        TessBaseAPISetPageSegMode(handle, saved_psm); /* restore even on early exit */
        return; /* not enough text on the page to tell confidently */
    }

    TessOrientation orientation;
    TessWritingDirection dir;
    TessTextlineOrder order;
    float deskew = 0;
    TessPageIteratorOrientation(pi, &orientation, &dir, &order, &deskew);
    TessPageIteratorDelete(pi); /* AnalyseLayout's iterator IS independently
                                 * owned, unlike GetPageIterator()'s - this
                                 * one really does need its own delete */
    (void)dir; (void)order; (void)deskew;

    TessBaseAPISetPageSegMode(handle, saved_psm); /* put it back the way it was */

    int correction = 0;
    const char *name = "up";
    switch (orientation) {
        case ORIENTATION_PAGE_UP:    correction = 0;   name = "up";    break;
        case ORIENTATION_PAGE_RIGHT: correction = -90; name = "right"; break;
        case ORIENTATION_PAGE_DOWN:  correction = 180; name = "down";  break;
        case ORIENTATION_PAGE_LEFT:  correction = 90;  name = "left";  break;
    }

    Value m = value_map_empty();
    map_set(&m, "orientation", value_string(name));
    map_set(&m, "correction_degrees", value_number(correction));
    *result = m;
}

/* ocr_set_char_whitelist(engine, chars) -> nil. Restricts recognition to
 * only the given characters (e.g. "0123456789" for a serial number field).
 * Pass "" to clear a previously-set whitelist. */
void fn_ocr_set_char_whitelist(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[1].type != VAL_STRING) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    TessBaseAPISetVariable(g_ocr_handles[slot], "tessedit_char_whitelist", args[1].as.string);
}

/* ocr_save_pdf(engine, image_path, output_base_path) -> bool. Renders a
 * searchable PDF: the original image, plus an invisible text layer over
 * it, so the result looks identical but its text is selectable/
 * copyable/searchable. output_base_path should NOT include ".pdf" -
 * Tesseract appends it.
 *
 * Unlike every other function here, this one takes a file PATH rather
 * than an already vision_load()-ed image: TessBaseAPIProcessPages loads
 * the file itself (through Tesseract's own Leptonica-linked I/O), which
 * is what lets this avoid pulling Leptonica's PIX type into Khan's side
 * of the bridge at all. */
void fn_ocr_save_pdf(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 3) return;
    if (args[1].type != VAL_STRING || args[2].type != VAL_STRING) return;

    int slot = ocr_resolve_slot(&args[0]);
    if (slot < 0) return;

    const char *image_path  = args[1].as.string;
    const char *output_base = args[2].as.string;

    Value *langv = map_get(&args[0], "lang");
    const char *lang = (langv && langv->type == VAL_STRING) ? langv->as.string : "eng";
    const char *datadir = ocr_guess_datapath(lang); /* pdf.ttf lives here too */

    TessBaseAPI *handle = g_ocr_handles[slot];

    TessResultRenderer *renderer = TessPDFRendererCreate(output_base, datadir, 0);
    if (!renderer) return;

    if (!TessResultRendererBeginDocument(renderer, "Khan OCR")) {
        TessDeleteResultRenderer(renderer);
        return;
    }

    BOOL ok = TessBaseAPIProcessPages(handle, image_path, NULL, 0, renderer);
    TessResultRendererEndDocument(renderer);
    TessDeleteResultRenderer(renderer);

    *result = value_bool(ok ? 1 : 0);
}

/* ===========================================================================
 * Registration
 * ========================================================================= */

void ocr_register_all(Environment *env) {
    env_define(env, "ocr_init",               value_native("ocr_init",               fn_ocr_init));
    env_define(env, "ocr_recognize",          value_native("ocr_recognize",          fn_ocr_recognize));
    env_define(env, "ocr_recognize_words",    value_native("ocr_recognize_words",    fn_ocr_recognize_words));
    env_define(env, "ocr_confidence",         value_native("ocr_confidence",         fn_ocr_confidence));
    env_define(env, "ocr_detect_orientation", value_native("ocr_detect_orientation", fn_ocr_detect_orientation));
    env_define(env, "ocr_set_page_seg_mode",  value_native("ocr_set_page_seg_mode",  fn_ocr_set_page_seg_mode));
    env_define(env, "ocr_set_char_whitelist", value_native("ocr_set_char_whitelist", fn_ocr_set_char_whitelist));
    env_define(env, "ocr_save_pdf",           value_native("ocr_save_pdf",           fn_ocr_save_pdf));
    env_define(env, "ocr_free",               value_native("ocr_free",               fn_ocr_free));
}

void ocr_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "ocr_init",               fn_ocr_init);
    vm_global_set_native(vm, "ocr_recognize",          fn_ocr_recognize);
    vm_global_set_native(vm, "ocr_recognize_words",    fn_ocr_recognize_words);
    vm_global_set_native(vm, "ocr_confidence",         fn_ocr_confidence);
    vm_global_set_native(vm, "ocr_detect_orientation", fn_ocr_detect_orientation);
    vm_global_set_native(vm, "ocr_set_page_seg_mode",  fn_ocr_set_page_seg_mode);
    vm_global_set_native(vm, "ocr_set_char_whitelist", fn_ocr_set_char_whitelist);
    vm_global_set_native(vm, "ocr_save_pdf",           fn_ocr_save_pdf);
    vm_global_set_native(vm, "ocr_free",               fn_ocr_free);
}
