#pragma once

#include <string_view>
#include <vector>

namespace lwi
{

/// One term of a `when` expression: an option id, and whether it was negated.
struct WhenTerm
{
    std::string_view id;
    bool negated = false;
};

/// Splits a `when` expression into its terms.
///
/// The grammar is a comma separated list of option ids, each optionally
/// prefixed with '!', all of which must hold. Deliberately not an expression
/// language: the stub runs elevated, and a parser is a parser.
///
/// Lives in the core rather than in either consumer because both need it and
/// they need the same answer. The stub evaluates the expression at install time
/// and the forge validates it at build time, and a forge that split terms
/// differently would approve expressions the stub then read another way, which
/// is worse than having no validation at all.
///
/// Views into the input, which therefore has to outlive the result. An empty
/// expression yields no terms, which reads as "no condition".
std::vector<WhenTerm> when_terms(std::string_view expression);

} // namespace lwi
