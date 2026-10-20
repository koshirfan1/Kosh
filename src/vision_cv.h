#ifndef KHAN_VISION_CV_H
#define KHAN_VISION_CV_H

#include "interpreter.h"
#include "vm.h"

/*
 * vision_cv — classical computer-vision operations layered on vision_lib's
 * image registry: convolution filters, thresholding, morphology, connected-
 * component blob detection, histograms, drawing primitives, and template
 * matching. All native for speed on real-sized photos.
 */
void vision_cv_register_all(Environment *env);
void vision_cv_register_all_vm(VM *vm);

#endif
