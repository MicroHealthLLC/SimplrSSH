/*
 * Vault Menu
 * Saved passwords are encrypted on the device and unlock automatically, so WiFi
 * reconnects with no prompts. An optional master PIN adds protection: then the
 * PIN is asked once per boot the first time a saved password is needed.
 * 'vault' command: status, pin (add/change), nopin, lock, unlock, reset.
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

// Runs `then` once saved passwords are readable, asking for the PIN only if one is set
void SSHTerminal::with_vault(std::function<void()> then)
{
    if (!vault::is_available()) {
        append_text("ERROR: Settings storage is unavailable; passwords can't be saved.\n");
        wizard_done();
        return;
    }
    if (vault::is_unlocked()) {
        then();
        return;
    }
    wizard.after_unlock = then;
    wizard.pin_attempts = 0;
    wizard_goto(WizardStep::VaultUnlock);
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

    if (!vault::is_available()) {
        append_text("ERROR: Settings storage is unavailable.\n");
        return;
    }

    if (sub.empty() || sub == "status") {
        std::string n = std::to_string(vault::count());
        if (!vault::has_pin()) {
            append_text(("Saved passwords: " + n + ", encrypted on this device and unlocked\n"
                         "automatically (no PIN). For extra protection: vault pin\n").c_str());
        } else {
            append_text(("Saved passwords: " + n + ", protected by your PIN (" +
                         (vault::is_unlocked() ? "unlocked" : "locked") + ").\n").c_str());
            append_text("  vault pin (change) | vault nopin | vault lock | vault unlock\n");
        }
        append_text("  vault reset - erase all saved passwords\n");
    } else if (sub == "pin") {
        wizard.action = vault::has_pin() ? WizardAction::ChangePin : WizardAction::SetPin;
        wizard_goto(vault::has_pin() ? WizardStep::VaultOldPin : WizardStep::VaultNewPin);
    } else if (sub == "nopin") {
        if (!vault::has_pin()) {
            append_text("No PIN is set; saved passwords already unlock automatically.\n");
        } else {
            wizard.action = WizardAction::RemovePin;
            wizard_goto(WizardStep::VaultOldPin);
        }
    } else if (sub == "lock") {
        if (!vault::has_pin()) {
            append_text("No PIN is set, so there is nothing to lock. Add one with: vault pin\n");
        } else {
            vault::lock();
            append_text("Locked. Saved passwords need the PIN again.\n");
        }
    } else if (sub == "unlock") {
        if (vault::is_unlocked()) {
            append_text("Saved passwords are already unlocked.\n");
        } else {
            with_vault([this]() {
                wizard_reset();
                append_text("Unlocked.\n");
            });
        }
    } else if (sub == "reset") {
        wizard_goto(WizardStep::VaultConfirmReset);
    } else {
        append_text("Usage: vault [status|pin|nopin|lock|unlock|reset]\n");
    }
}

bool SSHTerminal::vault_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::VaultUnlock:
            append_text("PIN to unlock saved passwords (Enter = skip): ");
            return true;
        case WizardStep::VaultOldPin:
            append_text("Current PIN: ");
            return true;
        case WizardStep::VaultNewPin:
            append_text("New PIN (min 4 chars, Enter = cancel): ");
            return true;
        case WizardStep::VaultConfirmPin:
            append_text("Confirm PIN: ");
            return true;
        case WizardStep::VaultConfirmReset:
            append_text("Erase ALL saved passwords? (y/n) [n]: ");
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
                append_text("Skipped - saved passwords stay locked.\n");
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
                                                       : "ERROR: Saved passwords are unreadable.\n");
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
            if (wizard.action == WizardAction::RemovePin) {
                esp_err_t err = vault::remove_pin(raw_input);
                if (err == ESP_OK) {
                    append_text("PIN removed. Saved passwords now unlock automatically.\n");
                    wizard_done();
                } else if (err == ESP_ERR_INVALID_ARG && ++wizard.pin_attempts < MAX_PIN_ATTEMPTS) {
                    append_text("Wrong PIN.\n");
                    wizard_prompt();
                } else {
                    append_text("Wrong PIN - giving up.\n");
                    wizard_done();
                }
                break;
            }
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
            esp_err_t err = wizard.action == WizardAction::ChangePin ? vault::change_pin(wizard.secret, wizard.pin)
                                                                     : vault::set_pin(wizard.pin);
            if (err == ESP_OK) {
                append_text(wizard.action == WizardAction::ChangePin
                            ? "PIN changed.\n"
                            : "PIN set. You'll enter it once after each restart to use saved\n"
                              "passwords. It can't be recovered - 'vault reset' erases them.\n");
            } else {
                append_text("ERROR: Failed to set the PIN.\n");
            }
            wizard_done();
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
