/*
 * Captive Portal Client
 * Fetches a network's sign-in page and sends its forms (esp_http_client): redirects are
 * followed by hand so cookies set on the way are kept (in RAM only, in the Cookies passed
 * in), HTTPS is verified with the built-in CA bundle, bodies are read into PSRAM.
 * Blocking; call from a worker task, never from the UI task.
 */

#ifndef PORTAL_CLIENT_HPP
#define PORTAL_CLIENT_HPP

#include "esp_err.h"
#include "portal.hpp"
#include <string>

namespace portal
{
    // Plain HTTP on purpose: a network with a sign-in page answers it with (or redirects it
    // to) that page; once the internet is reachable it answers 204, no content
    extern const char* const PROBE_URL;

    struct Result {
        Page page;              // The page it ended on (page.url, page.status)
        bool online = false;    // PROBE_URL got its 204: no sign-in needed (any more)
        std::string error;      // For the screen, "" if none
    };

    // Sends `request`, following redirects (HTTP, meta refresh, a literal script redirect on a
    // page with nothing to fill in), at most 8 steps
    esp_err_t fetch(const Request& request, Cookies& cookies, Result& out);
    // Fetches PROBE_URL: online, or the sign-in page the network sends instead
    esp_err_t probe(Cookies& cookies, Result& out);
}

#endif
