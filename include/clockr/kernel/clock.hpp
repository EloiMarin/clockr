#pragma once

#include <cstddef>

namespace clockr::kernel
{

/// The regions a program can run on. The enum is dense and zero-based, so a
/// tag is also an index into the per-region fixed arrays (value homes, grace
/// announcements, the debugger's per-thread queues).
enum class clock_tag
{
    audio,
    ui,
    worker,
};

/// Number of clock tags (regions). Keep in sync with the enum above: it sizes
/// every per-region array, so adding a region is a one-line change plus this
/// count.
inline constexpr std::size_t clock_tag_count { 3 };

/// The tag as an array index. The enum is dense from zero, so this is a cast;
/// it exists so the mapping is spelled once.
constexpr
std::size_t
clock_tag_index(clock_tag tag) noexcept
{
    return static_cast<std::size_t>(tag);
}

} // namespace clockr::kernel
