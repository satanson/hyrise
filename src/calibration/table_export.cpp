#include "string"
#include "fstream"

#include <storage/base_encoded_segment.hpp>
#include "table_export.hpp"
#include "storage/vector_compression/compressed_vector_type.hpp"

namespace opossum {
    TableExport::TableExport(const std::string &path_to_dir) : _path_to_dir(path_to_dir) {
      //TODO Check if file already exists;
      _create_table_meta_file();
      _create_column_meta_file();
      _create_segment_meta_file();
    }

    void TableExport::export_table(std::shared_ptr<const CalibrationTableWrapper> table_wrapper) const {
        _append_to_file(_get_table_meta_relative_path(), _export_table_meta_data(table_wrapper));
        _append_to_file(_get_column_meta_relative_path(), _export_column_meta_data(table_wrapper));
        _append_to_file(_get_segment_meta_relative_path(), _export_segment_meta_data(table_wrapper));
    }

    std::string TableExport::_get_table_meta_header() const {
      std::stringstream table_meta_header;

      table_meta_header << "TABLE_NAME" << _separator;
      table_meta_header << "ROW_COUNT" << _separator;
      table_meta_header << "CHUNK_SIZE" << "\n";

      return table_meta_header.str();
    }

    std::string TableExport::_get_column_meta_header() const {
      std::stringstream column_meta_header;

      column_meta_header << "TABLE_NAME" << _separator;
      column_meta_header << "COLUMN_NAME" << _separator;
      column_meta_header << "COLUMN_DATA_TYPE" << _separator

      return column_meta_header.str();
    }

    std::string TableExport::_get_segment_meta_header() const {
      std::stringstream segment_meta_header;

      segment_meta_header << "TABLE_NAME" << _separator;
      segment_meta_header << "COLUMN_NAME" << _separator;
      segment_meta_header << "CHUNK_ID" << _separator;
      segment_meta_header << "ENCODING_TYPE" << _separator;
      segment_meta_header << "COMPRESSION_TYPE" << "\n";

      return segment_meta_header.str();
    }

    //TODO Rewrite this
    void TableExport::_create_table_meta_file() const {
      _append_to_file(_get_table_meta_relative_path(), _get_table_meta_header());
    }

    void TableExport::_create_column_meta_file() const {
      _append_to_file(_get_column_meta_relative_path(), _get_column_meta_header());
    }

    void TableExport::_create_segment_meta_file() const {
      _append_to_file(_get_segment_meta_relative_path(), _get_segment_meta_header());
    }

    void TableExport::_append_to_file(const std::string& path,const std::string& str) const {
      std::fstream column_meta;
      column_meta.open(path, std::ofstream::out | std::ofstream::app);
      column_meta << str;
      column_meta.close();
    }

    //TODO Rewrite this
    const std::string TableExport::_get_table_meta_relative_path() const {
      return _path_to_dir + "/" + _table_meta_file_name + ".csv";
    }

    const std::string TableExport::_get_column_meta_relative_path() const {
      return _path_to_dir + "/" + _column_meta_file_name + ".csv";
    }

    const std::string TableExport::_get_segment_meta_relative_path() const {
      return _path_to_dir + "/" + _segment_meta_file_name + ".csv";
    }


    std::string TableExport::_export_table_meta_data(std::shared_ptr<const CalibrationTableWrapper> table_wrapper) const {
      std::stringstream ss;

      ss << table_wrapper->get_name() << _separator;
      ss << table_wrapper->get_table()->row_count() << _separator;
      ss << table_wrapper->get_table()->max_chunk_size() << "\n";

      return ss.str();
    }

    std::string TableExport::_export_column_meta_data(std::shared_ptr<const CalibrationTableWrapper> table_wrapper) const {
      std::stringstream ss;

      auto const table = table_wrapper->get_table();
      int column_count = table->column_count();
      const auto column_names = table->column_names();

      for (ColumnID column_id = ColumnID{0}; column_id < column_count; ++column_id) {
        ss << table_wrapper->get_name() << _separator;
        ss << table->column_name(column_id) << _separator;
        ss << table->column_data_type(column_id) << "\n";
      }

      return ss.str();
    }

    std::string TableExport::_export_segment_meta_data(std::shared_ptr<const CalibrationTableWrapper> table_wrapper) const {
      std::stringstream ss;
      const auto table = table_wrapper->get_table();
      const auto table_name = table_wrapper->get_name();
      const auto chunk_count = table->chunk_count();
      const auto column_count = table->column_count();

      for (ColumnID column_id{0}; column_id < column_count; ++column_id){
        const auto column_name = table->column_name(column_id);

        for (ChunkID chunk_id{0}; chunk_id < chunk_count; ++chunk_id) {
          ss << table_name << _separator;
          ss << column_name << _separator;
          ss << chunk_id << _separator;

          auto const segment =  table->get_chunk(chunk_id)->get_segment(column_id);

          // if segment is encoded write out values
          if (const auto encoded_segment = std::dynamic_pointer_cast<BaseEncodedSegment>(segment)){
            // Encoding Type
            ss << encoded_segment->encoding_type() << _separator;

            // Compressed Vector Type
            if (const auto compressed_vector_type = encoded_segment->compressed_vector_type()){
              ss << *compressed_vector_type << "\n";
            } else {
              ss << "null" << "\n";
            }
          // if segment is not encoded write default values for chunk;
          } else {
            ss << EncodingType::Unencoded << _separator;  // Encoding Type
            ss << "null" << "\n";                         // Compressed Vector Type
          }
        }
      }
      return ss.str();
    }


}
