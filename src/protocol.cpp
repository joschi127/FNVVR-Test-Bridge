// FNVVR-Test-Bridge. Copyright (c) 2026 joschi127. zlib licence, see LICENSE.

#include "protocol.h"

#include <cctype>
#include <cstdlib>

namespace bridge
{
    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        bool IEquals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        // Splits off the first word; rest is trimmed.
        std::string_view FirstWord(std::string_view s, std::string_view& rest)
        {
            size_t end = s.find_first_of(" \t");
            if (end == std::string_view::npos)
            {
                rest = {};
                return s;
            }
            rest = Trim(s.substr(end));
            return s.substr(0, end);
        }

        bool ParseHex(std::string_view s, uint32_t& out)
        {
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                s.remove_prefix(2);
            if (s.empty() || s.size() > 8)
                return false;
            uint32_t v = 0;
            for (char c : s)
            {
                int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' :
                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0)
                    return false;
                v = v * 16 + static_cast<uint32_t>(d);
            }
            out = v;
            return true;
        }

        Request Invalid(std::string why)
        {
            Request r;
            r.error = std::move(why);
            return r;
        }
    }

    Request Parse(std::string_view line)
    {
        if (line.size() > kMaxLine)
            return Invalid("line too long");
        for (char c : line)
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\r')
                return Invalid("control characters");
        line = Trim(line);
        if (line.empty())
            return Invalid("empty line");

        std::string_view rest;
        std::string_view word = FirstWord(line, rest);
        Request r;
        if (IEquals(word, "ping") && rest.empty())
            r.verb = Verb::Ping;
        else if (IEquals(word, "state") && rest.empty())
            r.verb = Verb::State;
        else if (IEquals(word, "run"))
        {
            if (rest.empty())
                return Invalid("run <console line>");
            r.verb = Verb::Run;
            r.text = rest;
        }
        else if (IEquals(word, "eval"))
        {
            if (rest.empty())
                return Invalid("eval <expression>");
            r.verb = Verb::Eval;
            r.text = rest;
        }
        else if (IEquals(word, "on") || IEquals(word, "evalon"))
        {
            bool eval = IEquals(word, "evalon");
            std::string_view text;
            std::string_view id = FirstWord(rest, text);
            if (!ParseHex(id, r.refId) || text.empty())
                return Invalid(eval ? "evalon <ref id, hex> <expression>" : "on <ref id, hex> <console line>");
            r.verb = eval ? Verb::EvalOn : Verb::RunOn;
            r.text = text;
        }
        else
        {
            r.verb = Verb::Run;
            r.text = line;
        }
        return r;
    }

    void LineBuffer::Feed(std::string_view bytes)
    {
        for (char c : bytes)
        {
            if (m_dropping)
            {
                if (c == '\n')
                    m_dropping = false;
                continue;
            }
            m_data.push_back(c);
            if (c != '\n' && m_data.size() > m_limit && m_data.find('\n') == std::string::npos)
            {
                m_data.clear();
                m_dropping = true;
                m_overflowed = true;
            }
        }
    }

    bool LineBuffer::Next(std::string& line, bool& overflow)
    {
        size_t end = m_data.find('\n');
        if (end != std::string::npos)
        {
            line = m_data.substr(0, end);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_data.erase(0, end + 1);
            overflow = false;
            return true;
        }
        if (m_overflowed)
        {
            m_overflowed = false;
            line.clear();
            overflow = true;
            return true;
        }
        return false;
    }

    std::vector<std::string> ConsoleLines(std::string_view text)
    {
        std::vector<std::string> out;
        while (!text.empty())
        {
            size_t end = text.find('\n');
            std::string_view l = text.substr(0, end);
            while (!l.empty() && l.back() == '\r')
                l.remove_suffix(1);
            if (!Trim(l).empty())
                out.push_back("| " + std::string(l));
            if (end == std::string_view::npos)
                break;
            text.remove_prefix(end + 1);
        }
        return out;
    }

    std::string Ok(std::string_view detail)
    {
        return detail.empty() ? "ok" : "ok " + std::string(detail);
    }

    std::string Err(std::string_view detail)
    {
        return "err " + std::string(detail);
    }
}
