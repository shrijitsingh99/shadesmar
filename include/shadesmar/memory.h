//
// Created by squadrick on 8/2/19.
//

#ifndef shadesmar_MEMORY_H
#define shadesmar_MEMORY_H

#include <cstdint>
#include <cstring>

#include <atomic>
#include <iostream>
#include <string>

#include <boost/interprocess/managed_shared_memory.hpp>

#include <msgpack.hpp>

#include <shadesmar/ipc_lock.h>
#include <shadesmar/macros.h>
#include <shadesmar/tmp.h>

using namespace boost::interprocess;
namespace shm {

static size_t max_buffer_size = (1U << 28);

uint8_t *create_memory_segment(const std::string &topic_name, int &fd,
                               size_t size) {
  while (true) {
    fd = shm_open(topic_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
      fchmod(fd, 0644);
      ftruncate(fd, size);
    }
    if (errno == EEXIST) {
      fd = shm_open(topic_name.c_str(), O_RDWR, 0644);
      if (fd < 0 && errno == ENOENT) {
        continue;
      }
    }
    break;
  }

  auto *ptr = static_cast<uint8_t *>(
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));

  return ptr;
}

struct Element {
  managed_shared_memory::handle_t addr_hdl{};
  size_t size{};
  bool empty = true;
  IPC_Lock mutex;
};

struct SharedQueue {
  std::atomic_uint32_t init{}, counter{}, queue_size{};
  Element *elements;

  SharedQueue() {
    init = 0;
    elements = nullptr;
  }

  void lock(uint32_t idx) { elements[idx].mutex.lock(); }
  void unlock(uint32_t idx) { elements[idx].mutex.unlock(); }
  void lock_sharable(uint32_t idx) { elements[idx].mutex.lock_sharable(); }
  void unlock_sharable(uint32_t idx) { elements[idx].mutex.unlock_sharable(); }
};

class Memory {
public:
  explicit Memory(const std::string &topic_name, uint32_t queue_size)
      : topic_name_(topic_name), queue_size_(queue_size) {

    if ((queue_size & (queue_size - 1)) != 0) {
      std::cerr << "queue_size must be power of two";
      exit(-1);
    }

    // TODO: Has contention on shared_queue_exists
    bool shared_queue_exists = tmp::exists(topic_name);
    if (!shared_queue_exists)
      tmp::write_topic(topic_name);

    size_t size = sizeof(SharedQueue) + queue_size * sizeof(Element);

    auto *base_addr = create_memory_segment(topic_name, fd_, size);
    auto *elem_addr = base_addr + sizeof(SharedQueue);
    DEBUG("FD="<<fd_);

    raw_buf_ = std::make_shared<managed_shared_memory>(
        open_or_create, (topic_name + "Raw").c_str(), max_buffer_size);

    if (!shared_queue_exists) {
      shared_queue_ = new (base_addr) SharedQueue();
      auto blank = new Element[queue_size];
      std::memcpy(shared_queue_->elements, blank, queue_size * sizeof(Element));
      init_info();
    } else {
      shared_queue_ = reinterpret_cast<SharedQueue *>(base_addr);
      shared_queue_->elements = reinterpret_cast<Element *>(elem_addr);
    }

    queue_size_ = shared_queue_->queue_size;
  }

  ~Memory() = default;

  void init_info() {
    uint32_t zero = 0;
    if (shared_queue_->init.compare_exchange_strong(zero, INFO_INIT)) {
      shared_queue_->queue_size = queue_size_;
      shared_queue_->counter = 0;
    }
  }

  bool write(void *data, size_t size) {
    uint32_t pos = counter() & (queue_size_ - 1); // modulo for power of 2
    shared_queue_->lock(pos);

    if (size > raw_buf_->get_free_memory()) {
      std::cerr << "Increase max_buffer_size" << std::endl;
      return false;
    }
    auto elem = &(shared_queue_->elements[pos]);
    void *addr = raw_buf_->get_address_from_handle(elem->addr_hdl);

    if (!elem->empty) {
      raw_buf_->deallocate(addr);
    }

    void *new_addr = raw_buf_->allocate(size);

    std::memcpy(new_addr, data, size);
    elem->addr_hdl = raw_buf_->get_handle_from_address(new_addr);
    elem->size = size;
    elem->empty = false;

    inc_counter();
    shared_queue_->unlock(pos);
    return true;
  }

  bool read_without_copy(msgpack::object_handle &oh, uint32_t pos) {
    pos &= queue_size_ - 1;
    shared_queue_->lock_sharable(pos);
    auto elem = &(shared_queue_->elements[pos]);
    if (elem->empty)
      return false;

    const char *dst = reinterpret_cast<const char *>(
        raw_buf_->get_address_from_handle(elem->addr_hdl));

    try {
      oh = msgpack::unpack(dst, elem->size);
    } catch (...) {
      return false;
    }

    shared_queue_->unlock_sharable(pos);
    return true;
  }

  bool read_with_copy(msgpack::object_handle &oh, uint32_t pos) {
    pos &= queue_size_ - 1;
    shared_queue_->lock_sharable(pos);
    auto elem = &(shared_queue_->elements[pos]);
    if (elem->empty)
      return false;

    auto size = elem->size;
    auto *src = malloc(elem->size);
    auto *dst = raw_buf_->get_address_from_handle(elem->addr_hdl);
    std::memcpy(src, dst, elem->size);

    shared_queue_->unlock_sharable(pos);

    try {
      oh = msgpack::unpack(reinterpret_cast<const char *>(src), size);
      free(src);
    } catch (...) {
      free(src);
      return false;
    }

    return true;
  }

  bool read_raw(void **msg, size_t &size, uint32_t pos) {
    pos &= queue_size_ - 1;
    shared_queue_->lock_sharable(pos);
    auto elem = &(shared_queue_->elements[pos]);
    if (elem->empty)
      return false;

    size = elem->size;
    *msg = malloc(elem->size);
    auto *dst = raw_buf_->get_address_from_handle(elem->addr_hdl);
    std::memcpy(*msg, dst, elem->size);

    shared_queue_->unlock_sharable(pos);

    return true;
  }

  // TODO: `counter` out of sync when number of pubs > 1.
  inline __attribute__((always_inline)) uint32_t counter() {
    return shared_queue_->counter.load();
  }

  inline __attribute__((always_inline)) void inc_counter() {
    shared_queue_->counter += 1;
  }

private:
  uint32_t queue_size_;
  int fd_;
  std::string topic_name_;
  std::shared_ptr<managed_shared_memory> raw_buf_;
  SharedQueue *shared_queue_;
};
} // namespace shm
#endif // shadesmar_MEMORY_H
