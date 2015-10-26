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
const uint32_t kBindTimeoutMs = 1000;

struct rdma_event_channel *rdma_create_event_channel(void);
const auto kEventChanDeleter = [](rdma_event_channel* p) { ::rdma_destroy_event_channel(p); };
class EventChanAutoDel
  : public std::unique_ptr< rdma_event_channel, decltype(kEventChanDeleter) > {
 public:
  EventChanAutoDel()
    : std::unique_ptr< rdma_event_channel, decltype(kEventChanDeleter) >(
        nullptr,
        kEventChanDeleter) {
  }
};

const auto kAddrDeleter = [](rdma_addrinfo* p) { ::rdma_freeaddrinfo(p); };
class AddrInfoAutoDel : public std::unique_ptr< rdma_addrinfo, decltype(kAddrDeleter) > {
 public:
  AddrInfoAutoDel()
    : std::unique_ptr< rdma_addrinfo, decltype(kAddrDeleter) >(nullptr, kAddrDeleter) {
  }
};

const auto kPdDeleter = [](ibv_pd* p) { ::ibv_dealloc_pd(p); };
class PdAutoDel : public std::unique_ptr< ibv_pd, decltype(kPdDeleter) > {
 public:
  PdAutoDel()
    : std::unique_ptr< ibv_pd, decltype(kPdDeleter) >(nullptr, kPdDeleter) {
  }
};

const auto kEpDeleter = [](rdma_cm_id* p) { ::rdma_destroy_ep(p); };
class EpAutoDel : public std::unique_ptr< rdma_cm_id, decltype(kEpDeleter) > {
 public:
  EpAutoDel()
    : std::unique_ptr< rdma_cm_id, decltype(kEpDeleter) >(nullptr, kEpDeleter) {
  }
};

const auto kCmIdDeleter = [](rdma_cm_id* p) { ::rdma_destroy_id(p); };
class CmIdAutoDel : public std::unique_ptr< rdma_cm_id, decltype(kCmIdDeleter) > {
 public:
  CmIdAutoDel()
    : std::unique_ptr< rdma_cm_id, decltype(kCmIdDeleter) >(nullptr, kCmIdDeleter) {
  }
};

const auto kEpDisconnect = [](rdma_cm_id* p) { ::rdma_disconnect(p); };
class ClientAutoDisconn : public std::unique_ptr< rdma_cm_id, decltype(kEpDisconnect) > {
 public:
  ClientAutoDisconn()
    : std::unique_ptr< rdma_cm_id, decltype(kEpDisconnect) >(nullptr, kEpDisconnect) {
  }
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
    bool listen_first,
    const char* myself_port_num_string,
    const char* other_port_num_string,
    std::atomic<bool>* myself_ready,
    const std::atomic<bool>* other_ready)
    : name_(name),
      listen_first_(listen_first),
      myself_port_num_string_(myself_port_num_string),
      other_port_num_string_(other_port_num_string),
      myself_ready_(myself_ready),
      other_ready_(other_ready),
      send_array_(new char[kSendArray]),
      remote_addr_base_(0),
      remote_rkey_(0) {
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
  int get_channel();
  int get_local_cm_id();
  int bind_local_cm_id();
  int get_pd();
  int get_addr(const char* port_num_string, bool passive, AddrInfoAutoDel* addr_autodel);
  int get_ep(rdma_addrinfo* addr, EpAutoDel* out);
  int reg_mr(
    ibv_pd* pd,
    void* memory,
    uint32_t length,
    bool allow_remote_read,
    bool allow_remote_write,
    MrAutoDel* out);
  int connect_to_client();
  int connect_to_server();

  int test_write();
  int test_read();

  const std::string name_;
  const bool listen_first_;
  const char* const myself_port_num_string_;
  const char* const other_port_num_string_;
  std::atomic<bool>* const myself_ready_;
  const std::atomic<bool>* const other_ready_;
  char* const send_array_;

  SharedCircularBufferContainer container_;

  EventChanAutoDel  channel_;
  CmIdAutoDel       local_cm_id_;
  sockaddr_storage  local_sin_;
  PdAutoDel         pd_;
  AddrInfoAutoDel myself_addr_;
  AddrInfoAutoDel other_addr_;
  EpAutoDel myself_ep_;
  EpAutoDel other_ep_;
  ClientAutoDisconn client_;
  MrAutoDel recv_mr_;
  MrAutoDel send_mr_;
  uint64_t  remote_addr_base_;
  uint32_t  remote_rkey_;

  std::thread myself_thread_;
};

int NodeThread::get_channel() {
  std::cout << name_ << ": rdma_create_event_channel..." << std::endl;
  rdma_event_channel* channel = ::rdma_create_event_channel();
  if (channel == nullptr) {
    std::cerr << name_ << ": rdma_create_event_channel failed " << errno << std::endl;
    return -1;
  }
  channel_.reset(channel);
  return 0;
}

int NodeThread::get_local_cm_id() {
  std::cout << name_ << ": rdma_create_id..." << std::endl;
  rdma_cm_id* local_id;
  int ret = ::rdma_create_id(channel_.get(), &local_id, nullptr, RDMA_PS_TCP);
  if (ret) {
    std::cerr << name_ << ": rdma_create_id failed " << errno << std::endl;
    return -1;
  }
  local_cm_id_.reset(local_id);
  return 0;
}

int NodeThread::bind_local_cm_id() {
  std::cout << name_ << ": rdma_resolve_addr..." << std::endl;

  sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&local_sin_);
  const rdma_addrinfo* local_addr = myself_addr_.get();
  rdma_cm_id* local_id = local_cm_id_.get();
  int ret = ::rdma_resolve_addr(
    local_id,
    nullptr,
    local_addr->ai_src_addr,
    kBindTimeoutMs);
  if (ret) {
    std::cerr << name_ << ": rdma_resolve_addr failed " << errno << std::endl;
    return -1;
  }
  return 0;
}

int NodeThread::get_pd() {
  std::cout << name_ << ": ibv_alloc_pd..." << std::endl;
  rdma_cm_id* cm_id = local_cm_id_.get();
  ibv_pd* pd = ::ibv_alloc_pd(cm_id->verbs);
  if (pd == nullptr) {
    std::cerr << name_ << ": ibv_alloc_pd failed " << errno << std::endl;
    return -1;
  }
  pd_.reset(pd);
  return 0;
}

int NodeThread::get_addr(const char* port_num_string, bool passive, AddrInfoAutoDel* out) {
  std::cout << name_ << ": rdma_getaddrinfo('" << port_num_string << "', passive=" << passive
    << ")..." << std::endl;
  rdma_addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  if (passive) {
    hints.ai_flags = RAI_PASSIVE;
  }
  hints.ai_port_space = RDMA_PS_TCP;

  rdma_addrinfo *addr;
  int ret = ::rdma_getaddrinfo(
    const_cast<char*>(kLocalNode),
    const_cast<char*>(port_num_string),
    &hints,
    &addr);
  if (ret) {
    std::cerr << name_ << ": rdma_getaddrinfo failed " << errno << std::endl;
    return ret;
  }
  out->reset(addr);
  return 0;
}

int NodeThread::get_ep(rdma_addrinfo* addr, EpAutoDel* out) {
  const bool passive = addr->ai_flags & RAI_PASSIVE;
  std::cout << name_ << ": rdma_create_ep" << (passive ? "(passive)" : "") << "..." << std::endl;

  rdma_cm_id* ep_id;
  int ret;
  if (passive) {
    ret = ::rdma_create_ep(&ep_id, addr, nullptr, nullptr);
  } else {
    ibv_qp_init_attr init_attr;
    std::memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = 1;
    init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = 256;
    init_attr.sq_sig_all = 1;
    ret = ::rdma_create_ep(&ep_id, addr, pd_.get(), &init_attr);
  }
  if (ret) {
    std::cerr << name_ << ": rdma_create_ep failed " << errno << std::endl;
    return errno;
  }
  out->reset(ep_id);
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
    << ", write=" << allow_remote_write << "..." << std::endl;
  // rdma_reg_write/read/msgs() can't specify pd alone.
  // Use the raw ibv_reg_mr() instead.
  int access = IBV_ACCESS_LOCAL_WRITE;
  if (allow_remote_write) {
    access |= IBV_ACCESS_REMOTE_WRITE;
  }
  if (allow_remote_read) {
    access |= IBV_ACCESS_REMOTE_READ;
  }
  ibv_mr* reg_mr = ::ibv_reg_mr(pd, memory, length, access);
  if (reg_mr == nullptr) {
    std::cerr << name_ << ": ibv_reg_mr failed " << errno;
    return errno;
  }
  out->reset(reg_mr);
  return 0;
}
int NodeThread::connect_to_client() {
  std::cout << name_ << ": rdma_listen..." << std::endl;
  int ret = ::rdma_listen(myself_ep_.get(), 0);
  if (ret) {
    std::cerr << name_ << ": rdma_listen failed " << errno;
    return errno;
  }

  myself_ready_->store(true);

  std::cout << name_ << ": rdma_get_request..." << std::endl;
  rdma_cm_id* client_id;
  ret = ::rdma_get_request(myself_ep_.get(), &client_id);
  if (ret) {
    std::cerr << name_ << ": rdma_get_request failed " << errno;
    return errno;
  }
  client_.reset(client_id);

  std::cout << name_ << ": rdma_accept..." << std::endl;
  ret = ::rdma_accept(client_id, nullptr);
  if (ret) {
    std::cerr << name_ << ": rdma_accept failed " << errno;
    return errno;
  }
  return 0;
}

int NodeThread::connect_to_server() {
  std::cout << name_ << ": waiting for other_ready..." << std::endl;

  while (!other_ready_->load());

  std::cerr << name_ << ": rdma_connect..." << std::endl;
  int ret = ::rdma_connect(other_ep_.get(), 0);
  if (ret) {
    std::cerr << name_ << ": rdma_connect failed " << errno;
    return ret;
  }

  return 0;
}

int NodeThread::test_read() {
  std::cout << name_ << ": waiting for other's write..." << std::endl;

  uint32_t cur_place = 0;  // so far only one message used.
  const Message* message = reinterpret_cast<const Message*>(container_.buffer_ + cur_place);
  message->header_.spin_while_zero_payload_length();

  uint32_t len = message->header_.payload_length_;
  std::string payload(message->payload_, len);
  std::cout << name_ << ": received a message: '" << payload << "'" << std::endl;

  return 0;
}

int NodeThread::test_write() {
  std::cout << name_ << ": writing to other's memory..." << std::endl;

  Message* send_buffer = reinterpret_cast<Message*>(send_array_);
  std::string payload("hello from ");
  payload += name_;
  send_buffer->prepare_message(payload.size(), payload.c_str());

  uint32_t cur_place = 0;  // so far only one message used.
  uint32_t msg_len = send_buffer->header_.get_message_length();
  int ret = ::rdma_post_write(
    client_.get(),
    nullptr,
    send_buffer,
    msg_len,
    send_mr_.get(),
    0,
    remote_addr_base_ + cur_place,
    remote_rkey_);
  if (ret) {
    std::cerr << name_ << ": rdma_post_write failed " << errno << std::endl;
    return ret;
  }

  return 0;
}


void NodeThread::thread_main() {
  std::cout << name_ << " started" << std::endl;
  if (container_.alloc()) {
    std::exit(1);
  }

  if (get_addr(myself_port_num_string_, true, &myself_addr_)) {
    std::exit(1);
  }
  if (get_addr(other_port_num_string_, false, &other_addr_)) {
    std::exit(1);
  }
/*
  if (get_channel()) {
    std::exit(1);
  }
  if (get_local_cm_id()) {
    std::exit(1);
  }
  if (bind_local_cm_id()) {
    std::exit(1);
  }
  if (get_pd()) {
    std::exit(1);
  }*/

  if (get_ep(myself_addr_.get(), &myself_ep_)) {
    std::exit(1);
  }
  if (get_ep(other_addr_.get(), &other_ep_)) {
    std::exit(1);
  }

  if (reg_mr(
    other_ep_->pd,
    container_.buffer_,
    sizeof(SharedCircularBuffer),
    false,  // remote-read=false. another node is only writing
    true,   // remote-write=true.
    &recv_mr_)) {
    std::exit(1);
  }

  if (reg_mr(
    other_ep_->pd,
    send_array_,
    kSendArray,
    false,  // remote-read=false. used only to send a message
    false,  // remote-write=false. used only to send a message
    &send_mr_)) {
    std::exit(1);
  }

  if (listen_first_) {
    if (connect_to_client()) {
      std::exit(1);
    }
    if (connect_to_server()) {
      std::exit(1);
    }
  } else {
    if (connect_to_server()) {
      std::exit(1);
    }
    if (connect_to_client()) {
      std::exit(1);
    }
  }

  std::cout << name_ << " establishied a bi-directional connection!" << std::endl;
/*
  if (listen_first_) {
    if (test_read()) {
      std::exit(1);
    }
    if (test_write()) {
      std::exit(1);
    }
  } else {
    if (test_write()) {
      std::exit(1);
    }
    if (test_read()) {
      std::exit(1);
    }
  }
*/
  std::cout << name_ << " ended" << std::endl;
  /*

  std::cout << name_ << ": ibv_query_qp..." << std::endl;
  ibv_qp_init_attr init_attr;
  ibv_qp_attr qp_attr;
  std::memset(&qp_attr, 0, sizeof(qp_attr));
  std::memset(&init_attr, 0, sizeof(init_attr));
  zap();
  ret = ::ibv_query_qp(client_id->qp, &qp_attr, IBV_QP_CAP, &init_attr);
  if (ret) {
    std::cerr << name_ << ": ibv_query_qp failed " << errno;
    return errno;
  }
   */
}

const char* kIpPort1 = "4242";
const char* kIpPort2 = "4243";
int main(int argc, char **argv) {
  (void) argc;
  (void) argv;

  std::atomic<bool> thread1_ready(false);
  std::atomic<bool> thread2_ready(false);
  NodeThread thread1("thread1", true, kIpPort1, kIpPort2, &thread1_ready, &thread2_ready);
  NodeThread thread2("thread2", false, kIpPort2, kIpPort1, &thread2_ready, &thread1_ready);

  thread1.launch();
  thread2.launch();

  std::cout << "Launched nodes. waiting.." << std::endl;

  thread1.join();
  thread2.join();

  struct ibv_qp_attr qp_attr;
  struct ibv_wc wc;

  std::cout << "Successfully exit" << std::endl;
  return 0;
}

