#ifndef KHAN_OCR_LIB_H
#define KHAN_OCR_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * ocr_lib — real text recognition backed by Tesseract (libtesseract, the
 * same OCR engine behind `tesseract` CLI / countless production scanners).
 *
 * This is a genuine bridge, not a mock: it links against libtesseract's C
 * API (tesseract/capi.h) and feeds it pixel buffers directly from
 * vision_lib's already-decoded image data (via vision_internal_data() and
 * friends) — no temp files, no shelling out to a CLI, no Leptonica PIX
 * plumbing needed on Khan's side.
 *
 * Requires libtesseract (+ its trained language data) to be installed on
 * the machine Khan is built on. See docs/ocr.md for install notes per
 * platform. This is a hard build-time dependency: if libtesseract-dev
 * isn't found, the build fails rather than silently degrading to a fake.
 *
 * An "ocr engine" at the Khan level is a plain map:
 *   {"__ocr_id": <handle>, "lang": <language code>}
 * The underlying TessBaseAPI instance lives in a native-side table indexed
 * by __ocr_id. Always call ocr_free() on engines you no longer need.
 *
 * Registers:
 *   ocr_init([lang], [datapath])   -> engine or nil
 *       lang defaults to "eng"; datapath overrides tessdata auto-detection
 *       (pass it if the build's default search path doesn't find your
 *       *.traineddata files — see docs/ocr.md).
 *   ocr_recognize(engine, image)   -> string or nil
 *       image must come from vision_load()/vision_new() and be 1, 3, or 4
 *       channels (grayscale, RGB, or RGBA). Preprocessing with
 *       vision_grayscale() first generally improves accuracy.
 *   ocr_confidence(engine)         -> number (0-100) or nil
 *       Mean confidence of the most recent ocr_recognize() call on this
 *       engine; nil if recognize() hasn't been called yet.
 *   ocr_free(engine)               -> nil
 */
void ocr_register_all(Environment *env);
void ocr_register_all_vm(VM *vm);

#endif
