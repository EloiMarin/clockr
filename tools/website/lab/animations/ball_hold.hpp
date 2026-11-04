#pragma once

// The Fling ball mechanics, shared between sketch TUs. The scan's whole state
// is a small inline user struct (position + velocity); the input carries the
// spring target, the step and the grabbed flag. While grabbed the fold is a
// tight critically damped spring; released it is slack, so the velocity held
// across release becomes the throw, the walls bounce it, and the slack spring
// reels it back toward the target.
//
// A four-wire overload adds a per-axis current: a drift of 1e-3 canvas units
// per second, applied positionally while the ball is free (the user's hand,
// while grabbed, dominates it). This is how sketches let another region push
// the ball around without the input struct leaving the 16-byte inline path.

#include "clockr/shell/recipe.hpp"

#include <algorithm>
#include <cstdint>

namespace lab_ball
{

using namespace clockr;

using kernel::clock_tag;

// Exactly 16 bytes, so the whole simulation runs without touching a pool cell.
struct ball_in
{
    float target { 0.5f };
    float dt { 0.0f };
    bool grabbed { false };
    std::int16_t cur { 0 };   // the field's push on this axis (1e-3 canvas / s)
    std::int16_t swim { 0 };  // an offset on the spring target (1e-3 canvas)
};

struct ball_state
{
    float x { 0.5f };
    float v { 0.0f };
};

constexpr float wall_margin { 0.02f };
constexpr float restitution { 0.78f };

// One tick of the axis: spring toward the target, the current's positional
// drift (never turned into velocity, so the slack spring still reels the ball
// home when the push fades), then the walls.
inline ball_state
fold_axis(const ball_state& s, const ball_in& in)
{
    const float d { std::clamp(in.dt, 0.0f, 0.05f) };
    // The swim offset rides the target — but only while free: the little
    // being has no opinion under the hand, so a grab pins it exactly where
    // it points regardless of what the swim brain was publishing.
    const float target { in.grabbed ? in.target : in.target + static_cast<float>(in.swim) * 0.001f };
    // Grabbed: a tight hand. Free: slack, so the throw lasts.
    const float omega { in.grabbed ? 11.0f : 2.3f };
    const float accel
    {
        omega * omega * (target - s.x) - 2.0f * omega * s.v
    };
    float v { s.v + accel * d };
    float x { s.x + v * d + static_cast<float>(in.cur) * 0.001f * d };

    // The walls: reflect with restitution, so a hard throw
    // skitters along the frame.
    if (x < wall_margin)
    {
        x = wall_margin + (wall_margin - x);
        v = -v * restitution;
    }
    else if (x > 1.0f - wall_margin)
    {
        x = (1.0f - wall_margin) - (x - (1.0f - wall_margin));
        v = -v * restitution;
    }

    return ball_state { x, v };
}

// Three wires, no current: the classic Fling axis.
template<typename target_recipe_t, typename dt_recipe_t, typename grab_recipe_t>
auto
ball_chain(const target_recipe_t& target, const dt_recipe_t& dt, const grab_recipe_t& grabbed)
{
    using namespace clockr::shell;

    return zip(target, dt, grabbed)
        >> lift<std::pair<std::pair<double, double>, bool>, ball_in>(
               [](const double& t, const double& d, const bool& g)
               {
                   return ball_in
                   {
                       static_cast<float>(t),
                       static_cast<float>(d),
                       g
                   };
               })
        >> scan<ball_in, ball_state>(
               ball_state { 0.5f, 0.0f },
               fold_axis);
}

// Four wires: the third operand carries the axis's current as a canvas/s
// double (clamped to ±32 while folding it into the 1e-3 tick field).
using carried_wire_t = std::pair<std::pair<std::pair<double, double>, bool>, double>;

// Five wires: current plus a swim offset on the spring target (both 1e-3).
using swim_wire_t = std::pair<std::pair<std::pair<std::pair<double, double>, bool>, double>, double>;

template<
    typename target_recipe_t,
    typename dt_recipe_t,
    typename grab_recipe_t,
    typename current_recipe_t
>
auto
ball_chain(const target_recipe_t& target, const dt_recipe_t& dt, const grab_recipe_t& grabbed, const current_recipe_t& current)
{
    using namespace clockr::shell;

    return zip(target, dt, grabbed, current)
        >> lift<carried_wire_t, ball_in>(
               [](const double& t, const double& d, const bool& g, const double& c)
               {
                   return ball_in
                   {
                       static_cast<float>(t),
                       static_cast<float>(d),
                       g,
                       static_cast<std::int16_t>(std::clamp(c, -32.0, 32.0) * 1000.0)
                   };
               })
        >> scan<ball_in, ball_state>(
               ball_state { 0.5f, 0.0f },
               fold_axis);
}

template<
    typename target_recipe_t,
    typename dt_recipe_t,
    typename grab_recipe_t,
    typename current_recipe_t,
    typename swim_recipe_t
>
auto
ball_chain(
    const target_recipe_t& target,
    const dt_recipe_t& dt,
    const grab_recipe_t& grabbed,
    const current_recipe_t& current,
    const swim_recipe_t& swim)
{
    using namespace clockr::shell;

    return zip(target, dt, grabbed, current, swim)
        >> lift<swim_wire_t, ball_in>(
               [](const double& t, const double& d, const bool& g, const double& c, const double& w)
               {
                   return ball_in
                   {
                       static_cast<float>(t),
                       static_cast<float>(d),
                       g,
                       static_cast<std::int16_t>(std::clamp(c, -32.0, 32.0) * 1000.0),
                       static_cast<std::int16_t>(std::clamp(w, -1.0, 1.0) * 1000.0)
                   };
               })
        >> scan<ball_in, ball_state>(
               ball_state { 0.5f, 0.0f },
               fold_axis);
}

} // namespace lab_ball
