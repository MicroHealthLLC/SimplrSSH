/*
 * Captive portal pages (portal.cpp): sign-in pages from untrusted networks become menus,
 * forms are sent the way a browser would, and cookies stay with the site that set them.
 */

#include "portal.hpp"
#include "test.hpp"
#include <cstring>

using namespace portal;

static Page parse(const std::string& html, const std::string& url = "http://portal.example/login?x=1")
{
    return parse_page(html.data(), html.size(), url);
}

static int field_named(const Page& page, const std::string& name)
{
    for (size_t i = 0; i < page.fields.size(); i++) {
        if (page.fields[i].name == name) {
            return (int)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

TEST(portal_parse_url)
{
    Url u;
    CHECK(parse_url("HTTPS://Portal.Example:8443/a/b?c=d#frag", u));
    CHECK(u.https);
    CHECK_EQ(u.host, std::string("portal.example"));
    CHECK_EQ(u.port, 8443);
    CHECK_EQ(u.path, std::string("/a/b?c=d"));
    CHECK_EQ(u.str(), std::string("https://portal.example:8443/a/b?c=d"));
    CHECK(parse_url("http://10.0.0.1", u));
    CHECK_EQ(u.str(), std::string("http://10.0.0.1/"));
    CHECK(parse_url("http://host?q=1", u));
    CHECK_EQ(u.path, std::string("/?q=1"));
}

TEST(portal_parse_url_rejects_unsafe)
{
    Url u;
    CHECK(!parse_url("ftp://host/", u));
    CHECK(!parse_url("javascript:alert(1)", u));
    CHECK(!parse_url("http://user:pass@evil.example/", u));
    CHECK(!parse_url("http://[::1]/", u));
    CHECK(!parse_url("http://host:0/", u));
    CHECK(!parse_url("http://host:99999/", u));
    CHECK(!parse_url("http:///path", u));
    CHECK(!parse_url("http://host/a b", u));
    CHECK(!parse_url("http://host/\r\nX-Injected: 1", u));
    CHECK(!parse_url("http://" + std::string(MAX_URL, 'a') + "/", u));
}

TEST(portal_resolve_url)
{
    const std::string base = "http://portal.example/dir/page.html?x=1";
    CHECK_EQ(resolve_url(base, "next.html"), std::string("http://portal.example/dir/next.html"));
    CHECK_EQ(resolve_url(base, "../up.html"), std::string("http://portal.example/up.html"));
    CHECK_EQ(resolve_url(base, "./a/./b/../c"), std::string("http://portal.example/dir/a/c"));
    CHECK_EQ(resolve_url(base, "/root?y=2"), std::string("http://portal.example/root?y=2"));
    CHECK_EQ(resolve_url(base, "?y=2"), std::string("http://portal.example/dir/page.html?y=2"));
    CHECK_EQ(resolve_url(base, "#top"), std::string("http://portal.example/dir/page.html?x=1"));
    CHECK_EQ(resolve_url(base, ""), std::string("http://portal.example/dir/page.html?x=1"));
    CHECK_EQ(resolve_url(base, "//cdn.example/x"), std::string("http://cdn.example/x"));
    CHECK_EQ(resolve_url("https://a.example/", "//b.example/x"), std::string("https://b.example/x"));
    CHECK_EQ(resolve_url(base, "https://Login.Example/auth"), std::string("https://login.example/auth"));
    CHECK_EQ(resolve_url(base, "  /spaced path "), std::string("http://portal.example/spaced%20path"));
}

TEST(portal_resolve_url_drops_other_schemes)
{
    const std::string base = "http://portal.example/";
    CHECK_EQ(resolve_url(base, "javascript:void(0)"), std::string(""));
    CHECK_EQ(resolve_url(base, "JavaScript:submit()"), std::string(""));
    CHECK_EQ(resolve_url(base, "mailto:desk@example.com"), std::string(""));
    CHECK_EQ(resolve_url(base, "data:text/html,hi"), std::string(""));
    CHECK_EQ(resolve_url("not a url", "page"), std::string(""));
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

static const char* CAFE = R"HTML(<!DOCTYPE html>
<html><head><title>Cafe &amp; WiFi</title>
<style>body { color: red; }</style>
<script>var x = "<form>"; </script>
</head><body>
<h1>Welcome to the Caf&eacute; WiFi</h1>
<p>Please accept our   terms&nbsp;of use.</p>
<!-- <input name="commented"> -->
<form method="POST" action="/accept">
  <input type="hidden" name="token" value="abc&amp;123">
  <label for="email">Email address</label>
  <input type="email" id="email" name="email" required placeholder="you@example.com">
  <input type="checkbox" name="tos" value="yes" required> I agree to the <a href="/terms">terms</a>
  <br>
  <label><input type="radio" name="plan" value="free" checked> Free hour</label>
  <label><input type="radio" name="plan" value="day"> Day pass</label>
  <select name="lang"><option value="en">English</option><option value="fr" selected>Fran&ccedil;ais</option></select>
  <textarea name="note">
hello</textarea>
  <input type="button" value="Needs JS" onclick="go()">
  <input type="text" name="off" disabled>
  <button type="submit" name="act" value="go">Connect <b>now</b></button>
</form>
<a href="javascript:void(0)">Script link</a>
<a href="#">Top</a>
<a href="https://cafe.example/help">Help</a>
</body></html>)HTML";

TEST(portal_page_text_and_title)
{
    Page p = parse(CAFE);
    CHECK_EQ(p.title, std::string("Cafe & WiFi"));
    CHECK(p.text.find("Welcome to the Cafe WiFi") != std::string::npos);
    CHECK(p.text.find("Please accept our terms of use.") != std::string::npos);
    CHECK(p.text.find("color") == std::string::npos);      // Style content isn't text
    CHECK(p.text.find("var x") == std::string::npos);      // Nor script
    CHECK(p.text.find("Connect") == std::string::npos);    // Button labels are items, not text
}

TEST(portal_page_forms_and_fields)
{
    Page p = parse(CAFE);
    CHECK_EQ(p.forms.size(), (size_t)1);
    CHECK(p.forms[0].post);
    CHECK_EQ(p.forms[0].action, std::string("http://portal.example/accept"));
    CHECK_EQ(field_named(p, "commented"), -1);
    CHECK_EQ(field_named(p, "off"), -1);                   // Disabled
    for (const auto& f : p.fields) {
        CHECK(f.label != "Needs JS");                       // type=button needs scripts
    }

    const Field& token = p.fields[field_named(p, "token")];
    CHECK(token.type == FieldType::Hidden);
    CHECK_EQ(token.value, std::string("abc&123"));

    const Field& email = p.fields[field_named(p, "email")];
    CHECK(email.type == FieldType::Text);
    CHECK_EQ(email.label, std::string("Email address"));
    CHECK(email.required);

    const Field& tos = p.fields[field_named(p, "tos")];
    CHECK(tos.type == FieldType::Checkbox);
    CHECK(!tos.checked);
    CHECK_EQ(tos.label, std::string("I agree to the terms"));

    int free_plan = -1;
    int day_plan = -1;
    for (size_t i = 0; i < p.fields.size(); i++) {
        if (p.fields[i].type == FieldType::Radio) {
            (p.fields[i].value == "free" ? free_plan : day_plan) = (int)i;
        }
    }
    CHECK(free_plan >= 0 && day_plan >= 0);
    CHECK(p.fields[free_plan].checked);
    CHECK_EQ(p.fields[day_plan].label, std::string("Day pass"));

    const Field& lang = p.fields[field_named(p, "lang")];
    CHECK(lang.type == FieldType::Select);
    CHECK_EQ(lang.options.size(), (size_t)2);
    CHECK_EQ(lang.selected, (size_t)1);
    CHECK_EQ(lang.options[1].label, std::string("Francais"));
    CHECK_EQ(lang.options[1].value, std::string("fr"));

    CHECK_EQ(p.fields[field_named(p, "note")].value, std::string("hello"));

    const Field& act = p.fields[field_named(p, "act")];
    CHECK(act.type == FieldType::Submit);
    CHECK_EQ(act.label, std::string("Connect now"));
}

TEST(portal_page_links)
{
    Page p = parse(CAFE);
    // javascript: and "#" links are dropped; the rest resolve
    CHECK_EQ(p.links.size(), (size_t)2);
    CHECK_EQ(p.links[0].label, std::string("terms"));
    CHECK_EQ(p.links[0].url, std::string("http://portal.example/terms"));
    CHECK_EQ(p.links[1].url, std::string("https://cafe.example/help"));
}

TEST(portal_items_and_text)
{
    Page p = parse(CAFE);
    std::vector<Item> list = items(p);
    // email, tos, 2 radios, lang, note, button, 2 links (hidden token not listed)
    CHECK_EQ(list.size(), (size_t)9);
    CHECK_EQ(item_text(p, list[0]), std::string("Email address *: (empty)"));
    CHECK_EQ(item_text(p, list[1]), std::string("[ ] I agree to the terms *"));
    CHECK_EQ(item_text(p, list[2]), std::string("(*) Free hour"));
    CHECK_EQ(item_text(p, list[4]), std::string("lang: Francais  (choose)"));
    CHECK_EQ(item_text(p, list[6]), std::string("[Connect now]"));
    CHECK_EQ(item_text(p, list[7]), std::string("Link: terms"));
}

TEST(portal_toggle_and_required)
{
    Page p = parse(CAFE);
    CHECK_EQ(missing_required(p, 0), std::string("Email address"));
    p.fields[field_named(p, "email")].value = "me@example.com";
    CHECK_EQ(missing_required(p, 0), std::string("I agree to the terms"));
    int tos = field_named(p, "tos");
    toggle(p, tos);
    CHECK(p.fields[tos].checked);
    CHECK_EQ(missing_required(p, 0), std::string(""));
    toggle(p, tos);
    CHECK(!p.fields[tos].checked);

    int free_plan = -1;
    int day_plan = -1;
    for (size_t i = 0; i < p.fields.size(); i++) {
        if (p.fields[i].type == FieldType::Radio) {
            (p.fields[i].value == "free" ? free_plan : day_plan) = (int)i;
        }
    }
    toggle(p, day_plan);
    CHECK(p.fields[day_plan].checked);
    CHECK(!p.fields[free_plan].checked);
    toggle(p, -1);
    toggle(p, 1000);
}

TEST(portal_submit_post)
{
    Page p = parse(CAFE);
    p.fields[field_named(p, "email")].value = "me+wifi@example.com";
    toggle(p, field_named(p, "tos"));
    Request r = submit(p, 0, field_named(p, "act"));
    CHECK(r.post);
    CHECK_EQ(r.url, std::string("http://portal.example/accept"));
    CHECK_EQ(r.referer, std::string("http://portal.example/login?x=1"));
    CHECK_EQ(r.body, std::string("token=abc%26123&email=me%2Bwifi%40example.com&tos=yes&plan=free&lang=fr&note=hello&act=go"));
}

TEST(portal_submit_get_replaces_query)
{
    Page p = parse("<form action='/go?old=1'><input name='room' value='12 B'><input type=checkbox name=ok>"
                   "<input type=image name=btn alt='Go'></form>");
    toggle(p, field_named(p, "ok"));
    Request r = submit(p, 0, field_named(p, "btn"));
    CHECK(!r.post);
    CHECK_EQ(r.url, std::string("http://portal.example/go?room=12+B&ok=on&btn.x=1&btn.y=1"));
    CHECK(r.body.empty());
    CHECK_EQ(submit(p, 5, -1).url, std::string(""));
}

TEST(portal_form_without_button_gets_send_item)
{
    Page p = parse("<form action=/a><input name=code></form><form action=/b><input name=x></form>");
    std::vector<Item> list = items(p);
    CHECK_EQ(list.size(), (size_t)4);
    CHECK(list[2].kind == Item::SubmitForm);
    CHECK_EQ(item_text(p, list[2]), std::string("[Send form 1]"));
    CHECK_EQ(submit(p, 1, -1).url, std::string("http://portal.example/b?x="));
}

TEST(portal_form_with_script_action_is_dropped)
{
    Page p = parse("<form action='javascript:login()'><input name=a></form>");
    CHECK(p.forms.empty());
    CHECK(p.fields.empty());
    CHECK(!has_controls(p));
}

TEST(portal_fields_outside_forms_are_ignored)
{
    Page p = parse("<input name=a><select name=b><option>x</option></select>");
    CHECK(p.fields.empty());
}

TEST(portal_meta_refresh_redirect)
{
    Page p = parse("<html><head><meta http-equiv=\"Refresh\" content=\"0; URL='/splash?id=7'\"></head></html>");
    CHECK_EQ(p.redirect, std::string("http://portal.example/splash?id=7"));
    CHECK(!has_controls(p));
    CHECK_EQ(items(p).size(), (size_t)1);
    CHECK_EQ(item_text(p, items(p)[0]), std::string("Continue to portal.example"));
}

TEST(portal_script_redirects)
{
    CHECK_EQ(parse("<script>window.location = \"https://login.example/a?b=1\";</script>").redirect,
             std::string("https://login.example/a?b=1"));
    CHECK_EQ(parse("<script>location.href='/next';</script>").redirect, std::string("http://portal.example/next"));
    CHECK_EQ(parse("<script>top.location.replace( \"/r\" )</script>").redirect, std::string("http://portal.example/r"));
    // Built at run time, compared, or a variable: unknown, so nothing
    CHECK_EQ(parse("<script>location.href = '/x?mac=' + mac;</script>").redirect, std::string(""));
    CHECK_EQ(parse("<script>if (location == '/x') {}</script>").redirect, std::string(""));
    CHECK_EQ(parse("<script>location.href = url;</script>").redirect, std::string(""));
    CHECK_EQ(parse("<script>location.href = 'javascript:x()';</script>").redirect, std::string(""));
}

TEST(portal_base_href)
{
    Page p = parse("<head><base href='https://auth.example/portal/'></head>"
                   "<form action='login'><input name=u></form><a href='help'>Help</a>");
    CHECK_EQ(p.forms[0].action, std::string("https://auth.example/portal/login"));
    CHECK_EQ(p.links[0].url, std::string("https://auth.example/portal/help"));
}

TEST(portal_label_fallbacks)
{
    Page p = parse("<form action=/a>"
                   "<p>Room number: <input name=room></p>"
                   "<p><input name=last placeholder='Last name'></p>"
                   "<p><input name=code aria-label='Voucher code' placeholder='xxxx'></p>"
                   "<p><input name=bare></p>"
                   "<p><input type=password name=pw></p>"
                   "</form>");
    CHECK_EQ(p.fields[field_named(p, "room")].label, std::string("Room number"));
    CHECK_EQ(p.fields[field_named(p, "last")].label, std::string("Last name"));
    CHECK_EQ(p.fields[field_named(p, "code")].label, std::string("Voucher code"));
    CHECK_EQ(p.fields[field_named(p, "bare")].label, std::string("bare"));
    const Field& pw = p.fields[field_named(p, "pw")];
    CHECK(pw.type == FieldType::Password);
    Page filled = p;
    filled.fields[field_named(p, "pw")].value = "secret";
    std::vector<Item> list = items(filled);
    CHECK_EQ(item_text(filled, list[4]), std::string("pw: ******"));
}

TEST(portal_option_without_value_sends_text)
{
    Page p = parse("<form action=/a><select name=s><option> Two  words </option><option value=''>Blank</option>"
                   "<option>Caf&eacute;</option></select></form>");
    const Field& s = p.fields[0];
    CHECK_EQ(s.options.size(), (size_t)3);
    CHECK_EQ(s.options[2].label, std::string("Cafe"));
    CHECK_EQ(s.options[2].value, std::string("Caf\xc3\xa9"));   // Sent as written, not as shown
    CHECK_EQ(s.options[0].value, std::string("Two words"));   // Stripped and collapsed, as browsers do
    CHECK_EQ(s.options[0].label, std::string("Two words"));
    CHECK_EQ(s.options[1].value, std::string(""));
    CHECK_EQ(s.selected, (size_t)0);
}

TEST(portal_printable_strips_control_characters)
{
    CHECK_EQ(printable("a\x1b[2Jb\tc\x7f\nd", 100), std::string("a[2Jb c d"));
    CHECK_EQ(printable("\xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x80\x94 ok\xe2\x80\xa6", 100),
             std::string("\"quoted\" - ok..."));
    CHECK_EQ(printable("caf\xc3\xa9 \xe4\xb8\xad", 100), std::string("cafe ?"));
    CHECK_EQ(printable("  many   spaces  ", 100), std::string("many spaces"));
    CHECK_EQ(printable("abcdef", 3), std::string("abc"));
    CHECK_EQ(printable("\xff\xfe broken \xc3", 100), std::string("?? broken ?"));
}

TEST(portal_numeric_entities)
{
    Page p = parse("<p>&#65;&#x42;&#0;&#xD800;&#99999999;&bogus;&amp</p>");
    CHECK_EQ(p.text, std::string("AB???&bogus;&amp"));
}

TEST(portal_bounds_hold_on_hostile_pages)
{
    std::string html = "<form action=/a>";
    for (int i = 0; i < 500; i++) {
        html += "<input name=f" + std::to_string(i) + " value='" + std::string(2000, 'v') + "'>";
    }
    html += "<select name=s>";
    for (int i = 0; i < 500; i++) {
        html += "<option>o</option>";
    }
    html += "</select></form>";
    for (int i = 0; i < 100; i++) {
        html += "<form action=/f" + std::to_string(i) + "></form><a href=/l" + std::to_string(i) + ">l</a>";
    }
    html += "<p>" + std::string(10000, 'x') + "</p>";
    Page p = parse(html);
    CHECK(p.fields.size() <= MAX_FIELDS);
    CHECK(p.forms.size() <= MAX_FORMS);
    CHECK(p.links.size() <= MAX_LINKS);
    CHECK(p.text.size() <= MAX_TEXT + 3);
    for (const auto& f : p.fields) {
        CHECK(f.value.size() <= MAX_VALUE);
        CHECK(f.options.size() <= MAX_OPTIONS);
    }
}

TEST(portal_truncated_and_broken_html)
{
    const char* broken[] = {
        "<", "<a", "<a href=", "<a href='", "<form action=/x><input name=a value='", "<!--", "<script>location='",
        "<select name=s><option>a", "<textarea name=t>abc", "&#", "<</>><<<>", "<form action=/x><button>Go",
        "<label for=x>Lbl", "<meta http-equiv=refresh content='0;url=", "</form></label></select></a>",
    };
    for (const char* html : broken) {
        Page p = parse(html);
        (void)items(p);
    }
    std::string zeros("<p>a\0b</p>", 10);
    Page p = parse(zeros);
    CHECK_EQ(p.text, std::string("ab"));
}

// ---------------------------------------------------------------------------
// Form encoding and cookies
// ---------------------------------------------------------------------------

TEST(portal_form_encode)
{
    CHECK_EQ(form_encode("a b&c=d/e*f-g.h_i~"), std::string("a+b%26c%3Dd%2Fe*f-g.h_i%7E"));
    CHECK_EQ(form_encode("\xc3\xa9\n"), std::string("%C3%A9%0A"));
}

TEST(portal_cookies_host_and_domain)
{
    Url login;
    parse_url("http://login.portal.example/x", login);
    Cookies jar;
    jar.store("session=abc; Path=/; HttpOnly", login);
    jar.store("wide=1; Domain=.portal.example", login);
    jar.store("evil=1; Domain=other.example", login);
    jar.store("tld=1; Domain=example", login);
    jar.store("bad name=1; x", login);
    jar.store("noequals", login);
    jar.store("ctl=a\x01b", login);
    CHECK_EQ(jar.size(), (size_t)3);

    Url same, sibling, other;
    parse_url("http://login.portal.example/y", same);
    parse_url("http://www.portal.example/", sibling);
    parse_url("http://other.example/", other);
    CHECK_EQ(jar.header_for(same), std::string("session=abc; wide=1; bad name=1"));
    CHECK_EQ(jar.header_for(sibling), std::string("wide=1"));
    CHECK_EQ(jar.header_for(other), std::string(""));
}

TEST(portal_cookies_secure_path_replace_delete)
{
    Url https_url, http_url, admin;
    parse_url("https://p.example/a", https_url);
    parse_url("http://p.example/a", http_url);
    parse_url("http://p.example/admin/x", admin);
    Cookies jar;
    jar.store("s=1; Secure", https_url);
    jar.store("a=1; Path=/admin", http_url);
    jar.store("v=1", http_url);
    jar.store("v=2", http_url);
    CHECK_EQ(jar.header_for(https_url), std::string("s=1; v=2"));
    CHECK_EQ(jar.header_for(http_url), std::string("v=2"));
    CHECK_EQ(jar.header_for(admin), std::string("a=1; v=2"));
    jar.store("v=gone; Max-Age=0", http_url);
    CHECK_EQ(jar.header_for(http_url), std::string(""));
    jar.wipe();
    CHECK_EQ(jar.size(), (size_t)0);
}

TEST(portal_cookie_jar_is_bounded)
{
    Url u;
    parse_url("http://p.example/", u);
    Cookies jar;
    for (int i = 0; i < 100; i++) {
        jar.store("c" + std::to_string(i) + "=1", u);
    }
    CHECK_EQ(jar.size(), MAX_COOKIES);
    jar.store("big=" + std::string(MAX_COOKIE_BYTES, 'x'), u);
    CHECK(jar.header_for(u).find("big=") == std::string::npos);
}

TEST(portal_wipe_clears_everything)
{
    Page p = parse(CAFE);
    p.fields[field_named(p, "email")].value = "me@example.com";
    wipe(p);
    CHECK(p.fields.empty());
    CHECK(p.text.empty());
    CHECK(p.url.empty());
    Request r;
    r.url = "http://x/";
    r.body = "pw=secret";
    wipe(r);
    CHECK(r.body.empty());
    CHECK(r.url.empty());
}
