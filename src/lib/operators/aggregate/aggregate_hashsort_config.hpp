#pragma once

namespace opossum {

struct AggregateHashSortConfig {
  size_t hash_table_size{100'000};
  float hash_table_max_load_factor{0.25f};
  size_t max_partitioning_counter{100'000};

  size_t buffer_flush_threshold{255};
  size_t group_prefetch_threshold{1'000};

  // Number of rows to fetch from a table to fill an initial run
  size_t initial_run_size{300'000};
};

}  // namespace opossum