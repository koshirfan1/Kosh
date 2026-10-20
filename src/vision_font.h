#ifndef KHAN_VISION_FONT_H
#define KHAN_VISION_FONT_H

/*
 * A compact 5x7 bitmap font — uppercase A-Z, digits 0-9, space, and a
 * handful of punctuation. Each glyph is 7 bytes; each byte's low 5 bits
 * are one row, MSB-of-the-5 on the left. Enough for labeling detection
 * results (e.g. "FACE", confidence scores) — not a general text renderer.
 */

#define VISION_FONT_WIDTH  5
#define VISION_FONT_HEIGHT 7

static const unsigned char VISION_FONT_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
static const unsigned char VISION_FONT_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
static const unsigned char VISION_FONT_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
static const unsigned char VISION_FONT_D[7] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C};
static const unsigned char VISION_FONT_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
static const unsigned char VISION_FONT_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
static const unsigned char VISION_FONT_G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E};
static const unsigned char VISION_FONT_H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
static const unsigned char VISION_FONT_I[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
static const unsigned char VISION_FONT_J[7] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C};
static const unsigned char VISION_FONT_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};
static const unsigned char VISION_FONT_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
static const unsigned char VISION_FONT_M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
static const unsigned char VISION_FONT_N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
static const unsigned char VISION_FONT_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
static const unsigned char VISION_FONT_P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
static const unsigned char VISION_FONT_Q[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
static const unsigned char VISION_FONT_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
static const unsigned char VISION_FONT_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
static const unsigned char VISION_FONT_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
static const unsigned char VISION_FONT_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
static const unsigned char VISION_FONT_V[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
static const unsigned char VISION_FONT_W[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A};
static const unsigned char VISION_FONT_X[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
static const unsigned char VISION_FONT_Y[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
static const unsigned char VISION_FONT_Z[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};

static const unsigned char VISION_FONT_0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
static const unsigned char VISION_FONT_1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
static const unsigned char VISION_FONT_2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
static const unsigned char VISION_FONT_3[7] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E};
static const unsigned char VISION_FONT_4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
static const unsigned char VISION_FONT_5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
static const unsigned char VISION_FONT_6[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
static const unsigned char VISION_FONT_7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
static const unsigned char VISION_FONT_8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
static const unsigned char VISION_FONT_9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};

static const unsigned char VISION_FONT_SPACE[7] = {0,0,0,0,0,0,0};
static const unsigned char VISION_FONT_DOT[7]   = {0,0,0,0,0,0x0C,0x0C};
static const unsigned char VISION_FONT_COMMA[7] = {0,0,0,0,0,0x0C,0x08};
static const unsigned char VISION_FONT_BANG[7]  = {0x04,0x04,0x04,0x04,0x04,0,0x04};
static const unsigned char VISION_FONT_QMARK[7] = {0x0E,0x11,0x01,0x02,0x04,0,0x04};
static const unsigned char VISION_FONT_COLON[7] = {0,0x0C,0x0C,0,0x0C,0x0C,0};
static const unsigned char VISION_FONT_DASH[7]  = {0,0,0,0x1F,0,0,0};
static const unsigned char VISION_FONT_PCT[7]   = {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13};
static const unsigned char VISION_FONT_UNK[7]   = {0x1F,0x11,0x0A,0x04,0x0A,0x11,0x1F};

static inline const unsigned char *vision_font_glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A'); /* fold to uppercase */
    switch (ch) {
        case 'A': return VISION_FONT_A; case 'B': return VISION_FONT_B;
        case 'C': return VISION_FONT_C; case 'D': return VISION_FONT_D;
        case 'E': return VISION_FONT_E; case 'F': return VISION_FONT_F;
        case 'G': return VISION_FONT_G; case 'H': return VISION_FONT_H;
        case 'I': return VISION_FONT_I; case 'J': return VISION_FONT_J;
        case 'K': return VISION_FONT_K; case 'L': return VISION_FONT_L;
        case 'M': return VISION_FONT_M; case 'N': return VISION_FONT_N;
        case 'O': return VISION_FONT_O; case 'P': return VISION_FONT_P;
        case 'Q': return VISION_FONT_Q; case 'R': return VISION_FONT_R;
        case 'S': return VISION_FONT_S; case 'T': return VISION_FONT_T;
        case 'U': return VISION_FONT_U; case 'V': return VISION_FONT_V;
        case 'W': return VISION_FONT_W; case 'X': return VISION_FONT_X;
        case 'Y': return VISION_FONT_Y; case 'Z': return VISION_FONT_Z;
        case '0': return VISION_FONT_0; case '1': return VISION_FONT_1;
        case '2': return VISION_FONT_2; case '3': return VISION_FONT_3;
        case '4': return VISION_FONT_4; case '5': return VISION_FONT_5;
        case '6': return VISION_FONT_6; case '7': return VISION_FONT_7;
        case '8': return VISION_FONT_8; case '9': return VISION_FONT_9;
        case ' ': return VISION_FONT_SPACE;
        case '.': return VISION_FONT_DOT;
        case ',': return VISION_FONT_COMMA;
        case '!': return VISION_FONT_BANG;
        case '?': return VISION_FONT_QMARK;
        case ':': return VISION_FONT_COLON;
        case '-': return VISION_FONT_DASH;
        case '%': return VISION_FONT_PCT;
        default:  return VISION_FONT_UNK;
    }
}

#endif
