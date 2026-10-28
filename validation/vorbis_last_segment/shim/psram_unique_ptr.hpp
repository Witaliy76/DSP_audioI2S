#pragma once
// Host stand-in. alloc(n) is n bytes, matching ps_ptr::alloc() in the library.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

template <typename T> class ps_ptr {
  public:
    ps_ptr() = default;
    ps_ptr(const ps_ptr& other) { copy_from_ptr(other.ptr_, other.count_); }
    ps_ptr& operator=(const ps_ptr& other) {
        if (this != &other) { copy_from_ptr(other.ptr_, other.count_); }
        return *this;
    }
    ps_ptr& operator=(T* raw) {
        reset();
        ptr_ = raw;
        count_ = raw == nullptr ? 0 : 1;
        return *this;
    }
    ps_ptr(ps_ptr&& other) noexcept { move_from(other); }
    ps_ptr& operator=(ps_ptr&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }
    ~ps_ptr() { reset(); }

    bool alloc(size_t bytes, const char* = nullptr, bool = true) {
        reset();
        bytes_ = bytes;
        count_ = bytes == 0 ? 0 : (bytes + sizeof(T) - 1) / sizeof(T);
        if (bytes == 0) { return true; }
        const size_t page = 4096;
        const size_t rounded = (bytes + page - 1) & ~(page - 1);
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, rounded + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (base_ == nullptr) { return false; }
        DWORD old = 0;
        VirtualProtect(base_ + rounded, page, PAGE_NOACCESS, &old);
        ptr_ = reinterpret_cast<T*>(base_ + rounded - bytes);
        return true;
    }
    bool calloc(size_t bytes, const char* name = nullptr, bool use = true) { return alloc(bytes, name, use); }
    bool alloc_array(size_t count, const char* = nullptr) {
        reset();
        count_ = count;
        bytes_ = count * sizeof(T);
        if (count == 0) { return true; }
        ptr_ = new T[count]();
        return true;
    }
    bool calloc_array(size_t count, const char* name = nullptr) { return alloc_array(count, name); }
    void reset() {
        if (base_ != nullptr) {
            VirtualFree(base_, 0, MEM_RELEASE);
        } else {
            delete[] ptr_;
        }
        ptr_ = nullptr;
        base_ = nullptr;
        count_ = 0;
        bytes_ = 0;
    }
    void clear() {
        if (ptr_ != nullptr && count_ != 0) { std::memset(ptr_, 0, count_ * sizeof(T)); }
    }
    bool valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return valid(); }
    T* get() { return ptr_; }
    const T* get() const { return ptr_; }
    T& operator*() { return *ptr_; }
    const T& operator*() const { return *ptr_; }
    T* operator->() { return ptr_; }
    const T* operator->() const { return ptr_; }
    T& operator[](size_t index) { return ptr_[index]; }
    const T& operator[](size_t index) const { return ptr_[index]; }
    T& at(size_t index) { return ptr_[index]; }
    const T& at(size_t index) const { return ptr_[index]; }
    size_t size() const { return count_; }
    size_t strlen() const {
        if (ptr_ == nullptr) { return 0; }
        return std::char_traits<char>::length(reinterpret_cast<const char*>(ptr_));
    }
    const char* c_get() const { return ptr_ == nullptr ? "" : reinterpret_cast<const char*>(ptr_); }
    void assign(const char* text) { copy_from(text, text == nullptr ? 0 : std::char_traits<char>::length(text)); }
    void copy_from(const void* src, size_t len, bool = true) {
        alloc(len);
        if (src != nullptr && len != 0 && ptr_ != nullptr) { std::memcpy(ptr_, src, len); }
    }
    void append(const char* text) { append(text, text == nullptr ? 0 : std::char_traits<char>::length(text)); }
    void append(const void* src, size_t len) {
        std::string joined = c_get();
        if (src != nullptr && len != 0) { joined.append(static_cast<const char*>(src), len); }
        copy_from(joined.data(), joined.size());
    }
    int index_of(const char* needle) const {
        if (ptr_ == nullptr || needle == nullptr) { return -1; }
        const char* found = std::strstr(c_get(), needle);
        return found == nullptr ? -1 : static_cast<int>(found - c_get());
    }
    ps_ptr substr(size_t start) const {
        if (ptr_ == nullptr || start >= count_) { return ps_ptr(); }
        return substr(start, count_ - start);
    }
    ps_ptr substr(size_t start, size_t len) const {
        ps_ptr out;
        if (ptr_ == nullptr || start >= count_) { return out; }
        size_t n = len;
        if (start + n > count_) { n = count_ - start; }
        out.copy_from(reinterpret_cast<const char*>(ptr_) + start, n);
        return out;
    }
    bool starts_with_icase(const char* prefix) const {
        if (prefix == nullptr) { return false; }
        const char* s = c_get();
        for (; *prefix != '\0'; ++prefix, ++s) {
            char a = *s;
            char b = *prefix;
            if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
            if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
            if (a != b) { return false; }
        }
        return true;
    }
    void set_name(const char*) {}
    void hex_dump(int) {}
    void println() {}
    void shrink_to_fit() {}

  private:
    void copy_from_ptr(const T* src, size_t count) {
        reset();
        count_ = count;
        bytes_ = count * sizeof(T);
        if (count == 0) { return; }
        ptr_ = new T[count]();
        if (src != nullptr) { std::memcpy(ptr_, src, count * sizeof(T)); }
    }
    void move_from(ps_ptr& other) {
        ptr_ = other.ptr_;
        base_ = other.base_;
        count_ = other.count_;
        bytes_ = other.bytes_;
        other.ptr_ = nullptr;
        other.base_ = nullptr;
        other.count_ = 0;
        other.bytes_ = 0;
    }
    T* ptr_ = nullptr;
    uint8_t* base_ = nullptr;
    size_t count_ = 0;
    size_t bytes_ = 0;
};
