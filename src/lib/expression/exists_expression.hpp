#pragma once

#include "abstract_expression.hpp"

namespace opossum {

enum class ExistsExpressionType { Exists, NotExists };

/**
 * SQL's EXISTS()
 */
class ExistsExpression : public AbstractExpression {
 public:
  ExistsExpression(const std::shared_ptr<AbstractExpression>& subquery,
                   const ExistsExpressionType init_exists_expression_type);

  std::shared_ptr<AbstractExpression> subquery() const;

  std::shared_ptr<AbstractExpression> deep_copy(
      std::unordered_map<const AbstractOperator*, std::shared_ptr<AbstractOperator>>& copied_ops
      = *std::make_unique<std::unordered_map<const AbstractOperator*, std::shared_ptr<AbstractOperator>>>())
  const override;
  std::string description(const DescriptionMode mode) const override;
  DataType data_type() const override;

  const ExistsExpressionType exists_expression_type;

 protected:
  bool _shallow_equals(const AbstractExpression& expression) const override;
  size_t _shallow_hash() const override;
  bool _on_is_nullable_on_lqp(const AbstractLQPNode& lqp) const override;
};

}  // namespace opossum
