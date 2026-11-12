#pragma once

/// @file
/// Forward declarations for the `clockr::replay` module.

#include <cstdint>

namespace clockr::replay
{

struct run_id;
struct branch_id;
struct entry_id;
struct region_step_id;
enum class entry_kind : std::uint8_t;

struct user_bytes;
struct encoded_value;
class value_codec;
class codec_registry;

struct scenario_entry;
struct fidelity;
struct scenario;
class scenario_builder;
struct branch_edit;

struct step_record;
struct run;

class program_factory;
struct replay_options;
class replayer;

struct value_mismatch;
struct run_diff;
struct compare_options;

} // namespace clockr::replay
