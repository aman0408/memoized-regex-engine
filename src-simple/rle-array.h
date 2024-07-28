#ifndef RLE_ARRAY_H
#define RLE_ARRAY_H

typedef struct RLEVector RLEVector;
typedef struct RLEData RLEData;

RLEVector *
RLEVector_create(int size, int runLength);

int RLEVector_get(RLEVector *vec, int index);

void RLEVector_set(RLEVector *vec, int index);

void RLEVector_print(RLEVector *vec);

/* Size of the runs in use */
int RLEVector_runSize(RLEVector *vec);

/* In runs, not entries */
int RLEVector_currSize(RLEVector *vec);

/* In runs, not entries */
int RLEVector_maxObservedSize(RLEVector *vec);

// How many bytes to represent this RLE vector at its peak?
int RLEVector_maxBytes(RLEVector *vec);

void RLEVector_destroy(RLEVector *vec);
int RLEVector_runSize(RLEVector *vec);
void encode_rle(RLEVector *vec, int *decoded);
void decode_rle(RLEVector *vec, int *decoded);
#endif