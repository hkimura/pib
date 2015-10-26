/**
 * @file hk_farm_like.cpp
 * FaRM-like local-spin/raw-writes communication prototype.
 */
#include <errno.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>


// this is a quite new flag, so not exists in many environment. define it here.
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT  26
#endif  // MAP_HUGE_SHIFT
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif  // MAP_HUGE_2MB
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif  // MAP_HUGE_1GB

const uint32_t kBufferSize = 1U << 21;
const uint32_t kSendArray = 1U << 12;
const char* kLocalNode = "127.0.0.1";
const char* kIpPort = "4242";
const uint32_t kBindTimeoutMs = 1000;

void zap() {
  // to understand what are happening in what order, put delays everywhere
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

const auto kAddrDeleter = [](rdma_addrinfo* p) { ::rdma_freeaddrinfo(p); };
class AddrInfoAutoDel : public std::unique_ptr< rdma_addrinfo, decltype(kAddrDeleter) > {
 public:
  AddrInfoAutoDel()
    : std::unique_ptr< rdma_addrinfo, decltype(kAddrDeleter) >(nullptr, kAddrDeleter) {
  }
};

// this one is a bit more complex, so can't simply use unique_ptr
class EpContainer {
 public:
  EpContainer() : cm_id_(nullptr), needs_disconnect_(false) {
  }
  ~EpContainer() {
    release();
  }
  void reset(rdma_cm_id* cm_id) {
    release();
    cm_id_ = cm_id;
    needs_disconnect_ = false;
  }
  void on_connect() {
    needs_disconnect_  = true;
  }

  void release() {
    if (cm_id_) {
      if (needs_disconnect_) {
        ::rdma_disconnect(cm_id_);
        needs_disconnect_ = false;
      }
      ::rdma_destroy_ep(cm_id_);
      cm_id_ = nullptr;
    }
  }
  rdma_cm_id* get() { return cm_id_; }

  rdma_cm_id* cm_id_;
  bool        needs_disconnect_;
};

const auto kMrDeleter = [](ibv_mr* p) { ::ibv_dereg_mr(p); };
class MrAutoDel : public std::unique_ptr< ibv_mr, decltype(kMrDeleter) > {
 public:
  MrAutoDel()
    : std::unique_ptr< ibv_mr, decltype(kMrDeleter) >(nullptr, kMrDeleter) {
  }
  MrAutoDel(ibv_mr* p) : std::unique_ptr< ibv_mr, decltype(kMrDeleter) >(p, kMrDeleter) {
  }
};

/**
 * Represents one message written by another node.
 */
struct Message {
  struct Header {
    /**
     * Actual byte length of this message _including_ this header.
     * It cannot be 0 as we don't allow 0-byte payload.
     * payload_length_==0 thus means the header is not yet written.
     */
    uint32_t  payload_length_;
    uint32_t  reserved_;

    void set_payload_length(uint32_t payload_length) {
      payload_length_ = payload_length;
      reserved_ = 0;
    }
    void spin_while_zero_payload_length() const {
      const std::atomic<uint32_t>* var
        = reinterpret_cast< const std::atomic<uint32_t>* >(&payload_length_);
      while (var->load(std::memory_order_acquire) == 0);
    }
    uint32_t get_aligned_payload_length() const {
      if (payload_length_ % 8 == 0) {
        return payload_length_;
      } else {
        return payload_length_ - (payload_length_ % 8) + 8;
      }
    }
    uint32_t get_message_length() const {
      return sizeof(Header) * 2 + get_aligned_payload_length();
    }
  };

  Header    header_;

  /**
   * The data sent from the remote node.
   * Actually of align8(payload_length).
   * So, do NOT do sizeof(Message).
   */
  char      payload_[8];

  /**
   * There actually is a copy of the header at the end of the message.
   * It must be the same value as length_ when the write to the
   * block is completed. RDMA guarantees the effect of remote writes
   * are in address order, so this protocol works.
   */
  // Header    header_re_;

  void prepare_message(uint32_t payload_length, const void* payload) {
    header_.set_payload_length(payload_length);
    std::memcpy(payload_, payload, payload_length);
    if (payload_length % 8 != 0) {
      std::memset(payload_ + payload_length, 0, 8 - (payload_length % 8));
    }
    Header* header_re = reinterpret_cast<Header*>(payload_ + header_.get_aligned_payload_length());
    header_re->set_payload_length(payload_length);
  }
};

struct SharedCircularBuffer {
  char data_[kBufferSize];
  void init() {
    std::memset(data_, 0, sizeof(data_));
  }
};

struct InitPacket {
  InitPacket() : buffer_addr_base_(0), buffer_rkey_(0), buffer_size_(0) {
  }
  void set(const SharedCircularBuffer* buffer, const ibv_mr* buffer_mr) {
    buffer_addr_base_ = reinterpret_cast<uintptr_t>(buffer);
    buffer_rkey_ = buffer_mr->rkey;
    buffer_size_ = kBufferSize;
  }
  uint64_t buffer_addr_base_;
  uint32_t buffer_rkey_;
  uint32_t buffer_size_;

  friend std::ostream& operator<<(std::ostream& o, const InitPacket& v) {
    o << "addr=0x" << std::hex << v.buffer_addr_base_
      << ", rkey=0x" << std::hex << v.buffer_rkey_
      << ", size=0x" << std::hex << v.buffer_size_;
    return o;
  }
};

struct SharedCircularBufferContainer {
  SharedCircularBufferContainer() : buffer_(nullptr) {}
  ~SharedCircularBufferContainer() { clear(); }
  void clear() {
    if (buffer_) {
      ::munmap(buffer_, sizeof(SharedCircularBuffer));
      buffer_ = nullptr;
    }
  }
  int alloc() {
    clear();
    buffer_ = reinterpret_cast<SharedCircularBuffer*>(::mmap(
      nullptr,
      sizeof(SharedCircularBuffer),
      PROT_READ | PROT_WRITE,
      MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGE_2MB | MAP_HUGETLB,
      -1,
      0));
    if (buffer_ == nullptr || buffer_ == MAP_FAILED) {
      std::cerr << "mmap() failed. No hugepages allocated, I guess."
        << " eg) sudo sh -c 'echo 2000 > /proc/sys/vm/nr_hugepages'" << std::endl;
      return errno;
    }
    buffer_->init();
    return 0;
  }

  SharedCircularBuffer* buffer_;
};

class NodeThread {
 public:
  NodeThread(
    const std::string& name,
    bool listen_first)
    : name_(name),
      listen_first_(listen_first),
      send_array_(new char[kSendArray]) {
  }
  ~NodeThread() {
    delete[] send_array_;
  }

  void launch() {
    // This version of the code just uses in-process thread, which shares the memory address space.
    // In reality, we should use fork() to test the behavior, but this is just a coding exercise.
    myself_thread_ = std::move(std::thread(&NodeThread::thread_main, this));
  }
  void join() {
    if (myself_thread_.joinable()) {
      myself_thread_.join();
    }
  }

 private:
  void thread_main();
  int thread_main_impl();
  int get_addr(bool passive);
  int create_server_ep();

  int reg_mr(
    ibv_pd* pd,
    void* memory,
    uint32_t length,
    bool allow_remote_read,
    bool allow_remote_write,
    MrAutoDel* out);
  int reg_send_array_mr(ibv_pd* pd);
  int reg_buffer_mr(ibv_pd* pd);
  int reg_init_packets_mr(ibv_pd* pd);

  int connect_to_client();
  int connect_to_server();

  int init_each_other(rdma_cm_id* other);

  int test_write(rdma_cm_id* other);
  int test_read(rdma_cm_id* other);

  const std::string name_;
  const bool listen_first_;
  char* const send_array_;

  SharedCircularBufferContainer container_;

  AddrInfoAutoDel addr_;
  EpContainer server_ep_;
  EpContainer client_ep_;
  MrAutoDel recv_mr_;
  MrAutoDel send_mr_;
  InitPacket my_init_packet_;
  InitPacket other_init_packet_;
  MrAutoDel my_init_mr_;
  MrAutoDel other_init_mr_;

  std::thread myself_thread_;
};

int NodeThread::get_addr(bool passive) {
  std::cout << name_ << ": rdma_getaddrinfo, passive=" << passive << ")..." << std::endl;
  rdma_addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  if (passive) {
    hints.ai_flags = RAI_PASSIVE;
  }
  hints.ai_port_space = RDMA_PS_TCP;

  zap();
  rdma_addrinfo *addr;
  int ret = ::rdma_getaddrinfo(
    const_cast<char*>(kLocalNode),
    const_cast<char*>(kIpPort),
    &hints,
    &addr);
  if (ret) {
    std::cerr << name_ << ": rdma_getaddrinfo failed " << std::strerror(errno) << std::endl;
    return ret;
  }
  addr_.reset(addr);
  return 0;
}

int NodeThread::create_server_ep() {
  std::cout << name_ << ": rdma_create_ep..." << std::endl;

  ibv_qp_init_attr init_attr;
  std::memset(&init_attr, 0, sizeof(init_attr));
  init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;
  init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
  init_attr.cap.max_inline_data = 256;
  init_attr.sq_sig_all = 1;
  rdma_cm_id* ep_id;
  zap();
  int ret = ::rdma_create_ep(&ep_id, addr_.get(), nullptr, &init_attr);
  if (ret) {
    std::cerr << name_ << ": rdma_create_ep failed " << std::strerror(errno) << std::endl;
    return errno;
  }
  server_ep_.reset(ep_id);
  return 0;
}
int NodeThread::reg_mr(
  ibv_pd* pd,
  void* memory,
  uint32_t length,
  bool allow_remote_read,
  bool allow_remote_write,
  MrAutoDel* out) {
  std::cout << name_ << ": ibv_reg_mr(read=" << allow_remote_read
    << ", write=" << allow_remote_write << ")..." << std::endl;
  // rdma_reg_write/read/msgs() can't specify pd alone.
  // Use the raw ibv_reg_mr() instead.
  int access = IBV_ACCESS_LOCAL_WRITE;
  if (allow_remote_write) {
    access |= IBV_ACCESS_REMOTE_WRITE;
  }
  if (allow_remote_read) {
    access |= IBV_ACCESS_REMOTE_READ;
  }
  zap();
  ibv_mr* reg_mr = ::ibv_reg_mr(pd, memory, length, access);
  if (reg_mr == nullptr) {
    std::cerr << name_ << ": ibv_reg_mr failed " << std::strerror(errno);
    return errno;
  }
  out->reset(reg_mr);
  return 0;
}

int NodeThread::reg_buffer_mr(ibv_pd* pd) {
  return reg_mr(
    pd,
    container_.buffer_,
    sizeof(SharedCircularBuffer),
    false,  // remote-read=false. another node is only writing
    true,   // remote-write=true.
    &recv_mr_);
}

int NodeThread::reg_send_array_mr(ibv_pd* pd) {
  return reg_mr(
    pd,
    send_array_,
    kSendArray,
    false,  // remote-read=false. used only to send a message
    false,  // remote-write=false. used only to send a message
    &send_mr_);
}

int NodeThread::reg_init_packets_mr(ibv_pd* pd) {
  // these are only for messaging. no remote write/read
  int ret = reg_mr(
    pd,
    &my_init_packet_,
    sizeof(my_init_packet_),
    false,
    false,
    &my_init_mr_);
  if (ret) {
    std::cerr << name_ << ": failed to reg my_init_packet_" << std::strerror(errno);
    return ret;
  }
  ret = reg_mr(
    pd,
    &other_init_packet_,
    sizeof(other_init_packet_),
    false,
    false,
    &other_init_mr_);
  if (ret) {
    std::cerr << name_ << ": failed to reg other_init_packet_" << std::strerror(errno);
    return ret;
  }
  return 0;
}

int NodeThread::connect_to_client() {
  std::cout << name_ << ": rdma_listen..." << std::endl;
  zap();
  int ret = ::rdma_listen(server_ep_.get(), 0);
  if (ret) {
    std::cerr << name_ << ": rdma_listen failed " << std::strerror(errno);
    return errno;
  }

  std::cout << name_ << ": rdma_get_request..." << std::endl;
  rdma_cm_id* client_id;
  zap();
  ret = ::rdma_get_request(server_ep_.get(), &client_id);
  if (ret) {
    std::cerr << name_ << ": rdma_get_request failed " << std::strerror(errno);
    return errno;
  }
  client_ep_.reset(client_id);

  std::cout << name_ << ": rdma_accept..." << std::endl;
  zap();
  ret = ::rdma_accept(client_id, nullptr);
  if (ret) {
    std::cerr << name_ << ": rdma_accept failed " << std::strerror(errno);
    return errno;
  }

  client_ep_.on_connect();
  return 0;
}

int NodeThread::connect_to_server() {
  std::cout << name_ << ": rdma_connect..." << std::endl;
  zap();
  int ret = ::rdma_connect(server_ep_.get(), nullptr);
  if (ret) {
    std::cerr << name_ << ": rdma_connect failed " << std::strerror(errno);
    return ret;
  }

  server_ep_.on_connect();
  return 0;
}

int NodeThread::init_each_other(rdma_cm_id* other) {
  my_init_packet_.set(container_.buffer_, recv_mr_.get());
  std::cout << name_ << ": my_init_pack=" << my_init_packet_ << std::endl;

  std::cout << name_ << ": rdma_post_recv..." << std::endl;
  zap();
  int ret = ::rdma_post_recv(
    other,
    nullptr,
    &other_init_packet_,
    sizeof(other_init_packet_),
    other_init_mr_.get());
  if (ret) {
    std::cerr << name_ << ": rdma_post_recv failed " << std::strerror(errno);
    return ret;
  }

  std::cout << name_ << ": rdma_post_send..." << std::endl;
  zap();
  ret = ::rdma_post_send(
    other,
    nullptr,
    &my_init_packet_,
    sizeof(my_init_packet_),
    my_init_mr_.get(),
    0);
  if (ret) {
    std::cerr << name_ << ": rdma_post_send failed " << std::strerror(errno);
    return ret;
  }

  ibv_wc wc;
  std::cout << name_ << ": rdma_get_send_comp..." << std::endl;
  zap();
  while ((ret = ::rdma_get_send_comp(other, &wc)) == 0);
  if (ret < 0) {
    std::cerr << name_ << ": rdma_get_send_comp failed " << std::strerror(errno);
    return ret;
  }

  std::cout << name_ << ": rdma_get_recv_comp..." << std::endl;
  zap();
  while ((ret = ::rdma_get_recv_comp(other, &wc)) == 0);
  if (ret < 0) {
    std::cerr << name_ << ": rdma_get_recv_comp failed " << std::strerror(errno);
    return ret;
  }

  zap();
  std::cout << name_ << ": other_init_pack=" << other_init_packet_ << std::endl;
  return 0;
}

int NodeThread::test_read(rdma_cm_id* other) {
  std::cout << name_ << ": waiting for other's write..." << std::endl;

  uint32_t cur_place = 0;  // so far only one message used.
  const Message* message = reinterpret_cast<const Message*>(container_.buffer_ + cur_place);
  message->header_.spin_while_zero_payload_length();

  uint32_t len = message->header_.payload_length_;
  std::string payload(message->payload_, len);
  std::cout << name_ << ": received a message: '" << payload << "'" << std::endl;

  return 0;
}

int NodeThread::test_write(rdma_cm_id* other) {
  std::cout << name_ << ": writing to other's memory..." << std::endl;

  Message* send_buffer = reinterpret_cast<Message*>(send_array_);
  std::string payload("hello from ");
  payload += name_;
  send_buffer->prepare_message(payload.size(), payload.c_str());

  uint32_t cur_place = 0;  // so far only one message used.
  uint32_t msg_len = send_buffer->header_.get_message_length();
  int ret = ::rdma_post_write(
    other,
    nullptr,
    send_buffer,
    msg_len,
    send_mr_.get(),
    0,
    other_init_packet_.buffer_addr_base_ + cur_place,
    other_init_packet_.buffer_rkey_);
  if (ret) {
    std::cerr << name_ << ": rdma_post_write failed " << std::strerror(errno) << std::endl;
    return ret;
  }

  return 0;
}
void NodeThread::thread_main() {
  std::cout << name_ << " started" << std::endl;
  int ret = thread_main_impl();
  if (ret) {
    std::cerr << name_ << " ended with error " << ret << ", errno=" << std::strerror(errno) << std::endl;
    std::exit(1);
  } else {
    std::cout << name_ << " ended without error" << std::endl;
  }
}

#define CHECK_RET(x)\
{\
  int __e = x;\
  if (__e) {\
    return __e;\
  }\
}

std::atomic< uint32_t > s_end_count(0);

int NodeThread::thread_main_impl() {
  CHECK_RET(container_.alloc());
  CHECK_RET(get_addr(listen_first_));

  CHECK_RET(create_server_ep());

  if (listen_first_) {
    CHECK_RET(connect_to_client());

    CHECK_RET(reg_buffer_mr(client_ep_.get()->pd));
    CHECK_RET(reg_send_array_mr(client_ep_.get()->pd));
    CHECK_RET(reg_init_packets_mr(client_ep_.get()->pd));

    CHECK_RET(init_each_other(client_ep_.get()));
  } else {
    CHECK_RET(reg_buffer_mr(server_ep_.get()->pd));
    CHECK_RET(reg_send_array_mr(server_ep_.get()->pd));
    CHECK_RET(reg_init_packets_mr(server_ep_.get()->pd));

    CHECK_RET(connect_to_server());

    CHECK_RET(init_each_other(server_ep_.get()));
  }

  std::cout << name_ << " establishied a bi-directional connection!" << std::endl;
  if (listen_first_) {
    CHECK_RET(test_read(client_ep_.get()));
    CHECK_RET(test_write(client_ep_.get()));
  } else {
    CHECK_RET(test_write(server_ep_.get()));
    CHECK_RET(test_read(server_ep_.get()));
  }
  zap();
  ++s_end_count;
  while (s_end_count != 2U);
  return 0;
}

int main(int argc, char **argv) {
  (void) argc;
  (void) argv;

  NodeThread thread1("thread1", true);
  NodeThread thread2("thread2", false);

  thread1.launch();
  zap();
  zap();
  zap();
  zap();
  thread2.launch();

  std::cout << "Launched nodes. waiting.." << std::endl;

  thread1.join();
  thread2.join();

  std::cout << "Successfully exit" << std::endl;
  return 0;
}
