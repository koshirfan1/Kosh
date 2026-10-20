#ifndef KHAN_VISION_CASCADE_H
#define KHAN_VISION_CASCADE_H

#include "interpreter.h"
#include "vm.h"

/*
 * vision_cascade — real Viola-Jones object detection using OpenCV-format
 * Haar cascade XML files (the same format as haarcascade_frontalface_default.xml).
 *
 * This is the one piece of `vision` that does actual detection, not just
 * pixel processing: it parses a cascade's stages/features, builds an
 * integral image, and runs a multi-scale sliding-window scan with the
 * standard cascade early-rejection.
 *
 * Registers:
 *   vision_cascade_load(path)     -> cascade or nil
 *   vision_cascade_free(cascade)  -> nil
 *   vision_detect_objects(image, cascade, scale_factor, min_neighbors, min_size)
 *       -> array of {"x","y","w","h"}
 */
void vision_cascade_register_all(Environment *env);
void vision_cascade_register_all_vm(VM *vm);

#endif
