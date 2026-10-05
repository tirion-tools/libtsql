// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFragmentFactory.cs
#include "tsql/ast/fragment_factory.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace tsql::ast {

namespace {
constexpr std::size_t kBlockSize = 64 * 1024;
}

FragmentFactory::FragmentFactory(FragmentFactory&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      cursor_(std::exchange(other.cursor_, nullptr)),
      remaining_(std::exchange(other.remaining_, 0)),
      nodes_(std::move(other.nodes_)),
      tokenStream_(other.tokenStream_) {
    other.blocks_.clear();
    other.nodes_.clear();
}

FragmentFactory& FragmentFactory::operator=(FragmentFactory&& other) noexcept {
    if (this != &other) {
        Release();
        blocks_ = std::move(other.blocks_);
        cursor_ = std::exchange(other.cursor_, nullptr);
        remaining_ = std::exchange(other.remaining_, 0);
        nodes_ = std::move(other.nodes_);
        tokenStream_ = other.tokenStream_;
        other.blocks_.clear();
        other.nodes_.clear();
    }
    return *this;
}

FragmentFactory::~FragmentFactory() { Release(); }

void FragmentFactory::Release() noexcept {
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (*it != nullptr) (*it)->~TSqlFragment();
    }
    nodes_.clear();
    blocks_.clear();
    cursor_ = nullptr;
    remaining_ = 0;
}

void* FragmentFactory::Allocate(std::size_t size, std::size_t align) {
    auto padding = [&](std::byte* p) {
        return static_cast<std::size_t>((align - reinterpret_cast<std::uintptr_t>(p) % align) % align);
    };
    if (cursor_ == nullptr || padding(cursor_) + size > remaining_) {
        std::size_t blockSize = std::max(kBlockSize, size + align);
        blocks_.push_back(std::make_unique<std::byte[]>(blockSize));
        cursor_ = blocks_.back().get();
        remaining_ = blockSize;
    }
    std::size_t pad = padding(cursor_);
    std::byte* result = cursor_ + pad;
    cursor_ = result + size;
    remaining_ -= pad + size;
    return result;
}

}  // namespace tsql::ast
