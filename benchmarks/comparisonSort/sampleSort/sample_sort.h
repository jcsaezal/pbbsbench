#include <algorithm>
#include <functional>
#include <random>

#include <parlay/parallel.h>
#include <parlay/primitives.h>
#include <parlay/random.h>
#include <parlay/slice.h>
#include <parlay/utilities.h>
#include <parlay/internal/uninitialized_sequence.h>

#include "heap_tree.h"

// **************************************************************
// Sample sort
// A generalization of quicksort to many pivots.
// This code picks up to 256 pivots by randomly selecting and
// then sorting them.
// It then puts the keys into buckets depending on which pivots
// they fall between and recursively sorts within the buckets.
// Makes use of a parlaylib bucket sort for the bucketing,
// and std::sort for the base case and sorting the pivots.
//
// This is taken from parlaylib/examples/samplesort.h with slight modifications
// **************************************************************

template <typename assignment_tag, typename Range, typename Less>
void sample_sort_(Range in, Range out, Less less, int level=1) {
  long n = in.size();
  using T = typename Range::value_type;
  
  // for the base case (small or recursion level greater than 2) use std::sort
  long cutoff = 256;
  if (n <= cutoff || level > 2) {
    if (in.begin() != out.begin())
      parlay::uninitialized_relocate_n(out.begin(), in.begin(), n);
    //parlay::copy(in, out);
    parlay::internal::quicksort(out.begin(), n, less);
    return;
  }

  // number of bits in bucket count (e.g. 8 would mean 256 buckets)
  int bits = std::min<long>(8, parlay::log2_up(n)-parlay::log2_up(cutoff)+1);
  long num_buckets = 1 << bits;

  // over-sampling ratio: keeps the buckets more balanced
  int over_ratio = 8;

  // create an over sample and sort it using std:sort
  parlay::random_generator gen;
  std::uniform_int_distribution<long> dis(0, n-1);
  auto oversample = parlay::tabulate(num_buckets * over_ratio, [&] (long i) {
    auto r = gen[i];
    return in[dis(r)];});
  std::sort(oversample.begin(), oversample.end());

  // sub sample to pick final pivots (num_buckets - 1 of them)
  auto pivots = parlay::tabulate(num_buckets-1, [&] (long i) {
    return oversample[(i+1)*over_ratio];});

  // check if any duplicates among the pivots
  bool duplicates = false;
  for (int i=0; i < num_buckets-2; i++)
    if (!less(pivots[i],pivots[i+1])) duplicates = true;
  
  // put pivots into efficient search tree and find buckets id for the input keys
  heap_tree ss(pivots);
  parlay::sequence<unsigned char> bucket_ids;
  if (duplicates)
    // if duplicates put keys equal to a pivot in next bucket
    // this ensures all keys equaling a duplicate are in a bucket by themselves
    bucket_ids = parlay::tabulate(n, [&] (long i) -> unsigned char {
			  auto b = ss.find(in[i], less);
			  return b + ((b < num_buckets-1) && !less(in[i],pivots[b]));});
  else
    bucket_ids = parlay::tabulate(n, [&] (long i) -> unsigned char {
			  return ss.find(in[i], less);});
  
  // sort into the buckets
  auto keys = parlay::internal::uninitialized_sequence<T>(n);
  auto keys_slice = parlay::make_slice(keys);
  //auto keys = parlay::sequence<T>(n);
  auto offsets = parlay::internal::count_sort<assignment_tag>(in, keys_slice,
							      parlay::make_slice(bucket_ids), num_buckets).first;

  // now recursively sort each bucket
  parlay::parallel_for(0, num_buckets, [&, &keys = keys, &offsets = offsets] (long i) {
    long first = offsets[i];
    long last = offsets[i+1];
    if (last-first == 0) return; // empty
    // if duplicate keys among pivots don't sort all-equal buckets
    if (i == 0 || i == num_buckets - 1 || less(pivots[i-1], pivots[i]))
      sample_sort_<assignment_tag>(keys_slice.cut(first,last), out.cut(first,last), less, level+1);
    //else parlay::uninitialized_relocate_n(keys_slice.begin(), out.begin(), last-first);
    else parlay::uninitialized_relocate_n(out.begin()+first, keys_slice.begin()+first, last-first);
      //parlay::copy(keys_slice.cut(first,last), out.cut(first,last));
  }, 1);
}

//  A wraper that calls sample_sort_
template <typename Range, typename Less = std::less<>>
void sample_sort(Range& in, Less less = {}) {
  auto ins = parlay::make_slice(in);
  sample_sort_<parlay::uninitialized_relocate_tag>(ins, ins, less);
}
