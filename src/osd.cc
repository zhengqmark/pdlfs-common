/*
 * Copyright (c) 2015-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "pdlfs-common/osd.h"
#include "pdlfs-common/env.h"

#include "osd_internal.h"

namespace pdlfs {

OSD::~OSD() {}

OSDEnv::OSDEnv(OSD* osd) { impl_ = new InternalImpl(osd); }

OSDEnv::~OSDEnv() { delete impl_; }

static bool ResolvePath(const Slice& path, Slice* parent, Slice* base) {
  const char* a = path.data();
  const char* b = strrchr(a, '/');

  *base = Slice(b + 1, a + path.size() - b - 1);
  if (b - a != 0) {
    *parent = Slice(a, b - a);
  } else {
    *parent = Slice("/");
  }
  return true;
}

bool OSDEnv::FileSetExists(const Slice& dirname) {
  return impl_->HasFileSet(dirname);
}

bool OSDEnv::FileExists(const Slice& fname) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return false;
  } else {
    return impl_->HasFile(fp);
  }
}

Status OSDEnv::ReadFileToString(const Slice& fname, std::string* data) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->GetFile(fp, data);
  }
}

Status OSDEnv::WriteStringToFile(const Slice& fname, const Slice& data) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->PutFile(fp, data);
  }
}

Status OSDEnv::GetFileSize(const Slice& fname, uint64_t* size) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->FileSize(fp, size);
  }
}

Status OSDEnv::MountFileSet(const MountOptions& options, const Slice& dirname) {
  Slice name;
  if (!options.set_name.empty()) {
    name = options.set_name;
  } else {
    Slice parent;
    if (!ResolvePath(dirname, &parent, &name)) {
      return Status::InvalidArgument(dirname, "path cannot be resolved");
    }
  }
  FileSet* fset = new FileSet(options, name);
  Status s = impl_->LinkFileSet(dirname, fset);
  if (!s.ok()) {
    delete fset;
  }
  return s;
}

Status OSDEnv::UnmountFileSet(const UnmountOptions& options,
                              const Slice& dirname) {
  return impl_->UnlinkFileSet(dirname, options.deletion);
}

Status OSDEnv::GetChildren(const Slice& dirname,
                           std::vector<std::string>* names) {
  return impl_->ListFileSet(dirname, names);
}

Status OSDEnv::SynFileSet(const Slice& dirname) {
  return impl_->SynFileSet(dirname);
}

Status OSDEnv::DeleteFile(const Slice& fname) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->DeleteFile(fp);
  }
}

Status OSDEnv::CopyFile(const Slice& src, const Slice& dst) {
  ResolvedPath sfp, dfp;
  if (!ResolvePath(src, &sfp.mntptr, &sfp.base)) {
    return Status::InvalidArgument(src, "path cannot be resolved");
  } else if (!ResolvePath(dst, &dfp.mntptr, &dfp.base)) {
    return Status::InvalidArgument(dst, "path cannot be resolved");
  } else {
    return impl_->CopyFile(sfp, dfp);
  }
}

Status OSDEnv::NewSequentialFile(const Slice& fname, SequentialFile** result) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->NewSequentialFile(fp, result);
  }
}

Status OSDEnv::NewRandomAccessFile(const Slice& fname,
                                   RandomAccessFile** result) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->NewRandomAccessFile(fp, result);
  }
}

Status OSDEnv::NewWritableFile(const Slice& fname, WritableFile** result) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return Status::InvalidArgument(fname, "path cannot be resolved");
  } else {
    return impl_->NewWritableFile(fp, result);
  }
}

std::string OSDEnv::TEST_LookupFile(const Slice& fname) {
  ResolvedPath fp;
  if (!ResolvePath(fname, &fp.mntptr, &fp.base)) {
    return std::string();
  } else {
    return impl_->TEST_GetObjectName(fp);
  }
}

static Status DoWriteStringToFile(OSD* osd, const Slice& data,
                                  const Slice& name, bool should_sync) {
  WritableFile* file;
  Status s = osd->NewWritableObj(name, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok() && should_sync) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  delete file;  // Will auto-close if we did not close above
  if (!s.ok()) {
    osd->Delete(name);
  }
  return s;
}

Status WriteStringToFile(OSD* osd, const Slice& data, const Slice& name) {
  return DoWriteStringToFile(osd, data, name, false);
}

Status WriteStringToFileSync(OSD* osd, const Slice& data, const Slice& name) {
  return DoWriteStringToFile(osd, data, name, true);
}

Status ReadFileToString(OSD* osd, const Slice& name, std::string* data) {
  data->clear();
  SequentialFile* file;
  Status s = osd->NewSequentialObj(name, &file);
  if (!s.ok()) {
    return s;
  }
  const size_t kBufferSize = 8192;
  char* space = new char[kBufferSize];
  while (true) {
    Slice fragment;
    s = file->Read(kBufferSize, &fragment, space);
    if (!s.ok()) {
      break;
    }
    AppendSliceTo(data, fragment);
    if (fragment.empty()) {
      break;
    }
  }
  delete[] space;
  delete file;
  return s;
}

class OSDAdaptor : public OSD {
 private:
  Env* env_;
  std::string prefix_;

  std::string FullPath(const Slice& name) {
    std::string path = prefix_ + name.data();
    return path;
  }

 public:
  OSDAdaptor(Env* env, const Slice& prefix) : env_(env) {
    AppendSliceTo(&prefix_, prefix);
    env_->CreateDir(prefix_);
    prefix_.append("/<obj>");
  }

  virtual ~OSDAdaptor() {}

  virtual Status NewSequentialObj(const Slice& name, SequentialFile** result) {
    return env_->NewSequentialFile(FullPath(name), result);
  }

  virtual Status NewRandomAccessObj(const Slice& name,
                                    RandomAccessFile** result) {
    return env_->NewRandomAccessFile(FullPath(name), result);
  }

  virtual Status NewWritableObj(const Slice& name, WritableFile** result) {
    return env_->NewWritableFile(FullPath(name), result);
  }

  virtual bool Exists(const Slice& name) {
    return env_->FileExists(FullPath(name));
  }

  virtual Status Size(const Slice& name, uint64_t* obj_size) {
    return env_->GetFileSize(FullPath(name), obj_size);
  }

  virtual Status Delete(const Slice& name) {
    return env_->DeleteFile(FullPath(name));
  }

  virtual Status Put(const Slice& name, const Slice& data) {
    return WriteStringToFile(env_, data, FullPath(name));
  }

  virtual Status Get(const Slice& name, std::string* data) {
    return ReadFileToString(env_, FullPath(name), data);
  }

  virtual Status Copy(const Slice& src, const Slice& target) {
    return env_->CopyFile(FullPath(src), FullPath(target));
  }

 private:
  // No copying allowed
  OSDAdaptor(const OSDAdaptor&);
  void operator=(const OSDAdaptor&);
};

OSD* NewOSDAdaptor(const Slice& prefix, Env* env) {
  if (env == NULL) env = Env::Default();
  return new OSDAdaptor(env, prefix);
}

}  // namespace pdlfs
