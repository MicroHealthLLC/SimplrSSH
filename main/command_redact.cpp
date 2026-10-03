/*
 * Command Redaction Implementation
 * Kept free of ESP-IDF headers so the host unit tests (test/host) build it as-is.
 */

#include "command_redact.hpp"

std::string redact_secrets(const std::string& cmd, bool* has_secret)
{
    *has_secret = false;
    size_t secret_arg;
    if (cmd.rfind("connect ", 0) == 0) {
        secret_arg = 1;
    } else if (cmd.rfind("ssh ", 0) == 0) {
        secret_arg = 3;
    } else if (cmd.rfind("sshkey ", 0) == 0) {
        secret_arg = 4;
    } else {
        return cmd;
    }

    // Find where each argument starts, honouring "quoted strings"
    size_t arg = 0;
    bool in_token = false;
    bool in_quotes = false;
    for (size_t i = cmd.find(' '); i < cmd.length(); i++) {
        char c = cmd[i];
        if (c == ' ' && !in_quotes) {
            in_token = false;
            continue;
        }
        if (!in_token) {
            if (arg++ == secret_arg) {
                *has_secret = true;
                return cmd.substr(0, i) + "****";
            }
            in_token = true;
        }
        if (c == '"') {
            in_quotes = !in_quotes;
        }
    }
    return cmd;
}
