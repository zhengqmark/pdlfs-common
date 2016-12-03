/*
 * Copyright (c) 2011 The LevelDB Authors.
 * Copyright (c) 2015-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "version_set.h"
#include "../merger.h"
#include "../two_level_iterator.h"
#include "memtable.h"
#include "table_cache.h"

#include "pdlfs-common/coding.h"
#include "pdlfs-common/dbfiles.h"
#include "pdlfs-common/env.h"
#include "pdlfs-common/leveldb/table_builder.h"
#include "pdlfs-common/log_reader.h"
#include "pdlfs-common/log_writer.h"
#include "pdlfs-common/strutil.h"

#include <stdio.h>
#include <algorithm>

namespace pdlfs {

namespace {
typedef DBOptions Options;

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
static int64_t MaxGrandParentOverlapBytes(const Options* options) {
  return options->level_factor * options->table_file_size;
}

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
static int64_t ExpandedCompactionByteSizeLimit(const Options* options) {
  return (2 * (options->level_factor + 2) + 1) * options->table_file_size;
}

// Note: the result for level zero is not really used since we set
// the Level-0 compaction threshold based on number of files.
static double MaxBytesForLevel(const Options* options, int level) {
  double result = options->l1_compaction_trigger *
                  options->table_file_size;  // Result for Level-1

  while (level > 1) {
    result *= options->level_factor;
    level--;
  }
  return result;
}

static int64_t MaxCompactionSizeForLevel(const Options* options, int level) {
  assert(options->enable_sublevel);
  return options->level_factor * options->table_file_size;
}

static uint64_t MaxFileSizeForLevel(const Options* options, int level) {
  // TODO(opt): we could vary per level to reduce number of files?
  return options->table_file_size;
}

static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}

static std::string SublevelInfo(
    const std::vector<std::vector<FileMetaData*> >& files,
    const Version::SublevelPool& input_pool,
    const Version::SublevelPool& output_pool) {
  assert(input_pool.size() == output_pool.size());
  std::string result;
  char buf[200];
  for (int i = 0; i < input_pool.size(); ++i) {
    snprintf(buf, sizeof(buf), "level %d:\n", i);
    result.append(buf);
    snprintf(buf, sizeof(buf), "input pool %5d - %5d:\n", input_pool[i].first,
             input_pool[i].first + input_pool[i].second - 1);
    result.append(buf);
    for (int j = 0; j < input_pool[i].second; ++j) {
      int row = input_pool[i].first + j;
      assert(row < files.size());
      snprintf(buf, sizeof(buf), "\tsublevel %4d:\n", j);
      result.append(buf);
      for (std::vector<FileMetaData*>::const_iterator iter = files[row].begin();
           iter != files[row].end(); ++iter) {
        snprintf(buf, sizeof(buf), "\t\t[%s\t,\t%s]\n",
                 (*iter)->smallest.DebugString().c_str(),
                 (*iter)->largest.DebugString().c_str());
        result.append(buf);
      }
      result.append("\n");
    }

    snprintf(buf, sizeof(buf), "output pool %5d - %5d:\n", output_pool[i].first,
             output_pool[i].first + output_pool[i].second - 1);
    result.append(buf);
    for (int j = 0; j < output_pool[i].second; ++j) {
      int row = output_pool[i].first + j;
      assert(row < files.size());
      snprintf(buf, sizeof(buf), "\tsublevel %4d:\n", j);
      result.append(buf);
      for (std::vector<FileMetaData*>::const_iterator iter = files[row].begin();
           iter != files[row].end(); ++iter) {
        snprintf(buf, sizeof(buf), "\t\t[%s\t,\t%s]\n",
                 (*iter)->smallest.DebugString().c_str(),
                 (*iter)->largest.DebugString().c_str());
        result.append(buf);
      }
      result.append("\n");
    }
  }
  return result;
}
}  // namespace

Version::Version(VersionSet* vset)
    : vset_(vset),
      next_(this),
      prev_(this),
      refs_(0),
      files_(vset->options_->enable_sublevel ? 2
                                             : config::kMaxMemCompactLevel + 1),
      file_to_compact_(NULL),
      file_to_compact_level_(-1),
      compaction_score_(-1),
      compaction_level_(-1) {
  if (vset->options_->enable_sublevel) {
    input_pool_.push_back(std::make_pair(0, 1));
    output_pool_.push_back(std::make_pair(0, 1));
    input_pool_.push_back(std::make_pair(1, 1));
    output_pool_.push_back(std::make_pair(2, 0));
  }
}

Version::~Version() {
  assert(refs_ == 0);

  // Remove from linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  for (int level = 0; level < files_.size(); level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData* f = files_[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        delete f;
      }
    }
  }
}

int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files, const Slice& key) {
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData* f = files[mid];
    if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      right = mid;
    }
  }
  return right;
}

static bool AfterFile(const Comparator* ucmp, const Slice* user_key,
                      const FileMetaData* f) {
  // NULL user_key occurs before all keys and is therefore never after *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

static bool BeforeFile(const Comparator* ucmp, const Slice* user_key,
                       const FileMetaData* f) {
  // NULL user_key occurs after all keys and is therefore never before *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    // Need to check against all files
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // Binary search over file list
  uint32_t index = 0;
  if (smallest_user_key != NULL) {
    // Find the earliest possible internal key for smallest_user_key
    InternalKey small(*smallest_user_key, kMaxSequenceNumber,
                      kValueTypeForSeek);
    index = FindFile(icmp, files, small.Encode());
  }

  if (index >= files.size()) {
    // beginning of range is after all files, so no overlap.
    return false;
  }

  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 24-byte value containing the file number, file size, and sequence offset,
// all encoded using EncodeFixed64.
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>* flist)
      : icmp_(icmp), flist_(flist), index_(flist->size()) {  // Marks as invalid
  }
  virtual bool Valid() const { return index_ < flist_->size(); }
  virtual void Seek(const Slice& target) {
    index_ = FindFile(icmp_, *flist_, target);
  }
  virtual void SeekToFirst() { index_ = 0; }
  virtual void SeekToLast() {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  virtual void Next() {
    assert(Valid());
    index_++;
  }
  virtual void Prev() {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();  // Marks as invalid
    } else {
      index_--;
    }
  }
  virtual Slice key() const {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  virtual Slice value() const {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_ + 8, (*flist_)[index_]->file_size);
    EncodeFixed64(value_buf_ + 16, (*flist_)[index_]->seq_off);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  virtual Status status() const { return Status::OK(); }

 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData*>* const flist_;
  uint32_t index_;

  // Backing store for value().  Holds the file number and size.
  mutable char value_buf_[24];
};

static Iterator* GetFileIterator(void* arg, const ReadOptions& options,
                                 const Slice& file_value, TableGetStats*) {
  TableCache* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 24) {
    return NewErrorIterator(
        Status::Corruption("FileReader invoked with unexpected value"));
  } else {
    return cache->NewIterator(options, DecodeFixed64(file_value.data()),
                              DecodeFixed64(file_value.data() + 8),
                              DecodeFixed64(file_value.data() + 16));
  }
}

Iterator* Version::NewConcatenatingIterator(const ReadOptions& options,
                                            int level) const {
  assert(level < files_.size());
  return NewTwoLevelIterator(
      new LevelFileNumIterator(vset_->icmp_, &files_[level]), &GetFileIterator,
      vset_->table_cache_, options);
}

void Version::AddIterators(const ReadOptions& options,
                           std::vector<Iterator*>* iters) {
  // Merge all level zero files together since they may overlap
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters->push_back(vset_->table_cache_->NewIterator(
        options, files_[0][i]->number, files_[0][i]->file_size,
        files_[0][i]->seq_off));
  }

  // For levels > 0, we can use a concatenating iterator that sequentially
  // walks through the non-overlapping files in the level, opening them
  // lazily.
  for (int level = 1; level < files_.size(); level++) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

// Callback from TableCache::Get()
namespace {
enum SaverState { kNotFound, kFound, kDeleted, kCorrupt };

struct Saver {
  SaverState state;
  const ReadOptions* options;
  const Comparator* ucmp;
  Slice user_key;
  Buffer* buf;
};
}

static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else {
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      if (s->state == kFound) {
        assert(parsed_key.sequence <= kMaxSequenceNumber);
        s->buf->Fill(v.data(), std::min(v.size(), s->options->limit));
      }
    }
  }
}

static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
  return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                                 bool (*func)(void*, int, FileMetaData*)) {
  // TODO(sanjay): Change Version::Get() to use this function.
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  // Search level-0 in order from newest to oldest.
  std::vector<FileMetaData*> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    FileMetaData* f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(f);
    }
  }
  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!(*func)(arg, 0, tmp[i])) {
        return;
      }
    }
  }

  // Search other levels.
  for (int level = 1; level < files_.size(); level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Binary search to find earliest index whose largest key >= internal_key.
    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      FileMetaData* f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {
        // All of "f" is past any data for user_key
      } else {
        if (!(*func)(arg, level, f)) {
          return;
        }
      }
    }
  }
}

bool Version::Get(const ReadOptions& options, const LookupKey& k, Buffer* buf,
                  Status* s, GetStats* stats) {
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();
  const Comparator* ucmp = vset_->icmp_.user_comparator();

  FileMetaData* last_file_read = NULL;
  int last_file_read_level = -1;

  // We can search level-by-level since entries never hop across
  // levels.  Therefore we are guaranteed that if we find data
  // in an smaller level, later levels are irrelevant.
  std::vector<FileMetaData*> tmp;
  FileMetaData* tmp2;
  for (int level = 0; level < files_.size(); level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0) continue;

    // Get the list of files to search in this level
    FileMetaData* const* files = &files_[level][0];
    if (level == 0) {
      // Level-0 files may overlap each other.  Find all files that
      // overlap user_key and process them in order from newest to oldest.
      tmp.reserve(num_files);
      for (uint32_t i = 0; i < num_files; i++) {
        FileMetaData* f = files[i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
            ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
          tmp.push_back(f);
        }
      }
      if (tmp.empty()) continue;

      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      files = &tmp[0];
      num_files = tmp.size();
    } else {
      // Binary search to find earliest index whose largest key >= ikey.
      uint32_t index = FindFile(vset_->icmp_, files_[level], ikey);
      if (index >= num_files) {
        files = NULL;
        num_files = 0;
      } else {
        tmp2 = files[index];
        if (ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0) {
          // All of "tmp2" is past any data for user_key
          files = NULL;
          num_files = 0;
        } else {
          files = &tmp2;
          num_files = 1;
        }
      }
    }

    for (uint32_t i = 0; i < num_files; ++i) {
      if (last_file_read != NULL && stats->seek_file == NULL) {
        // We have had more than one seek for this read.  Charge the 1st file.
        stats->seek_file = last_file_read;
        stats->seek_file_level = last_file_read_level;
      }

      FileMetaData* f = files[i];
      last_file_read = f;
      last_file_read_level = level;

      Saver saver;
      saver.state = kNotFound;
      saver.options = &options;
      saver.ucmp = ucmp;
      saver.user_key = user_key;
      saver.buf = buf;
      TableGetStats tstats;
      *s =
          vset_->table_cache_->Get(options, f->number, f->file_size, f->seq_off,
                                   ikey, &saver, SaveValue, &tstats);
      stats->AddTableGetStat(tstats);

      if (!s->ok()) {
        return true;  // Read error
      }
      switch (saver.state) {
        case kNotFound:
          break;  // Keep searching in other files
        case kFound:
          return true;  // Found
        case kDeleted:
          *s = Status::NotFound(Slice());
          return true;
        case kCorrupt:
          *s = Status::Corruption("Corrupted key for ", user_key);
          return true;
      }
    }
  }

  *s = Status::NotFound(Slice());
  return false;
}

bool Version::UpdateStats(const GetStats& stats) {
  FileMetaData* f = stats.seek_file;
  if (f != NULL) {
    f->allowed_seeks--;
    if (f->allowed_seeks <= 0 && file_to_compact_ == NULL) {
      file_to_compact_ = f;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

bool Version::RecordReadSample(Slice internal_key) {
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }

  struct State {
    GetStats stats;  // Holds first matching file
    int matches;

    static bool Match(void* arg, int level, FileMetaData* f) {
      State* state = reinterpret_cast<State*>(arg);
      state->matches++;
      if (state->matches == 1) {
        // Remember first match.
        state->stats.seek_file = f;
        state->stats.seek_file_level = level;
      }
      // We can stop iterating once we have a second match.
      return state->matches < 2;
    }
  };

  State state;
  state.matches = 0;
  ForEachOverlapping(ikey.user_key, internal_key, &state, &State::Match);

  // Must have at least two matches since we want to merge across
  // files. But what if we have a single file that contains many
  // overwrites and deletions?  Should we have another mechanism for
  // finding such files?
  if (state.matches >= 2) {
    // 1MB cost is about 1 seek (see comment in Builder::Apply).
    return UpdateStats(state.stats);
  }
  return false;
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

bool Version::OverlapInLevel(int level, const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
  assert(level < files_.size());
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

int Version::NumLevels() const { return files_.size(); }

int Version::NumSublevelsInLevel(int level) const {
  assert(vset_->options_->enable_sublevel);
  assert(level >= 0);
  assert(input_pool_.size() == output_pool_.size());
  if (level >= input_pool_.size()) return 0;
  if (level == 0) return 1;
  return input_pool_[level].second + output_pool_[level].second;
}

int Version::NumFilesInLevel_sub(const SublevelPool& pool, int level) const {
  assert(vset_->options_->enable_sublevel);
  int count = 0;
  for (int i = pool[level].first, end = pool[level].first + pool[level].second;
       i < end; ++i) {
    assert(i < files_.size());
    count += files_[i].size();
  }
  return count;
}

int Version::NumFilesInLevel_sub(int level) const {
  assert(vset_->options_->enable_sublevel);
  assert(level >= 0);
  assert(input_pool_.size() == output_pool_.size());
  int count = 0;
  if (level == 0) {
    count = files_[0].size();
  } else if (level < input_pool_.size()) {
    count += NumFilesInLevel_sub(input_pool_, level);
    count += NumFilesInLevel_sub(output_pool_, level);
  }
  return count;
}

int64_t Version::NumBytesInLevel_sub(const SublevelPool& pool,
                                     int level) const {
  int64_t bytes = 0;
  for (int i = pool[level].first, end = pool[level].first + pool[level].second;
       i < end; ++i) {
    assert(i < files_.size());
    for (int j = 0; j < files_[i].size(); ++j) {
      bytes += files_[i][j]->file_size;
    }
  }
  return bytes;
}

int64_t Version::NumBytesInLevel_sub(int level) const {
  assert(vset_->options_->enable_sublevel);
  assert(level >= 0);
  assert(input_pool_.size() == output_pool_.size());
  int64_t bytes = 0;
  if (level == 0) {
    bytes += NumBytesInLevel_sub(input_pool_, 0);
  } else if (level < input_pool_.size()) {
    bytes += NumBytesInLevel_sub(input_pool_, level);
    bytes += NumBytesInLevel_sub(output_pool_, level);
  }
  return bytes;
}

int Version::NumLevels_sub() const {
  assert(vset_->options_->enable_sublevel);
  assert(input_pool_.size() == output_pool_.size());
  return input_pool_.size();
}

int Version::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                        const Slice& largest_user_key) {
  assert(config::kMaxMemCompactLevel < files_.size());
  int level = 0;
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<FileMetaData*> overlaps;

    while (level < config::kMaxMemCompactLevel) {
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      if (level + 2 < files_.size()) {
        // Check that file does not overlap too many grandparent bytes.
        GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
        const int64_t sum = TotalFileSize(overlaps);
        if (sum > MaxGrandParentOverlapBytes(vset_->options_)) {
          break;
        }
      }
      level++;
    }
  }
  return level;
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
void Version::GetOverlappingInputs(int level, const InternalKey* begin,
                                   const InternalKey* end,
                                   std::vector<FileMetaData*>* inputs) {
  assert(level >= 0);
  assert(level < files_.size());
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != NULL) {
    user_begin = begin->user_key();
  }
  if (end != NULL) {
    user_end = end->user_key();
  }
  const Comparator* user_cmp = vset_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != NULL && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != NULL && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs->push_back(f);
      if (level == 0) {
        // Level-0 files may overlap each other.  So check if the newly
        // added file has expanded the range.  If so, restart search.
        if (begin != NULL && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != NULL && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < files_.size(); level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" ---\n");
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      AppendNumberTo(&r, files[i]->number);
      r.push_back(':');
      AppendNumberTo(&r, files[i]->file_size);
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append("]\n");
    }
  }
  return r;
}

// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
class VersionSet::Builder {
 private:
  // Helper to sort by v->files_[file_number].smallest
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      int r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        // Break ties by file number
        return (f1->number < f2->number);
      }
    }
  };

  typedef std::set<FileMetaData*, BySmallestKey> FileSet;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet* added_files;
    std::set<uint64_t> updated_files;
  };

  VersionSet* vset_;
  Version* base_;
  std::vector<LevelState> levels_;
  InternalKey truncated_key_;

 public:
  // Initialize a builder with the files from *base and other info from *vset
  Builder(VersionSet* vset, Version* base)
      : vset_(vset), base_(base), levels_(base->files_.size()) {
    base_->Ref();
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < levels_.size(); level++) {
      levels_[level].added_files = new FileSet(cmp);
    }
  }

  ~Builder() {
    for (int level = 0; level < levels_.size(); level++) {
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      for (FileSet::const_iterator it = added->begin(); it != added->end();
           ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }
    base_->Unref();
  }

  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    if (!vset_->options_->enable_sublevel) {
      // Make sure the highest level is empty
      if (vset_->compact_pointer_.size() <= edit->max_level_ + 1) {
        vset_->compact_pointer_.resize(edit->max_level_ + 2);
      }
      if (levels_.size() <= edit->max_level_ + 1) {
        int from = levels_.size();
        levels_.resize(edit->max_level_ + 2);
        BySmallestKey cmp;
        cmp.internal_comparator = &vset_->icmp_;
        for (int level = from; level < levels_.size(); level++) {
          levels_[level].added_files = new FileSet(cmp);
        }
      }
      // Update compaction pointers
      for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
        const int level = edit->compact_pointers_[i].first;
        assert(level <= edit->max_level_);
        vset_->compact_pointer_[level] =
            edit->compact_pointers_[i].second.Encode().ToString();
      }
    }

    // Delete files
    const VersionEdit::DeletedFileSet& del = edit->deleted_files_;
    for (VersionEdit::DeletedFileSet::const_iterator iter = del.begin();
         iter != del.end(); ++iter) {
      const int level = iter->first;
      assert(level < levels_.size());
      const uint64_t number = iter->second;
      assert(level <= edit->max_level_);
      levels_[level].deleted_files.insert(number);
    }

    // Add new files
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      assert(level < levels_.size());
      FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
      f->refs = 1;

      // We arrange to automatically compact this file after
      // a certain number of seeks.  Let's assume:
      //   (1) One seek costs 10ms
      //   (2) Writing or reading 1MB costs 10ms (100MB/s)
      //   (3) A compaction of 1MB does 25MB of IO:
      //         1MB read from this level
      //         10-12MB read from next level (boundaries may be misaligned)
      //         10-12MB written to next level
      // This implies that 25 seeks cost the same as the compaction
      // of 1MB of data.  I.e., one seek costs approximately the
      // same as the compaction of 40KB of data.  We are a little
      // conservative and allow approximately one seek for every 16KB
      // of data before triggering a compaction.
      f->allowed_seeks = (f->file_size / 16384);
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;
      assert(level <= edit->max_level_);
      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files->insert(f);
    }

    truncated_key_ = edit->truncate_key_;
    // Update files, should only happen when sublevel is enabled
    const VersionEdit::UpdatedFileSet& updated = edit->updated_files_;
    for (VersionEdit::UpdatedFileSet::const_iterator iter = updated.begin();
         iter != updated.end(); ++iter) {
      assert(vset_->options_->enable_sublevel);
      const int level = iter->first;
      assert(level < levels_.size());
      const uint64_t number = iter->second;
      levels_[level].updated_files.insert(number);
    }
  }

  // Save the current state in *v.
  void SaveTo(Version* v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    if (v->files_.size() < levels_.size()) {
      v->files_.resize(levels_.size());
    }
    for (int level = 0; level < levels_.size(); level++) {
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      if (level < base_->files_.size()) {
        const std::vector<FileMetaData*>& base_files = base_->files_[level];
        std::vector<FileMetaData*>::const_iterator base_iter =
            base_files.begin();
        std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
        const FileSet* added = levels_[level].added_files;
        v->files_[level].reserve(base_files.size() + added->size());
        for (FileSet::const_iterator added_iter = added->begin();
             added_iter != added->end(); ++added_iter) {
          // Add all smaller files listed in base_
          for (std::vector<FileMetaData*>::const_iterator bpos =
                   std::upper_bound(base_iter, base_end, *added_iter, cmp);
               base_iter != bpos; ++base_iter) {
            MaybeAddFile(v, level, *base_iter);
          }
          MaybeAddFile(v, level, *added_iter);
        }

        // Add remaining base files
        for (; base_iter != base_end; ++base_iter) {
          MaybeAddFile(v, level, *base_iter);
        }
      } else {
        const FileSet* added = levels_[level].added_files;
        v->files_[level].reserve(added->size());
        for (FileSet::const_iterator added_iter = added->begin();
             added_iter != added->end(); ++added_iter) {
          MaybeAddFile(v, level, *added_iter);
        }
      }
#ifndef NDEBUG
      // Make sure there is no overlap in levels > 0
      if (level > 0) {
        for (uint32_t i = 1; i < v->files_[level].size(); i++) {
          const InternalKey& prev_end = v->files_[level][i - 1]->largest;
          const InternalKey& this_begin = v->files_[level][i]->smallest;
          if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
            fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                    prev_end.DebugString().c_str(),
                    this_begin.DebugString().c_str());
            abort();
          }
        }
      }
#endif
    }

    // If sublevel is not enabled, make sure the highest level is always empty
    assert(vset_->options_->enable_sublevel || v->files_.back().empty());
  }

  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // File is deleted: do nothing
    } else if (levels_[level].updated_files.count(f->number) > 0) {
      // File is updated (which means truncated now): create new meta data and
      // set smallest to be the truncated key
      assert(vset_->options_->enable_sublevel);
      assert(vset_->icmp_.Compare(f->smallest, truncated_key_) < 0);
      assert(vset_->icmp_.Compare(f->largest, truncated_key_) >= 0);
      FileMetaData* updated_f = new FileMetaData(*f);
      updated_f->refs = 1;
      updated_f->smallest = truncated_key_;
      v->files_[level].push_back(updated_f);
    } else {
      std::vector<FileMetaData*>* files = &v->files_[level];
      if (level > 0 && !files->empty()) {
#if 0
        if(vset_->icmp_.Compare((*files)[files->size()-1]->largest, f->smallest)>=0) {
          fprintf(stderr, "MAF %d, %s V.S. %s\n", level,
                  (*files)[files->size()-1]->largest.DebugString().c_str(),
                  f->smallest.DebugString().c_str());
          fprintf(stderr, "version files:\n");
          for(int i = 0; i<v->files_.size(); ++i) {
            fprintf(stderr, "level %d:", i);
            for(int j = 0; j<v->files_[i].size(); ++j) {
              fprintf(stderr, "\n    %4d: [%s,\t\t%s]", j,
                      v->files_[i][j]->smallest.DebugString().c_str(),
                      v->files_[i][j]->largest.DebugString().c_str());
              assert(!(i>0&&j>0&&
                       vset_->icmp_.Compare(v->files_[i][j]->smallest, v->files_[i][j-1]->largest)<=0));
            }
            fprintf(stderr, "\n");
          }
          fprintf(stderr, "added files:\n");
          for(int i = 0; i<levels_.size(); ++i) {
            fprintf(stderr, "level %d:", i);
            for(FileSet::const_iterator iter = levels_[i].added_files->begin();
                iter!=levels_[i].added_files->end(); ++iter) {
              fprintf(stderr, "\n    [%s,\t\t%s]",
                      (*iter)->smallest.DebugString().c_str(),
                      (*iter)->largest.DebugString().c_str());
            }
            fprintf(stderr, "\n");
          }
          if(vset_->options_->enable_sublevel) {
            fprintf(stderr, "level information:\n");
            const Version &cur = *vset_->current_;
            assert(cur.input_pool_.size()==cur.output_pool_.size());
            for(int i = 0; i<cur.input_pool_.size(); ++i) {
              fprintf(stderr, "level %d:\n    input pool: %5d\t-\t%4d\n    output pool: %4d\t-\t%4d\n",
                      i, cur.input_pool_[i].first, cur.input_pool_[i].first+cur.input_pool_[i].second-1,
                      cur.output_pool_[i].first, cur.output_pool_[i].first+cur.output_pool_[i].second-1);
            }
          }
          fflush(stderr);
      }
#endif
        // Must not overlap
        assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest,
                                    f->smallest) < 0);
      }
      f->refs++;
      files->push_back(f);
    }
  }
};

VersionSet::VersionSet(const std::string& dbname, const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(NULL),
      descriptor_log_(NULL),
      dummy_versions_(this),
      current_(NULL),
      compact_pointer_(
          options->enable_sublevel ? 0 : config::kMaxMemCompactLevel + 1) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
  delete descriptor_log_;
  delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
  // Make "v" current
  assert(v->refs_ == 0);
  assert(v != current_);
  assert(options_->enable_sublevel ||
         v->files_.size() <= compact_pointer_.size());
  if (current_ != NULL) {
    current_->Unref();
  }
  current_ = v;
  v->Ref();

  // Append to linked list
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::ForeighApply(VersionEdit* edit) {
  if (edit->has_comparator_ &&
      edit->comparator_ != icmp_.user_comparator()->Name()) {
    return Status::InvalidArgument(
        edit->comparator_ + " does not match existing comparator ",
        icmp_.user_comparator()->Name());
  }

  uint64_t next_file_number = next_file_number_;
  uint64_t last_sequence = last_sequence_;
  uint64_t log_number = log_number_;
  uint64_t prev_log_number = prev_log_number_;

  if (edit->has_log_number_) {
    assert(log_number <= edit->log_number_);
    log_number = edit->log_number_;
  }

  if (edit->has_prev_log_number_) {
    assert(prev_log_number <= edit->prev_log_number_);
    prev_log_number = edit->prev_log_number_;
  }

  if (edit->has_next_file_number_) {
    assert(next_file_number <= edit->next_file_number_);
    next_file_number = edit->next_file_number_;
  }

  if (edit->has_last_sequence_) {
    assert(last_sequence <= edit->last_sequence_);
    last_sequence = edit->last_sequence_;
  }

  assert(log_number < next_file_number);
  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
    if (options_->enable_sublevel) {
      ReorganizeSublevels(v, edit);
    }
  }
  // No need to finalize the new version since we are not going to
  // do any compaction.

  // Install the new version
  AppendVersion(v);
  log_number_ = log_number;
  prev_log_number_ = prev_log_number;
  next_file_number_ = next_file_number;
  last_sequence_ = last_sequence;
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
    if (options_->enable_sublevel) {
      ReorganizeSublevels(v, edit);
    }
  }
  Finalize(v);

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == NULL) {
    // No reason to unlock *mu here since we only hit this path in the
    // first call to LogAndApply (when opening the database).
    assert(descriptor_file_ == NULL);
    assert(manifest_file_number_ != 0);
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    edit->SetNextFile(next_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = new log::Writer(descriptor_file_);
      s = WriteSnapshot(descriptor_log_);
    }
  }

  // Unlock during expensive MANIFEST log write
  {
    mu->Unlock();

    // Write new record to MANIFEST log
    if (s.ok()) {
      std::string record;
      edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
      if (s.ok()) {
        s = descriptor_file_->Sync();
      }
      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST write: %s", s.ToString().c_str());
      }
    }

    // If we just created a new descriptor file, install it by either writing a
    // new CURRENT file that points to it or removing the alternative
    // descriptor file to speedup the next recovery.
    if (s.ok() && !new_manifest_file.empty()) {
      if (!options_->rotating_manifest) {
        s = SetCurrentFile(env_, dbname_, manifest_file_number_);
      } else {
        std::string names[2];
        assert(manifest_file_number_ < 3);
        names[0] = DescriptorFileName(dbname_, 3 - manifest_file_number_);
        names[1] = CurrentFileName(dbname_);
        for (size_t i = 0; i < 2; i++) {
          Log(options_->info_log, "Delete %s", names[i].c_str());
          env_->DeleteFile(names[i]);
        }
      }
    }

    mu->Lock();
  }

  // Install the new version
  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      delete descriptor_log_;
      delete descriptor_file_;
      descriptor_log_ = NULL;
      descriptor_file_ = NULL;
      env_->DeleteFile(new_manifest_file);
    }
  }

  return s;
}

void VersionSet::ReorganizeSublevels(Version* version, VersionEdit* edit) {
  static int count = 0;
  assert(options_->enable_sublevel);
  assert(version->input_pool_.size() == version->output_pool_.size());
  assert(version->input_pool_.size() == 2);

  // If Any sublevel is empty, remove it. Except it is the only sublevel of any
  // input pool

  // If the output pool of level i is empty and the top sublevel of the input
  // pool of level i+1 is non-empty (or level i+1 does not exist),
  // it means we just finished one round of compaction of all sublevels in level
  // i.
  // Create another sublevel in level i+1's input pool.

  // If the total size of level i exceeds the maximum size, we need to prepare
  // it for compaction.
  // That is, if its output pool is empty, move all sublevels but the top one in
  // its input pool to its output pool.
  // If there is only one sublevel in its input pool, move it to the output
  // pool.

  // If the output pool of the last level is non-empty, we need to make room for
  // its compaction.
  // That is, create another level after it.

  bool new_input_sublevel = false;
  const std::vector<std::vector<FileMetaData*> > files = version->files_;
  version->files_.clear();
  version->files_.reserve(files.size() + 1);
  version->input_pool_.clear();
  version->input_pool_.reserve(current_->input_pool_.size() + 1);
  version->output_pool_.clear();
  version->output_pool_.reserve(current_->output_pool_.size() + 1);
  for (int level = 0; level < current_->input_pool_.size(); ++level) {
    if (level == 0) {
      version->files_.push_back(files[0]);
      version->input_pool_.push_back(std::make_pair(0, 1));
      version->output_pool_.push_back(std::make_pair(0, 1));
      // Hacky way of determining whether the compaction happened at level 0
      if (!edit->deleted_files_.empty() &&
          edit->deleted_files_.begin()->first == 0) {
        new_input_sublevel = true;
      }
    } else {
      int base_sublevel = version->files_.size();
      int bytes = 0;
      bool first = true;
      if (new_input_sublevel) {
        version->files_.push_back(std::vector<FileMetaData*>());
        first = false;
      }
      for (int i = 0; i < current_->input_pool_[level].second; ++i) {
        int row = current_->input_pool_[level].first + i;
        if (first || !files[row].empty()) {
          bytes += TotalFileSize(files[row]);
          version->files_.push_back(files[row]);
        }
        first = false;
      }
      assert(!first);
      int length = version->files_.size() - base_sublevel;
      assert(version->input_pool_.size() == level);
      version->input_pool_.push_back(std::make_pair(base_sublevel, length));

      new_input_sublevel = false;
      base_sublevel = version->files_.size();
      for (int i = 0; i < current_->output_pool_[level].second; ++i) {
        int row = current_->output_pool_[level].first + i;
        if (!files[row].empty()) {
          version->files_.push_back(files[row]);
        }
      }
      length = version->files_.size() - base_sublevel;
      if (length == 0 && level + 1 < current_->input_pool_.size() &&
          current_->input_pool_[level + 1].second > 0) {
        new_input_sublevel = true;
      }
      assert(version->output_pool_.size() == level);
      if (length == 0 && bytes >= MaxBytesForLevel(options_, level) - 1) {
        fprintf(stderr, "VersionSet::ReorganizeSublevels(%d)\n", ++count);
        if (version->input_pool_[level].second == 1) {
          assert(version->input_pool_[level].first ==
                 version->files_.size() - 1);
          version->files_.push_back(
              version->files_[version->files_.size() - 1]);
          version->files_[version->files_.size() - 2].clear();
          version->input_pool_[level].second = 2;
        }
        fprintf(stderr, "%d version->input_pool_[level].second=%d\n", count,
                version->input_pool_[level].second);
        fprintf(stderr, "%d version->input_pool_[level].second-1=%d\n", count,
                version->input_pool_[level].second - 1);
        length = version->input_pool_[level].second - 1;
        fprintf(stderr, "%d length=%d\n", count, length);
        fprintf(stderr, "~VersionSet::ReorganizeSublevels(%d)\n", count);
        fflush(stderr);
        if (length == 0) {
          fprintf(stderr, "%d length2=%d\n", count, length);
          fflush(stderr);
          fprintf(stderr, "abort1\n");
          fflush(stderr);
          abort();
        }
        version->input_pool_[level].second = 1;
        version->output_pool_.push_back(
            std::make_pair(version->input_pool_[level].first + 1, length));
      } else {
        version->output_pool_.push_back(std::make_pair(base_sublevel, length));
      }
#if 1
      uint64_t bbytes = 0;
      for (int i = version->input_pool_[level].first,
               end = version->input_pool_[level].first +
                     version->input_pool_[level].second;
           i < end; ++i) {
        bbytes += TotalFileSize(version->files_[i]);
      }
      for (int i = version->output_pool_[level].first,
               end = version->output_pool_[level].first +
                     version->output_pool_[level].second;
           i < end; ++i) {
        bbytes += TotalFileSize(version->files_[i]);
      }
      double score =
          static_cast<double>(bbytes) / MaxBytesForLevel(options_, level);
      const bool size_compaction = (score >= 1);
      if (length == 0 && size_compaction) {
        fprintf(stderr, "abort2\n");
        fflush(stderr);
        abort();
      }
#endif
    }
  }
  assert(version->input_pool_.size() == version->output_pool_.size());
  if (version->output_pool_[version->output_pool_.size() - 1].second > 0) {
    version->files_.push_back(std::vector<FileMetaData*>());
    version->input_pool_.push_back(
        std::make_pair(version->files_.size() - 1, 1));
    version->output_pool_.push_back(std::make_pair(version->files_.size(), 0));
  }
  assert(version->output_pool_[version->output_pool_.size() - 1].first ==
         version->files_.size());
  assert(version->output_pool_[version->output_pool_.size() - 1].second == 0);

#if 0
  fprintf(stderr, "before reorganize:\n");
  fprintf(stderr, "%s", SublevelInfo(files, current_->input_pool_, current_->output_pool_).c_str());
  fprintf(stderr, "after reorganize:\n");
  fprintf(stderr, "%s", SublevelInfo(version->files_, version->input_pool_, version->output_pool_).c_str());
  fflush(stderr);
#endif
}

Status VersionSet::Recover() {
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      if (this->status->ok()) *this->status = s;
    }
  };

  // TODO change this function to enable sublevel recovery

  // Try all three candidates, including the odd/even manifest files,
  // and the one that is referenced by "CURRENT"
  std::string dscnames[3];
  dscnames[0] = DescriptorFileName(dbname_, 1);
  if (!env_->FileExists(dscnames[0])) {
    dscnames[0].clear();
  }
  dscnames[1] = DescriptorFileName(dbname_, 2);
  if (!env_->FileExists(dscnames[1])) {
    dscnames[1].clear();
  }
  Status status;

  // Read "CURRENT" file, which contains a pointer to the current manifest file
  if (env_->FileExists(CurrentFileName(dbname_))) {
    std::string current;
    Status s;
    s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
    if (s.ok() && !current.empty()) {
      if (current[current.size() - 1] != '\n') {
        s = Status::Corruption("CURRENT file does not end with newline");
      } else {
        current.resize(current.size() - 1);
        dscnames[2] = dbname_ + "/" + current;
        if (dscnames[2] == dscnames[0] || dscnames[2] == dscnames[1]) {
          dscnames[2].clear();
        }
      }
    }
    if (!s.ok()) {
      Log(options_->info_log, "CURRENT read: %s", s.ToString().c_str());
      if (status.ok()) {
        status = s;
      }
    }
  }

  Version* current = current_;
  current->Ref();
  Builder* candidates[3];
  candidates[0] = candidates[1] = candidates[2] = NULL;
  Builder* selected = NULL;
  uint64_t final_next_file = 0;
  uint64_t final_last_seq = 0;
  uint64_t final_log_number = 0;
  uint64_t final_prev_log_number = 0;

  for (size_t i = 0; i < 3; i++) {
    if (!dscnames[i].empty()) {
      SequentialFile* file;
      Status s = env_->NewSequentialFile(dscnames[i], &file);
      if (s.ok()) {
        bool have_log_number = false;
        bool have_prev_log_number = false;
        bool have_next_file = false;
        bool have_last_sequence = false;
        uint64_t next_file = 0;
        uint64_t last_seq = 0;
        uint64_t log_number = 0;
        uint64_t prev_log_number = 0;
        Builder* builder = new Builder(this, current);

        {
          LogReporter reporter;
          reporter.status = &s;
          log::Reader reader(file, &reporter, true /*checksum*/,
                             0 /*initial_offset*/);
          Slice record;
          std::string scratch;
          while (reader.ReadRecord(&record, &scratch) && s.ok()) {
            VersionEdit edit;
            s = edit.DecodeFrom(record);
            if (s.ok()) {
              if (edit.has_comparator_ &&
                  edit.comparator_ != icmp_.user_comparator()->Name()) {
                s = Status::InvalidArgument(
                    edit.comparator_ + " does not match existing comparator ",
                    icmp_.user_comparator()->Name());
              }
            }

            if (s.ok()) {
              builder->Apply(&edit);
            }

            if (edit.has_log_number_) {
              log_number = edit.log_number_;
              have_log_number = true;
            }

            if (edit.has_prev_log_number_) {
              prev_log_number = edit.prev_log_number_;
              have_prev_log_number = true;
            }

            if (edit.has_next_file_number_) {
              next_file = edit.next_file_number_;
              have_next_file = true;
            }

            if (edit.has_last_sequence_) {
              last_seq = edit.last_sequence_;
              have_last_sequence = true;
            }
          }
        }
        delete file;
        file = NULL;

        if (s.ok()) {
          if (!have_next_file) {
            s = Status::Corruption("no next_file entry in descriptor");
          } else if (!have_log_number) {
            s = Status::Corruption("no log_number entry in descriptor");
          } else if (!have_last_sequence) {
            s = Status::Corruption("no last_seq_number entry in descriptor");
          }

          if (!have_prev_log_number) {
            prev_log_number = 0;
          }

          MarkFileNumberUsed(prev_log_number);
          MarkFileNumberUsed(log_number);
        }

        if (s.ok()) {
          candidates[i] = builder;

          if (last_seq >= final_last_seq && next_file >= final_next_file) {
            if (log_number >= final_log_number) {
              if (prev_log_number >= final_prev_log_number) {
                final_last_seq = last_seq;
                final_log_number = log_number;
                final_prev_log_number = prev_log_number;
                final_next_file = next_file;
                selected = builder;
              }
            }
          }
        } else {
          delete builder;
        }
      }

      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST read: %s", s.ToString().c_str());
        if (status.ok()) {
          status = s;
        }
      }
    }
  }

  if (status.ok()) {
    if (selected == NULL) {
      status = Status::Corruption(dbname_, "no valid manifest available");
    } else {
      Version* v = new Version(this);
      selected->SaveTo(v);
      // Install the chosen one
      Finalize(v);
      AppendVersion(v);

      if (!options_->rotating_manifest) {
        next_file_number_ = final_next_file + 1;
        manifest_file_number_ = final_next_file;
      } else {
        next_file_number_ = final_next_file;
        if (selected == candidates[0]) {
          manifest_file_number_ = 2;
        } else {
          manifest_file_number_ = 1;
        }
      }

      log_number_ = final_log_number;
      prev_log_number_ = final_prev_log_number;
      last_sequence_ = final_last_seq;
    }
  }

  for (size_t i = 0; i < 3; i++) {
    delete candidates[i];
  }
  current->Unref();
  return status;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

void VersionSet::Finalize(Version* v) {
  // Precomputed best level for next compaction
  int best_level = -1;
  double best_score = -1;

  assert(options_->enable_sublevel || v->files_.back().empty());
  if (options_->enable_sublevel) {
    assert(v->input_pool_.size() == v->output_pool_.size());
    for (int level = 0; level < v->input_pool_.size() - 1; ++level) {
      double score;
      if (level == 0) {
        assert(v->input_pool_[0].first == 0 &&
               v->input_pool_[0].first == v->output_pool_[0].first);
        assert(v->input_pool_[0].second == 1 &&
               v->input_pool_[0].second == v->output_pool_[0].second);
        score = v->files_[0].size() /
                static_cast<double>(options_->l0_compaction_trigger);
      } else {
        uint64_t bytes = 0;
        for (int i = v->input_pool_[level].first,
                 end =
                     v->input_pool_[level].first + v->input_pool_[level].second;
             i < end; ++i) {
          bytes += TotalFileSize(v->files_[i]);
        }
        for (int i = v->output_pool_[level].first,
                 end = v->output_pool_[level].first +
                       v->output_pool_[level].second;
             i < end; ++i) {
          bytes += TotalFileSize(v->files_[i]);
        }
        score = static_cast<double>(bytes) / MaxBytesForLevel(options_, level);
      }
      if (score > best_score) {
        best_level = level;
        best_score = score;
      }
    }
  } else {
    for (int level = 0; level < v->files_.size() - 1; level++) {
      double score;
      if (level == 0) {
        // We treat level-0 specially by bounding the number of files
        // instead of number of bytes for two reasons:
        //
        // (1) With larger write-buffer sizes, it is nice not to do too
        // many level-0 compactions.
        //
        // (2) The files in level-0 are merged on every read and
        // therefore we wish to avoid too many files when the individual
        // file size is small (perhaps because of a small write-buffer
        // setting, or very high compression ratios, or lots of
        // overwrites/deletions).
        score = v->files_[level].size() /
                static_cast<double>(options_->l0_compaction_trigger);
      } else {
        // Compute the ratio of current size to size limit.
        const uint64_t bytes = TotalFileSize(v->files_[level]);
        score = static_cast<double>(bytes) / MaxBytesForLevel(options_, level);
      }

      if (score > best_score) {
        best_level = level;
        best_score = score;
      }
    }
  }
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
  // TODO: Break up into multiple records to reduce memory usage on recovery?

  // TODO add sublevel information to enable sublevel recovery

  // Save metadata
  VersionEdit edit;
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  assert(options_->enable_sublevel ||
         compact_pointer_.size() == current_->files_.size());
  // Save compaction pointers
  for (int level = 0; level < compact_pointer_.size(); level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  // Save files
  for (int level = 0; level < current_->files_.size(); level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->seq_off, f->smallest,
                   f->largest);
    }
  }

  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0);
  assert(level < current_->files_.size());
  if (options_->enable_sublevel)
    return current_->NumFilesInLevel_sub(level);
  else
    return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
  assert(!options_->enable_sublevel ||
         current_->output_pool_.size() == current_->input_pool_.size());
  int total_level = options_->enable_sublevel ? current_->input_pool_.size()
                                              : current_->files_.size();
  size_t size = sizeof(scratch->buffer) - 1;
  char* next_position = scratch->buffer;
  int number = sprintf(next_position, "files[ ");
  size -= number;
  next_position += number;
  for (int level = 0; level < total_level && size > 0; ++level) {
    int num_files;
    if (options_->enable_sublevel) {
      num_files = current_->NumFilesInLevel_sub(level);
      number = snprintf(next_position, size, " %d@%d&%d", num_files,
                        current_->input_pool_[level].second,
                        current_->output_pool_[level].second);
    } else {
      num_files = (int)current_->files_[level].size();
      number = snprintf(next_position, size, " %d", num_files);
    }
    size -= number;
    next_position += number;
  }
  if (size >= 2) {
    sprintf(next_position, " ]");
  }
  return scratch->buffer;
}

uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < v->files_.size(); level++) {
    const std::vector<FileMetaData*>& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        // Entire file is after "ikey", so ignore
        if (level > 0) {
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for
          // "ikey".
          break;
        }
      } else {
        // "ikey" falls in the range for this table.  Add the
        // approximate offset of "ikey" within the table.
        Table* tableptr;
        Iterator* iter = table_cache_->NewIterator(
            ReadOptions(), files[i]->number, files[i]->file_size,
            files[i]->seq_off, &tableptr);
        if (tableptr != NULL) {
          result += tableptr->ApproximateOffsetOf(ikey.Encode());
        }
        delete iter;
      }
    }
  }
  return result;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < v->files_.size(); level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < current_->files_.size());
  return TotalFileSize(current_->files_[level]);
}

int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  for (int level = 1; level < current_->files_.size() - 1; level++) {
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      const FileMetaData* f = current_->files_[level][i];
      current_->GetOverlappingInputs(level + 1, &f->smallest, &f->largest,
                                     &overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        result = sum;
      }
    }
  }
  return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs,
                          InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2,
                           InternalKey* smallest, InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;

  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  const int total_level =
      options_->enable_sublevel ? c->NumInputSublevels() : 2;
  const int base_level =
      options_->enable_sublevel ? c->base_input_sublevel_ : c->level();
  const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : total_level);
  Iterator** list = new Iterator*[space];
  int num = 0;
  for (int which = 0; which < total_level; which++) {
    if (!c->inputs_[which].empty()) {
      if (base_level + which == 0) {
        const std::vector<FileMetaData*>& files = c->inputs_[which];
        for (size_t i = 0; i < files.size(); i++) {
          list[num++] =
              table_cache_->NewIterator(options, files[i]->number,
                                        files[i]->file_size, files[i]->seq_off);
        }
      } else {
        // Create concatenating iterator for the files from this level
        list[num++] = NewTwoLevelIterator(
            new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
            &GetFileIterator, table_cache_, options);
      }
    }
  }
  assert(num <= space);
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}

Compaction* VersionSet::PickCompaction(bool allow_seek_compaction) {
  Compaction* c;
  int level;

  // We prefer compactions triggered by too much data in a level over
  // the compactions triggered by seeks.
  const bool size_compaction = (current_->compaction_score_ >= 1);
  const bool seek_compaction = (current_->file_to_compact_ != NULL);
  if (size_compaction) {
    level = current_->compaction_level_;
    assert(level >= 0);
    if (options_->enable_sublevel) {
      assert(current_->input_pool_.size() == current_->output_pool_.size());
      assert(level < current_->input_pool_.size());
    } else {
      assert(level < current_->files_.size());
      assert(current_->files_.size() == compact_pointer_.size());
    }
    c = new Compaction(options_, level, this);

    if (options_->enable_sublevel) {
      SetupSublevelInputs(level, c);
    } else {
      // Pick the first file that comes after compact_pointer_[level]
      for (size_t i = 0; i < current_->files_[level].size(); i++) {
        FileMetaData* f = current_->files_[level][i];
        if (compact_pointer_[level].empty() ||
            icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) {
          c->inputs_[0].push_back(f);
          break;
        }
      }
      if (c->inputs_[0].empty()) {
        // Wrap-around to the beginning of the key space
        c->inputs_[0].push_back(current_->files_[level][0]);
      }
    }
  } else if (allow_seek_compaction && seek_compaction) {
    level = current_->file_to_compact_level_;
    c = new Compaction(options_, level, this);
    c->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return NULL;
  }

  if (!options_->enable_sublevel) {
    // Files in level 0 may overlap each other, so pick up all overlapping ones
    if (level == 0) {
      InternalKey smallest, largest;
      GetRange(c->inputs_[0], &smallest, &largest);
      // Note that the next call will discard the file we placed in
      // c->inputs_[0] earlier and replace it with an overlapping set
      // which will include the picked file.
      current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
      assert(!c->inputs_[0].empty());
    }

    SetupOtherInputs(c);
  }

  return c;
}

void VersionSet::SetupSublevelInputs(int level, Compaction* c) {
  assert(options_->enable_sublevel);
  assert(level >= 0);
  assert(current_->output_pool_.size() > level);
  assert(current_->output_pool_[level].second > 0);
  if (current_->output_pool_[level].second == 0) {
    fprintf(stderr, "abort3\n");
    fflush(stderr);
    abort();
  }
  assert(c->inputs_.size() == current_->output_pool_[level].second);

  assert(c->base_input_sublevel_ == current_->output_pool_[level].first);
  assert(current_->input_pool_.size() > level + 1);
  assert(current_->input_pool_[level + 1].second > 0);
  assert(current_->input_pool_[level + 1].first < current_->files_.size());
  assert(c->output_sublevel_ == current_->input_pool_[level + 1].first);
  // Pick up the table with the smallest left bound
  FileMetaData* f = NULL;
  int sublevel = -1;
  const int output_pool_level_2nd = current_->output_pool_[level].second;
  for (int i = 0; i < output_pool_level_2nd; ++i) {
    const int output_pool_level_1st = current_->output_pool_[level].first;
    const int row = i + output_pool_level_1st;
    const bool is_empty = current_->files_[row].empty();
    if (!is_empty &&
        (f == NULL ||
         icmp_.Compare(current_->files_[row][0]->smallest, f->smallest) < 0)) {
      f = current_->files_[row][0];
#ifndef NDEBUG
      if (level > 0 && current_->files_[row].size() > 1)
        assert(icmp_.Compare(current_->files_[row][1]->smallest, f->largest) >
               0);
#endif
      sublevel = i;
    }
  }
  assert(f != NULL);
  InternalKey left_bound = f->smallest;
  InternalKey right_bound = f->largest;

  // Get the range covering all overlapping files in all sublevels of this level
  if (level > 0) {
    c->start_key_ = left_bound;
    const Comparator* user_cmp = icmp_.user_comparator();
    int row_start = current_->output_pool_[level].first;
    std::vector<int> next_visit(current_->output_pool_[level].second);
    next_visit[sublevel] = 1;
    bool has_changed;
    do {
      has_changed = false;
      for (int i = 0; i < next_visit.size(); ++i) {
        int row = i + row_start;
        const Slice right_key = right_bound.user_key();
        while (next_visit[i] < current_->files_[row].size() &&
               user_cmp->Compare(
                   current_->files_[row][next_visit[i]]->largest.user_key(),
                   right_key) <= 0) {
          ++next_visit[i];
        }
        if (next_visit[i] == current_->files_[row].size()) continue;
        assert(user_cmp->Compare(
                   current_->files_[row][next_visit[i]]->largest.user_key(),
                   right_key) > 0);
        const InternalKey file_start =
            current_->files_[row][next_visit[i]]->smallest;
        if (user_cmp->Compare(file_start.user_key(), right_bound.user_key()) <=
            0) {
          right_bound = current_->files_[row][next_visit[i]]->largest;
          has_changed = true;
          ++next_visit[i];
        }
      }
    } while (has_changed);
  }
  for (int i = 0; i < c->inputs_.size(); ++i) {
    int row = i + current_->output_pool_[level].first;
    current_->GetOverlappingInputs(row, &left_bound, &right_bound,
                                   &c->inputs_[i]);
  }
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  assert(current_->files_.size() == compact_pointer_.size());
  assert(level < compact_pointer_.size());
  InternalKey smallest, largest;
  GetRange(c->inputs_[0], &smallest, &largest);

  current_->GetOverlappingInputs(level + 1, &smallest, &largest,
                                 &c->inputs_[1]);

  // Get entire range covered by compaction
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    const int64_t expanded0_size = TotalFileSize(expanded0);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size <
            ExpandedCompactionByteSizeLimit(options_)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      current_->GetOverlappingInputs(level + 1, &new_start, &new_limit,
                                     &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) {
        Log(options_->info_log,
            "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
            level, int(c->inputs_[0].size()), int(c->inputs_[1].size()),
            long(inputs0_size), long(inputs1_size), int(expanded0.size()),
            int(expanded1.size()), long(expanded0_size), long(inputs1_size));
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  if (level + 2 < current_->files_.size()) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }
  if (false) {
    Log(options_->info_log, "Compacting %d '%s' .. '%s'", level,
        smallest.DebugString().c_str(), largest.DebugString().c_str());
  }

  // Update the place where we will do the next compaction for this level.
  // We update this immediately instead of waiting for the VersionEdit
  // to be applied so that if the compaction fails, we will try a different
  // key range next time.
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

Compaction* VersionSet::CompactRange(int level, const InternalKey* begin,
                                     const InternalKey* end) {
  assert(level < current_->files_.size());
  if (options_->enable_sublevel) {
    // TODO implement this
    assert(begin == NULL);
    return NULL;
  } else {
    assert(current_->files_.size() == compact_pointer_.size());
    std::vector<FileMetaData*> inputs;
    current_->GetOverlappingInputs(level, begin, end, &inputs);
    if (inputs.empty()) {
      return NULL;
    }

    // Avoid compacting too much in one shot in case the range is large.
    // But we cannot do this for level-0 since level-0 files can overlap
    // and we must not pick one file and drop another older file if the
    // two files overlap.
    if (level > 0) {
      const uint64_t limit = MaxFileSizeForLevel(options_, level);
      uint64_t total = 0;
      for (size_t i = 0; i < inputs.size(); i++) {
        uint64_t s = inputs[i]->file_size;
        total += s;
        if (total >= limit) {
          inputs.resize(i + 1);
          break;
        }
      }
    }

    Compaction* c = new Compaction(options_, level, this);
    c->inputs_[0] = inputs;
    SetupOtherInputs(c);
    return c;
  }
}

Compaction::Compaction(const Options* options, int level, VersionSet* vset)
    : options_(options),
      level_(level),
      base_input_sublevel_(options->enable_sublevel
                               ? vset->current()->output_pool_[level].first
                               : -1),
      output_sublevel_(options->enable_sublevel
                           ? vset->current()->input_pool_[level + 1].first
                           : -1),
      max_output_file_size_(MaxFileSizeForLevel(options, level)),
      max_grand_parent_overlap_bytes_(MaxGrandParentOverlapBytes(options)),
      max_compaction_size_(options->enable_sublevel
                               ? MaxCompactionSizeForLevel(options, level)
                               : -1),
      input_version_(vset->current()),
      inputs_(options->enable_sublevel
                  ? vset->current()->output_pool_[level].second
                  : 2),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0),
      level_ptrs_(options->enable_sublevel ? 0 : vset->current()->NumLevels()) {
  input_version_->Ref();
  for (int i = 0; i < level_ptrs_.size(); i++) {
    level_ptrs_[i] = 0;
  }
}

Compaction::~Compaction() {
  if (input_version_ != NULL) {
    input_version_->Unref();
  }
}

FileMetaData* Compaction::GetTheOnlyFile() const {
  for (int i = 0; i < inputs_.size(); ++i)
    if (!inputs_[i].empty()) return inputs_[i][0];
  assert(false);
  return NULL;
}

int Compaction::TotalNumInputFiles(const bool need_truncate,
                                   const InternalKey* truncate_key) const {
  int count = 0;
  for (int i = 0; i < inputs_.size(); ++i) {
    if (need_truncate) {
      for (std::vector<FileMetaData *>::const_iterator
               iter = inputs_[i].begin();
           iter != inputs_[i].end() &&
           input_version_->vset_->icmp_.Compare((*iter)->smallest,
                                                *truncate_key) < 0;
           ++count, ++iter)
        ;
    } else
      count += inputs_[i].size();
  }
  return count;
}

int64_t Compaction::TotalNumInputBytes(const bool need_truncate,
                                       const InternalKey* truncate_key) const {
  int64_t bytes = 0;
  for (int which = 0; which < inputs_.size(); which++) {
    for (std::vector<FileMetaData *>::const_iterator
             iter = inputs_[which].begin();
         iter != inputs_[which].end() &&
         (!need_truncate ||
          input_version_->vset_->icmp_.Compare((*iter)->smallest,
                                               *truncate_key) < 0);
         bytes += (*iter)->file_size, ++iter)
      ;
  }
  return bytes;
}

bool Compaction::IsTrivialMove() const {
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  if (!options_->enable_sublevel) {
    return (num_input_files(0) == 1 && num_input_files(1) == 0 &&
            (!options_->enable_should_stop_before ||
             TotalFileSize(grandparents_) <= max_grand_parent_overlap_bytes_));
  } else {
    return TotalNumInputFiles(false, NULL) == 1;
  }
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  int input_base_level =
      options_->enable_sublevel ? base_input_sublevel_ : level_;
  for (int which = 0; which < inputs_.size(); which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->DeleteFile(input_base_level + which, inputs_[which][i]->number);
    }
  }
}

void Compaction::AddInputDeletionsOrUpdates(VersionEdit* edit,
                                            const InternalKey& key) {
  assert(options_->enable_sublevel);
  edit->SetUpdateTruncate(key);
  const InternalKeyComparator& icmp = input_version_->vset_->icmp_;

  for (int which = 0; which < inputs_.size(); which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      const FileMetaData* meta = inputs_[which][i];
      if (icmp.Compare(meta->largest, key) < 0) {
        edit->DeleteFile(base_input_sublevel_ + which,
                         inputs_[which][i]->number);
      } else {
        if (icmp.Compare(meta->smallest, key) < 0) {
          edit->UpdateFile(base_input_sublevel_ + which, meta->number);
        }
        if (level_ > 0) {
          assert(i == inputs_[which].size() - 1 ||
                 icmp.Compare(inputs_[which][i + 1]->smallest, key) > 0);
          break;
        }
      }
    }
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Maybe use binary search to find right entry instead of linear search?
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < input_version_->files_.size(); lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    for (; level_ptrs_[lvl] < files.size();) {
      FileMetaData* f = files[level_ptrs_[lvl]];
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // Key falls in this file's range, so definitely not base level
          return false;
        }
        break;
      }
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  if (!options_->enable_should_stop_before) return false;
  if (options_->enable_sublevel) {
    // TODO implement this function if we observe sometimes too many files are
    // compacted in one compaction
    return false;
  }
  // Scan to find earliest grandparent file that contains key.
  const InternalKeyComparator* icmp = &input_version_->vset_->icmp_;
  while (grandparent_index_ < grandparents_.size() &&
         icmp->Compare(internal_key,
                       grandparents_[grandparent_index_]->largest.Encode()) >
             0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  seen_key_ = true;

  if (overlapped_bytes_ > max_grand_parent_overlap_bytes_) {
    // Too much overlap for current output; start new output
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}

void Compaction::ReleaseInputs() {
  if (input_version_ != NULL) {
    input_version_->Unref();
    input_version_ = NULL;
  }
}

}  // namespace pdlfs
