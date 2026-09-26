/*
 * Vault Menu
 * Master PIN prompts for the encrypted password vault: creating the PIN the
 * first time a password is saved, unlocking it when a saved password is needed,
 * and the 'vault' command (status, lock, unlock, change PIN, reset).
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "esp_log.h"
#include <algorithm>

static const int MAX_PIN_ATTEMPTS = 3;

static std::string trim_lower(const std::string& s)
{
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) {
        return "";
    }
    std::string t = s.substr(start, s.find_last_not_of(' ') - start + 1);
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    return t;
}

// Runs `then` once the vault is unlocked, asking for (or creating) the PIN first
void SSHTerminal::with_vault(std::function<void()> then)
{
    if (!vault::is_mounted()) {
        append_text("ERROR: Secure storage is unavailable.\n");
        wizard_done();
        return;
    }
    if (vault::is_unlocked()) {
        then();
        return;
    }

    wizard.after_unlock = then;
    wizard.pin_attempts = 0;
    if (vault::is_set_up()) {
        wizard_goto(WizardStep::VaultUnlock);
    } else {
        append_text("Saved passwords are encrypted with a master PIN.\n");
        wizard_goto(WizardStep::VaultNewPin);
    }
}

void SSHTerminal::clear_all_saved_secret_flags()
{
    for (auto& p : profiles) {
        p.secret_saved = false;
    }
    for (auto& n : saved_networks) {
        n.secret_saved = false;
    }
    profile_store::save(profiles);
    network_store::save(saved_networks);
}

void SSHTerminal::handle_vault_command(const std::string& command)
{
    size_t space = command.find(' ');
    std::string sub = space == std::string::npos ? "" : trim_lower(command.substr(space));

    wizard_reset();

    if (!vault::is_mounted()) {
        append_text("ERROR: Secure storage is unavailable.\n");
        return;
    }

    if (sub.empty() || sub == "status") {
        if (!vault::is_set_up()) {
            append_text("Vault: no PIN yet (created when you first save a password)\n");
        } else {
            append_text(vault::is_unlocked() ? "Vault: PIN set, unlocked\n" : "Vault: PIN set, locked\n");
        }
        append_text("  vault lock | vault unlock | vault pin | vault reset\n");
    } else if (sub == "lock") {
        vault::lock();
        append_text("Vault locked. Saved passwords need the PIN again.\n");
    } else if (sub == "unlock") {
        if (!vault::is_set_up()) {
            append_text("No PIN set yet.\n");
        } else if (vault::is_unlocked()) {
            append_text("Vault already unlocked.\n");
        } else {
            with_vault([this]() {
                wizard_reset();
                append_text("Vault unlocked.\n");
            });
        }
    } else if (sub == "pin") {
        if (!vault::is_set_up()) {
            append_text("No PIN set yet. One is created when you first save a password.\n");
        } else {
            wizard.action = WizardAction::ChangePin;
            wizard_goto(WizardStep::VaultOldPin);
        }
    } else if (sub == "reset") {
        wizard_goto(WizardStep::VaultConfirmReset);
    } else {
        append_text("Usage: vault [status|lock|unlock|pin|reset]\n");
    }
}

bool SSHTerminal::vault_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::VaultUnlock:
            append_text("Master PIN (Enter = skip): ");
            return true;
        case WizardStep::VaultOldPin:
            append_text("Current PIN: ");
            return true;
        case WizardStep::VaultNewPin:
            append_text(wizard.action == WizardAction::ChangePin
                        ? "New PIN (min 4 chars): "
                        : "Create master PIN (min 4 chars, Enter = cancel): ");
            return true;
        case WizardStep::VaultConfirmPin:
            append_text("Confirm PIN: ");
            return true;
        case WizardStep::VaultConfirmReset:
            append_text("Erase ALL saved passwords and the PIN? (y/n) [n]: ");
            wizard.choices = {"n", "y"};
            return true;
        default:
            return false;
    }
}

bool SSHTerminal::vault_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::VaultUnlock: {
            if (raw_input.empty()) {
                append_text("Skipped - vault stays locked.\n");
                wizard_done();
                break;
            }
            append_text("Unlocking...\n");
            refresh_display_now();
            esp_err_t err = vault::unlock(raw_input);
            if (err == ESP_OK) {
                auto then = wizard.after_unlock;
                wizard.after_unlock = nullptr;
                if (then) {
                    then();
                } else {
                    wizard_done();
                }
            } else if (err == ESP_ERR_INVALID_ARG && ++wizard.pin_attempts < MAX_PIN_ATTEMPTS) {
                append_text("Wrong PIN.\n");
                wizard_prompt();
            } else {
                append_text(err == ESP_ERR_INVALID_ARG ? "Wrong PIN - giving up.\n"
                                                       : "ERROR: Vault key file is unreadable.\n");
                wizard_done();
            }
            break;
        }

        case WizardStep::VaultOldPin: {
            if (raw_input.empty()) {
                append_text("Cancelled.\n");
                wizard_done();
                break;
            }
            refresh_display_now();
            esp_err_t err = vault::unlock(raw_input);
            if (err == ESP_OK) {
                vault::wipe(wizard.secret);
                wizard.secret = raw_input;  // Needed again to re-wrap the key
                wizard_goto(WizardStep::VaultNewPin);
            } else if (err == ESP_ERR_INVALID_ARG && ++wizard.pin_attempts < MAX_PIN_ATTEMPTS) {
                append_text("Wrong PIN.\n");
                wizard_prompt();
            } else {
                append_text("Wrong PIN - giving up.\n");
                wizard_done();
            }
            break;
        }

        case WizardStep::VaultNewPin:
            if (raw_input.empty()) {
                append_text("Cancelled.\n");
                wizard_done();
            } else if (raw_input.size() < VAULT_MIN_PIN_LENGTH) {
                append_text("PIN must be at least 4 characters.\n");
                wizard_prompt();
            } else {
                vault::wipe(wizard.pin);
                wizard.pin = raw_input;
                wizard_goto(WizardStep::VaultConfirmPin);
            }
            break;

        case WizardStep::VaultConfirmPin: {
            if (raw_input != wizard.pin) {
                append_text("PINs don't match.\n");
                vault::wipe(wizard.pin);
                wizard_goto(WizardStep::VaultNewPin);
                break;
            }
            refresh_display_now();
            if (wizard.action == WizardAction::ChangePin) {
                if (vault::change_pin(wizard.secret, wizard.pin) == ESP_OK) {
                    append_text("PIN changed.\n");
                } else {
                    append_text("ERROR: Failed to change PIN.\n");
                }
                wizard_done();
                break;
            }
            if (vault::setup(wizard.pin) != ESP_OK) {
                append_text("ERROR: Failed to create the vault.\n");
                wizard_done();
                break;
            }
            vault::wipe(wizard.pin);
            append_text("PIN set. It can't be recovered - 'vault reset' erases saved passwords.\n");
            auto then = wizard.after_unlock;
            wizard.after_unlock = nullptr;
            if (then) {
                then();
            } else {
                wizard_done();
            }
            break;
        }

        case WizardStep::VaultConfirmReset:
            if (trim_lower(input) == "y" || trim_lower(input) == "yes") {
                vault::reset();
                clear_all_saved_secret_flags();
                append_text("All saved passwords erased. Profiles and networks are kept;\n"
                            "their passwords will be asked for again.\n");
            } else {
                append_text("Nothing erased.\n");
            }
            wizard_done();
            break;

        default:
            return false;
    }
    return true;
}
