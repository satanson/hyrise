#include <fstream>

#include "base_test.hpp"
#include "json.hpp"
#include "operators/join_hash.hpp"
#include "operators/join_index.hpp"
#include "operators/join_mpsm.hpp"
#include "operators/join_nested_loop.hpp"
#include "operators/join_reference_operator.hpp"
#include "operators/join_sort_merge.hpp"
#include "operators/print.hpp"
#include "operators/table_wrapper.hpp"
#include "storage/index/adaptive_radix_tree/adaptive_radix_tree_index.hpp"
#include "storage/index/b_tree/b_tree_index.hpp"
#include "storage/index/group_key/group_key_index.hpp"
#include "storage/index/group_key/composite_group_key_index.hpp"
#include "storage/chunk_encoder.hpp"
#include "utils/make_bimap.hpp"

/**
 * This file contains the main tests for Hyrise's join operators.
 * Testing is done by comparing the result of any given join operator with that of the JoinReferenceOperator for a
 * number of configurations (i.e. JoinModes, predicates, data types, ...)
 */

using namespace std::string_literals;  // NOLINT

namespace {

using namespace opossum;  // NOLINT

enum class InputSide { Left, Right };

enum class InputTableType {
  // Input Tables are data
  Data,
  // Input Tables are reference Tables with all Segments of a Chunk having the same PosList
  SharedPosList,
  // Input Tables are reference Tables with each Segment using a different PosList
  IndividualPosLists
};

std::unordered_map<InputTableType, std::string> input_table_type_to_string{
    {InputTableType::Data, "Data"},
    {InputTableType::SharedPosList, "SharedPosList"},
    {InputTableType::IndividualPosLists, "IndividualPosLists"}};

std::unordered_map<SegmentIndexType, std::string> segment_index_type_to_string{
    {SegmentIndexType::BTree, "Data"},
    {SegmentIndexType::AdaptiveRadixTree, "Data"},
    {SegmentIndexType::GroupKey, "GroupKey"},
    {SegmentIndexType::CompositeGroupKey, "CompositeGroupKey"}};

struct IndexConfiguration {
  SegmentIndexType index_type;
  std::vector<ColumnID> column_ids;

  auto to_tuple() const {
    return std::tie(index_type, column_ids);
  }
};

bool operator<(const IndexConfiguration& l, const IndexConfiguration& r) {
  return l.to_tuple() < r.to_tuple();
}

struct InputTableConfiguration {
  InputSide side{};
  ChunkOffset chunk_size{};
  size_t table_size{};
  InputTableType table_type{};
  EncodingType encoding_type{};

  // Only for JoinIndex
  std::vector<IndexConfiguration> index_configurations;

  auto to_tuple() const { return std::tie(side, chunk_size, table_size, table_type, encoding_type, index_configurations); }
};

bool operator<(const InputTableConfiguration& l, const InputTableConfiguration& r) {
  return l.to_tuple() < r.to_tuple();
}

class BaseJoinOperatorFactory;

struct JoinTestConfiguration {
  InputTableConfiguration input_left;
  InputTableConfiguration input_right;
  JoinMode join_mode{JoinMode::Inner};
  DataType data_type_left{DataType::Int};
  DataType data_type_right{DataType::Int};
  bool nullable_left{false};
  bool nullable_right{false};
  PredicateCondition predicate_condition{PredicateCondition::Equals};
  std::vector<OperatorJoinPredicate> secondary_predicates;
  std::shared_ptr<BaseJoinOperatorFactory> join_operator_factory;

  // Only for JoinHash
  std::optional<size_t> radix_bits;

  void swap_input_sides() {
    std::swap(input_left, input_right);
    std::swap(data_type_left, data_type_right);
    std::swap(nullable_left, nullable_right);

    for (auto& secondary_predicate : secondary_predicates) {
      std::swap(secondary_predicate.column_ids.first, secondary_predicate.column_ids.second);
      secondary_predicate.predicate_condition = flip_predicate_condition(secondary_predicate.predicate_condition);
    }
  }

  auto to_tuple() const {
    return std::tie(input_left, input_right, join_mode, data_type_left, data_type_right, nullable_left, nullable_right,
                    predicate_condition, secondary_predicates);
  }
};

bool operator<(const JoinTestConfiguration& l, const JoinTestConfiguration& r) { return l.to_tuple() < r.to_tuple(); }

// Virtual interface to create a join operator
class BaseJoinOperatorFactory {
 public:
  virtual ~BaseJoinOperatorFactory() = default;
  virtual std::shared_ptr<AbstractJoinOperator> create_operator(
      const std::shared_ptr<const AbstractOperator>& left, const std::shared_ptr<const AbstractOperator>& right,
      const JoinMode mode, const OperatorJoinPredicate& primary_predicate,
      const std::vector<OperatorJoinPredicate>& secondary_predicates, const JoinTestConfiguration& configuration) = 0;
};

template <typename JoinOperator>
class JoinOperatorFactory : public BaseJoinOperatorFactory {
 public:
  std::shared_ptr<AbstractJoinOperator> create_operator(const std::shared_ptr<const AbstractOperator>& left,
                                                        const std::shared_ptr<const AbstractOperator>& right,
                                                        const JoinMode mode,
                                                        const OperatorJoinPredicate& primary_predicate,
                                                        const std::vector<OperatorJoinPredicate>& secondary_predicates,
                                                        const JoinTestConfiguration& configuration) override {
    if constexpr (std::is_same_v<JoinOperator, JoinHash>) {
      return std::make_shared<JoinOperator>(left, right, mode, primary_predicate, secondary_predicates,
                                            configuration.radix_bits);
    } else {
      return std::make_shared<JoinOperator>(left, right, mode, primary_predicate, secondary_predicates);
    }
  }
};
// Order of columns in the input tables
const std::unordered_map<DataType, size_t> data_type_order = {
    {DataType::Int, 0u}, {DataType::Float, 1u}, {DataType::Double, 2u}, {DataType::Long, 3u}, {DataType::String, 4u},
};

}  // namespace

namespace opossum {

class JoinTestRunner : public BaseTestWithParam<JoinTestConfiguration> {
 public:
  template <typename JoinOperator>
  static std::vector<JoinTestConfiguration> create_configurations() {
    auto configurations = std::vector<JoinTestConfiguration>{};

    /**
     * For each `all_*` set, the first element is the default - which is chosen to be sensible. Thus the elements are
     * not necessarily sorted.
     */
    auto all_data_types = std::vector<DataType>{};
    hana::for_each(data_type_pairs, [&](auto pair) {
      const DataType d = hana::first(pair);
      all_data_types.emplace_back(d);
    });

    const auto all_predicate_conditions = std::vector{
        PredicateCondition::Equals,      PredicateCondition::NotEquals,
        PredicateCondition::GreaterThan, PredicateCondition::GreaterThanEquals,
        PredicateCondition::LessThan,    PredicateCondition::LessThanEquals,
    };
    const auto all_join_modes = std::vector{JoinMode::Inner,         JoinMode::Left, JoinMode::Right,
                                            JoinMode::FullOuter,     JoinMode::Semi, JoinMode::AntiNullAsFalse,
                                            JoinMode::AntiNullAsTrue};
    const auto all_table_sizes = std::vector{10u, 15u, 0u};
    const auto all_left_nulls = std::vector{true, false};
    const auto all_right_nulls = std::vector{true, false};
    const auto all_chunk_sizes = std::vector{10u, 3u, 1u};
    const auto all_secondary_predicate_sets = std::vector<std::vector<OperatorJoinPredicate>>{
        {},
        {{{ColumnID{0}, ColumnID{0}}, PredicateCondition::LessThan}},
        {{{ColumnID{0}, ColumnID{0}}, PredicateCondition::GreaterThanEquals}},
        {{{ColumnID{0}, ColumnID{0}}, PredicateCondition::NotEquals}}};

    const auto all_swap_input_sides = std::vector{false, true};
    const auto all_input_table_types =
        std::vector{InputTableType::Data, InputTableType::IndividualPosLists, InputTableType::SharedPosList};

    // Only indices on non-nullable integer columns for now
    const auto all_index_configuration_sets =
        std::vector<std::vector<IndexConfiguration>>{
        {{SegmentIndexType::AdaptiveRadixTree, {ColumnID{0}}}},
        {{SegmentIndexType::BTree, {ColumnID{0}}}},
        {{SegmentIndexType::CompositeGroupKey, {ColumnID{0}}}},
        {{SegmentIndexType::GroupKey, {ColumnID{0}}}},
    };
    const auto all_encoding_types = std::vector{
    EncodingType::Unencoded,
    EncodingType::Dictionary,
//    EncodingType::RunLength,
//    EncodingType::FixedStringDictionary,
//    EncodingType::FrameOfReference,
//    EncodingType::LZ4
    };

    // clang-format off
    JoinTestConfiguration default_configuration{
      InputTableConfiguration{
        InputSide::Left, all_chunk_sizes.front(), all_table_sizes.front(), all_input_table_types.front(), EncodingType::Unencoded, {}},
      InputTableConfiguration{
        InputSide::Right, all_chunk_sizes.front(), all_table_sizes.front(), all_input_table_types.front(), EncodingType::Unencoded, {}},
      JoinMode::Inner,
      DataType::Int,
      DataType::Int,
      true,
      true,
      PredicateCondition::Equals,
      all_secondary_predicate_sets.front(),
      std::make_shared<JoinOperatorFactory<JoinOperator>>(),
      std::nullopt
    };
    // clang-format on

    const auto add_configuration_if_supported = [&](const auto& configuration) {
      // String vs non-String comparisons are not supported in Hyrise and therefore cannot be tested
      if ((configuration.data_type_left == DataType::String) != (configuration.data_type_right == DataType::String)) {
        return;
      }

      if (JoinOperator::supports(configuration.join_mode, configuration.predicate_condition,
                                 configuration.data_type_left, configuration.data_type_right,
                                 !configuration.secondary_predicates.empty())) {
        configurations.emplace_back(configuration);
      }
    };

    // JoinOperators (e.g. JoinHash) might pick a "common type"
    // Test that this works for all data_type_left/data_type_right combinations
    for (const auto data_type_left : all_data_types) {
      for (const auto data_type_right : all_data_types) {
        auto join_test_configuration = default_configuration;
        join_test_configuration.data_type_left = data_type_left;
        join_test_configuration.data_type_right = data_type_right;

        add_configuration_if_supported(join_test_configuration);
      }
    }

    // JoinOperators (e.g. JoinHash) swap input sides depending on TableSize and JoinMode.
    // Test that predicate_condition and secondary predicates are flipped accordingly
    for (const auto predicate_condition : {PredicateCondition::Equals, PredicateCondition::LessThan}) {
      for (const auto join_mode : all_join_modes) {
        for (const auto left_table_size : all_table_sizes) {
          for (const auto right_table_size : all_table_sizes) {
            for (const auto& secondary_predicates : all_secondary_predicate_sets) {
              auto join_test_configuration = default_configuration;
              join_test_configuration.predicate_condition = predicate_condition;
              join_test_configuration.join_mode = join_mode;
              join_test_configuration.input_left.table_size = left_table_size;
              join_test_configuration.input_right.table_size = right_table_size;
              join_test_configuration.secondary_predicates = secondary_predicates;

              add_configuration_if_supported(join_test_configuration);
            }
          }
        }
      }
    }

    // Anti* joins have different behaviours with NULL values.
    // Also test table sizes, as an empty right input table is a special case where a NULL value from the left side
    // would get emitted.
    for (const auto join_mode : {JoinMode::AntiNullAsTrue, JoinMode::AntiNullAsFalse}) {
      for (const auto left_table_size : all_table_sizes) {
        for (const auto right_table_size : all_table_sizes) {
          auto join_test_configuration = default_configuration;
          join_test_configuration.join_mode = join_mode;
          join_test_configuration.nullable_left = true;
          join_test_configuration.nullable_right = true;
          join_test_configuration.input_left.table_size = left_table_size;
          join_test_configuration.input_right.table_size = right_table_size;

          add_configuration_if_supported(join_test_configuration);
        }
      }
    }

    // JoinOperators need to deal with differently sized Chunks (e.g., smaller last Chunk)
    // Trigger those via testing all table_size/chunk_size combinations
    for (const auto& left_table_size : all_table_sizes) {
      for (const auto& right_table_size : all_table_sizes) {
        for (const auto& chunk_size : all_chunk_sizes) {
          auto join_test_configuration = default_configuration;
          join_test_configuration.input_left.table_size = left_table_size;
          join_test_configuration.input_right.table_size = right_table_size;
          join_test_configuration.input_left.chunk_size = chunk_size;
          join_test_configuration.input_right.chunk_size = chunk_size;

          add_configuration_if_supported(join_test_configuration);
        }
      }
    }

    // Different JoinModes have different handling of NULL values.
    // Additionally, JoinOperators (e.g., JoinSortMerge) have vastly different paths for different PredicateConditions
    // Test all combinations
    for (const auto& join_mode : all_join_modes) {
      for (const auto predicate_condition : all_predicate_conditions) {
        auto join_test_configuration = default_configuration;
        join_test_configuration.join_mode = join_mode;
        join_test_configuration.nullable_left = true;
        join_test_configuration.nullable_right = true;
        join_test_configuration.predicate_condition = predicate_condition;

        add_configuration_if_supported(join_test_configuration);
      }
    }

    // The input tables are designed to have exclusive values. Test that these are handled correctly for different
    // JoinModes by swapping the input tables.
    // Additionally, go through all PredicateCondition to test especially the JoinSortMerge's different paths for these
    for (const auto& predicate_condition : all_predicate_conditions) {
      for (const auto& join_mode : all_join_modes) {
        auto join_test_configuration = default_configuration;
        join_test_configuration.join_mode = join_mode;
        join_test_configuration.predicate_condition = predicate_condition;

        join_test_configuration.swap_input_sides();

        add_configuration_if_supported(join_test_configuration);
      }
    }

    // Test all combinations of reference/data input tables. This tests mostly the composition of the output table
    for (const auto& left_input_table_type : all_input_table_types) {
      for (const auto& right_input_table_type : all_input_table_types) {
        auto join_test_configuration = default_configuration;
        join_test_configuration.input_left.table_type = left_input_table_type;
        join_test_configuration.input_right.table_type = right_input_table_type;

        add_configuration_if_supported(join_test_configuration);
      }
    }

    // Test MPJ support for all join modes. Swap the input tables to trigger cases especially in the Anti* modes
    // where a secondary predicate evaluating to FALSE might "save" a tuple from being discarded
    for (const auto& join_mode : all_join_modes) {
      for (const auto& secondary_predicates : all_secondary_predicate_sets) {
        for (const auto& predicate_condition : {PredicateCondition::Equals, PredicateCondition::NotEquals}) {
          for (const auto swap_input_sides : all_swap_input_sides) {
            auto join_test_configuration = default_configuration;
            join_test_configuration.join_mode = join_mode;
            join_test_configuration.secondary_predicates = secondary_predicates;
            join_test_configuration.predicate_condition = predicate_condition;

            if (swap_input_sides) {
              join_test_configuration.swap_input_sides();
            }

            add_configuration_if_supported(join_test_configuration);
          }
        }
      }
    }

    // JoinHash specific configurations varying the number of radix_bits.
    // The number of radix bits affects the partitioning inside the JoinHash operator, thus table/chunk sizes and
    // different join modes need to be tested in combination, since they all use this partitioning.
    if constexpr (std::is_same_v<JoinOperator, JoinHash>) {
      for (const auto radix_bits : {1, 2, 5}) {
        for (const auto join_mode : {JoinMode::Inner, JoinMode::Right, JoinMode::Semi}) {
          for (const auto left_table_size : all_table_sizes) {
            for (const auto right_table_size : all_table_sizes) {
              for (const auto chunk_size : all_chunk_sizes) {
                for (const auto predicate_condition : {PredicateCondition::Equals, PredicateCondition::NotEquals}) {
                  auto join_test_configuration = default_configuration;
                  join_test_configuration.join_mode = join_mode;
                  join_test_configuration.predicate_condition = predicate_condition;
                  join_test_configuration.input_left.table_size = left_table_size;
                  join_test_configuration.input_left.chunk_size = chunk_size;
                  join_test_configuration.input_right.table_size = right_table_size;
                  join_test_configuration.input_right.chunk_size = chunk_size;
                  join_test_configuration.radix_bits = radix_bits;

                  add_configuration_if_supported(join_test_configuration);
                }
              }
            }
          }
        }
      }
    }

    // Add JoinIndex specific configurations
    if constexpr (std::is_same_v<JoinOperator, JoinIndex>) {
      for (const auto& index_configurations : all_index_configuration_sets) {
        for (const auto join_mode : all_join_modes) {
          for (const auto left_table_size : all_table_sizes) {
            for (const auto right_table_size : all_table_sizes) {
              for (const auto chunk_size : all_chunk_sizes) {
                for (const auto predicate_condition : all_predicate_conditions) {
                  auto join_test_configuration = default_configuration;
                  join_test_configuration.join_mode = join_mode;
                  join_test_configuration.predicate_condition = predicate_condition;
                  join_test_configuration.data_type_left = DataType::Int;
                  join_test_configuration.nullable_left = false;
                  join_test_configuration.data_type_right = DataType::Int;
                  join_test_configuration.nullable_right = false;
                  join_test_configuration.input_left.table_size = left_table_size;
                  join_test_configuration.input_left.encoding_type = EncodingType::Dictionary;
                  join_test_configuration.input_left.chunk_size = chunk_size;
                  join_test_configuration.input_left.index_configurations = index_configurations;
                  join_test_configuration.input_right.table_size = right_table_size;
                  join_test_configuration.input_right.encoding_type = EncodingType::Dictionary;
                  join_test_configuration.input_right.chunk_size = chunk_size;
                  join_test_configuration.input_right.index_configurations = index_configurations;

                  add_configuration_if_supported(join_test_configuration);
                }
              }
            }
          }
        }
      }
    }


    return configurations;
  }

  static std::string get_table_path(const InputTableConfiguration& key) {
    const auto& [side, chunk_size, table_size, input_table_type, encoding_type, index_configurations] = key;

    const auto side_str = side == InputSide::Left ? "left" : "right";
    const auto table_size_str = std::to_string(table_size);

    return "resources/test_data/tbl/join_test_runner/input_table_"s + side_str + "_" + table_size_str + ".tbl";
  }

  static std::shared_ptr<Table> get_table(const InputTableConfiguration& key) {
    auto input_table_iter = input_tables.find(key);
    if (input_table_iter == input_tables.end()) {
      const auto& [side, chunk_size, table_size, input_table_type, encoding_type, index_configurations] = key;
      std::ignore = side;
      std::ignore = table_size;

      auto table = load_table(get_table_path(key), chunk_size);

      /**
       * Encode Table
       */
      if (encoding_type != EncodingType::Unencoded) {
        ChunkEncoder::encode_all_chunks(table, encoding_type);
      }

      /**
       * Create Indices
       */
      for (const auto& index_configuration : index_configurations) {
        switch (index_configuration.index_type) {
          case SegmentIndexType::Invalid:
            Fail("Expected index type");
            break;
          case SegmentIndexType::GroupKey:
            table->create_index<GroupKeyIndex>(index_configuration.column_ids);
            break;
          case SegmentIndexType::CompositeGroupKey:
            table->create_index<CompositeGroupKeyIndex>(index_configuration.column_ids);
            break;
          case SegmentIndexType::AdaptiveRadixTree:
            table->create_index<AdaptiveRadixTreeIndex>(index_configuration.column_ids);
            break;
          case SegmentIndexType::BTree:
            table->create_index<BTreeIndex>(index_configuration.column_ids);
            break;

        }
      }

      /**
       * Create a Reference-Table pointing 1-to-1 and in-order to the rows in the original table. This tests the
       * writing of the output table in the JoinOperator, which has to make sure not to create ReferenceSegments
       * pointing to ReferenceSegments.
       */
      if (input_table_type != InputTableType::Data) {
        const auto reference_table = std::make_shared<Table>(table->column_definitions(), TableType::References);

        for (auto chunk_id = ChunkID{0}; chunk_id < table->chunk_count(); ++chunk_id) {
          const auto input_chunk = table->get_chunk(chunk_id);

          Segments reference_segments;

          if (input_table_type == InputTableType::SharedPosList) {
            const auto pos_list = std::make_shared<PosList>();
            for (auto chunk_offset = ChunkOffset{0}; chunk_offset < input_chunk->size(); ++chunk_offset) {
              pos_list->emplace_back(chunk_id, chunk_offset);
            }

            for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
              reference_segments.emplace_back(std::make_shared<ReferenceSegment>(table, column_id, pos_list));
            }

          } else if (input_table_type == InputTableType::IndividualPosLists) {
            for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
              const auto pos_list = std::make_shared<PosList>();
              for (auto chunk_offset = ChunkOffset{0}; chunk_offset < input_chunk->size(); ++chunk_offset) {
                pos_list->emplace_back(chunk_id, chunk_offset);
              }

              reference_segments.emplace_back(std::make_shared<ReferenceSegment>(table, column_id, pos_list));
            }
          }

          reference_table->append_chunk(reference_segments);
        }

        table = reference_table;
      }

      input_table_iter = input_tables.emplace(key, table).first;
    }

    return input_table_iter->second;
  }

  static inline std::map<InputTableConfiguration, std::shared_ptr<Table>> input_tables;
  // Cache reference table to avoid redundant computation of the same
  static inline std::map<JoinTestConfiguration, std::shared_ptr<const Table>> expected_output_tables;
};

TEST_P(JoinTestRunner, TestJoin) {
  const auto configuration = GetParam();

  const auto input_table_left = get_table(configuration.input_left);
  const auto input_table_right = get_table(configuration.input_right);

  const auto input_op_left = std::make_shared<TableWrapper>(input_table_left);
  const auto input_op_right = std::make_shared<TableWrapper>(input_table_right);

  const auto column_id_left = ColumnID{static_cast<ColumnID::base_type>(
      2 * data_type_order.at(configuration.data_type_left) + (configuration.nullable_left ? 1 : 0))};
  const auto column_id_right = ColumnID{static_cast<ColumnID::base_type>(
      2 * data_type_order.at(configuration.data_type_right) + (configuration.nullable_right ? 1 : 0))};

  const auto primary_predicate =
      OperatorJoinPredicate{{column_id_left, column_id_right}, configuration.predicate_condition};

  const auto join_op = configuration.join_operator_factory->create_operator(
      input_op_left, input_op_right, configuration.join_mode, primary_predicate, configuration.secondary_predicates,
      configuration);

  auto expected_output_table_iter = expected_output_tables.find(configuration);

  const auto join_reference_op = std::make_shared<JoinReferenceOperator>(
      input_op_left, input_op_right, configuration.join_mode, primary_predicate, configuration.secondary_predicates);

  input_op_left->execute();
  input_op_right->execute();

  auto actual_table = std::shared_ptr<const Table>{};
  auto expected_table = std::shared_ptr<const Table>{};
  auto table_difference_message = std::optional<std::string>{};

  const auto print_configuration_info = [&]() {
    std::cout << "====================== JoinOperator ========================" << std::endl;
    std::cout << join_op->description(DescriptionMode::MultiLine) << std::endl;
    if (configuration.radix_bits) {
      std::cout << "RadixBits: " << *configuration.radix_bits << std::endl;
    }
    std::cout << "===================== Left Input Table =====================" << std::endl;
    Print::print(input_table_left, PrintFlags::PrintIgnoreChunkBoundaries);
    std::cout << "ChunkSize: " << configuration.input_left.chunk_size << std::endl;
    std::cout << "TableType: " << input_table_type_to_string.at(configuration.input_left.table_type) << std::endl;
    std::cout << "EncodingType: " << encoding_type_to_string.left.at(configuration.input_left.encoding_type) << std::endl;
    // MARCEL: Print info about indices on the left input table
    std::cout << get_table_path(configuration.input_left) << std::endl;
    std::cout << std::endl;
    std::cout << "===================== Right Input Table ====================" << std::endl;
    Print::print(input_table_right, PrintFlags::PrintIgnoreChunkBoundaries);
    std::cout << "ChunkSize: " << configuration.input_right.chunk_size << std::endl;
    std::cout << "TableType: " << input_table_type_to_string.at(configuration.input_right.table_type) << std::endl;
    std::cout << "EncodingType: " << encoding_type_to_string.left.at(configuration.input_right.encoding_type) << std::endl;
    // MARCEL: Print info about indices on the right input table
    std::cout << get_table_path(configuration.input_right) << std::endl;
    std::cout << std::endl;
    std::cout << "==================== Actual Output Table ===================" << std::endl;
    if (join_op->get_output()) {
      Print::print(join_op->get_output(), PrintFlags::PrintIgnoreChunkBoundaries);
      std::cout << std::endl;
    } else {
      std::cout << "No Table produced by the join operator under test" << std::endl;
    }
    std::cout << "=================== Expected Output Table ==================" << std::endl;
    if (join_reference_op->get_output()) {
      Print::print(join_reference_op->get_output(), PrintFlags::PrintIgnoreChunkBoundaries);
      std::cout << std::endl;
    } else {
      std::cout << "No Table produced by the reference join operator" << std::endl;
    }
    std::cout << "======================== Difference ========================" << std::endl;
    std::cout << *table_difference_message << std::endl;
    std::cout << "============================================================" << std::endl;
  };

  try {
    // Cache reference table to avoid redundant computation of the same
    if (expected_output_table_iter == expected_output_tables.end()) {
      join_reference_op->execute();
      const auto expected_output_table = join_reference_op->get_output();
      expected_output_table_iter = expected_output_tables.emplace(configuration, expected_output_table).first;
    }
    join_op->execute();
  } catch (...) {
    // If an error occurred in the join operator under test, we still want to see the test configuration
    print_configuration_info();
    throw;
  }

  actual_table = join_op->get_output();
  expected_table = expected_output_table_iter->second;

  table_difference_message = check_table_equal(actual_table, expected_table, OrderSensitivity::No, TypeCmpMode::Strict,
                                               FloatComparisonMode::AbsoluteDifference);
  if (table_difference_message) {
    print_configuration_info();
    FAIL();
  }
}

// clang-format off
INSTANTIATE_TEST_CASE_P(JoinNestedLoop, JoinTestRunner, testing::ValuesIn(JoinTestRunner::create_configurations<JoinNestedLoop>()), );  // NOLINT
INSTANTIATE_TEST_CASE_P(JoinHash, JoinTestRunner, testing::ValuesIn(JoinTestRunner::create_configurations<JoinHash>()), );  // NOLINT
INSTANTIATE_TEST_CASE_P(JoinSortMerge, JoinTestRunner, testing::ValuesIn(JoinTestRunner::create_configurations<JoinSortMerge>()), );  // NOLINT
INSTANTIATE_TEST_CASE_P(JoinIndex, JoinTestRunner, testing::ValuesIn(JoinTestRunner::create_configurations<JoinIndex>()), );  // NOLINT
INSTANTIATE_TEST_CASE_P(JoinMPSM, JoinTestRunner, testing::ValuesIn(JoinTestRunner::create_configurations<JoinMPSM>()), );  // NOLINT
// clang-format on

}  // namespace opossum