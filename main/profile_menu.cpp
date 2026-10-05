/*
 * Connection Profile Menu
 * Menu-driven, line-based wizard for listing, adding, editing, deleting and
 * connecting with saved SSH profiles. Prompts are printed into the terminal and
 * answered on the normal input line; numbered menus can also be cycled by rolling the
 * trackball left/right (Tab5 keyboard: Up/Down) and confirmed with a trackball press or Enter.
 *
 * This file also holds the wizard plumbing shared with the WiFi menu
 * (wifi_menu.cpp) and the master PIN prompts (vault_menu.cpp).
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "esp_log.h"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <algorithm>

static const char *TAG = "PROFILE_MENU";

static const size_t MAX_NAME_LEN = 32;
static const size_t MAX_FIELD_LEN = 64;

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
}

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool is_number(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
}

static std::vector<std::string> numbered_choices(int count, bool with_zero)
{
    std::vector<std::string> choices;
    for (int i = 1; i <= count; i++) {
        choices.push_back(std::to_string(i));
    }
    if (with_zero) {
        choices.push_back("0");
    }
    return choices;
}

// ---------------------------------------------------------------------------
// Shared wizard plumbing
// ---------------------------------------------------------------------------

bool SSHTerminal::wizard_input_masked() const
{
    switch (wizard.step) {
        case WizardStep::Password:
        case WizardStep::KeyPassphrase:
        case WizardStep::ConnectSecret:
        case WizardStep::WifiPassword:
        case WizardStep::VaultUnlock:
        case WizardStep::VaultOldPin:
        case WizardStep::VaultNewPin:
        case WizardStep::VaultConfirmPin:
        case WizardStep::ChatKey:
            return true;
        default:
            return false;
    }
}

std::string SSHTerminal::input_display_text() const
{
    if (wizard_input_masked()) {
        return std::string(current_input.length(), '*');
    }
    return current_input;
}

void SSHTerminal::wizard_goto(WizardStep step)
{
    wizard.step = step;
    wizard.choices.clear();
    wizard_prompt();
}

void SSHTerminal::wizard_prompt()
{
    if (profile_step_prompt() || wifi_step_prompt() || vault_step_prompt() || host_step_prompt() ||
        storage_step_prompt() || home_step_prompt() || chat_step_prompt()) {
        return;
    }
}

void SSHTerminal::wizard_handle_input(const std::string& raw_input)
{
    // Echo the answer on the prompt line
    append_text(wizard_input_masked() ? std::string(raw_input.length(), '*').c_str() : raw_input.c_str());
    append_text("\n");

    const std::string input = trim(raw_input);

    if (!wizard_input_masked() && to_lower(input) == "cancel") {
        append_text("Cancelled.\n");
        if (wizard.step == WizardStep::HostTrust) {
            ssh_teardown();  // Close the half-open connection waiting for trust
        }
        go_home();
        return;
    }

    if (profile_step_input(raw_input, input) || wifi_step_input(raw_input, input) ||
        vault_step_input(raw_input, input) || host_step_input(raw_input, input) ||
        storage_step_input(raw_input, input) || home_step_input(raw_input, input) ||
        chat_step_input(raw_input, input)) {
        return;
    }
}

void SSHTerminal::wizard_cycle_choice(int direction)
{
    if (wizard.choices.empty()) {
        return;
    }

    auto it = std::find(wizard.choices.begin(), wizard.choices.end(), current_input);
    int count = wizard.choices.size();
    int pos;
    if (it == wizard.choices.end()) {
        pos = direction > 0 ? count - 1 : 0;
    } else {
        // Trackball left (direction > 0) moves to the previous choice, right to the next
        pos = (int)(it - wizard.choices.begin()) + (direction > 0 ? -1 : 1);
        pos = (pos + count) % count;
    }

    current_input = wizard.choices[pos];
    cursor_pos = current_input.length();
    update_input_display();
}

// Finish the current action: back to the menu it was started from, or home
void SSHTerminal::wizard_done()
{
    WizardStep menu = wizard.menu;
    wizard_reset();
    if (menu != WizardStep::None) {
        wizard.menu = menu;
        wizard_goto(menu);
    } else {
        go_home();
    }
}

void SSHTerminal::wizard_reset()
{
    vault::wipe(wizard.secret);
    vault::wipe(wizard.pin);
    wizard = ProfileWizard();
}

// ---------------------------------------------------------------------------
// Connection profiles
// ---------------------------------------------------------------------------

bool SSHTerminal::key_is_encrypted(const std::string& key_name)
{
    const char* key = get_loaded_key(key_name.c_str(), NULL);
    return key && strstr(key, "ENCRYPTED") != NULL;
}

std::string SSHTerminal::profile_summary(const ConnectionProfile& p, bool secret_saved)
{
    std::string s = p.username + "@" + p.host;
    if (p.port != 22) {
        s += ":" + std::to_string(p.port);
    }
    if (p.auth == ConnectionProfile::Auth::Key) {
        s += " [key " + p.key_name;
        if (key_is_encrypted(p.key_name)) {
            s += secret_saved ? ", pass saved" : ", pass: ask";
        }
        s += "]";
    } else {
        s += secret_saved ? " [pw: saved]" : " [pw: ask]";
    }
    return s;
}

void SSHTerminal::handle_profile_command(const std::string& command)
{
    // command is "profile[s] [sub] [ref...]"
    size_t first_space = command.find(' ');
    std::string rest = first_space == std::string::npos ? "" : trim(command.substr(first_space));
    size_t space = rest.find(' ');
    std::string sub = to_lower(rest.substr(0, space));
    std::string ref = space == std::string::npos ? "" : trim(rest.substr(space + 1));

    wizard_reset();

    if (sub.empty() || sub == "menu") {
        wizard.menu = WizardStep::MainMenu;
        wizard_goto(WizardStep::MainMenu);
    } else if (sub == "list" || sub == "ls") {
        list_profiles();
    } else if (sub == "add" || sub == "new") {
        begin_profile_action(WizardAction::Add, ref);
    } else if (sub == "edit") {
        begin_profile_action(WizardAction::Edit, ref);
    } else if (sub == "delete" || sub == "del" || sub == "rm") {
        begin_profile_action(WizardAction::Delete, ref);
    } else if (sub == "connect" || sub == "go") {
        begin_profile_action(WizardAction::Connect, ref);
    } else {
        append_text("Usage: profile [list|add|edit|delete|connect] [NAME|#]\n");
        append_text("  'profile' alone opens the menu\n");
    }
}

void SSHTerminal::list_profiles()
{
    if (profiles.empty()) {
        append_text("No saved profiles. Use 'profile add' to create one.\n");
        return;
    }

    append_text("Saved profiles:\n");
    for (size_t i = 0; i < profiles.size(); i++) {
        std::string line = " " + std::to_string(i + 1) + ") " + profiles[i].name +
                           "  " + profile_summary(profiles[i], profiles[i].secret_saved) + "\n";
        append_text(line.c_str());
    }
}

int SSHTerminal::find_profile(const std::string& ref)
{
    if (is_number(ref)) {
        int n = std::atoi(ref.c_str());
        return (n >= 1 && n <= (int)profiles.size()) ? n - 1 : -1;
    }

    std::string wanted = to_lower(ref);
    for (size_t i = 0; i < profiles.size(); i++) {
        if (to_lower(profiles[i].name) == wanted) {
            return i;
        }
    }
    return -1;
}

void SSHTerminal::begin_profile_action(WizardAction action, const std::string& ref)
{
    wizard.action = action;

    if (action == WizardAction::Add) {
        if (profiles.size() >= PROFILE_MAX_COUNT) {
            append_text("Profile limit reached. Delete a profile first.\n");
            wizard_done();
            return;
        }
        wizard.draft = ConnectionProfile();
        if (!loaded_keys.empty()) {
            wizard.draft.auth = ConnectionProfile::Auth::Key;
        }
        wizard.index = -1;
        wizard.editing = false;
        append_text("-- New profile ('cancel' or Esc to abort) --\n");
        wizard_goto(WizardStep::Name);
        return;
    }

    if (profiles.empty()) {
        append_text("No saved profiles. Use 'profile add' to create one.\n");
        wizard_done();
        return;
    }

    if (!ref.empty()) {
        int index = find_profile(ref);
        if (index < 0) {
            append_text("Profile not found: ");
            append_text(ref.c_str());
            append_text("\n");
            list_profiles();
            wizard_done();
            return;
        }
        on_profile_picked(index);
        return;
    }

    wizard_goto(WizardStep::PickProfile);
}

void SSHTerminal::on_profile_picked(int index)
{
    wizard.index = index;
    const ConnectionProfile& p = profiles[index];

    switch (wizard.action) {
        case WizardAction::Connect:
            start_profile_connect(index);
            break;
        case WizardAction::Edit:
            wizard.draft = p;
            wizard.editing = true;
            wizard.secret_change = SecretChange::Keep;
            append_text("-- Editing '");
            append_text(p.name.c_str());
            append_text("' (Enter keeps [current]) --\n");
            wizard_goto(WizardStep::Name);
            break;
        case WizardAction::Delete:
            wizard_goto(WizardStep::ConfirmDelete);
            break;
        default:
            wizard_done();
            break;
    }
}

bool SSHTerminal::profile_step_prompt()
{
    const ConnectionProfile& d = wizard.draft;
    const bool can_keep_secret = d.secret_saved && wizard.secret_change == SecretChange::Keep;
    std::string text;

    switch (wizard.step) {
        case WizardStep::MainMenu:
            append_text("\n== Connection Profiles ==\n"
                        " 1) Connect\n"
                        " 2) List\n"
                        " 3) Add\n"
                        " 4) Edit\n"
                        " 5) Delete\n"
                        " 0) Back\n");
            text = "Select [0-5]: ";
            wizard.choices = numbered_choices(5, true);
            break;

        case WizardStep::PickProfile: {
            const char* verb = wizard.action == WizardAction::Connect ? "connect to"
                             : wizard.action == WizardAction::Edit ? "edit" : "delete";
            list_profiles();
            text = "Profile to " + std::string(verb) + " [1-" + std::to_string(profiles.size()) +
                   ", 0=back]: ";
            wizard.choices = numbered_choices(profiles.size(), true);
            break;
        }

        case WizardStep::Name:
            text = wizard.editing ? "Name [" + d.name + "]: " : "Name: ";
            break;

        case WizardStep::Host:
            text = d.host.empty() ? "Host/IP: " : "Host/IP [" + d.host + "]: ";
            break;

        case WizardStep::Port:
            text = "Port [" + std::to_string(d.port) + "]: ";
            break;

        case WizardStep::User:
            text = d.username.empty() ? "Username: " : "Username [" + d.username + "]: ";
            break;

        case WizardStep::AuthMethod:
            append_text("Authentication:\n"
                        " 1) SSH key from SD card\n"
                        " 2) Password\n");
            text = std::string("Select [") + (d.auth == ConnectionProfile::Auth::Key ? "1" : "2") + "]: ";
            wizard.choices = numbered_choices(2, false);
            break;

        case WizardStep::PickKey: {
            auto keys = get_loaded_key_names();
            append_text("SSH keys on SD card:\n");
            int current = 0;
            for (size_t i = 0; i < keys.size(); i++) {
                std::string line = " " + std::to_string(i + 1) + ") " + keys[i] +
                                   (key_is_encrypted(keys[i]) ? " (passphrase)" : "") + "\n";
                append_text(line.c_str());
                if (keys[i] == to_lower(d.key_name)) {
                    current = i + 1;
                }
            }
            text = "Select key [" + (current ? std::to_string(current) : "1-" + std::to_string(keys.size())) +
                   ", 0=back]: ";
            wizard.choices = numbered_choices(keys.size(), true);
            break;
        }

        case WizardStep::Password:
            text = can_keep_secret ? "Password (Enter = keep saved, - = ask each connect): "
                                   : "Password (Enter = ask each connect): ";
            break;

        case WizardStep::KeyPassphrase:
            text = can_keep_secret ? "Key passphrase (Enter = keep saved, - = ask each connect): "
                                   : "Key passphrase (Enter = ask each connect): ";
            break;

        case WizardStep::Review: {
            bool saved = wizard.secret_change == SecretChange::Set || can_keep_secret;
            append_text("-- Review --\n");
            append_text((" Name: " + d.name + "\n").c_str());
            append_text((" " + profile_summary(d, saved) + "\n").c_str());
            if (wizard.secret_change == SecretChange::Set) {
                append_text(" (password will be stored encrypted)\n");
            }
            append_text(" 1) Save\n"
                        " 2) Save and connect\n"
                        " 3) Change something\n"
                        " 0) Discard\n");
            text = "Select [1]: ";
            wizard.choices = {"1", "2", "3", "0"};
            break;
        }

        case WizardStep::ConfirmDelete:
            text = "Delete '" + profiles[wizard.index].name + "'? (y/n) [n]: ";
            wizard.choices = {"n", "y"};
            break;

        case WizardStep::ConnectSecret: {
            const ConnectionProfile& p = profiles[wizard.index];
            text = p.auth == ConnectionProfile::Auth::Key
                 ? "Passphrase for " + p.key_name + ": "
                 : "Password for " + p.username + "@" + p.host + ": ";
            break;
        }

        default:
            return false;
    }

    append_text(text.c_str());
    return true;
}

bool SSHTerminal::profile_step_input(const std::string& raw_input, const std::string& input)
{
    ConnectionProfile& d = wizard.draft;
    const bool can_keep_secret = d.secret_saved && wizard.secret_change == SecretChange::Keep;

    switch (wizard.step) {
        case WizardStep::MainMenu:
            if (input == "1") {
                begin_profile_action(WizardAction::Connect, "");
            } else if (input == "2") {
                list_profiles();
                wizard_prompt();
            } else if (input == "3") {
                begin_profile_action(WizardAction::Add, "");
            } else if (input == "4") {
                begin_profile_action(WizardAction::Edit, "");
            } else if (input == "5") {
                begin_profile_action(WizardAction::Delete, "");
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;

        case WizardStep::PickProfile: {
            if (input == "0") {
                wizard_done();
                break;
            }
            int index = input.empty() ? -1 : find_profile(input);
            if (index < 0) {
                append_text("Invalid choice.\n");
                wizard_prompt();
            } else {
                on_profile_picked(index);
            }
            break;
        }

        case WizardStep::Name: {
            if (input.empty() && wizard.editing) {
                wizard_goto(WizardStep::Host);
                break;
            }
            const char* error = NULL;
            if (input.empty()) {
                error = "Name is required.\n";
            } else if (is_number(input)) {
                error = "Name can't be only digits (numbers select from lists).\n";
            } else if (input.length() > MAX_NAME_LEN) {
                error = "Name is too long (max 32).\n";
            } else {
                int existing = find_profile(input);
                if (existing >= 0 && existing != wizard.index) {
                    error = "A profile with that name already exists.\n";
                }
            }
            if (error) {
                append_text(error);
                wizard_prompt();
            } else {
                d.name = input;
                wizard_goto(WizardStep::Host);
            }
            break;
        }

        case WizardStep::Host:
            if (input.empty() && !d.host.empty()) {
                wizard_goto(WizardStep::Port);
            } else if (input.empty() || input.find(' ') != std::string::npos || input.length() > MAX_FIELD_LEN) {
                append_text("Enter a host name or IP address.\n");
                wizard_prompt();
            } else {
                d.host = input;
                wizard_goto(WizardStep::Port);
            }
            break;

        case WizardStep::Port:
            if (input.empty()) {
                wizard_goto(WizardStep::User);
            } else if (!is_number(input) || input.length() > 5 ||
                       std::atoi(input.c_str()) < 1 || std::atoi(input.c_str()) > 65535) {
                append_text("Port must be 1-65535.\n");
                wizard_prompt();
            } else {
                d.port = std::atoi(input.c_str());
                wizard_goto(WizardStep::User);
            }
            break;

        case WizardStep::User:
            if (input.empty() && !d.username.empty()) {
                wizard_goto(WizardStep::AuthMethod);
            } else if (input.empty() || input.find(' ') != std::string::npos || input.length() > MAX_FIELD_LEN) {
                append_text("Enter a username.\n");
                wizard_prompt();
            } else {
                d.username = input;
                wizard_goto(WizardStep::AuthMethod);
            }
            break;

        case WizardStep::AuthMethod: {
            std::string choice = input;
            if (choice.empty()) {
                choice = d.auth == ConnectionProfile::Auth::Key ? "1" : "2";
            }
            ConnectionProfile::Auth chosen;
            if (choice == "1") {
                if (loaded_keys.empty()) {
                    append_text("No SSH keys loaded. Put .pem files in /sdcard/ssh_keys/ and reboot.\n");
                    wizard_prompt();
                    break;
                }
                chosen = ConnectionProfile::Auth::Key;
            } else if (choice == "2") {
                chosen = ConnectionProfile::Auth::Password;
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
                break;
            }
            if (chosen != d.auth) {
                // A saved password is not a key passphrase (or vice versa)
                wizard.secret_change = SecretChange::Clear;
                vault::wipe(wizard.secret);
                d.auth = chosen;
            }
            wizard_goto(chosen == ConnectionProfile::Auth::Key ? WizardStep::PickKey : WizardStep::Password);
            break;
        }

        case WizardStep::PickKey: {
            if (input == "0") {
                wizard_goto(WizardStep::AuthMethod);
                break;
            }
            auto keys = get_loaded_key_names();
            std::string selected;
            if (input.empty()) {
                if (get_loaded_key(d.key_name.c_str(), NULL)) {
                    selected = to_lower(d.key_name);
                }
            } else if (is_number(input)) {
                int n = std::atoi(input.c_str());
                if (n >= 1 && n <= (int)keys.size()) {
                    selected = keys[n - 1];
                }
            }
            if (selected.empty()) {
                append_text("Invalid choice.\n");
                wizard_prompt();
                break;
            }
            if (selected != to_lower(d.key_name)) {
                // Different key: a saved passphrase belongs to the old one
                wizard.secret_change = SecretChange::Clear;
                vault::wipe(wizard.secret);
            }
            d.key_name = selected;
            if (key_is_encrypted(selected)) {
                wizard_goto(WizardStep::KeyPassphrase);
            } else {
                wizard.secret_change = SecretChange::Clear;
                wizard_goto(WizardStep::Review);
            }
            break;
        }

        case WizardStep::Password:
        case WizardStep::KeyPassphrase:
            // Not trimmed: spaces can be part of a password
            vault::wipe(wizard.secret);
            if (raw_input == "-") {
                wizard.secret_change = SecretChange::Clear;
            } else if (!raw_input.empty()) {
                wizard.secret_change = SecretChange::Set;
                wizard.secret = raw_input;
            } else if (!can_keep_secret) {
                wizard.secret_change = SecretChange::Clear;
            }
            wizard_goto(WizardStep::Review);
            break;

        case WizardStep::Review:
            if (input.empty() || input == "1") {
                save_profile_draft(false);
            } else if (input == "2") {
                save_profile_draft(true);
            } else if (input == "3") {
                // Walk the steps again with the draft values as defaults
                wizard.editing = true;
                wizard_goto(WizardStep::Name);
            } else if (input == "0") {
                append_text("Discarded.\n");
                wizard_done();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;

        case WizardStep::ConfirmDelete:
            if (to_lower(input) == "y" || to_lower(input) == "yes") {
                ConnectionProfile removed = profiles[wizard.index];
                profiles.erase(profiles.begin() + wizard.index);
                vault::remove(removed.secret_id());
                if (profile_store::save(profiles) == ESP_OK) {
                    append_text(("Deleted '" + removed.name + "'.\n").c_str());
                } else {
                    append_text("ERROR: Failed to save profiles to flash.\n");
                }
            } else {
                append_text("Not deleted.\n");
            }
            wizard_done();
            break;

        case WizardStep::ConnectSecret: {
            ConnectionProfile chosen = profiles[wizard.index];
            std::string secret = raw_input;
            wizard_reset();
            if (secret.empty()) {
                append_text("Cancelled.\n");
            } else {
                connect_profile(chosen, secret);
            }
            vault::wipe(secret);
            break;
        }

        default:
            return false;
    }
    return true;
}

void SSHTerminal::save_profile_draft(bool connect_after)
{
    if (wizard.secret_change == SecretChange::Set) {
        // Storing a password needs the vault unlocked (or created)
        with_vault([this, connect_after]() { commit_profile_draft(connect_after); });
    } else {
        commit_profile_draft(connect_after);
    }
}

void SSHTerminal::commit_profile_draft(bool connect_after)
{
    ConnectionProfile p = wizard.draft;
    if (p.id.empty()) {
        p.id = profile_store::new_id();
    }

    if (wizard.secret_change == SecretChange::Set) {
        if (vault::put(p.secret_id(), wizard.secret) == ESP_OK) {
            p.secret_saved = true;
        } else {
            append_text("ERROR: Could not store the password; it will be asked each connect.\n");
            p.secret_saved = false;
        }
    } else if (wizard.secret_change == SecretChange::Clear) {
        vault::remove(p.secret_id());
        p.secret_saved = false;
    }

    int index = wizard.index;
    if (index < 0) {
        profiles.push_back(p);
        index = profiles.size() - 1;
    } else {
        profiles[index] = p;
    }

    if (profile_store::save(profiles) == ESP_OK) {
        append_text(("Profile '" + p.name + "' saved.\n").c_str());
    } else {
        append_text("ERROR: Failed to save profiles to flash.\n");
    }

    if (connect_after) {
        wizard_reset();
        start_profile_connect(index);
    } else {
        wizard_done();
    }
}

// Connects with a profile, fetching the saved secret or asking for it
void SSHTerminal::start_profile_connect(int index)
{
    const ConnectionProfile p = profiles[index];

    if (ssh_connected) {
        append_text("Already connected. Type 'exit' to disconnect first.\n");
        wizard_reset();
        return;
    }
    if (!wifi_connected) {
        append_text("WiFi not connected. Type 'wifi' to pick a network.\n");
        wizard_reset();
        return;
    }
    if (p.auth == ConnectionProfile::Auth::Key && !get_loaded_key(p.key_name.c_str(), NULL)) {
        append_text(("ERROR: Key '" + p.key_name + "' is not on the SD card.\n").c_str());
        append_text("Use 'profile edit' to pick another key.\n");
        wizard_reset();
        return;
    }

    bool needs_secret = p.auth == ConnectionProfile::Auth::Password || key_is_encrypted(p.key_name);
    if (!needs_secret) {
        wizard_reset();
        connect_profile(p, "");
        return;
    }

    wizard.action = WizardAction::Connect;
    wizard.index = index;
    if (!p.secret_saved) {
        wizard_goto(WizardStep::ConnectSecret);
        return;
    }

    with_vault([this]() {
        ConnectionProfile chosen = profiles[wizard.index];
        std::string secret;
        if (vault::get(chosen.secret_id(), secret) == ESP_OK) {
            wizard_reset();
            connect_profile(chosen, secret);
            vault::wipe(secret);
        } else {
            append_text("Saved password could not be read; enter it now.\n");
            wizard_goto(WizardStep::ConnectSecret);
        }
    });
}

void SSHTerminal::connect_profile(const ConnectionProfile& profile, const std::string& secret)
{
    ESP_LOGI(TAG, "Connecting with profile '%s'", profile.name.c_str());
    append_text(("Profile '" + profile.name + "'\n").c_str());
    refresh_display_now();

    if (profile.auth == ConnectionProfile::Auth::Key) {
        size_t key_len = 0;
        const char* key_data = get_loaded_key(profile.key_name.c_str(), &key_len);
        if (!key_data || key_len == 0) {
            append_text(("ERROR: Key '" + profile.key_name + "' is not on the SD card.\n").c_str());
            return;
        }
        connect_with_key(profile.host.c_str(), profile.port, profile.username.c_str(), key_data, key_len,
                         secret.empty() ? NULL : secret.c_str());
    } else {
        connect(profile.host.c_str(), profile.port, profile.username.c_str(), secret.c_str());
    }
}
