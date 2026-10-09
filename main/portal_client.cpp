/*
 * Captive Portal Client Implementation
 */

#include "portal_client.hpp"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform_util.h"
#include <cstring>
#include <strings.h>
#include <vector>

static const char *TAG = "PORTAL";

namespace portal
{

const char* const PROBE_URL = "http://connectivitycheck.gstatic.com/generate_204";

static const int MAX_HOPS = 8;
static const int TIMEOUT_MS = 10000;
static const size_t MAX_SET_COOKIES = 20;   // Per response
static const char* USER_AGENT = "Mozilla/5.0 (Linux; SimplrSSH) AppleWebKit/537.36 (KHTML, like Gecko)";

// Response headers the client needs, collected by the event handler
struct Capture {
    std::vector<std::string> set_cookies;
    std::string location;
    std::string content_type;
};

static esp_err_t on_event(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data || !evt->header_key || !evt->header_value) {
        return ESP_OK;
    }
    Capture* c = (Capture*)evt->user_data;
    if (strcasecmp(evt->header_key, "Set-Cookie") == 0) {
        if (c->set_cookies.size() < MAX_SET_COOKIES && strlen(evt->header_value) <= MAX_COOKIE_BYTES + 512) {
            c->set_cookies.push_back(evt->header_value);
        }
    } else if (strcasecmp(evt->header_key, "Location") == 0) {
        c->location = std::string(evt->header_value).substr(0, MAX_URL + 1);
    } else if (strcasecmp(evt->header_key, "Content-Type") == 0) {
        c->content_type = std::string(evt->header_value).substr(0, 100);
    }
    return ESP_OK;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// A page the parser can read (no type given counts: some portals leave it out)
static bool is_text(const std::string& content_type)
{
    std::string t;
    for (char c : content_type) {
        t += (char)tolower((unsigned char)c);
    }
    return t.empty() || t.find("html") != std::string::npos || t.find("text/") != std::string::npos;
}

// One request, no redirects followed. On success `body` (PSRAM, or NULL) holds up to
// MAX_PAGE_BYTES of the response; the caller frees it with free_body().
static esp_err_t request_once(const Request& req, const Url& url, Cookies& cookies, Capture& cap, int& status,
                              char*& body, size_t& len, std::string& error)
{
    body = NULL;
    len = 0;
    std::string address = url.str();
    esp_http_client_config_t cfg = {};
    cfg.url = address.c_str();
    cfg.method = req.post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = TIMEOUT_MS;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.event_handler = on_event;
    cfg.user_data = &cap;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        error = "Out of memory";
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "text/html,application/xhtml+xml,*/*;q=0.8");
    std::string cookie = cookies.header_for(url);
    if (!cookie.empty()) {
        esp_http_client_set_header(client, "Cookie", cookie.c_str());
    }
    Url from;
    if (!req.referer.empty() && parse_url(req.referer, from) && (url.https || !from.https)) {
        esp_http_client_set_header(client, "Referer", req.referer.c_str());   // Never https -> http
    }
    if (req.post) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    }

    esp_err_t err = esp_http_client_open(client, req.post ? req.body.size() : 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        error = "Can't reach " + url.host + (url.https ? " (no connection, or its certificate isn't valid)" : "");
        return err;
    }
    size_t sent = 0;
    while (req.post && sent < req.body.size()) {
        int n = esp_http_client_write(client, req.body.data() + sent, req.body.size() - sent);
        if (n <= 0) {
            break;
        }
        sent += n;
    }
    if (sent < (req.post ? req.body.size() : 0) || esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        error = "No answer from " + url.host;
        return ESP_FAIL;
    }
    status = esp_http_client_get_status_code(client);
    for (const auto& c : cap.set_cookies) {
        cookies.store(c, url);
    }

    if (!is_redirect(status) && status != 204 && is_text(cap.content_type)) {
        body = (char*)heap_caps_malloc(MAX_PAGE_BYTES, MALLOC_CAP_SPIRAM);
        if (!body) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            error = "Out of memory";
            return ESP_ERR_NO_MEM;
        }
        int n;
        while (len < MAX_PAGE_BYTES && (n = esp_http_client_read(client, body + len, MAX_PAGE_BYTES - len)) > 0) {
            len += n;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

// Pages may hold what the user typed (echoed back): overwritten before freeing
static void free_body(char*& body, size_t len)
{
    if (body) {
        mbedtls_platform_zeroize(body, len);
        heap_caps_free(body);
        body = NULL;
    }
}

esp_err_t fetch(const Request& request, Cookies& cookies, Result& out)
{
    out = Result();
    Request req = request;
    for (int hop = 0; hop < MAX_HOPS; hop++) {
        Url url;
        if (!parse_url(req.url, url)) {
            out.error = "The page gave an address the device can't open.";
            wipe(req);
            return ESP_FAIL;
        }
        Capture cap;
        int status = 0;
        char* body = NULL;
        size_t len = 0;
        esp_err_t err = request_once(req, url, cookies, cap, status, body, len, out.error);
        if (err != ESP_OK) {
            wipe(req);
            return err;
        }

        if (is_redirect(status) && !cap.location.empty()) {
            std::string next = resolve_url(url.str(), cap.location);
            if (next.empty()) {
                out.error = "The network sent the device to an address it can't open.";
                wipe(req);
                return ESP_FAIL;
            }
            if (status != 307 && status != 308) {
                wipe(req.body);   // 301/302/303: the next request is a plain GET
                req.post = false;
            }
            req.referer = url.str();
            req.url = next;
            continue;
        }

        if (body) {
            out.page = parse_page(body, len, url.str());
        } else {
            out.page = Page();
            out.page.url = url.str();
        }
        out.page.status = status;
        free_body(body, len);
        ESP_LOGI(TAG, "Page: status %d, %u fields, %u links", status, (unsigned)out.page.fields.size(),
                 (unsigned)out.page.links.size());

        // A page with nothing to fill in that only moves on (refresh or script): follow it
        if (!has_controls(out.page) && !out.page.redirect.empty()) {
            wipe(req.body);
            req.post = false;
            req.referer = url.str();
            req.url = out.page.redirect;
            continue;
        }
        wipe(req);
        return ESP_OK;
    }
    wipe(req);
    out.error = "The sign-in page kept redirecting.";
    return ESP_FAIL;
}

esp_err_t probe(Cookies& cookies, Result& out)
{
    Request req;
    req.url = PROBE_URL;
    esp_err_t err = fetch(req, cookies, out);
    out.online = err == ESP_OK && out.page.status == 204 && out.page.url == PROBE_URL;
    return err;
}

}
