/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 **/

#ifndef QUICKSTEP_CLI_SIMPLE_SOCKET_SIMPLE_SOCKET_CONNECTION_HPP_
#define QUICKSTEP_CLI_SIMPLE_SOCKET_SIMPLE_SOCKET_CONNECTION_HPP_

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/simple_socket/SimpleSocketContent.hpp"
#include "utility/Macros.hpp"

#include "glog/logging.h"

namespace quickstep {

class SimpleSocketConnection {
 public:
  explicit SimpleSocketConnection(const int socket_fd)
      : socket_fd_(socket_fd),
        request_data_(nullptr) {
    try {
      receiveRequest();
    } catch (const std::exception &e) {
      error_message_ = std::make_unique<std::string>(e.what());
    }
  }

  ~SimpleSocketConnection() {
    if (request_data_ != nullptr) {
      std::free(request_data_);
      request_data_ = nullptr;
      shutdown(socket_fd_, SHUT_RDWR);
      close(socket_fd_);
    }
  }

  const SimpleSocketContent& getRequest() const {
    return request_;
  }

  bool hasError() const  {
    return error_message_ != nullptr;
  }

  const std::string& getErrorMessage() const {
    DCHECK(hasError());
    return *error_message_;
  }

  void sendResponse(const std::string &stdout_str,
                    const std::string &stderr_str) const {
    SimpleSocketContent response;
    response.setField("stdout", stdout_str);
    response.setField("stderr", stderr_str);
    sendResponse(response);
  }

 private:
  static constexpr std::size_t kMaxRequestDataLength = 0x10000000LL;

  void receiveRequest() {
    request_data_length_ = receiveUInt64();
    if (request_data_length_ >= kMaxRequestDataLength) {
      throw std::runtime_error(
          "Overflow request data length = " + std::to_string(request_data_length_));
    }

    request_data_ = std::malloc(request_data_length_);
    receiveData(request_data_, request_data_length_);

    // Decode request data.
    const std::uint64_t *size_ptr =
        static_cast<const std::uint64_t*>(request_data_);
    const std::uint64_t num_fields = Ntohll(*size_ptr++);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> field_sizes;
    for (std::size_t i = 0; i < num_fields; ++i) {
      const std::uint64_t key_size = Ntohll(*size_ptr++);
      const std::uint64_t value_size = Ntohll(*size_ptr++);
      field_sizes.emplace_back(key_size, value_size);
    }

    const char *data_ptr = reinterpret_cast<const char*>(size_ptr);
    for (std::size_t i = 0; i < num_fields; ++i) {
      const auto &fs = field_sizes[i];
      const std::uint64_t key_size = fs.first;
      const std::uint64_t value_size = fs.second;
      const char *key = data_ptr;
      const char *value = data_ptr + key_size;
      request_.setField(key, key_size, value, value_size);
      data_ptr += key_size + value_size;
    }

    CHECK_EQ(static_cast<const char*>(request_data_) + request_data_length_,
             data_ptr);
  }

  inline void receiveData(void *dst, std::size_t bytes) const {
    while (bytes != 0) {
      const ssize_t bytes_read = read(socket_fd_, dst, bytes);
      if (bytes_read <= 0) {
        throw std::runtime_error("Error receiving data from socket connection"
                                 " at SimpleSocketConnection::receiveData()");
      }
      bytes -= bytes_read;
      dst = static_cast<char*>(dst) + bytes_read;
    }
  }

  inline std::uint64_t receiveUInt64() const {
    std::uint64_t code = 0;
    receiveData(&code, sizeof(std::uint64_t));
    return Ntohll(code);
  }

  inline void writeUInt64(const std::uint64_t value) const {
    const uint64_t code = Htonll(value);
    write(socket_fd_, &code, sizeof(std::uint64_t));
  }

  void sendResponse(const SimpleSocketContent &response) const {
    // Calculate field sizes.
    std::uint64_t total_size = 0;
    std::vector<std::pair<const char*, const char*>> field_ptrs;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> field_sizes;
    for (const auto &it : response.fields()) {
      const std::string &key = it.first;
      const StringPiece &value = it.second;
      field_ptrs.emplace_back(key.data(), value.first);
      field_sizes.emplace_back(key.length(), value.second);
      total_size += key.length() + value.second;
    }

    const std::size_t num_fields = field_ptrs.size();
    total_size += sizeof(std::uint64_t) * (1 + 2 * num_fields);

    writeUInt64(total_size);
    writeUInt64(num_fields);
    for (const auto &it : field_sizes) {
      writeUInt64(it.first);
      writeUInt64(it.second);
    }
    for (std::size_t i = 0; i < num_fields; ++i) {
      const auto &fp = field_ptrs[i];
      const auto &fs = field_sizes[i];
      write(socket_fd_, fp.first, fs.first);
      write(socket_fd_, fp.second, fs.second);
    }
  }

  inline static std::uint64_t Ntohll(const std::uint64_t code) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(code);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(code >> 32);
    const std::uint64_t value =
        (static_cast<std::uint64_t>(ntohl(lo32)) << 32)
            | static_cast<std::uint64_t>(ntohl(hi32));
    return value;
  }

  inline static std::uint64_t Htonll(const std::uint64_t code) {
    const std::uint32_t lo32 = static_cast<std::uint32_t>(code);
    const std::uint32_t hi32 = static_cast<std::uint32_t>(code >> 32);
    return (static_cast<std::uint64_t>(htonl(lo32)) << 32)
               | static_cast<std::uint64_t>(htonl(hi32));
  }

  const int socket_fd_;
  std::uint64_t request_data_length_;
  void *request_data_;
  SimpleSocketContent request_;
  std::unique_ptr<std::string> error_message_;

  DISALLOW_COPY_AND_ASSIGN(SimpleSocketConnection);
};

}  // namespace quickstep

#endif  // QUICKSTEP_CLI_SIMPLE_SOCKET_SIMPLE_SOCKET_CONNECTION_HPP_
