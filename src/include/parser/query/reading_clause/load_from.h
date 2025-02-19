#pragma once

#include "parser/expression/parsed_expression.h"
#include "reading_clause.h"

namespace kuzu {
namespace parser {

class LoadFrom : public ReadingClause {
public:
    LoadFrom() : ReadingClause{common::ClauseType::LOAD_FROM} {}

    inline bool hasObjectName() const { return !objectName.empty(); }
    inline std::string getObjectname() const { return objectName; }
    inline void setObjectName(const std::string& name) { objectName = name; }

    inline bool hasFilePaths() const { return !filePaths.empty(); }
    inline std::vector<std::string> getFilePaths() const { return filePaths; }
    inline void setFilePaths(std::vector<std::string> filePaths_) {
        filePaths = std::move(filePaths_);
    }

    inline void setParingOptions(parsing_option_t options) { parsingOptions = std::move(options); }
    inline const parsing_option_t& getParsingOptionsRef() const { return parsingOptions; }

    inline void setColumnNameDataTypes(
        std::vector<std::pair<std::string, std::string>> nameDataTypes) {
        columnNameDataTypes = std::move(nameDataTypes);
    }
    inline const std::vector<std::pair<std::string, std::string>>&
    getColumnNameDataTypesRef() const {
        return columnNameDataTypes;
    }

    inline void setWherePredicate(std::unique_ptr<ParsedExpression> expression) {
        wherePredicate = std::move(expression);
    }
    inline bool hasWherePredicate() const { return wherePredicate != nullptr; }
    inline const ParsedExpression* getWherePredicate() const { return wherePredicate.get(); }

private:
    std::string objectName;
    std::vector<std::string> filePaths;
    std::vector<std::pair<std::string, std::string>> columnNameDataTypes;
    parsing_option_t parsingOptions;
    std::unique_ptr<ParsedExpression> wherePredicate;
};

} // namespace parser
} // namespace kuzu
