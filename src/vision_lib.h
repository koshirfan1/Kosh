#ifndef KHAN_VISION_LIB_H
#define KHAN_VISION_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * vision_lib — real image I/O and pixel-level processing for Khan.
 *
 * Backed by stb_image.h / stb_image_write.h (public domain, vendored in
 * src/). Supports loading PNG/JPEG/BMP/GIF/TGA/PSD/HDR and saving to
 * PNG/BMP/JPEG/TGA.
 *
 * An "image" at the Khan level is a plain map:
 *   {"__vision_id": <handle>, "width": w, "height": h, "channels": c}
 * The actual pixel buffer lives in a native-side table indexed by
 * __vision_id — it is NOT copied into a Khan array, since a single
 * photo can be tens of millions of bytes and Khan arrays box every
 * element as a Value. Always call vision_free() on images you no
 * longer need; the native buffer is not garbage collected.
 *
 * Registers:
 *   vision_load(path)                    -> image or nil
 *   vision_new(width, height, channels)  -> blank (black) image or nil
 *   vision_free(image)                   -> nil
 *   vision_save(image, path)             -> bool (format from extension)
 *   vision_get_pixel(image, x, y)        -> {"r","g","b"[,"a"]} or nil
 *   vision_set_pixel(image, x, y, r,g,b) -> bool
 *   vision_grayscale(image)              -> new image
 *   vision_invert(image)                 -> new image
 *   vision_crop(image, x, y, w, h)       -> new image or nil
 *   vision_resize(image, w, h)           -> new image (nearest-neighbor)
 *   vision_flip_h(image)                 -> new image
 *   vision_flip_v(image)                 -> new image
 *   vision_average_brightness(image)     -> number 0-255
 *   vision_brightness_stats(image)       -> {"min","max","average"} or nil
 *   vision_is_grayscale(image, tolerance)-> bool
 *
 * NOT included: face/object detection. Doing that for real needs a
 * trained model (e.g. a Haar cascade or a small CNN) which is out of
 * scope for a vendored single-header image library — earlier versions
 * of this package faked `detect_faces`, and this rewrite would rather
 * ship less and be honest about it than ship a hardcoded fake.
 */
void vision_register_all(Environment *env);
void vision_register_all_vm(VM *vm);

/* ---------------------------------------------------------------------------
 * Internal cross-file accessors (used by vision_cv.c, vision_cascade.c).
 * Not part of the Khan-facing API.
 * ------------------------------------------------------------------------- */
int vision_internal_get_slot(Value img_map);
unsigned char *vision_internal_data(int slot);
int vision_internal_width(int slot);
int vision_internal_height(int slot);
int vision_internal_channels(int slot);
Value vision_internal_wrap(unsigned char *data, int w, int h, int c); /* takes ownership of data */

#endif
