#pragma once

#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "base_column.hpp"

namespace opossum {

// ValueColumn is a specific column type that stores all its values in a vector
template <typename T>
class ValueColumn : public BaseColumn {
 public:
  // return the value at a certain position. If you want to write efficient operators, back off!
  virtual const AllTypeVariant operator[](const size_t i) const { return _values.at(i); }

  // add a value to the end
  virtual void append(const AllTypeVariant& val) { _values.push_back(type_cast<T>(val)); }

  // returns all values
  const std::vector<T>& values() const { return _values; }

  // return the number of entries
  virtual size_t size() const { return _values.size(); }

  // visitor pattern, see base_column.hpp
  virtual void visit(ColumnVisitable& visitable, std::shared_ptr<ColumnVisitableContext> context = nullptr) {
    visitable.handle_value_column(*this, std::move(context));
  }

 protected:
  std::vector<T> _values;
};
}  // namespace opossum
