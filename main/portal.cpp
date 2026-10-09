/*
 * Captive Portal Pages Implementation
 * A small HTML tokenizer that keeps only what a sign-in page needs: forms and their fields,
 * links, visible text, and literal redirects. Kept free of ESP-IDF headers so the host unit
 * tests (test/host) build it as-is.
 */

#include "portal.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace portal
{

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static std::string trim(const std::string& s)
{
    size_t a = 0;
    size_t b = s.size();
    while (a < b && is_space(s[a])) {
        a++;
    }
    while (b > a && is_space(s[b - 1])) {
        b--;
    }
    return s.substr(a, b - a);
}

static bool starts_with_nocase(const char* p, const char* end, const char* word)
{
    size_t n = strlen(word);
    if ((size_t)(end - p) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)p[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

static void zero(std::string& s)
{
    volatile char* p = s.empty() ? NULL : &s[0];
    for (size_t i = 0; i < s.size(); i++) {
        p[i] = 0;
    }
    s.clear();
}

static void append_utf8(std::string& out, unsigned long cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = '?';
    }
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// HTML character references -> UTF-8 (the common named ones and every numeric one)
static std::string decode_entities(const char* p, const char* end, size_t max)
{
    static const struct { const char* name; unsigned long cp; } NAMED[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"ndash", 0x2013}, {"mdash", 0x2014},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"hellip", 0x2026}, {"bull", 0x2022}, {"middot", 0xB7}, {"euro", 0x20AC},
    };
    // Latin-1 letters U+00C0-U+00FF in order (&eacute; and the like; "times" and "divide" too)
    static const char* const LATIN1[] = {
        "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute",
        "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc",
        "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
        "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute",
        "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde", "ograve", "oacute", "ocirc",
        "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
    };
    std::string out;
    while (p < end && out.size() < max) {
        if (*p != '&') {
            out += *p++;
            continue;
        }
        const char* semi = (const char*)memchr(p, ';', std::min<size_t>(end - p, 12));
        if (!semi) {
            out += *p++;
            continue;
        }
        std::string ref(p + 1, semi);
        unsigned long cp = 0;
        bool known = false;
        if (ref.size() > 1 && ref[0] == '#') {
            bool hex = ref[1] == 'x' || ref[1] == 'X';
            const char* digits = ref.c_str() + (hex ? 2 : 1);
            char* stop = NULL;
            cp = strtoul(digits, &stop, hex ? 16 : 10);
            known = *digits && stop && *stop == '\0';
        } else {
            for (const auto& n : NAMED) {
                if (ref == n.name) {
                    cp = n.cp;
                    known = true;
                    break;
                }
            }
            for (size_t i = 0; !known && i < sizeof(LATIN1) / sizeof(LATIN1[0]); i++) {
                if (ref == LATIN1[i]) {
                    cp = 0xC0 + i;
                    known = true;
                }
            }
        }
        if (!known) {
            out += *p++;
            continue;
        }
        append_utf8(out, cp);
        p = semi + 1;
    }
    return out;
}

std::string printable(const std::string& s, size_t max)
{
    std::string out;
    bool space = true;   // Drops leading spaces and runs of them
    for (size_t i = 0; i < s.size() && out.size() < max; i++) {
        unsigned char c = s[i];
        unsigned long cp = c;
        if (c >= 0x80) {
            int extra = c >= 0xF8 ? -1 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
            cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : c & 0x1F;
            for (int k = 0; k < extra; k++) {
                if (i + 1 >= s.size() || ((unsigned char)s[i + 1] & 0xC0) != 0x80) {
                    extra = -1;   // Cut short
                    break;
                }
                cp = (cp << 6) | ((unsigned char)s[++i] & 0x3F);
            }
            if (extra < 0) {
                cp = 0xFFFD;   // Not UTF-8 (or another encoding): shown as '?'
            }
        }
        const char* text = NULL;
        char one[2] = {0, 0};
        if (cp < 0x80 && cp >= 0x20 && cp != 0x7F) {
            one[0] = (char)cp;
            text = one;
        } else if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0xA0) {
            text = " ";
        } else if (cp == 0x2018 || cp == 0x2019) {
            text = "'";
        } else if (cp == 0x201C || cp == 0x201D) {
            text = "\"";
        } else if (cp == 0x2013 || cp == 0x2014) {
            text = "-";
        } else if (cp == 0x2026) {
            text = "...";
        } else if (cp == 0x2022 || cp == 0xB7) {
            text = "*";
        } else if (cp == 0xA9) {
            text = "(c)";
        } else if (cp >= 0xC0 && cp <= 0xFF) {
            // Accented Latin letters as their plain letter, so "Cafe" reads well on the screen
            static const char PLAIN[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
            one[0] = PLAIN[cp - 0xC0];
            text = one;
        } else if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
            continue;   // Control characters never reach the screen
        } else {
            text = "?";
        }
        for (const char* t = text; *t && out.size() < max; t++) {
            if (*t == ' ') {
                if (!space) {
                    out += ' ';
                }
                space = true;
            } else {
                out += *t;
                space = false;
            }
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

std::string Url::str() const
{
    std::string s = (https ? "https" : "http") + std::string("://") + host;
    if (port != (https ? 443 : 80)) {
        s += ":" + std::to_string(port);
    }
    return s + path;
}

bool parse_url(const std::string& text, Url& out)
{
    if (text.size() > MAX_URL) {
        return false;
    }
    size_t colon = text.find("://");
    if (colon == std::string::npos) {
        return false;
    }
    std::string scheme = lower(text.substr(0, colon));
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    Url u;
    u.https = scheme == "https";
    u.port = u.https ? 443 : 80;
    size_t start = colon + 3;
    size_t end = text.find_first_of("/?#", start);
    std::string authority = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (authority.find('@') != std::string::npos || authority.empty()) {
        return false;   // No credentials in URLs (a classic way to disguise the real host)
    }
    size_t port_at = authority.find(':');
    u.host = lower(authority.substr(0, port_at));
    if (port_at != std::string::npos) {
        std::string port = authority.substr(port_at + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), ::isdigit)) {
            return false;
        }
        u.port = atoi(port.c_str());
        if (u.port < 1 || u.port > 65535) {
            return false;
        }
    }
    if (u.host.empty() || u.host.size() > 253 || u.host[0] == '.' || u.host[0] == '-') {
        return false;
    }
    for (char c : u.host) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '.') {
            return false;   // Also rules out IPv6 literals: the device is IPv4 only
        }
    }
    std::string rest = end == std::string::npos ? "" : text.substr(end);
    rest = rest.substr(0, rest.find('#'));
    if (rest.empty() || rest[0] == '?') {
        rest = "/" + rest;
    }
    for (char c : rest) {
        if ((unsigned char)c <= ' ' || (unsigned char)c >= 0x7F) {
            return false;   // Callers percent-encode; raw spaces or bytes would break the request line
        }
    }
    u.path = rest;
    out = u;
    return true;
}

// "/a/./b/../c" -> "/a/c" (the query is left alone)
static std::string remove_dot_segments(const std::string& path)
{
    size_t q = path.find('?');
    std::string p = path.substr(0, q);
    std::string query = q == std::string::npos ? "" : path.substr(q);
    std::vector<std::string> parts;
    size_t pos = 1;
    bool trailing = false;
    while (pos <= p.size()) {
        size_t slash = p.find('/', pos);
        std::string seg = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        bool last = slash == std::string::npos;
        if (seg == ".") {
            trailing = last;
        } else if (seg == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            trailing = last;
        } else {
            parts.push_back(seg);
            trailing = false;
        }
        if (last) {
            break;
        }
        pos = slash + 1;
    }
    std::string out;
    for (const auto& s : parts) {
        out += "/" + s;
    }
    if (out.empty() || trailing) {
        out += "/";
    }
    return out + query;
}

// Spaces and bytes a browser would percent-encode in a link
static std::string clean_ref(const std::string& raw)
{
    std::string ref;
    std::string t = trim(raw);
    for (unsigned char c : t) {
        if (c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c <= ' ' || c >= 0x7F || c == '"' || c == '<' || c == '>') {
            static const char HEX[] = "0123456789ABCDEF";
            ref += '%';
            ref += HEX[c >> 4];
            ref += HEX[c & 15];
        } else {
            ref += (char)c;
        }
    }
    return ref;
}

std::string resolve_url(const std::string& base_text, const std::string& raw_ref)
{
    std::string ref = clean_ref(raw_ref);
    if (ref.size() > MAX_URL) {
        return "";
    }
    // A scheme: letters (digits, + - .) then ':' before any / ? #
    size_t colon = ref.find(':');
    if (colon != std::string::npos && colon > 0 && colon < ref.find_first_of("/?#") && isalpha((unsigned char)ref[0])) {
        bool scheme = std::all_of(ref.begin(), ref.begin() + colon, [](char c) {
            return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.';
        });
        if (scheme) {
            Url u;
            return parse_url(ref, u) ? u.str() : "";   // javascript:, mailto:, data: ... -> ""
        }
    }
    Url base;
    if (!parse_url(base_text, base)) {
        return "";
    }
    if (ref.compare(0, 2, "//") == 0) {
        Url u;
        return parse_url((base.https ? "https:" : "http:") + ref, u) ? u.str() : "";
    }
    ref = ref.substr(0, ref.find('#'));
    Url u = base;
    if (ref.empty()) {
        // The page itself
    } else if (ref[0] == '/') {
        u.path = remove_dot_segments(ref);
    } else if (ref[0] == '?') {
        u.path = base.path.substr(0, base.path.find('?')) + ref;
    } else {
        std::string dir = base.path.substr(0, base.path.find('?'));
        dir = dir.substr(0, dir.rfind('/') + 1);
        u.path = remove_dot_segments(dir + ref);
    }
    std::string out = u.str();
    return out.size() <= MAX_URL ? out : "";
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------

namespace
{
    struct Attr {
        std::string name;    // Lower case
        std::string value;   // Entities decoded
    };

    struct Hints {
        std::string aria, placeholder, title, before, after;
    };

    const char* const BLOCK_TAGS[] = {
        "p", "div", "br", "li", "ul", "ol", "tr", "table", "h1", "h2", "h3", "h4", "h5", "h6",
        "form", "section", "article", "header", "footer", "nav", "main", "hr", "dd", "dt", "dl",
        "fieldset", "legend", "center", "blockquote", "pre", "aside", "td", "th", "body",
    };

    bool is_block(const std::string& tag)
    {
        for (const char* b : BLOCK_TAGS) {
            if (tag == b) {
                return true;
            }
        }
        return false;
    }

    class Parser
    {
    public:
        Parser(Page& page) : page(page), base(page.url) {}
        void run(const char* p, const char* end);

    private:
        Page& page;
        std::string base;
        std::vector<Hints> hints;
        int form = -1;
        bool text_full = false;

        bool in_label = false;
        std::string label_for, label_text;
        std::vector<int> label_fields;
        std::vector<std::pair<std::string, std::string>> label_map;   // for= id -> text

        int select_field = -1;
        bool in_option = false;
        Option option;
        std::string option_text;   // Printable, for the screen
        std::string option_raw;    // As written, sent when the option has no value=
        bool option_has_value = false;
        bool option_selected = false;
        bool select_marked = false;

        int button_field = -1;
        std::string button_text;

        bool in_link = false;
        std::string link_url, link_text, link_title;

        int after_field = -1;
        std::string recent;   // Text since the last block or field, for the next field's label

        const std::string* attr(const std::vector<Attr>& attrs, const char* name) const;
        void text(const std::string& decoded);
        void add_page_text(const std::string& s);
        void block();
        void start_tag(const std::string& tag, const std::vector<Attr>& attrs, const char*& p, const char* end);
        void end_tag(const std::string& tag);
        int add_field(Field f, const std::vector<Attr>& attrs);
        void finish_option();
        void finish_button();
        void finish_link();
        void meta(const std::vector<Attr>& attrs);
        void script(const char* p, const char* end);
        void set_redirect(const std::string& ref);
        void finish();
    };

    const std::string* Parser::attr(const std::vector<Attr>& attrs, const char* name) const
    {
        for (const auto& a : attrs) {
            if (a.name == name) {
                return &a.value;
            }
        }
        return NULL;
    }

    void Parser::add_page_text(const std::string& s)
    {
        if (text_full) {
            return;
        }
        for (char c : s) {
            if (c == ' ' && (page.text.empty() || page.text.back() == ' ' || page.text.back() == '\n')) {
                continue;
            }
            if (page.text.size() >= MAX_TEXT) {
                while (!page.text.empty() && page.text.back() == ' ') {
                    page.text.pop_back();
                }
                page.text += "...";
                text_full = true;
                return;
            }
            page.text += c;
        }
    }

    void Parser::block()
    {
        while (!page.text.empty() && page.text.back() == ' ') {
            page.text.pop_back();
        }
        if (!text_full && !page.text.empty() && page.text.back() != '\n') {
            page.text += '\n';
        }
        after_field = -1;
        recent.clear();
    }

    void Parser::text(const std::string& decoded)
    {
        std::string t = printable(decoded, MAX_TEXT);
        bool lead = !decoded.empty() && is_space(decoded[0]);
        bool trail = !decoded.empty() && is_space(decoded.back());
        if (t.empty()) {
            if (lead) {
                t = " ";
            } else {
                return;
            }
        } else {
            t = (lead ? " " : "") + t + (trail ? " " : "");
        }
        if (in_option) {
            option_text += t;
            if (option_raw.size() < MAX_VALUE) {
                option_raw += decoded;
            }
            return;
        }
        if (button_field >= 0) {
            button_text += t;
            return;
        }
        if (select_field >= 0) {
            return;   // Whitespace between options
        }
        if (in_label && label_text.size() < MAX_LABEL * 2) {
            label_text += t;
        }
        if (in_link && link_text.size() < MAX_LABEL * 2) {
            link_text += t;
        }
        if (after_field >= 0) {
            if (hints[after_field].after.size() < MAX_LABEL * 2) {
                hints[after_field].after += t;   // A checkbox's text: not the next field's
            }
        } else {
            recent += t;
            if (recent.size() > MAX_LABEL * 2) {
                recent.erase(0, recent.size() - MAX_LABEL * 2);
            }
        }
        add_page_text(t);
    }

    int Parser::add_field(Field f, const std::vector<Attr>& attrs)
    {
        if (form < 0 || page.fields.size() >= MAX_FIELDS || attr(attrs, "disabled")) {
            return -1;
        }
        f.form = form;
        const std::string* v;
        if ((v = attr(attrs, "name"))) {
            f.name = v->substr(0, MAX_VALUE);
        }
        if ((v = attr(attrs, "id"))) {
            f.id = v->substr(0, MAX_LABEL);
        }
        f.required = attr(attrs, "required") != NULL;
        Hints h;
        if ((v = attr(attrs, "aria-label"))) {
            h.aria = printable(*v, MAX_LABEL);
        }
        if ((v = attr(attrs, "placeholder"))) {
            h.placeholder = printable(*v, MAX_LABEL);
        }
        if ((v = attr(attrs, "title"))) {
            h.title = printable(*v, MAX_LABEL);
        }
        h.before = printable(recent, MAX_LABEL * 2);
        recent.clear();
        page.fields.push_back(f);
        hints.push_back(h);
        int index = page.fields.size() - 1;
        if (in_label) {
            label_fields.push_back(index);
        }
        after_field = (f.type == FieldType::Checkbox || f.type == FieldType::Radio) ? index : -1;
        return index;
    }

    void Parser::finish_option()
    {
        if (!in_option) {
            return;
        }
        in_option = false;
        Field& f = page.fields[select_field];
        if (f.options.size() < MAX_OPTIONS) {
            option.label = printable(option_text, MAX_LABEL);
            if (!option_has_value) {
                // No value=: the text is sent, spaces trimmed and collapsed as browsers do
                for (char c : trim(option_raw)) {
                    if (!is_space(c)) {
                        option.value += c;
                    } else if (option.value.back() != ' ') {
                        option.value += ' ';
                    }
                }
                option.value = option.value.substr(0, MAX_VALUE);
            }
            if (option.label.empty()) {
                option.label = option.value.empty() ? "(none)" : printable(option.value, MAX_LABEL);
            }
            if (option_selected && !select_marked) {
                f.selected = f.options.size();
                select_marked = true;
            }
            f.options.push_back(option);
        }
        option_text.clear();
    }

    void Parser::finish_button()
    {
        if (button_field < 0) {
            return;
        }
        std::string label = printable(button_text, MAX_LABEL);
        Field& f = page.fields[button_field];
        if (!label.empty()) {
            f.label = label;
        }
        button_field = -1;
        button_text.clear();
    }

    void Parser::finish_link()
    {
        if (!in_link) {
            return;
        }
        in_link = false;
        std::string label = printable(link_text, MAX_LABEL);
        if (label.empty()) {
            label = link_title;
        }
        if (!label.empty() && !link_url.empty() && page.links.size() < MAX_LINKS) {
            page.links.push_back({label, link_url});
        }
    }

    void Parser::set_redirect(const std::string& ref)
    {
        if (page.redirect.empty()) {
            std::string url = resolve_url(base, ref);
            if (!url.empty() && url != page.url) {
                page.redirect = url;
            }
        }
    }

    // <meta http-equiv="refresh" content="0; url=/next">
    void Parser::meta(const std::vector<Attr>& attrs)
    {
        const std::string* equiv = attr(attrs, "http-equiv");
        const std::string* content = attr(attrs, "content");
        if (!equiv || !content || lower(*equiv) != "refresh") {
            return;
        }
        size_t semi = content->find_first_of(";,");
        if (semi == std::string::npos) {
            return;
        }
        std::string rest = trim(content->substr(semi + 1));
        if (lower(rest.substr(0, 3)) == "url") {
            rest = trim(rest.substr(3));
            if (!rest.empty() && rest[0] == '=') {
                rest = trim(rest.substr(1));
            }
        }
        if (rest.size() >= 2 && (rest[0] == '\'' || rest[0] == '"')) {
            size_t close = rest.find(rest[0], 1);
            rest = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        }
        if (!rest.empty()) {
            set_redirect(rest);
        }
    }

    // Only a literal: location = "...", location.href = '...', location.replace("...") or .assign(...)
    void Parser::script(const char* p, const char* end)
    {
        static const char WORD[] = "location";
        const size_t n = sizeof(WORD) - 1;
        while (page.redirect.empty()) {
            const char* at = std::search(p, end, WORD, WORD + n);
            if (at == end) {
                return;
            }
            const char* q = at + n;
            p = q;
            if (end - q >= 5 && strncmp(q, ".href", 5) == 0) {
                q += 5;
            }
            while (q < end && is_space(*q)) {
                q++;
            }
            if (q < end && *q == '=' && (q + 1 == end || q[1] != '=')) {
                q++;
            } else if (end - q >= 9 && strncmp(q, ".replace(", 9) == 0) {
                q += 9;
            } else if (end - q >= 8 && strncmp(q, ".assign(", 8) == 0) {
                q += 8;
            } else {
                continue;
            }
            while (q < end && is_space(*q)) {
                q++;
            }
            if (q >= end || (*q != '"' && *q != '\'')) {
                continue;
            }
            char quote = *q++;
            const char* close = q;
            while (close < end && *close != quote && *close != '\n' && close - q <= (long)MAX_URL) {
                close++;
            }
            if (close >= end || *close != quote) {
                continue;
            }
            const char* after = close + 1;
            while (after < end && is_space(*after)) {
                after++;
            }
            if (after < end && *after == '+') {
                continue;   // Built from parts at run time: can't know the address
            }
            std::string target(q, close);
            if (target.find('\\') == std::string::npos) {
                set_redirect(target);
            }
        }
    }

    void Parser::start_tag(const std::string& tag, const std::vector<Attr>& attrs, const char*& p, const char* end)
    {
        const std::string* v;
        if (is_block(tag)) {
            block();
        }

        // Elements whose content is raw text
        if (tag == "script" || tag == "style" || tag == "title" || tag == "textarea") {
            const char* close = p;
            std::string closer = "</" + tag;
            while (close < end && !starts_with_nocase(close, end, closer.c_str())) {
                close++;
            }
            if (tag == "script") {
                script(p, close);
            } else if (tag == "title" && page.title.empty()) {
                page.title = printable(decode_entities(p, close, MAX_LABEL * 4), MAX_LABEL);
            } else if (tag == "textarea") {
                Field f;
                f.type = FieldType::Textarea;
                f.value = decode_entities(p, close, MAX_VALUE);
                if (!f.value.empty() && f.value[0] == '\n') {
                    f.value.erase(0, 1);
                }
                add_field(f, attrs);
            }
            p = close;
            const char* gt = (const char*)memchr(p, '>', end - p);
            p = gt ? gt + 1 : end;
            return;
        }

        if (tag == "base" && (v = attr(attrs, "href"))) {
            std::string url = resolve_url(base, *v);
            if (!url.empty()) {
                base = url;
            }
        } else if (tag == "meta") {
            meta(attrs);
        } else if (tag == "form") {
            if (form >= 0 || page.forms.size() >= MAX_FORMS) {
                return;   // Nested forms are ignored, as browsers do
            }
            Form f;
            const std::string* action = attr(attrs, "action");
            f.action = resolve_url(base, action ? *action : "");
            const std::string* method = attr(attrs, "method");
            f.post = method && lower(*method) == "post";
            if (f.action.empty()) {
                return;   // javascript: or another scheme: nothing to send it to
            }
            page.forms.push_back(f);
            form = page.forms.size() - 1;
        } else if (tag == "label") {
            in_label = true;
            label_text.clear();
            label_fields.clear();
            label_for = (v = attr(attrs, "for")) ? *v : "";
        } else if (tag == "a") {
            finish_link();
            const std::string* href = attr(attrs, "href");
            link_url = href ? resolve_url(base, *href) : "";
            if (link_url == page.url) {
                link_url.clear();   // "#" and the like: the same page
            }
            link_text.clear();
            link_title = (v = attr(attrs, "title")) ? printable(*v, MAX_LABEL) : "";
            in_link = true;
        } else if (tag == "input") {
            std::string type = (v = attr(attrs, "type")) ? lower(*v) : "text";
            Field f;
            if (type == "password") {
                f.type = FieldType::Password;
            } else if (type == "hidden") {
                f.type = FieldType::Hidden;
            } else if (type == "checkbox") {
                f.type = FieldType::Checkbox;
            } else if (type == "radio") {
                f.type = FieldType::Radio;
            } else if (type == "submit") {
                f.type = FieldType::Submit;
            } else if (type == "image") {
                f.type = FieldType::Image;
            } else if (type == "button" || type == "reset" || type == "file") {
                return;   // Need scripts or files: nothing the device can do with them
            }
            if ((v = attr(attrs, "value"))) {
                f.value = v->substr(0, MAX_VALUE);
            }
            f.checked = attr(attrs, "checked") != NULL;
            if (f.type == FieldType::Submit) {
                f.label = f.value.empty() ? "Submit" : printable(f.value, MAX_LABEL);
            } else if (f.type == FieldType::Image) {
                f.label = (v = attr(attrs, "alt")) && !v->empty() ? printable(*v, MAX_LABEL) : "Submit";
            }
            add_field(f, attrs);
        } else if (tag == "button") {
            finish_button();
            std::string type = (v = attr(attrs, "type")) ? lower(*v) : "submit";
            if (type != "submit") {
                return;
            }
            Field f;
            f.type = FieldType::Submit;
            if ((v = attr(attrs, "value"))) {
                f.value = v->substr(0, MAX_VALUE);
            }
            f.label = f.value.empty() ? "Submit" : printable(f.value, MAX_LABEL);
            button_field = add_field(f, attrs);
        } else if (tag == "select") {
            Field f;
            f.type = FieldType::Select;
            select_field = add_field(f, attrs);
            select_marked = false;
        } else if (tag == "option" && select_field >= 0) {
            finish_option();
            in_option = true;
            option = Option();
            option_text.clear();
            option_raw.clear();
            option_selected = attr(attrs, "selected") != NULL;
            option_has_value = (v = attr(attrs, "value")) != NULL;
            if (option_has_value) {
                option.value = v->substr(0, MAX_VALUE);
            }
        }
    }

    void Parser::end_tag(const std::string& tag)
    {
        if (is_block(tag)) {
            block();
        }
        if (tag == "form") {
            finish_button();
            form = -1;
        } else if (tag == "label") {
            std::string text = printable(label_text, MAX_LABEL);
            if (!text.empty()) {
                if (!label_for.empty()) {
                    label_map.push_back({label_for, text});
                }
                for (int i : label_fields) {
                    page.fields[i].label = text;
                }
            }
            in_label = false;
            label_fields.clear();
        } else if (tag == "a") {
            finish_link();
        } else if (tag == "button") {
            finish_button();
        } else if (tag == "option") {
            finish_option();
        } else if (tag == "select") {
            finish_option();
            select_field = -1;
        }
    }

    void Parser::finish()
    {
        finish_option();
        finish_button();
        finish_link();
        for (auto& f : page.fields) {
            if (f.selected >= f.options.size()) {
                f.selected = 0;
            }
        }
        for (const auto& [id, label] : label_map) {
            for (auto& f : page.fields) {
                if (f.id == id && f.type != FieldType::Submit && f.type != FieldType::Image) {
                    f.label = label;
                }
            }
        }
        for (size_t i = 0; i < page.fields.size(); i++) {
            Field& f = page.fields[i];
            const Hints& h = hints[i];
            if (!f.label.empty()) {
                continue;
            }
            bool box = f.type == FieldType::Checkbox || f.type == FieldType::Radio;
            std::string after = printable(h.after, MAX_LABEL);
            std::string before = printable(h.before, MAX_LABEL * 2);
            if (before.size() > MAX_LABEL) {
                before = trim(before.substr(before.size() - MAX_LABEL));
            }
            // A checkbox's text usually follows it; a text box's comes before it or is its placeholder
            std::vector<const std::string*> candidates = box
                ? std::vector<const std::string*>{&h.aria, &after, &h.title, &before, &h.placeholder}
                : std::vector<const std::string*>{&h.aria, &h.placeholder, &h.title, &before, &after};
            for (const std::string* c : candidates) {
                if (!c->empty()) {
                    f.label = *c;
                    break;
                }
            }
            if (f.label.empty()) {
                f.label = f.name.empty() ? "Field" : printable(f.name, MAX_LABEL);
            }
            while (!f.label.empty() && f.label.back() == ':') {
                f.label.pop_back();
            }
        }
        while (!page.text.empty() && (page.text.back() == '\n' || page.text.back() == ' ')) {
            page.text.pop_back();
        }
    }

    void Parser::run(const char* p, const char* end)
    {
        while (p < end) {
            if (*p != '<') {
                const char* lt = (const char*)memchr(p, '<', end - p);
                const char* stop = lt ? lt : end;
                text(decode_entities(p, stop, (size_t)(stop - p)));
                p = stop;
                continue;
            }
            if (end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
                const char* close = std::search(p + 4, end, "-->", "-->" + 3);
                p = close == end ? end : close + 3;
                continue;
            }
            if (end - p >= 2 && (p[1] == '!' || p[1] == '?')) {
                const char* gt = (const char*)memchr(p, '>', end - p);
                p = gt ? gt + 1 : end;
                continue;
            }
            bool closing = end - p >= 2 && p[1] == '/';
            const char* q = p + (closing ? 2 : 1);
            if (q >= end || !isalpha((unsigned char)*q)) {
                text("<");
                p++;
                continue;
            }
            std::string tag;
            while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == ':')) {
                if (tag.size() < 32) {
                    tag += (char)tolower((unsigned char)*q);
                }
                q++;
            }
            std::vector<Attr> attrs;
            while (q < end && *q != '>') {
                if (is_space(*q) || *q == '/') {
                    q++;
                    continue;
                }
                Attr a;
                while (q < end && !is_space(*q) && *q != '=' && *q != '>' && *q != '/') {
                    if (a.name.size() < 32) {
                        a.name += (char)tolower((unsigned char)*q);
                    }
                    q++;
                }
                while (q < end && is_space(*q)) {
                    q++;
                }
                if (q < end && *q == '=') {
                    q++;
                    while (q < end && is_space(*q)) {
                        q++;
                    }
                    const char* vstart = q;
                    const char* vend;
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char quote = *q++;
                        vstart = q;
                        while (q < end && *q != quote) {
                            q++;
                        }
                        vend = q;
                        if (q < end) {
                            q++;
                        }
                    } else {
                        while (q < end && !is_space(*q) && *q != '>') {
                            q++;
                        }
                        vend = q;
                    }
                    a.value = decode_entities(vstart, vend, MAX_URL);
                } else if (a.name.empty()) {
                    q++;   // Stray character: skip it
                    continue;
                }
                if (!a.name.empty() && attrs.size() < 32) {
                    attrs.push_back(a);
                }
            }
            p = q < end ? q + 1 : end;
            if (closing) {
                end_tag(tag);
            } else {
                start_tag(tag, attrs, p, end);
            }
        }
        finish();
    }
}

Page parse_page(const char* html, size_t len, const std::string& url)
{
    Page page;
    page.url = url;
    Parser parser(page);
    parser.run(html, html + std::min(len, MAX_PAGE_BYTES));
    return page;
}

bool has_controls(const Page& page)
{
    for (const auto& f : page.fields) {
        if (f.type != FieldType::Hidden) {
            return true;
        }
    }
    return !page.links.empty();
}

// ---------------------------------------------------------------------------
// Menu items and form state
// ---------------------------------------------------------------------------

std::vector<Item> items(const Page& page)
{
    std::vector<Item> out;
    std::vector<bool> has_button(page.forms.size(), false);
    for (size_t i = 0; i < page.fields.size(); i++) {
        const Field& f = page.fields[i];
        if (f.type == FieldType::Hidden) {
            continue;
        }
        if (f.type == FieldType::Submit || f.type == FieldType::Image) {
            has_button[f.form] = true;
        }
        out.push_back({Item::FieldItem, (int)i});
    }
    for (size_t i = 0; i < page.forms.size(); i++) {
        if (!has_button[i]) {
            out.push_back({Item::SubmitForm, (int)i});
        }
    }
    if (!page.redirect.empty()) {
        out.push_back({Item::Redirect, 0});
    }
    for (size_t i = 0; i < page.links.size(); i++) {
        out.push_back({Item::LinkItem, (int)i});
    }
    return out;
}

std::string item_text(const Page& page, const Item& item)
{
    if (item.kind == Item::SubmitForm) {
        return page.forms.size() > 1 ? "[Send form " + std::to_string(item.index + 1) + "]" : std::string("[Send]");
    }
    if (item.kind == Item::Redirect) {
        Url u;
        return "Continue to " + (parse_url(page.redirect, u) ? u.host : std::string("the next page"));
    }
    if (item.kind == Item::LinkItem) {
        return "Link: " + page.links[item.index].label;
    }
    const Field& f = page.fields[item.index];
    std::string label = f.label + (f.required ? " *" : "");
    switch (f.type) {
        case FieldType::Checkbox:
            return (f.checked ? "[x] " : "[ ] ") + label;
        case FieldType::Radio:
            return (f.checked ? "(*) " : "( ) ") + label;
        case FieldType::Select:
            return label + ": " + (f.options.empty() ? std::string("(no choices)") : f.options[f.selected].label) +
                   "  (choose)";
        case FieldType::Submit:
        case FieldType::Image:
            return "[" + f.label + "]";
        case FieldType::Password:
            return label + ": " + (f.value.empty() ? std::string("(empty)") : std::string(std::min<size_t>(f.value.size(), 12), '*'));
        default: {
            std::string shown = printable(f.value, 40);
            return label + ": " + (shown.empty() ? std::string("(empty)") : shown);
        }
    }
}

void toggle(Page& page, int index)
{
    if (index < 0 || (size_t)index >= page.fields.size()) {
        return;
    }
    Field& f = page.fields[index];
    if (f.type == FieldType::Checkbox) {
        f.checked = !f.checked;
    } else if (f.type == FieldType::Radio) {
        for (auto& other : page.fields) {
            if (other.type == FieldType::Radio && other.form == f.form && other.name == f.name) {
                other.checked = false;
            }
        }
        f.checked = true;
    }
}

std::string missing_required(const Page& page, int form)
{
    for (const auto& f : page.fields) {
        if (f.form != form || !f.required) {
            continue;
        }
        switch (f.type) {
            case FieldType::Text:
            case FieldType::Password:
            case FieldType::Textarea:
                if (f.value.empty()) {
                    return f.label;
                }
                break;
            case FieldType::Checkbox:
                if (!f.checked) {
                    return f.label;
                }
                break;
            case FieldType::Radio: {
                bool any = std::any_of(page.fields.begin(), page.fields.end(), [&](const Field& o) {
                    return o.type == FieldType::Radio && o.form == form && o.name == f.name && o.checked;
                });
                if (!any) {
                    return f.label;
                }
                break;
            }
            default:
                break;
        }
    }
    return "";
}

std::string form_encode(const std::string& s)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

Request submit(const Page& page, int form, int submitter)
{
    Request r;
    if (form < 0 || (size_t)form >= page.forms.size()) {
        return r;
    }
    const Form& target = page.forms[form];
    std::string body;
    auto add = [&body](const std::string& name, const std::string& value) {
        if (name.empty()) {
            return;
        }
        if (!body.empty()) {
            body += '&';
        }
        body += form_encode(name) + "=" + form_encode(value);
    };
    for (size_t i = 0; i < page.fields.size(); i++) {
        const Field& f = page.fields[i];
        if (f.form != form) {
            continue;
        }
        switch (f.type) {
            case FieldType::Text:
            case FieldType::Password:
            case FieldType::Hidden:
            case FieldType::Textarea:
                add(f.name, f.value);
                break;
            case FieldType::Checkbox:
            case FieldType::Radio:
                if (f.checked) {
                    add(f.name, f.value.empty() ? "on" : f.value);
                }
                break;
            case FieldType::Select:
                if (!f.options.empty()) {
                    add(f.name, f.options[f.selected].value);
                }
                break;
            case FieldType::Submit:
                if ((int)i == submitter) {
                    add(f.name, f.value);
                }
                break;
            case FieldType::Image:
                if ((int)i == submitter) {
                    std::string prefix = f.name.empty() ? "" : f.name + ".";
                    add(prefix + "x", "1");
                    add(prefix + "y", "1");
                }
                break;
        }
    }
    r.post = target.post;
    r.referer = page.url;
    if (r.post) {
        r.url = target.action;
        r.body = body;
    } else {
        r.url = target.action.substr(0, target.action.find('?')) + "?" + body;
        zero(body);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Cookies (RAM only, gone when the sign-in ends)
// ---------------------------------------------------------------------------

static bool domain_match(const std::string& host, const std::string& domain)
{
    return host == domain ||
           (host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == '.');
}

void Cookies::store(const std::string& set_cookie, const Url& from)
{
    size_t semi = set_cookie.find(';');
    std::string pair = set_cookie.substr(0, semi);
    size_t eq = pair.find('=');
    if (eq == std::string::npos || pair.size() > MAX_COOKIE_BYTES) {
        return;
    }
    Cookie c;
    c.name = trim(pair.substr(0, eq));
    c.value = trim(pair.substr(eq + 1));
    c.domain = from.host;
    c.path = "/";
    if (c.name.empty()) {
        return;
    }
    for (char ch : c.name + c.value) {
        if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F || ch == ';') {
            return;   // Would break the Cookie header
        }
    }
    bool remove = false;
    while (semi != std::string::npos) {
        size_t next = set_cookie.find(';', semi + 1);
        std::string a = trim(set_cookie.substr(semi + 1, next == std::string::npos ? std::string::npos : next - semi - 1));
        semi = next;
        size_t aeq = a.find('=');
        std::string key = lower(trim(a.substr(0, aeq)));
        std::string value = aeq == std::string::npos ? "" : trim(a.substr(aeq + 1));
        if (key == "domain" && !value.empty()) {
            std::string d = lower(value[0] == '.' ? value.substr(1) : value);
            if (!domain_match(from.host, d) || (d != from.host && d.find('.') == std::string::npos)) {
                return;   // Another site's (or a whole top-level domain's) cookie
            }
            c.domain = d;
            c.host_only = d == from.host;
        } else if (key == "path" && !value.empty() && value[0] == '/') {
            c.path = value.substr(0, 256);
        } else if (key == "secure") {
            c.secure = true;
        } else if (key == "max-age") {
            remove = atol(value.c_str()) <= 0;
        }
    }
    for (size_t i = 0; i < jar.size(); i++) {
        if (jar[i].name == c.name && jar[i].domain == c.domain && jar[i].path == c.path) {
            zero(jar[i].value);
            jar.erase(jar.begin() + i);
            break;
        }
    }
    if (remove) {
        return;
    }
    jar.push_back(c);
    if (jar.size() > MAX_COOKIES) {
        zero(jar.front().value);
        jar.erase(jar.begin());
    }
}

std::string Cookies::header_for(const Url& to) const
{
    std::string out;
    for (const auto& c : jar) {
        bool host_ok = c.host_only ? to.host == c.domain : domain_match(to.host, c.domain);
        bool path_ok = to.path.compare(0, c.path.size(), c.path) == 0;
        if (host_ok && path_ok && (!c.secure || to.https)) {
            out += (out.empty() ? "" : "; ") + c.name + "=" + c.value;
        }
    }
    return out;
}

void Cookies::wipe()
{
    for (auto& c : jar) {
        zero(c.value);
    }
    jar.clear();
}

void wipe(Page& page)
{
    for (auto& f : page.fields) {
        zero(f.value);
    }
    zero(page.text);
    page = Page();
}

void wipe(std::string& s)
{
    zero(s);
}

void wipe(Request& request)
{
    zero(request.body);
    zero(request.url);
    request = Request();
}

}
