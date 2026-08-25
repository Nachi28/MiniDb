#include "minidb/lexer.hpp"

#include <string>
#include <vector>

#include "minidb/diagnostic.hpp"
#include "test_framework.hpp"

using namespace minidb;

namespace {
std::vector<Token> lex(const std::string& source) { return Lexer(source).tokenize(); }
}  // namespace

TEST(Lexer, EmptyInputYieldsOnlyEndOfInput) {
    const std::vector<Token> tokens = lex("   \n\t ");
    REQUIRE_EQ(tokens.size(), static_cast<std::size_t>(1));
    REQUIRE(tokens[0].type == TokenType::EndOfInput);
}

TEST(Lexer, TokenizesASelect) {
    const std::vector<Token> tokens = lex("SELECT * FROM users;");
    REQUIRE_EQ(tokens.size(), static_cast<std::size_t>(6));
    REQUIRE(tokens[0].type == TokenType::KwSelect);
    REQUIRE(tokens[1].type == TokenType::Star);
    REQUIRE(tokens[2].type == TokenType::KwFrom);
    REQUIRE(tokens[3].type == TokenType::Identifier);
    REQUIRE_EQ(tokens[3].lexeme, std::string("users"));
    REQUIRE(tokens[4].type == TokenType::Semicolon);
    REQUIRE(tokens[5].type == TokenType::EndOfInput);
}

TEST(Lexer, KeywordsAreCaseInsensitiveButIdentifiersKeepTheirCase) {
    const std::vector<Token> tokens = lex("sElEcT MyTable");
    REQUIRE(tokens[0].type == TokenType::KwSelect);
    REQUIRE(tokens[1].type == TokenType::Identifier);
    REQUIRE_EQ(tokens[1].lexeme, std::string("MyTable"));
}

TEST(Lexer, DistinguishesIntegerAndRealLiterals) {
    const std::vector<Token> tokens = lex("42 3.5 1e3 2E-2");
    REQUIRE(tokens[0].type == TokenType::IntegerLit);
    REQUIRE_EQ(tokens[0].literal.as_integer(), static_cast<std::int64_t>(42));
    REQUIRE(tokens[1].type == TokenType::RealLit);
    REQUIRE_EQ(tokens[1].literal.as_double(), 3.5);
    REQUIRE(tokens[2].type == TokenType::RealLit);
    REQUIRE_EQ(tokens[2].literal.as_double(), 1000.0);
    REQUIRE(tokens[3].type == TokenType::RealLit);
    REQUIRE_EQ(tokens[3].literal.as_double(), 0.02);
}

TEST(Lexer, StringLiteralsUseDoubledQuoteEscape) {
    const std::vector<Token> tokens = lex("'it''s fine'");
    REQUIRE(tokens[0].type == TokenType::StringLit);
    REQUIRE_EQ(tokens[0].literal.as_text(), std::string("it's fine"));
}

TEST(Lexer, DoubleQuotesProduceIdentifiers) {
    const std::vector<Token> tokens = lex("\"select\"");
    REQUIRE(tokens[0].type == TokenType::Identifier);
    REQUIRE_EQ(tokens[0].lexeme, std::string("select"));
}

TEST(Lexer, RecognisesTwoCharacterOperators) {
    const std::vector<Token> tokens = lex("<= >= <> != == ||");
    REQUIRE(tokens[0].type == TokenType::LtEq);
    REQUIRE(tokens[1].type == TokenType::GtEq);
    REQUIRE(tokens[2].type == TokenType::NotEq);
    REQUIRE(tokens[3].type == TokenType::NotEq);
    REQUIRE(tokens[4].type == TokenType::Eq);
    REQUIRE(tokens[5].type == TokenType::Concat);
}

TEST(Lexer, SkipsLineAndBlockComments) {
    const std::vector<Token> tokens = lex("SELECT -- trailing\n/* block */ 1");
    REQUIRE_EQ(tokens.size(), static_cast<std::size_t>(3));
    REQUIRE(tokens[0].type == TokenType::KwSelect);
    REQUIRE(tokens[1].type == TokenType::IntegerLit);
}

TEST(Lexer, TracksLineAndColumn) {
    const std::vector<Token> tokens = lex("SELECT\n  name");
    REQUIRE_EQ(tokens[1].line, static_cast<std::size_t>(2));
    REQUIRE_EQ(tokens[1].col, static_cast<std::size_t>(3));
}

TEST(Lexer, ReportsUnterminatedString) {
    REQUIRE_THROWS_AS(lex("SELECT 'oops"), SqlError);
}

TEST(Lexer, ReportsUnterminatedBlockComment) {
    REQUIRE_THROWS_AS(lex("SELECT /* oops"), SqlError);
}

TEST(Lexer, ReportsUnexpectedCharacter) {
    REQUIRE_THROWS_AS(lex("SELECT #"), SqlError);
}

TEST(Lexer, ReportsMalformedNumericLiteral) {
    REQUIRE_THROWS_AS(lex("SELECT 12abc"), SqlError);
}
