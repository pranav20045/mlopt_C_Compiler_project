#include "mloptc/lexer.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

using mloptc::DiagnosticEngine;
using mloptc::Lexer;
using mloptc::Token;
using mloptc::TokenKind;

namespace {

/// Lexes `source` and returns the token kinds, with the trailing EndOfFile
/// dropped so tests stay readable.
std::vector<TokenKind> kindsOf(std::string_view source, DiagnosticEngine& diags) {
  Lexer lexer(source, diags);
  const std::vector<Token> tokens = lexer.tokenize();

  std::vector<TokenKind> kinds;
  for (const Token& t : tokens) {
    if (t.kind == TokenKind::EndOfFile) {
      break;
    }
    kinds.push_back(t.kind);
  }
  return kinds;
}

std::vector<TokenKind> kindsOf(std::string_view source) {
  DiagnosticEngine diags;
  auto kinds = kindsOf(source, diags);
  EXPECT_FALSE(diags.hasErrors()) << "unexpected lexer error for: " << source;
  return kinds;
}

std::vector<Token> tokensOf(std::string_view source, DiagnosticEngine& diags) {
  Lexer lexer(source, diags);
  return lexer.tokenize();
}

} // namespace

// --- Structural guarantees ---------------------------------------------------

TEST(Lexer, EmptyInputYieldsOnlyEndOfFile) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("", diags);

  ASSERT_EQ(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].kind, TokenKind::EndOfFile);
  EXPECT_FALSE(diags.hasErrors());
}

TEST(Lexer, AlwaysEndsWithExactlyOneEndOfFile) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("int x = 1;", diags);

  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.back().kind, TokenKind::EndOfFile);

  int eofCount = 0;
  for (const Token& t : tokens) {
    if (t.kind == TokenKind::EndOfFile) {
      ++eofCount;
    }
  }
  EXPECT_EQ(eofCount, 1);
}

// --- Keywords and identifiers ------------------------------------------------

TEST(Lexer, RecognisesAllKeywords) {
  EXPECT_EQ(kindsOf("int void if else while for return break continue print"),
            (std::vector{TokenKind::KwInt, TokenKind::KwVoid, TokenKind::KwIf, TokenKind::KwElse,
                         TokenKind::KwWhile, TokenKind::KwFor, TokenKind::KwReturn,
                         TokenKind::KwBreak, TokenKind::KwContinue, TokenKind::KwPrint}));
}

TEST(Lexer, KeywordPrefixesAreIdentifiers) {
  // A classic lexer bug: matching "if" inside "iffy". Identifiers must be
  // lexed maximally *first*, then looked up in the keyword table.
  EXPECT_EQ(kindsOf("iffy intx returning _int for2"),
            (std::vector{TokenKind::Identifier, TokenKind::Identifier, TokenKind::Identifier,
                         TokenKind::Identifier, TokenKind::Identifier}));
}

TEST(Lexer, IdentifiersAreCaseSensitive) {
  EXPECT_EQ(kindsOf("Int INT int"),
            (std::vector{TokenKind::Identifier, TokenKind::Identifier, TokenKind::KwInt}));
}

TEST(Lexer, IdentifierLexemeIsPreserved) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("myVar _x1", diags);

  ASSERT_GE(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].lexeme, "myVar");
  EXPECT_EQ(tokens[1].lexeme, "_x1");
}

// --- Integer literals --------------------------------------------------------

TEST(Lexer, LexesIntegerValues) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("0 7 42 2147483647", diags);

  ASSERT_GE(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].intValue, 0);
  EXPECT_EQ(tokens[1].intValue, 7);
  EXPECT_EQ(tokens[2].intValue, 42);
  EXPECT_EQ(tokens[3].intValue, 2147483647);
  EXPECT_FALSE(diags.hasErrors());
}

TEST(Lexer, AcceptsIntMinMagnitude) {
  // 2147483648 does not fit in int32, but the spec permits it as the operand
  // of a unary minus. The lexer accepts it; the parser enforces the rest.
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("2147483648", diags);

  ASSERT_GE(tokens.size(), 1U);
  EXPECT_EQ(tokens[0].intValue, 2147483648LL);
  EXPECT_FALSE(diags.hasErrors());
}

TEST(Lexer, ReportsOutOfRangeLiteral) {
  DiagnosticEngine diags;
  tokensOf("2147483649", diags);
  EXPECT_TRUE(diags.hasErrors());
}

TEST(Lexer, HugeLiteralDoesNotOverflowInternally) {
  // Accumulation must stop once past the limit; otherwise this would overflow
  // int64 -- which, built with UBSan, would abort the test run.
  DiagnosticEngine diags;
  tokensOf("999999999999999999999999999999999999", diags);
  EXPECT_TRUE(diags.hasErrors());
}

TEST(Lexer, RejectsNumberFollowedByLetters) {
  DiagnosticEngine diags;
  tokensOf("123abc", diags);
  EXPECT_TRUE(diags.hasErrors());
}

// --- Operators ---------------------------------------------------------------

TEST(Lexer, PrefersLongestOperator) {
  // Maximal munch: "<=" must lex as one token, not '<' followed by '='.
  EXPECT_EQ(kindsOf("< <= > >= == != = !"),
            (std::vector{TokenKind::Less, TokenKind::LessEqual, TokenKind::Greater,
                         TokenKind::GreaterEqual, TokenKind::EqualEqual, TokenKind::BangEqual,
                         TokenKind::Equal, TokenKind::Bang}));
}

TEST(Lexer, LexesArithmeticAndPunctuation) {
  EXPECT_EQ(kindsOf("+-*/% (){}[],;"),
            (std::vector{TokenKind::Plus, TokenKind::Minus, TokenKind::Star, TokenKind::Slash,
                         TokenKind::Percent, TokenKind::LParen, TokenKind::RParen,
                         TokenKind::LBrace, TokenKind::RBrace, TokenKind::LBracket,
                         TokenKind::RBracket, TokenKind::Comma, TokenKind::Semicolon}));
}

TEST(Lexer, LexesLogicalOperators) {
  EXPECT_EQ(kindsOf("&& ||"), (std::vector{TokenKind::AmpAmp, TokenKind::PipePipe}));
}

TEST(Lexer, SingleAmpersandIsAnError) {
  DiagnosticEngine diags;
  tokensOf("a & b", diags);
  EXPECT_TRUE(diags.hasErrors());
}

TEST(Lexer, SinglePipeIsAnError) {
  DiagnosticEngine diags;
  tokensOf("a | b", diags);
  EXPECT_TRUE(diags.hasErrors());
}

TEST(Lexer, UnexpectedCharacterIsReportedButLexingContinues) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("int @ x", diags);

  EXPECT_TRUE(diags.hasErrors());
  // Recovery matters: we must still see the tokens after the bad character.
  ASSERT_GE(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].kind, TokenKind::KwInt);
  EXPECT_EQ(tokens[1].kind, TokenKind::Unknown);
  EXPECT_EQ(tokens[2].kind, TokenKind::Identifier);
}

TEST(Lexer, HighByteDoesNotCrash) {
  // Guards the std::isalpha UB trap: a byte >= 0x80 is negative as a signed
  // char. Under UBSan this test fails loudly if we regress to <cctype>.
  DiagnosticEngine diags;
  const std::string source = "int x\xC3\xA9 = 1;";
  tokensOf(source, diags);
  EXPECT_TRUE(diags.hasErrors());
}

// --- Comments and whitespace -------------------------------------------------

TEST(Lexer, SkipsLineComments) {
  EXPECT_EQ(kindsOf("int // this is ignored\nx"),
            (std::vector{TokenKind::KwInt, TokenKind::Identifier}));
}

TEST(Lexer, SkipsBlockComments) {
  EXPECT_EQ(kindsOf("int /* ignored\nacross lines */ x"),
            (std::vector{TokenKind::KwInt, TokenKind::Identifier}));
}

TEST(Lexer, SkipsConsecutiveComments) {
  EXPECT_EQ(kindsOf("/*a*/ /*b*/ // c\n int"), (std::vector{TokenKind::KwInt}));
}

TEST(Lexer, BlockCommentsDoNotNest) {
  // Per the spec: the first "*/" closes the comment, so "x" is a real token.
  EXPECT_EQ(kindsOf("/* outer /* inner */ x"), (std::vector{TokenKind::Identifier}));
}

TEST(Lexer, ReportsUnterminatedBlockComment) {
  DiagnosticEngine diags;
  tokensOf("int /* never closed", diags);
  EXPECT_TRUE(diags.hasErrors());
}

TEST(Lexer, SlashIsNotSwallowedByCommentCheck) {
  EXPECT_EQ(kindsOf("a / b"),
            (std::vector{TokenKind::Identifier, TokenKind::Slash, TokenKind::Identifier}));
}

// --- Source locations --------------------------------------------------------

TEST(Lexer, TracksLineAndColumn) {
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("int x;\n  print(x);", diags);

  ASSERT_GE(tokens.size(), 4U);
  EXPECT_EQ(tokens[0].loc.line, 1U);
  EXPECT_EQ(tokens[0].loc.column, 1U);
  EXPECT_EQ(tokens[1].loc.column, 5U); // 'x'

  EXPECT_EQ(tokens[3].loc.line, 2U); // 'print'
  EXPECT_EQ(tokens[3].loc.column, 3U);
}

TEST(Lexer, CarriageReturnsDoNotInflateLineNumbers) {
  // Windows line endings: "\r\n" must advance the line exactly once.
  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf("a\r\nb\r\nc", diags);

  ASSERT_GE(tokens.size(), 3U);
  EXPECT_EQ(tokens[0].loc.line, 1U);
  EXPECT_EQ(tokens[1].loc.line, 2U);
  EXPECT_EQ(tokens[2].loc.line, 3U);
}

// --- A realistic program -----------------------------------------------------

TEST(Lexer, LexesAWholeFunction) {
  constexpr std::string_view source = R"(
// Greatest common divisor.
int gcd(int a, int b) {
  while (b != 0) {
    int t = b;
    b = a % b;
    a = t;
  }
  return a;
}
)";

  DiagnosticEngine diags;
  const std::vector<Token> tokens = tokensOf(source, diags);

  EXPECT_FALSE(diags.hasErrors());
  EXPECT_EQ(tokens.front().kind, TokenKind::KwInt);
  EXPECT_EQ(tokens.back().kind, TokenKind::EndOfFile);
  EXPECT_GT(tokens.size(), 30U);
}
