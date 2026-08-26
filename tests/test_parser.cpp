#include "minidb/parser.hpp"

#include <string>
#include <variant>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {

// Parses an expression and renders it fully parenthesised, so a single string
// comparison pins down associativity and precedence.
std::string shape(const std::string& source) {
    Parser parser(source);
    return to_string(*parser.parse_expression());
}

Statement parse_one(const std::string& source) {
    Parser parser(source);
    return parser.parse_statement();
}

}  // namespace

TEST(Parser, MultiplicationBindsTighterThanAddition) {
    REQUIRE_EQ(shape("1 + 2 * 3"), std::string("(1 + (2 * 3))"));
    REQUIRE_EQ(shape("1 * 2 + 3"), std::string("((1 * 2) + 3)"));
}

TEST(Parser, ArithmeticIsLeftAssociative) {
    REQUIRE_EQ(shape("1 - 2 - 3"), std::string("((1 - 2) - 3)"));
    REQUIRE_EQ(shape("8 / 4 / 2"), std::string("((8 / 4) / 2)"));
}

TEST(Parser, ParenthesesOverridePrecedence) {
    REQUIRE_EQ(shape("(1 + 2) * 3"), std::string("((1 + 2) * 3)"));
}

TEST(Parser, AndBindsTighterThanOr) {
    REQUIRE_EQ(shape("a = 1 AND b = 2 OR c = 3"),
               std::string("(((a = 1) AND (b = 2)) OR (c = 3))"));
    REQUIRE_EQ(shape("a = 1 OR b = 2 AND c = 3"),
               std::string("((a = 1) OR ((b = 2) AND (c = 3)))"));
}

TEST(Parser, ComparisonBindsTighterThanNot) {
    REQUIRE_EQ(shape("NOT a = 1"), std::string("(NOT (a = 1))"));
}

TEST(Parser, NotBindsTighterThanAnd) {
    REQUIRE_EQ(shape("NOT a AND b"), std::string("((NOT a) AND b)"));
}

TEST(Parser, UnaryMinusBindsTighterThanArithmetic) {
    REQUIRE_EQ(shape("-a + b"), std::string("((-a) + b)"));
    REQUIRE_EQ(shape("-a * b"), std::string("((-a) * b)"));
}

TEST(Parser, ConcatSitsBetweenComparisonAndArithmetic) {
    REQUIRE_EQ(shape("a || b = c"), std::string("((a || b) = c)"));
    REQUIRE_EQ(shape("a || b + c"), std::string("(a || (b + c))"));
}

TEST(Parser, ParsesIsAndIsNot) {
    REQUIRE_EQ(shape("x IS NULL"), std::string("(x IS NULL)"));
    REQUIRE_EQ(shape("x IS NOT NULL"), std::string("(x IS NOT NULL)"));
}

TEST(Parser, ParsesCreateTable) {
    const Statement stmt = parse_one("CREATE TABLE users (id INTEGER, name TEXT, score REAL)");
    const CreateTableStmt& create = std::get<CreateTableStmt>(stmt);
    REQUIRE_EQ(create.table, std::string("users"));
    REQUIRE_EQ(create.columns.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(create.columns[0].name, std::string("id"));
    REQUIRE(create.columns[0].type == ValueType::Integer);
    REQUIRE(create.columns[1].type == ValueType::Text);
    REQUIRE(create.columns[2].type == ValueType::Real);
    REQUIRE(!create.if_not_exists);
}

TEST(Parser, ParsesIfNotExists) {
    const Statement stmt = parse_one("CREATE TABLE IF NOT EXISTS t (a INT)");
    REQUIRE(std::get<CreateTableStmt>(stmt).if_not_exists);
}

TEST(Parser, ParsesMultiRowInsert) {
    const Statement stmt = parse_one("INSERT INTO t (a, b) VALUES (1, 'x'), (2, 'y')");
    const InsertStmt& insert = std::get<InsertStmt>(stmt);
    REQUIRE_EQ(insert.table, std::string("t"));
    REQUIRE_EQ(insert.columns.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(insert.rows.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(insert.rows[1].size(), static_cast<std::size_t>(2));
}

TEST(Parser, ParsesSelectWithEveryClause) {
    const Statement stmt =
        parse_one("SELECT name, score * 2 AS doubled FROM t WHERE score > 10 "
                  "ORDER BY score DESC, name LIMIT 5");
    const SelectStmt& select = std::get<SelectStmt>(stmt);
    REQUIRE_EQ(select.items.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(select.items[1].alias, std::string("doubled"));
    REQUIRE_EQ(select.table, std::string("t"));
    REQUIRE(select.where != nullptr);
    REQUIRE_EQ(select.order_by.size(), static_cast<std::size_t>(2));
    REQUIRE(select.order_by[0].descending);
    REQUIRE(!select.order_by[1].descending);
    REQUIRE_EQ(select.limit, static_cast<std::int64_t>(5));
}

TEST(Parser, ParsesSelectStar) {
    const Statement stmt = parse_one("SELECT * FROM t");
    const SelectStmt& select = std::get<SelectStmt>(stmt);
    REQUIRE_EQ(select.items.size(), static_cast<std::size_t>(1));
    REQUIRE(select.items[0].star);
    REQUIRE_EQ(select.limit, static_cast<std::int64_t>(-1));
}

TEST(Parser, ParsesSeveralStatements) {
    Parser parser("CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT * FROM t;");
    REQUIRE_EQ(parser.parse_program().size(), static_cast<std::size_t>(3));
}

TEST(Parser, ReportsMissingKeyword) {
    REQUIRE_THROWS_AS(parse_one("CREATE users (id INT)"), SqlError);
}

TEST(Parser, ReportsUnknownColumnType) {
    REQUIRE_THROWS_AS(parse_one("CREATE TABLE t (a BLOB)"), SqlError);
}

TEST(Parser, ReportsUnclosedParenthesis) {
    REQUIRE_THROWS_AS(parse_one("SELECT (1 + 2 FROM t"), SqlError);
}

TEST(Parser, ReportsMissingExpression) {
    REQUIRE_THROWS_AS(parse_one("SELECT * FROM t WHERE"), SqlError);
}

TEST(Parser, ErrorCarriesSourcePosition) {
    // `FRM` lexes as an identifier, so the statement parses as a FROM-less
    // SELECT and the complaint lands on the leftover token.
    bool threw = false;
    try {
        Parser parser("SELECT * FRM t");
        parser.parse_program();
    } catch (const SqlError& e) {
        threw = true;
        REQUIRE_EQ(e.line(), static_cast<std::size_t>(1));
        REQUIRE(e.pos() > 0);
    }
    REQUIRE(threw);
}

TEST(Parser, ParsesFunctionCalls) {
    REQUIRE_EQ(shape("COUNT(*)"), std::string("COUNT(*)"));
    REQUIRE_EQ(shape("SUM(a)"), std::string("SUM(a)"));
    REQUIRE_EQ(shape("ROUND(a, 2)"), std::string("ROUND(a, 2)"));
    REQUIRE_EQ(shape("LENGTH()"), std::string("LENGTH()"));
}

TEST(Parser, FunctionCallsNestAndCombine) {
    REQUIRE_EQ(shape("ABS(a) + 1"), std::string("(ABS(a) + 1)"));
    REQUIRE_EQ(shape("SUM(a * 2)"), std::string("SUM((a * 2))"));
    REQUIRE_EQ(shape("COALESCE(a, b + 1)"), std::string("COALESCE(a, (b + 1))"));
}

TEST(Parser, AnIdentifierWithoutParensIsStillAColumn) {
    Parser parser("count");
    const ExprPtr e = parser.parse_expression();
    REQUIRE(e->kind == ExprKind::Column);
}

TEST(Parser, ParsesGroupByAndHaving) {
    const Statement stmt = parse_one(
        "SELECT region, SUM(x) FROM t GROUP BY region, id HAVING SUM(x) > 10 "
        "ORDER BY region LIMIT 3");
    const SelectStmt& select = std::get<SelectStmt>(stmt);
    REQUIRE_EQ(select.group_by.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(to_string(*select.group_by[0]), std::string("region"));
    REQUIRE(select.having != nullptr);
    REQUIRE_EQ(select.order_by.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(select.limit, static_cast<std::int64_t>(3));
}

TEST(Parser, HavingIsAllowedWithoutGroupBy) {
    const Statement stmt = parse_one("SELECT COUNT(*) FROM t HAVING COUNT(*) > 1");
    const SelectStmt& select = std::get<SelectStmt>(stmt);
    REQUIRE(select.group_by.empty());
    REQUIRE(select.having != nullptr);
}

TEST(Parser, ReportsMissingByAfterGroup) {
    REQUIRE_THROWS_AS(parse_one("SELECT a FROM t GROUP a"), SqlError);
}

TEST(Parser, ReportsUnclosedArgumentList) {
    REQUIRE_THROWS_AS(parse_one("SELECT SUM(a FROM t"), SqlError);
}
