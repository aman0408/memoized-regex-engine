#include <stdio.h>
#include <stdlib.h>
#include "rle-array.h"

struct RLEVector
{
  int currNumOfRuns;
  int maxNumOfRuns; /* High water mark */
  int nBitsInRun;   /* Length of the runs we encode */
  int size;         /* length on input */
  // int *data;
  RLEData *compressed;
};

struct RLEData
{
  unsigned int value;
  int count; // Count of k-element groups
};

RLEVector *RLEVector_create(int size, int runLength)
{
  // printf("Creating Size: %d RunLength: %d\n", size, runLength);
  RLEVector *vec = malloc(sizeof *vec);
  vec->currNumOfRuns = 0;
  vec->maxNumOfRuns = 0;
  vec->nBitsInRun = runLength;
  vec->size = size;
  vec->compressed = (RLEData *)malloc(sizeof(RLEData) * ((size / runLength) + 1));
  vec->compressed[0].value = 0;
  vec->compressed[0].count = size / runLength;
  return vec;
}

int RLEVector_get(RLEVector *vec, int index)
{
  // printf("Getting index: %d Current runs: %d \n", index, vec->currNumOfRuns);
  // RLEVector_print(vec);
  int total_count = 0;
  int k = vec->nBitsInRun;
  // int bitPos = (index % k);
  for (int i = 0; i < vec->currNumOfRuns; i++)
  {
    total_count += vec->compressed[i].count * k;
    if (index < total_count)
    {
      // Extract the bit at the desired position
      int bit_pos = k - 1 - (index % k);
      // printf("Got %d\n", (vec->compressed[i].value >> bit_pos) & 1);
      return ((vec->compressed[i].value >> bit_pos) & 1);
    }
  }
  // printf("Got 0\n");
  return 0;
}

void RLEVector_set(RLEVector *vec, int index)
{
  int *decoded = (int *)malloc(vec->size * sizeof(int));
  decode_rle(vec, decoded);
  // printf("Decoded: ");
  // for (int i =0;i<vec->size;i++){
  //   printf("%d ", decoded[i]);
  // }
  // printf("\n");
  decoded[index] = 1;
  // printf("After setting decoded: ");
  // for (int i = 0; i < vec->size; i++)
  // {
  //   printf("%d ", decoded[i]);
  // }
  // printf("\n");
  encode_rle(vec, decoded);
  free(decoded);
  // int k = vec->nBitsInRun;
  // vec->data[index] = 1;
  // RLEData *rle = vec->compressed;
  // int idx = 0;
  // int i = 0;
  // int n = vec->size;
  // RLEData *newRLE = (RLEData *)malloc(((n / k) + 1) * sizeof(RLEData));
  // vec->currNumOfRuns = 0;
  // while (i < n)
  // {
  //   int count = 1;
  //   int value = 0;
  //   for (int j = 0; j < k && i + j < n; j++)
  //   {
  //     value = (value << 1) | vec->data[i + j];
  //   }
  //   while (i + k < n)
  //   {
  //     int next_value = 0;
  //     for (int j = 0; j < k && i + k + j < n; j++)
  //     {
  //       next_value = (next_value << 1) | vec->data[i + k + j];
  //     }

  //     if (value != next_value)
  //     {
  //       break;
  //     }

  //     count++;
  //     i += k;
  //   }
  //   printf("Value: %d Count: %d\n", value, count);
  //   newRLE[idx].value = value;
  //   newRLE[idx].count = count;
  //   idx++;
  //   i += k;
  // }
  // vec->currNumOfRuns = idx;
  // if (vec->currNumOfRuns > vec->maxNumOfRuns)
  // {
  //   vec->maxNumOfRuns = vec->currNumOfRuns;
  // }
  // vec->compressed = newRLE;
  // RLEVector_print(vec);
  // free(rle);
}

void encode_rle(RLEVector *vec, int *decoded) {
  int k = vec->nBitsInRun;
  int idx = 0;
  int i = 0;
  int n = vec->size;
  RLEData *newRLE = (RLEData *)malloc(((n / k) + 1) * sizeof(RLEData));
  vec->currNumOfRuns = 0;
  while (i < n)
  {
    int count = 1;
    int value = 0;
    for (int j = 0; j < k && i + j < n; j++)
    {
      value = (value << 1) | decoded[i + j];
    }
    while (i + k < n)
    {
      int next_value = 0;
      for (int j = 0; j < k && i + k + j < n; j++)
      {
        next_value = (next_value << 1) | decoded[i + k + j];
      }

      if (value != next_value)
      {
        break;
      }

      count++;
      i += k;
    }
    // printf("Value: %d Count: %d\n", value, count);
    newRLE[idx].value = value;
    newRLE[idx].count = count;
    idx++;
    i += k;
  }
  vec->currNumOfRuns = idx;
  if (vec->currNumOfRuns > vec->maxNumOfRuns)
  {
    vec->maxNumOfRuns = vec->currNumOfRuns;
  }
  free(vec->compressed);
  vec->compressed = newRLE;
}

void decode_rle(RLEVector *vec, int *decoded)
{
  int decoded_index = 0;

  for (int i = 0; i < vec->currNumOfRuns; i++)
  {
    for (int j = 0; j < vec->compressed[i].count; j++)
    {
      for (int bit = vec->nBitsInRun - 1; bit >= 0; bit--)
      {
        if (decoded_index < vec->size)
        {
          decoded[decoded_index++] = (vec->compressed[i].value >> bit) & 1;
        }
      }
    }
  }
}

void RLEVector_print(RLEVector *vec)
{
  for (int i = 0; i < (vec->currNumOfRuns); i++)
  {
    printf("Value: ");
    for (int j = vec->nBitsInRun - 1; j >= 0; j--)
    {
      printf("%d", (vec->compressed[i].value >> j) & 1);
    }
    printf(", Count: %d\n", vec->compressed[i].count);

    // printf("Group Value: %0*X, Count: %d, Start: %d\n", (vec->nBitsInRun) / 4, vec->compressed[i].value, vec->compressed[i].count, vec->compressed[i].start); // Hexadecimal representation
  }
}

int RLEVector_currSize(RLEVector *vec)
{
  return vec->currNumOfRuns;
}

int RLEVector_maxObservedSize(RLEVector *vec)
{
  return vec->maxNumOfRuns;
}

int RLEVector_maxBytes(RLEVector *vec)
{
  return sizeof(RLEVector)                                  /* Internal overhead */
         + sizeof(RLEData) * RLEVector_maxObservedSize(vec) /* Cost per node */
      ;
}

void RLEVector_destroy(RLEVector *vec)
{
  free(vec);
  return;
}

int RLEVector_runSize(RLEVector *vec)
{
  return vec->nBitsInRun;
}