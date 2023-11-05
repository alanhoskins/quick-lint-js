// Copyright (C) 2020  Matthew "strager" Glazar
// See end of file for extended copyright information.

#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <quick-lint-js/container/flexible-array.h>
#include <quick-lint-js/port/memory-resource.h>
#include <quick-lint-js/port/span.h>
#include <quick-lint-js/util/cast.h>
#include <utility>

#if defined(QLJS_DEBUG) && QLJS_DEBUG
#define QLJS_DEBUG_BUMP_ALLOCATOR 1
#else
#define QLJS_DEBUG_BUMP_ALLOCATOR 0
#endif

namespace quick_lint_js {
// A memory allocator with a few features:
//
// * no per-object free()/delete
// * bulk free() via rewind() (doesn't call destructors)
// * in-place growing of allocations
//
// Internally, Linked_Bump_Allocator maintains a linked list of chunks.
class Linked_Bump_Allocator final : public Memory_Resource {
 private:
  struct Chunk_Header;
  using Chunk = Flexible_Array<char, Chunk_Header>;

 public:
  explicit Linked_Bump_Allocator(const char* debug_owner);

  Linked_Bump_Allocator(const Linked_Bump_Allocator&) = delete;
  Linked_Bump_Allocator& operator=(const Linked_Bump_Allocator&) = delete;

  ~Linked_Bump_Allocator() override;

  // Deallocate previously-allocated memory.
  void release();

  struct Rewind_State {
    // Private to linked_bump_allocator. Do not use.
    Chunk* chunk_;
    char* next_allocation_;
    char* chunk_end_;
  };

  // Calls allocator->rewind() when destructed.
  class Rewind_Guard {
   public:
    explicit Rewind_Guard(Linked_Bump_Allocator* allocator)
        : allocator_(allocator), rewind_(allocator->prepare_for_rewind()) {}

    Rewind_Guard(const Rewind_Guard&) = delete;
    Rewind_Guard& operator=(const Rewind_Guard&) = delete;

    Rewind_Guard(Rewind_Guard&&) = delete;
    Rewind_Guard& operator=(Rewind_Guard&&) = delete;

    ~Rewind_Guard() { this->allocator_->rewind(std::move(this->rewind_)); }

   private:
    Linked_Bump_Allocator* allocator_;
    Rewind_State rewind_;
  };

  // See rewind().
  Rewind_State prepare_for_rewind() {
    return Rewind_State{
        .chunk_ = this->chunk_,
        .next_allocation_ = this->next_allocation_,
        .chunk_end_ = this->chunk_end_,
    };
  }

  // Deallocate all allocations made since the creation of the Rewind_State
  // (returned by this->prepare_for_rewind()).
  void rewind(Rewind_State&& r);

  // Calls this->prepare_for_rewind() immediately then this->rewind() on
  // destruction.
  [[nodiscard]] Rewind_Guard make_rewind_guard() { return Rewind_Guard(this); }

  // Allocate space for an instance of T, then construct T.
  template <class T, class... Args>
  T* new_object(Args&&... args) {
    return new (this->allocate_bytes(sizeof(T), alignof(T)))
        T(std::forward<Args>(args)...);
  }

  // Allocate space for an instance of T, then construct T via copy or move
  // construction.
  template <class T>
  T* new_object_copy(T&& value) {
    return this->new_object<T>(std::forward<T>(value));
  }

  // Allocate space for objects.size() instances of T, then construct result[i]
  // via copy or move construction from objects[i] for i from 0 to
  // objects.size()-1.
  template <class T>
  Span<T> new_objects_copy(Span<const T> objects) {
    Span<T> new_objects = this->allocate_uninitialized_span<T>(
        narrow_cast<std::size_t>(objects.size()));
    std::uninitialized_copy(objects.begin(), objects.end(),
                            new_objects.begin());
    return new_objects;
  }

  // Allocate space for object.size() instances of T. Does not construct any
  // T-s.
  template <class T>
  [[nodiscard]] Span<T> allocate_uninitialized_span(std::size_t size) {
    std::size_t byte_size = size * sizeof(T);
    T* items =
        reinterpret_cast<T*>(this->allocate_bytes(byte_size, alignof(T)));
    return Span<T>(items, narrow_cast<Span_Size>(size));
  }

  // Allocate space for object.size() instances of T, then default-construct
  // object.size() instances.
  template <class T>
  [[nodiscard]] Span<T> allocate_span(Span_Size size) {
    return this->allocate_span<T>(narrow_cast<std::size_t>(size));
  }

  // Allocate space for object.size() instances of T, then default-construct
  // object.size() instances.
  template <class T>
  [[nodiscard]] Span<T> allocate_span(std::size_t size) {
    Span<T> items = this->allocate_uninitialized_span<T>(size);
    for (T& item : items) {
      // FIXME(strager): I think we technically need to std::launder after
      // placement new.
      new (&item) T();
    }
    return items;
  }

  // Given previously-allocated space for old_size instances of T, allocate
  // adjacent space for (new_size-old_size) instances of T after the old
  // allocation and return true.
  //
  // If adjacent space is not available, do nothing and return false.
  template <class T>
  bool try_grow_array_in_place(T* array, std::size_t old_size,
                               std::size_t new_size) {
    return this->try_grow_array_in_place_impl(reinterpret_cast<char*>(array),
                                              old_size * sizeof(T),
                                              new_size * sizeof(T));
  }

  // For testing only.
  std::size_t remaining_bytes_in_current_chunk() const {
    return narrow_cast<std::size_t>(this->chunk_end_ - this->next_allocation_);
  }

  class Disable_Guard {
   public:
    ~Disable_Guard();

   private:
    explicit Disable_Guard(Linked_Bump_Allocator*);

#if QLJS_DEBUG_BUMP_ALLOCATOR
    Linked_Bump_Allocator* alloc_;
#endif

    friend class Linked_Bump_Allocator;
  };

  // In debug modes, cause all allocations to fail with a precondition failure
  // until the Disable_Guard is destructed.
  [[nodiscard]] Disable_Guard disable() { return Disable_Guard(this); }

 protected:
  void* do_allocate(std::size_t bytes, std::size_t align) override;
  void do_deallocate(void* p, std::size_t bytes,
                     [[maybe_unused]] std::size_t align) override;

 private:
  struct alignas(alignof(void*)) Chunk_Header {
    explicit Chunk_Header(Chunk* previous) : previous(previous) {}

    Chunk* previous;  // Linked list.
  };

  static inline constexpr std::size_t default_chunk_size = 4096 - sizeof(Chunk);

  // Given previously-allocated space of old_byte_size bytes, allocate adjacent
  // space for (new_byte_size-old_byte_size) bytes after the old allocation and
  // return true.
  //
  // If adjacent space is not available, do nothing and return false.
  bool try_grow_array_in_place_impl(char* array, std::size_t old_byte_size,
                                    std::size_t new_byte_size);

  [[nodiscard]] void* allocate_bytes(std::size_t size, std::size_t align);

  void deallocate_bytes([[maybe_unused]] void* p,
                        [[maybe_unused]] std::size_t size) {
    // TODO(strager): Mark memory as unallocated for Valgrind and ASAN.
  }

  void did_allocate_bytes([[maybe_unused]] void* p,
                          [[maybe_unused]] std::size_t size);

  void did_deallocate_bytes([[maybe_unused]] void* p,
                            [[maybe_unused]] std::size_t size);

  void append_chunk(std::size_t size, std::size_t align);

  void assert_not_disabled() const;

#if QLJS_DEBUG_BUMP_ALLOCATOR
  bool is_disabled() const;
#endif

  Chunk* chunk_ = nullptr;
  char* next_allocation_ = nullptr;
  char* chunk_end_ = nullptr;

#if QLJS_DEBUG_BUMP_ALLOCATOR
  // Used only if QLJS_DEBUG_BUMP_ALLOCATOR.
  int disabled_count_ = 0;
#endif
};
}

// quick-lint-js finds bugs in JavaScript programs.
// Copyright (C) 2020  Matthew "strager" Glazar
//
// This file is part of quick-lint-js.
//
// quick-lint-js is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// quick-lint-js is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with quick-lint-js.  If not, see <https://www.gnu.org/licenses/>.
