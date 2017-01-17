/*
 * Copyright (c) 2011 The LevelDB Authors.
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "block_builder.h"
#include "format.h"

#include <assert.h>
#include <algorithm>

#include "pdlfs-common/coding.h"
#include "pdlfs-common/crc32c.h"

// BlockBuilder generates blocks where keys are prefix-compressed:
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
//
// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.
namespace pdlfs {

BlockBuilder::BlockBuilder(int restart_interval, const Comparator* cmp)
    : restart_interval_(restart_interval),
      cmp_(cmp),
      counter_(0),
      finished_(false) {
  restarts_.push_back(0);  // First restart point is at offset 0
  if (restart_interval_ < 1) {
    restart_interval_ = 1;
  }
  if (cmp_ == NULL) {
    cmp_ = BytewiseComparator();
  }
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // First restart point is at offset 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  if (!finished_)
    return (buffer_.size() +                       // Raw data buffer
            restarts_.size() * sizeof(uint32_t) +  // Restart array
            sizeof(uint32_t));                     // Restart array length
  else {
    return buffer_.size();
  }
}

Slice BlockBuilder::Finish() {
  assert(!finished_);
  // Append restart array
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  PutFixed32(&buffer_, restarts_.size());  // Remember the array size
  finished_ = true;
  return buffer_;
}

Slice BlockBuilder::Finalize(uint64_t padding_target) {
  assert(finished_);
  Slice contents = buffer_;  // Contents without the trailer and padding
  char trailer[kBlockTrailerSize];
  trailer[0] = kNoCompression;
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  buffer_.append(trailer, sizeof(trailer));
  if (buffer_.size() < padding_target) {
    buffer_.resize(padding_target, 0);
  }
  return buffer_;
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= restart_interval_);
  assert(buffer_.empty() || cmp_->Compare(key, last_key_piece) > 0);
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // See how much sharing to do with previous string
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}

}  // namespace pdlfs
