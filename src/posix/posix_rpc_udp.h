/*
 * Copyright (c) 2019 Carnegie Mellon University,
 * Copyright (c) 2019 Triad National Security, LLC, as operator of
 *     Los Alamos National Laboratory.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */
#pragma once

#include "posix_rpc.h"

#include <stddef.h>
#include <sys/socket.h>

namespace pdlfs {
// RPC srv impl using UDP.
class PosixUDPServer : public PosixSocketServer {
 public:
  explicit PosixUDPServer(rpc::If* srv, size_t max_msgsz = 1432);
  virtual ~PosixUDPServer() { BGStop(); }

  // On OK, BGStart() should then be called to start receiving client data.
  virtual Status OpenAndBind(const std::string& uri);

 private:
  // State for each incoming procedure call.
  struct CallState {
    struct sockaddr_storage addr;  // Location of the caller
    socklen_t addrlen;
    size_t msgsz;  // Payload size
    char msg[1];
  };
  void HandleIncomingCall(CallState* call);
  virtual Status BGLoop(int myid);
  const size_t max_msgsz_;  // Buffer size for incoming rpc messages
  rpc::If* const srv_;
};

// UDP client.
class PosixUDPCli : public rpc::If {
 public:
  explicit PosixUDPCli(uint64_t timeout, size_t max_msgsz = 1432);
  virtual ~PosixUDPCli();

  // Each call results in 1 UDP send and 1 UDP receive.
  virtual Status Call(Message& in, Message& out) RPCNOEXCEPT;
  // If we fail to open, error status will be set and the next Call()
  // operation will return it.
  void Open(const std::string& uri);

 private:
  // No copying allowed
  void operator=(const PosixUDPCli&);
  PosixUDPCli(const PosixUDPCli& other);
  const uint64_t rpc_timeout_;  // In microseconds
  const size_t max_msgsz_;
  Status status_;
  int fd_;
};

}  // namespace pdlfs