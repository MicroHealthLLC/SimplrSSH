/*
 * Captive portal pages
 * The sign-in page some WiFi networks (cafes, hotels) show before letting traffic through,
 * turned into something the menus can show: its text, the fields of its forms (text,
 * checkboxes, radio buttons, lists, buttons) and its links. No scripts run; only a literal
 * script or meta-refresh redirect is noticed. Also URL resolution, form encoding and an
 * in-memory cookie jar. Free of ESP-IDF headers: the host tests (test/host) build it as-is.
 *
 * Everything here comes from whatever network the device joined: untrusted. Sizes are
 * bounded and text meant for the screen is reduced to printable ASCII.
 */

#ifndef PORTAL_HPP
#define PORTAL_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace portal
{
    static const size_t MAX_PAGE_BYTES = 192 * 1024;  // Response body read (PSRAM)
    static const size_t MAX_TEXT = 1500;               // Page text shown
    static const size_t MAX_FORMS = 8;
    static const size_t MAX_FIELDS = 60;               // Including hidden ones
    static const size_t MAX_OPTIONS = 40;              // Per list
    static const size_t MAX_LINKS = 12;
    static const size_t MAX_LABEL = 80;
    static const size_t MAX_VALUE = 512;               // Field values (typed or from the page)
    static const size_t MAX_URL = 1024;
    static const size_t MAX_COOKIES = 30;
    static const size_t MAX_COOKIE_BYTES = 1024;       // name=value of one cookie

    // http(s)://host[:port]/path?query - IPv4 or DNS names only, no user@ part
    struct Url {
        bool https = false;
        std::string host;    // Lower case
        int port = 80;
        std::string path;    // "/..." including the query, never the #fragment
        std::string str() const;
    };
    bool parse_url(const std::string& text, Url& out);
    // `ref` (as written in a page) relative to `base`; "" if it isn't an http(s) URL
    std::string resolve_url(const std::string& base, const std::string& ref);

    enum class FieldType { Text, Password, Hidden, Checkbox, Radio, Select, Textarea, Submit, Image };
    struct Option {
        std::string label;
        std::string value;
    };
    struct Field {
        FieldType type = FieldType::Text;
        int form = 0;
        std::string name;
        std::string id;
        std::string value;
        std::string label;   // Printable
        bool checked = false;
        bool required = false;
        std::vector<Option> options;
        size_t selected = 0;
    };
    struct Form {
        std::string action;  // Resolved URL
        bool post = false;
    };
    struct Link {
        std::string label;   // Printable
        std::string url;     // Resolved
    };
    struct Page {
        std::string url;         // Where it came from
        int status = 0;          // HTTP status
        std::string title;       // Printable
        std::string text;        // Printable, at most MAX_TEXT
        std::vector<Form> forms;
        std::vector<Field> fields;
        std::vector<Link> links;
        std::string redirect;    // Meta refresh or literal script redirect, resolved ("" if none)
    };
    Page parse_page(const char* html, size_t len, const std::string& url);
    // Visible fields or links: something the user can act on
    bool has_controls(const Page& page);

    // What the menu lists, numbered from 1: visible fields, a Submit for forms without a
    // button, the redirect (if any) and links
    struct Item {
        enum Kind { FieldItem, SubmitForm, Redirect, LinkItem } kind;
        int index;   // Field, form or link index
    };
    std::vector<Item> items(const Page& page);
    std::string item_text(const Page& page, const Item& item);

    // Checkbox: flips it. Radio: picks it (others with its name in the form are cleared)
    void toggle(Page& page, int field);
    // Label of the first required field of `form` left empty / unticked, "" if none
    std::string missing_required(const Page& page, int form);

    struct Request {
        bool post = false;
        std::string url;
        std::string body;        // application/x-www-form-urlencoded
        std::string referer;
    };
    // The request a form makes when sent with button `submitter` (a field index, or -1)
    Request submit(const Page& page, int form, int submitter);
    std::string form_encode(const std::string& s);

    // Printable ASCII for the screen: entities already decoded, other characters as '?'
    std::string printable(const std::string& s, size_t max);

    class Cookies
    {
    public:
        // One Set-Cookie header received from `from`
        void store(const std::string& set_cookie, const Url& from);
        // Cookie header value for a request to `to` ("" if none)
        std::string header_for(const Url& to) const;
        size_t size() const { return jar.size(); }
        void wipe();
    private:
        struct Cookie {
            std::string name, value, domain, path;
            bool host_only = true;
            bool secure = false;
        };
        std::vector<Cookie> jar;
    };

    // Overwrites typed values and the page before freeing them (they may hold personal data)
    void wipe(Page& page);
    void wipe(Request& request);
    void wipe(std::string& s);
}

#endif
