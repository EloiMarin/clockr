#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace clockr::shell::detail
{

/// A small, constexpr-friendly type-erased buffer that stores one closure value
/// by value. It is the storage behind the AST's stateful nodes (`lift_t` /
/// `scan_t`): the closure lives inside the recipe node itself, so a pointer to
/// it is a valid constant expression and can be used as
/// `node_descriptor.self` without any heap allocation. The runtime build path
/// can clone it into heap-owned storage via `clone_function()`.
class closure_cell
{
public:
    using clone_fn = std::shared_ptr<void>(*)(const void*);
    using destroy_fn = void(*)(void*);
    using move_fn = void(*)(void* src, void* dst);
    using copy_fn = void(*)(const void* src, void* dst);

    constexpr closure_cell() = default;

    template<typename F>
    constexpr explicit closure_cell(F&& value)
    {
        using T = std::remove_cvref_t<F>;
        static_assert(sizeof(T) <= capacity, "closure too large for inline storage");
        static_assert(alignof(T) <= alignment, "closure over-aligned for inline storage");

        std::construct_at(reinterpret_cast<T*>(m_data.data()), std::forward<F>(value));

        m_destroy = +[](void* ptr) { std::destroy_at(static_cast<T*>(ptr)); };
        m_clone = +[](const void* ptr) -> std::shared_ptr<void> {
            return std::shared_ptr<void>(new T(*static_cast<const T*>(ptr)));
        };
        m_copy = +[](const void* src, void* dst) {
            std::construct_at(static_cast<T*>(dst), *static_cast<const T*>(src));
        };
        m_move = +[](void* src, void* dst) {
            // Move-construct the destination, then destroy the source: the
            // source's object is relinquished here, so no lifetime may be left
            // for `other`'s destructor to run later.
            std::construct_at(static_cast<T*>(dst), std::move(*static_cast<T*>(src)));
            std::destroy_at(static_cast<T*>(src));
        };
    }

    constexpr closure_cell(closure_cell&& other) noexcept
    {
        *this = std::move(other);
    }

    // Copying a recipe copies the closures and delay seeds it holds. Under
    // constant evaluation the buffer is byte-copied (a constant is never
    // destroyed — the premise that keeps recipes constant-initializable); at
    // runtime the copy is a real copy construction, so a held value that owns
    // a resource (a string, a tracked struct) is independent of the original
    // instead of aliasing it.
    constexpr closure_cell(const closure_cell& other)
    {
        copy_from(other);
    }

    constexpr closure_cell& operator=(closure_cell&& other) noexcept
    {
        if (this != &other)
        {
            reset();

            if (other.m_move)
            {
                other.m_move(other.m_data.data(), m_data.data());
                m_destroy = std::exchange(other.m_destroy, nullptr);
                m_clone = std::exchange(other.m_clone, nullptr);
                m_move = std::exchange(other.m_move, nullptr);
                m_copy = std::exchange(other.m_copy, nullptr);
            }
        }

        return *this;
    }

    constexpr closure_cell& operator=(const closure_cell& other)
    {
        if (this != &other)
        {
            reset();
            copy_from(other);
        }

        return *this;
    }

    constexpr ~closure_cell()
    {
        reset();
    }

    [[nodiscard]] constexpr bool has_value() const noexcept
    {
        return m_destroy != nullptr;
    }

    [[nodiscard]] constexpr const void* self() const noexcept
    {
        return m_data.data();
    }

    [[nodiscard]] constexpr clone_fn clone_function() const noexcept
    {
        return m_clone;
    }

    // `std::byte` has no relational operators, so a `= default` `operator==`
    // (which would compare `m_data` element-wise) is ill-formed; compare the
    // inline buffer byte-for-byte through `std::to_integer` so the recipe stays
    // value-comparable (and the comparison is `constexpr`-clean).
    constexpr bool operator==(const closure_cell& other) const noexcept
    {
        if (m_destroy != other.m_destroy || m_clone != other.m_clone
            || m_move != other.m_move || m_copy != other.m_copy)
            return false;

        for (std::size_t i { 0 }; i < m_data.size(); ++i)
            if (std::to_integer<std::uint8_t>(m_data[i]) != std::to_integer<std::uint8_t>(other.m_data[i]))
                return false;

        return true;
    }

private:
    constexpr void copy_from(const closure_cell& other)
    {
        if (other.m_copy == nullptr)
            return;

        if (std::is_constant_evaluated())
            m_data = other.m_data;
        else
            other.m_copy(other.m_data.data(), m_data.data());

        m_destroy = other.m_destroy;
        m_clone = other.m_clone;
        m_move = other.m_move;
        m_copy = other.m_copy;
    }

    constexpr void reset() noexcept
    {
        if (m_destroy)
        {
            m_destroy(m_data.data());
            m_destroy = nullptr;
            m_clone = nullptr;
            m_move = nullptr;
            m_copy = nullptr;
        }
    }

    // Kept small deliberately: every AST node embeds one, so this capacity
    // sets the floor for sizeof(node) and hence construction-time stack
    // (recipes compose through by-value arenas). The constructor
    // static_asserts below police every closure; raise only with a measured
    // reason.
    static constexpr std::size_t capacity { 64 };
    static constexpr std::size_t alignment { alignof(std::max_align_t) };

    alignas(alignment) std::array<std::byte, capacity> m_data {};
    destroy_fn m_destroy { nullptr };
    clone_fn m_clone { nullptr };
    move_fn m_move { nullptr };
    copy_fn m_copy { nullptr };
};

} // namespace clockr::shell::detail
