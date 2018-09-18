/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/policy/newb_udp.hpp"

#include "caf/config.hpp"

#include "caf/io/network/native_socket.hpp"
#include "caf/io/newb.hpp"

#ifdef CAF_WINDOWS
# ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
# endif // WIN32_LEAN_AND_MEAN
# ifndef NOMINMAX
#   define NOMINMAX
# endif
# ifdef CAF_MINGW
#   undef _WIN32_WINNT
#   undef WINVER
#   define _WIN32_WINNT WindowsVista
#   define WINVER WindowsVista
#   include <w32api.h>
# endif
# include <io.h>
# include <windows.h>
# include <winsock2.h>
# include <ws2ipdef.h>
# include <ws2tcpip.h>
#else
# include <unistd.h>
# include <arpa/inet.h>
# include <cerrno>
# include <fcntl.h>
# include <netdb.h>
# include <netinet/in.h>
# include <netinet/ip.h>
# include <netinet/tcp.h>
# include <sys/socket.h>
# include <sys/types.h>
# ifdef CAF_POLL_MULTIPLEXER
#   include <poll.h>
# elif defined(CAF_EPOLL_MULTIPLEXER)
#   include <sys/epoll.h>
# else
#   error "neither CAF_POLL_MULTIPLEXER nor CAF_EPOLL_MULTIPLEXER defined"
# endif
#endif

namespace caf {
namespace policy {

udp_transport::udp_transport()
    : maximum{std::numeric_limits<uint16_t>::max()},
      first_message{true},
      writing{false},
      written{0},
      offline_sum{0} {
  // nop
}

io::network::rw_state udp_transport::read_some(io::newb_base* parent) {
  CAF_LOG_TRACE(CAF_ARG(parent->fd()));
  memset(sender.address(), 0, sizeof(sockaddr_storage));
  io::network::socket_size_type len = sizeof(sockaddr_storage);
  auto buf_ptr = static_cast<io::network::socket_recv_ptr>(receive_buffer.data());
  auto buf_len = receive_buffer.size();
  auto sres = ::recvfrom(parent->fd(), buf_ptr, buf_len,
                         0, sender.address(), &len);
  if (io::network::is_error(sres, true)) {
    CAF_LOG_ERROR("recvfrom failed" << CAF_ARG(sres));
    return io::network::rw_state::failure;
  }
  if (sres == 0)
    CAF_LOG_INFO("Received empty datagram");
  else if (sres > static_cast<io::network::signed_size_type>(buf_len))
    CAF_LOG_WARNING("recvfrom cut of message, only received "
                    << CAF_ARG(buf_len) << " of " << CAF_ARG(sres) << " bytes");
  received_bytes = (sres > 0) ? static_cast<size_t>(sres) : 0;
  *sender.length() = static_cast<size_t>(len);
  if (first_message) {
    endpoint = sender;
    first_message = false;
  }
  return io::network::rw_state::success;
}

void udp_transport::prepare_next_read(io::newb_base*) {
  received_bytes = 0;
  receive_buffer.resize(maximum);
}

io::network::rw_state udp_transport::write_some(io::newb_base* parent) {
  using namespace caf::io::network;
  CAF_LOG_TRACE(CAF_ARG(parent->fd()) << CAF_ARG(send_buffer.size()));
  socket_size_type len = static_cast<socket_size_type>(*endpoint.clength());
  auto buf_ptr = reinterpret_cast<socket_send_ptr>(send_buffer.data() + written);
  auto buf_len = send_sizes.front();
  auto sres = ::sendto(parent->fd(), buf_ptr, buf_len,
                       0, endpoint.caddress(), len);
  if (is_error(sres, true)) {
    CAF_LOG_ERROR("sendto failed" << CAF_ARG(sres));
    return io::network::rw_state::failure;
  }
  size_t sent_bytes = (sres > 0) ? static_cast<size_t>(sres) : 0;
  // TODO: This only works if we always write send_sizes.front()
  if (sent_bytes != buf_len) {
    CAF_LOG_ERROR("failed to send complete datagram");
    return io::network::rw_state::failure;
  }
  send_sizes.pop_front();
  written += sent_bytes;
  auto remaining = send_buffer.size() - written;
  if (remaining == 0)
    prepare_next_write(parent);
  return io::network::rw_state::success;
}

void udp_transport::prepare_next_write(io::newb_base* parent) {
  written = 0;
  send_buffer.clear();
  send_sizes.clear();
  if (offline_buffer.empty()) {
    writing = false;
    parent->stop_writing();
  } else {
    // Add size of last chunk.
    offline_sizes.push_back(offline_buffer.size() - offline_sum);
    // Switch buffers.
    send_buffer.swap(offline_buffer);
    send_sizes.swap(offline_sizes);
    // Reset sum.
    offline_sum = 0;
  }
}

io::byte_buffer& udp_transport::wr_buf() {
  if (!offline_buffer.empty()) {
    auto chunk_size = offline_buffer.size() - offline_sum;
    offline_sizes.push_back(chunk_size);
    offline_sum += chunk_size;
  }
  return offline_buffer;
}

void udp_transport::flush(io::newb_base* parent) {
  CAF_ASSERT(parent != nullptr);
  CAF_LOG_TRACE(CAF_ARG(offline_buffer.size()));
  if (!offline_buffer.empty() && !writing) {
    parent->start_writing();
    writing = true;
    prepare_next_write(parent);
  }
}

expected<io::network::native_socket>
udp_transport::connect(const std::string& host, uint16_t port,
        optional<io::network::protocol::network> preferred) {
  auto res = io::network::new_remote_udp_endpoint_impl(host, port, preferred);
  if (!res)
    return std::move(res.error());
  endpoint = res->second;
  return res->first;
}

expected<io::network::native_socket>
accept_udp::create_socket(uint16_t port, const char* host, bool reuse) {
  auto res = io::network::new_local_udp_endpoint_impl(port, host, reuse);
  if (!res)
    return std::move(res.error());
  return (*res).first;
}

std::pair<io::network::native_socket, transport_ptr>
accept_udp::accept_event(io::newb_base*) {
  auto res = io::network::new_local_udp_endpoint_impl(0, nullptr);
  if (!res) {
    CAF_LOG_DEBUG("failed to create local endpoint");
    return {io::network::invalid_native_socket, nullptr};
  }
  auto sock = std::move(res->first);
  transport_ptr ptr{new udp_transport};
  return {sock, std::move(ptr)};
}

void accept_udp::init(io::newb_base& n) {
  n.start();
}

} // namespace policy
} // namespace caf
