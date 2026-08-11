#include "lwi/options.h"

namespace lwi
{
namespace
{

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::vector<WhenTerm> when_terms(std::string_view expression)
{
    std::vector<WhenTerm> out;
    if (trim(expression).empty())
    {
        return out;
    }

    size_t start = 0;
    while (true)
    {
        size_t end = expression.find(',', start);
        if (end == std::string_view::npos)
        {
            end = expression.size();
        }

        WhenTerm term;
        term.id = trim(expression.substr(start, end - start));
        if (!term.id.empty() && term.id.front() == '!')
        {
            term.negated = true;
            // Trimmed again so "! extras" is the same term as "!extras". A
            // space after the bang is not a different meaning, and treating it
            // as an id that starts with a space would fail the build for no
            // reason a reader could see.
            term.id = trim(term.id.substr(1));
        }
        out.push_back(term);

        if (end == expression.size())
        {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace lwi
