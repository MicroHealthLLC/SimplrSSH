/*
 * Command Redaction
 * Masks the password/passphrase argument of local commands that take one
 * (connect SSID PASS, ssh HOST PORT USER PASS, sshkey ... KEYFILE PASSPHRASE),
 * so it is never echoed on screen or written to command history.
 */

#ifndef COMMAND_REDACT_HPP
#define COMMAND_REDACT_HPP

#include <string>

// Returns cmd with the secret argument (and everything after it) replaced by "****";
// *has_secret is set when the command carries one and must stay out of history.
std::string redact_secrets(const std::string& cmd, bool* has_secret);

#endif
