#pragma once

// A heap-owned (no pool) user struct that counts its destructions, so the
// recorder/server reference hand-off is observable synchronously. Shared by
// the recorder and server suites; the two copies it replaces were identical.

namespace clockr::debug_test
{

struct tracked
{
    int* destroyed { nullptr };

    explicit tracked(int* counter) :
        destroyed { counter }
    {}

    tracked(const tracked&) = delete;
    tracked& operator=(const tracked&) = delete;

    ~tracked()
    {
        if (destroyed != nullptr)
            ++(*destroyed);
    }
};

} // namespace clockr::debug_test
