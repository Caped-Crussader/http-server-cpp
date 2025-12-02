// Thread-safe memory pool for high-performance object allocation
// Uses lock-free stack for minimal contention between threads

#ifndef MEMORY_POOL_H_
#define MEMORY_POOL_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <unordered_set>
#include <vector>

namespace http_server {

// A thread-safe, lock-free memory pool for fixed-size objects.
// Pre-allocates objects to avoid heap allocation overhead during request handling.
template <typename T>
class MemoryPool {
 public:
  explicit MemoryPool(size_t pool_size) : pool_size_(pool_size), free_head_(nullptr) {
    // Pre-allocate all nodes
    nodes_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
      nodes_.emplace_back(std::unique_ptr<Node>(new Node()));
      // Store data pointer for O(1) lookup
      pool_data_ptrs_.insert(reinterpret_cast<T*>(nodes_.back()->data));
    }
    
    // Build the free list
    for (size_t i = 0; i < pool_size; ++i) {
      Node* node = nodes_[i].get();
      node->next.store(free_head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      free_head_.store(node, std::memory_order_relaxed);
    }
  }

  ~MemoryPool() = default;

  // Non-copyable and non-movable
  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;
  MemoryPool(MemoryPool&&) = delete;
  MemoryPool& operator=(MemoryPool&&) = delete;

  // Acquire an object from the pool. Returns nullptr if pool is exhausted.
  T* Acquire() {
    Node* node = PopFreeNode();
    if (node == nullptr) {
      // Pool exhausted, fall back to heap allocation
      return new T();
    }
    // Construct object in-place
    return new (&node->data) T();
  }

  // Release an object back to the pool.
  // The object must have been acquired from this pool or allocated with new.
  void Release(T* obj) {
    if (obj == nullptr) return;

    // Check if this object belongs to our pool
    Node* node = FindNode(obj);
    if (node != nullptr) {
      // Destroy the object
      obj->~T();
      // Return node to free list
      PushFreeNode(node);
    } else {
      // Object was heap-allocated (pool was exhausted)
      delete obj;
    }
  }

  // Get pool statistics
  size_t pool_size() const { return pool_size_; }
  
  size_t available_count() const {
    size_t count = 0;
    Node* current = free_head_.load(std::memory_order_acquire);
    while (current != nullptr) {
      ++count;
      current = current->next.load(std::memory_order_relaxed);
    }
    return count;
  }

 private:
  struct Node {
    std::atomic<Node*> next{nullptr};
    alignas(T) char data[sizeof(T)];
  };

  size_t pool_size_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::unordered_set<T*> pool_data_ptrs_;  // O(1) lookup for pool membership
  std::atomic<Node*> free_head_;

  // Lock-free pop from free list
  Node* PopFreeNode() {
    Node* old_head = free_head_.load(std::memory_order_acquire);
    while (old_head != nullptr) {
      Node* new_head = old_head->next.load(std::memory_order_relaxed);
      if (free_head_.compare_exchange_weak(old_head, new_head,
                                            std::memory_order_release,
                                            std::memory_order_acquire)) {
        return old_head;
      }
      // old_head is updated by compare_exchange_weak on failure
    }
    return nullptr;  // Pool exhausted
  }

  // Lock-free push to free list
  void PushFreeNode(Node* node) {
    Node* old_head = free_head_.load(std::memory_order_acquire);
    do {
      node->next.store(old_head, std::memory_order_relaxed);
    } while (!free_head_.compare_exchange_weak(old_head, node,
                                                std::memory_order_release,
                                                std::memory_order_acquire));
  }

  // Find the node containing this object (if it belongs to our pool)
  // O(1) lookup using hash set instead of O(n) linear search
  Node* FindNode(T* obj) {
    if (pool_data_ptrs_.find(obj) == pool_data_ptrs_.end()) {
      return nullptr;  // Not from our pool
    }
    // Calculate node address from data pointer using offsetof-style arithmetic
    // Node layout: [atomic<Node*> next][alignas(T) char data[sizeof(T)]]
    char* data_addr = reinterpret_cast<char*>(obj);
    char* node_addr = data_addr - offsetof(Node, data);
    return reinterpret_cast<Node*>(node_addr);
  }
};

}  // namespace http_server

#endif  // MEMORY_POOL_H_
