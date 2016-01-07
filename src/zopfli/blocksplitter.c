/*
Copyright 2011 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

/*Modified by Felix Hanau*/

#include "blocksplitter.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "deflate.h"
#include "lz77.h"
#include "util.h"

typedef struct SplitCostContext {
  const unsigned short* litlens;
  const unsigned short* dists;
  size_t start;
  size_t end;
} SplitCostContext;


/*
 Gets the cost which is the sum of the cost of the left and the right section
 of the data.
 */
static double SplitCost(size_t i, SplitCostContext* c, double* first, double* second, unsigned char searchext) {
  *first = ZopfliCalculateBlockSize(c->litlens, c->dists, c->start, i, 2, searchext);
  *second = ZopfliCalculateBlockSize(c->litlens, c->dists, i, c->end, 2, searchext);
  return *first + *second;
}

/*
Finds minimum of function f(i) where is is of type size_t, f(i) is of type
double, i is in range start-end (excluding end).
*/
static size_t FindMinimum(SplitCostContext* context, size_t start, size_t end, double* size, double* biggersize, const ZopfliOptions* options) {
  double first, second;

  if (options->midsplit){
    *size = SplitCost(start + (end - start) / 2, context, &first, &second, options->searchext & 2);
    *biggersize = end - start + (end - start) / 2 > start + (end - start) / 2 - start ? second : first;
    return start + (end - start) / 2;
  }


  size_t startsize = end - start;
  /* Try to find minimum by recursively checking multiple points. */
#define NUM 9  /* Good value: 9. */
  size_t i;
  size_t p[NUM];
  double vp[NUM];
  double prevstore = -1;
  double prevpos = ZOPFLI_LARGE_FLOAT;
  size_t besti;
  double best;
  double lastbest = ZOPFLI_LARGE_FLOAT;
  size_t pos = start;
  size_t ostart = start;
  size_t oend = end;
  for (;;) {
    if (end - start <= options->num) break;
    if (end - start <= startsize/100 && startsize > 600 && options->num == 3) break;

    for (i = 0; i < options->num; i++) {
      p[i] = start + (i + 1) * ((end - start) / (options->num + 1));
      if (options->num % 2 && i == (options->num - 1) / 2 && prevstore != -1 && (prevpos == p[i]  || options->num == 3)){
        vp[i] = best;
        continue;
      }
      vp[i] = SplitCost(p[i], context, &first, &second, options->searchext & 2);
    }
    besti = 0;
    best = vp[0];
    prevstore = best;
    for (i = 1; i < options->num; i++) {
      if (vp[i] < best) {
        best = vp[i];
        besti = i;
        prevpos = vp[i];
      }
    }
    if (best > lastbest) break;

    start = besti == 0 ? start : p[besti - 1];
    end = besti == options->num - 1 ? end : p[besti + 1];

    pos = p[besti];
    lastbest = best;
  }
  *size = best;
  *biggersize = oend - pos > pos - ostart ? second : first;
  return pos;
#undef NUM
}

static void AddSorted(size_t value, size_t** out, size_t* outsize) {
  size_t i;
  ZOPFLI_APPEND_DATA(value, out, outsize);
  for (i = 0; i + 1 < *outsize; i++) {
    if ((*out)[i] > value) {
      size_t j;
      for (j = *outsize - 1; j > i; j--) {
        (*out)[j] = (*out)[j - 1];
      }
      (*out)[i] = value;
      break;
    }
  }
}

/*
Finds next block to try to split, the largest of the available ones.
The largest is chosen to make sure that if only a limited amount of blocks is
requested, their sizes are spread evenly.
llsize: the size of the LL77 data, which is the size of the done array here.
done: array indicating which blocks starting at that position are no longer
    splittable (splitting them increases rather than decreases cost).
splitpoints: the splitpoints found so far.
npoints: the amount of splitpoints found so far.
lstart: output variable, giving start of block.
lend: output variable, giving end of block.
returns 1 if a block was found, 0 if no block found (all are done).
*/
static int FindLargestSplittableBlock(
    size_t llsize, const unsigned char* done,
    const size_t* splitpoints, size_t npoints,
    size_t* lstart, size_t* lend) {
  size_t longest = 0;
  int found = 0;
  size_t i;
  for (i = 0; i <= npoints; i++) {
    size_t start = i == 0 ? 0 : splitpoints[i - 1];
    size_t end = i == npoints ? llsize - 1 : splitpoints[i];
    if (!done[start] && end - start > longest) {
      int st = (*lstart == start);
      int en = (*lend == end);
      found = 1 + st + en;
      *lstart = start;
      *lend = end;
      longest = end - start;
    }
  }
  return found;
}

static void ZopfliBlockSplitLZ77(const unsigned short* litlens,
                          const unsigned short* dists,
                          size_t llsize, size_t** splitpoints,
                          size_t* npoints, const ZopfliOptions* options) {
  if (llsize < options->noblocksplitlz) return;  /* This code fails on tiny files. */

  size_t llpos;
  unsigned numblocks = 1;
  double splitcost, origcost;
  double prevcost = ZOPFLI_LARGE_FLOAT;
  double nprevcost;
  int splittingleft = 0;
  unsigned char* done = (unsigned char*)calloc(llsize, 1);
  if (!done) exit(1); /* Allocation failed. */
  size_t lstart = 0;
  size_t lend = llsize;
  for (;;) {
    SplitCostContext c;

    if (numblocks >= options->blocksplittingmax && options->blocksplittingmax) {
      break;
    }

    c.litlens = litlens;
    c.dists = dists;
    c.start = lstart;
    c.end = lend;
    assert(lstart < lend);
    llpos = FindMinimum(&c, lstart + 1, lend, &splitcost, &nprevcost, options);
    assert(llpos > lstart);
    assert(llpos < lend);

    if (prevcost == ZOPFLI_LARGE_FLOAT || splittingleft == 1){
    origcost = ZopfliCalculateBlockSize(litlens, dists, lstart, lend, 2, options->searchext & 2);
    }
    else {
      origcost = prevcost;
    }
    prevcost = nprevcost;

    if (splitcost > origcost || llpos == lstart + 1 || llpos == lend) {
      done[lstart] = 1;
    } else {
      AddSorted(llpos, splitpoints, npoints);
      numblocks++;
    }


    splittingleft = FindLargestSplittableBlock(llsize, done, *splitpoints, *npoints, &lstart, &lend);
    if (!splittingleft) {
      break;  /* No further split will probably reduce compression. */
    }

    if (lend - lstart < options->noblocksplitlz) {
      break;
    }
  }

  free(done);
}

void ZopfliBlockSplit(const ZopfliOptions* options,
                      const unsigned char* in, size_t instart, size_t inend,
                      size_t** splitpoints, size_t* npoints, SymbolStats** stats) {
  size_t pos = 0;
  size_t i;
  ZopfliBlockState s;
  size_t* lz77splitpoints = 0;
  size_t nlz77points = 0;
  ZopfliLZ77Store store;

  ZopfliInitLZ77Store(&store);

  s.options = options;
  s.blockstart = instart;
  s.blockend = inend;

  *npoints = 0;
  *splitpoints = 0;

  /* Unintuitively, Using a simple LZ77 method here instead of ZopfliLZ77Optimal
  results in better blocks. */
  ZopfliLZ77Greedy(&s, in, instart, inend, &store);

  /* Blocksplitting likely wont improve compression on small files */
  if (inend - instart < options->noblocksplit){
    SymbolStats* statsp = (SymbolStats*)malloc(sizeof(SymbolStats));
    GetStatistics(&store, &statsp[0]);
    *stats = statsp;
    return;
  }

  ZopfliBlockSplitLZ77(store.litlens, store.dists, store.size, &lz77splitpoints, &nlz77points, options);

  SymbolStats* statsp = (SymbolStats*)malloc((nlz77points + 1) * sizeof(SymbolStats));
  if (!(statsp)){
    exit(1);
  }

  /* Convert LZ77 positions to positions in the uncompressed input. */
  pos = instart;
  if (nlz77points) {
    for (i = 0; i < store.size; i++) {
      size_t length = store.dists[i] == 0 ? 1 : store.litlens[i];
      if (lz77splitpoints[*npoints] == i) {
        size_t temp = store.size;
        size_t shift = *npoints ? lz77splitpoints[*npoints - 1] : 0;
        store.size = i - shift;
        store.dists += shift;
        store.litlens += shift;
        GetStatistics(&store, &statsp[*npoints]);
        store.size = temp;
        store.dists -= shift;
        store.litlens -= shift;
        ZOPFLI_APPEND_DATA(pos, splitpoints, npoints);
        if (*npoints == nlz77points) break;
      }
      pos += length;
    }
  }
  assert(*npoints == nlz77points);

  size_t shift = *npoints ? lz77splitpoints[*npoints - 1] : 0;
  store.size -= shift;
  store.dists += shift;
  store.litlens += shift;

  GetStatistics(&store, &statsp[*npoints]);
  store.size += shift;
  store.dists -= shift;
  store.litlens -= shift;

  free(lz77splitpoints);
  ZopfliCleanLZ77Store(&store);
  *stats = statsp;
}
