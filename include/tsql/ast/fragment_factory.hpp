// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlFragmentFactory.cs
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

#include "tsql/ast/fragment.hpp"

namespace tsql::ast {

/// Creates AST nodes and owns them: nodes live in arena blocks and are destroyed with the factory.
/// Every created node points at the factory's token stream (C#: TSqlFragmentFactory.SetTokenStream).
class FragmentFactory {
public:
    FragmentFactory() = default;
    explicit FragmentFactory(::tsql::ast::ScriptTokenStream* tokenStream) : tokenStream_(tokenStream) {}
    FragmentFactory(const FragmentFactory&) = delete;
    FragmentFactory& operator=(const FragmentFactory&) = delete;
    FragmentFactory(FragmentFactory&& other) noexcept;
    FragmentFactory& operator=(FragmentFactory&& other) noexcept;
    ~FragmentFactory();

    void SetTokenStream(::tsql::ast::ScriptTokenStream* tokenStream) { tokenStream_ = tokenStream; }
    ::tsql::ast::ScriptTokenStream* TokenStream() const { return tokenStream_; }

    template <class T>
    T* CreateFragment() {
        static_assert(std::is_base_of<TSqlFragment, T>::value, "CreateFragment<T>: T must be an AST node");
        static_assert(!std::is_abstract<T>::value, "CreateFragment<T>: T is an abstract Ast.xml class");
        nodes_.push_back(nullptr);  // grow first so ownership tracking cannot fail after construction
        T* node = new (Allocate(sizeof(T), alignof(T))) T();
        nodes_.back() = node;
        node->ScriptTokenStream = tokenStream_;
        return node;
    }

    /// Number of nodes created so far.
    std::size_t FragmentCount() const { return nodes_.size(); }

private:
    void* Allocate(std::size_t size, std::size_t align);
    void Release() noexcept;

    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::byte* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::vector<TSqlFragment*> nodes_;
    ::tsql::ast::ScriptTokenStream* tokenStream_ = nullptr;
};

}  // namespace tsql::ast
