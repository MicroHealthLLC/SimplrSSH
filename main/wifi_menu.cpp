/*
 * WiFi Menu
 * Menu-driven WiFi setup: scan for networks, pick one from a numbered list,
 * type the password (masked), and optionally save it (encrypted) so the device
 * auto-connects at boot to the strongest saved network in range.
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "esp_log.h"
#include <cctype>
#include <cstdlib>
#include <algorithm>

static const char *TAG = "WIFI_MENU";

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) {
        return "";
    }
    return s.substr(start, s.find_last_not_of(' ') - start + 1);
}

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static bool is_yes(const std::string& input, bool default_yes)
{
    std::string a = to_lower(input);
    if (a.empty()) {
        return default_yes;
    }
    return a == "y" || a == "yes";
}

static int find_ssid(const std::vector<SavedNetwork>& networks, const std::string& ssid)
{
    for (size_t i = 0; i < networks.size(); i++) {
        if (networks[i].ssid == ssid) {
            return i;
        }
    }
    return -1;
}

static const char* signal_bars(int rssi)
{
    if (rssi >= -55) return "####";
    if (rssi >= -67) return "###";
    if (rssi >= -75) return "##";
    return "#";
}

void SSHTerminal::handle_wifi_command(const std::string& command)
{
    size_t space = command.find(' ');
    std::string rest = space == std::string::npos ? "" : trim(command.substr(space));
    size_t sub_end = rest.find(' ');
    std::string sub = to_lower(rest.substr(0, sub_end));
    std::string ref = sub_end == std::string::npos ? "" : trim(rest.substr(sub_end + 1));

    wizard_reset();

    if (sub.empty() || sub == "menu") {
        wizard.menu = WizardStep::WifiMenu;
        wizard_goto(WizardStep::WifiMenu);
    } else if (sub == "scan") {
        wifi_scan_and_pick();
    } else if (sub == "list" || sub == "ls") {
        list_saved_networks();
    } else if (sub == "saved") {
        if (saved_networks.empty()) {
            append_text("No saved networks. Use 'wifi scan' to add one.\n");
        } else {
            wizard.action = WizardAction::WifiConnectSaved;
            wizard_goto(WizardStep::WifiPickSaved);
        }
    } else if (sub == "forget") {
        if (saved_networks.empty()) {
            append_text("No saved networks.\n");
        } else if (!ref.empty()) {
            int index = find_saved_network(ref);
            if (index < 0) {
                append_text("Saved network not found.\n");
                list_saved_networks();
            } else {
                wizard.action = WizardAction::WifiForget;
                wizard.wifi_ssid = saved_networks[index].ssid;
                wizard_goto(WizardStep::WifiConfirmForget);
            }
        } else {
            wizard.action = WizardAction::WifiForget;
            wizard_goto(WizardStep::WifiPickSaved);
        }
    } else if (sub == "off") {
        disconnect_wifi();
    } else {
        append_text("Usage: wifi [scan|saved|list|forget|off]\n");
        append_text("  'wifi' alone opens the menu\n");
    }
}

void SSHTerminal::list_saved_networks()
{
    if (saved_networks.empty()) {
        append_text("No saved networks. Use 'wifi scan' to add one.\n");
        return;
    }
    append_text("Saved networks:\n");
    for (size_t i = 0; i < saved_networks.size(); i++) {
        const SavedNetwork& n = saved_networks[i];
        std::string line = " " + std::to_string(i + 1) + ") " + n.ssid +
                           (n.secret_saved ? "  (password saved)" : "  (no password saved)");
        if (wifi_connected && n.ssid == wifi_ssid) {
            line += " - connected";
        }
        append_text((line + "\n").c_str());
    }
}

int SSHTerminal::find_saved_network(const std::string& ref)
{
    int exact = find_ssid(saved_networks, ref);
    if (exact >= 0) {
        return exact;
    }
    if (!ref.empty() && std::all_of(ref.begin(), ref.end(), ::isdigit)) {
        int n = std::atoi(ref.c_str());
        if (n >= 1 && n <= (int)saved_networks.size()) {
            return n - 1;
        }
    }
    for (size_t i = 0; i < saved_networks.size(); i++) {
        if (to_lower(saved_networks[i].ssid) == to_lower(ref)) {
            return i;
        }
    }
    return -1;
}

void SSHTerminal::wifi_scan_and_pick()
{
    if (ssh_connected) {
        append_text("Disconnect SSH first ('exit') before changing WiFi.\n");
        wizard_done();
        return;
    }

    append_text("Scanning for networks...\n");
    refresh_display_now();

    std::vector<WifiScanResult> results;
    if (scan_wifi(results) != ESP_OK) {
        append_text("ERROR: WiFi scan failed.\n");
        wizard_done();
        return;
    }
    wizard.scan = results;
    wizard_goto(WizardStep::WifiPickScan);
}

void SSHTerminal::wifi_choose_network(const std::string& ssid, bool open)
{
    if (ssh_connected) {
        append_text("Disconnect SSH first ('exit') before changing WiFi.\n");
        wizard_done();
        return;
    }

    wizard.wifi_ssid = ssid;
    wizard.wifi_open = open;

    int saved = find_ssid(saved_networks, ssid);
    if (saved >= 0 && saved_networks[saved].secret_saved) {
        with_vault([this]() {
            int i = find_ssid(saved_networks, wizard.wifi_ssid);
            std::string password;
            if (i >= 0 && vault::get(saved_networks[i].secret_id(), password) == ESP_OK) {
                wifi_connect_with(password, true);
                vault::wipe(password);
            } else {
                append_text("Saved password could not be read.\n");
                wizard_goto(WizardStep::WifiPassword);
            }
        });
    } else if (open) {
        wifi_connect_with("", false);
    } else {
        wizard_goto(WizardStep::WifiPassword);
    }
}

void SSHTerminal::wifi_connect_with(const std::string& password, bool from_vault)
{
    const std::string ssid = wizard.wifi_ssid;
    append_text(("Connecting to " + ssid + "...\n").c_str());
    refresh_display_now();

    if (init_wifi(ssid.c_str(), password.c_str()) != ESP_OK) {
        append_text(("Could not connect to " + ssid + ".\n").c_str());
        wizard_goto(WizardStep::WifiPassword);
        return;
    }

    append_text(("Connected to " + ssid + ".\n").c_str());

    int saved = find_ssid(saved_networks, ssid);
    bool up_to_date = saved >= 0 && (from_vault || (password.empty() && !saved_networks[saved].secret_saved));
    if (up_to_date) {
        wizard_reset();
        return;
    }

    vault::wipe(wizard.secret);
    wizard.secret = password;
    wizard_goto(WizardStep::WifiSave);
}

void SSHTerminal::wifi_remember_network()
{
    int existing = find_ssid(saved_networks, wizard.wifi_ssid);
    if (existing < 0 && saved_networks.size() >= NETWORK_MAX_COUNT) {
        append_text("Saved network limit reached. Use 'wifi forget' first.\n");
        wizard_reset();
        return;
    }

    auto store = [this]() {
        SavedNetwork n;
        n.ssid = wizard.wifi_ssid;
        if (wizard.secret.empty()) {
            vault::remove(n.secret_id());
        } else if (vault::put(n.secret_id(), wizard.secret) == ESP_OK) {
            n.secret_saved = true;
        } else {
            append_text("ERROR: Could not store the password.\n");
            wizard_reset();
            return;
        }

        int i = find_ssid(saved_networks, n.ssid);
        if (i >= 0) {
            saved_networks[i] = n;
        } else {
            saved_networks.push_back(n);
        }
        if (network_store::save(saved_networks) == ESP_OK) {
            append_text(("Saved " + n.ssid + (n.secret_saved ? " (password encrypted).\n" : ".\n")).c_str());
        } else {
            append_text("ERROR: Failed to save networks to flash.\n");
        }
        wizard_reset();
    };

    if (wizard.secret.empty()) {
        store();
    } else {
        with_vault(store);
    }
}

// Joins the strongest saved network in range; called once after boot
void SSHTerminal::wifi_auto_connect()
{
    if (saved_networks.empty() || wifi_connected) {
        return;
    }

    append_text("Looking for saved WiFi networks...\n");
    refresh_display_now();

    std::vector<WifiScanResult> results;
    if (scan_wifi(results) != ESP_OK) {
        append_text("WiFi scan failed. Type 'wifi' to connect.\n");
        return;
    }

    // Scan results are strongest first
    std::vector<WifiScanResult> candidates;
    bool needs_pin = false;
    for (const auto& r : results) {
        int i = find_ssid(saved_networks, r.ssid);
        if (i >= 0) {
            candidates.push_back(r);
            needs_pin = needs_pin || saved_networks[i].secret_saved;
        }
    }
    if (candidates.empty()) {
        append_text("No saved WiFi network in range. Type 'wifi' to choose one.\n");
        return;
    }

    wizard_reset();
    wizard.scan = candidates;

    auto attempt = [this]() {
        std::vector<WifiScanResult> in_range = wizard.scan;
        wizard_reset();
        for (const auto& c : in_range) {
            int i = find_ssid(saved_networks, c.ssid);
            std::string password;
            if (i < 0 || (!saved_networks[i].secret_saved && !c.open)) {
                continue;  // Locked network without a saved password
            }
            if (saved_networks[i].secret_saved &&
                (!vault::is_unlocked() || vault::get(saved_networks[i].secret_id(), password) != ESP_OK)) {
                continue;
            }
            append_text(("Auto-connecting to " + c.ssid + "...\n").c_str());
            refresh_display_now();
            esp_err_t err = init_wifi(c.ssid.c_str(), password.c_str());
            vault::wipe(password);
            if (err == ESP_OK) {
                append_text(("Connected to " + c.ssid + ". Type 'profile' to connect SSH.\n").c_str());
                return;
            }
            ESP_LOGW(TAG, "Auto-connect to %s failed", c.ssid.c_str());
        }
        append_text("Auto-connect failed. Type 'wifi' to choose a network.\n");
    };

    if (needs_pin && !vault::is_unlocked() && vault::is_set_up()) {
        append_text("Enter your PIN to auto-connect to saved WiFi.\n");
        with_vault(attempt);
    } else {
        attempt();
    }
}

void SSHTerminal::run_startup_tasks()
{
    wifi_auto_connect();
}

bool SSHTerminal::wifi_step_prompt()
{
    std::string text;

    switch (wizard.step) {
        case WizardStep::WifiMenu:
            append_text("\n== WiFi ==\n");
            append_text(wifi_connected ? ("Connected: " + wifi_ssid + "\n").c_str() : "Not connected\n");
            append_text(" 1) Scan and connect\n"
                        " 2) Connect to saved network\n"
                        " 3) Forget saved network\n"
                        " 4) Disconnect\n"
                        " 0) Exit menu\n");
            text = "Select [0-4]: ";
            wizard.choices = {"1", "2", "3", "4", "0"};
            break;

        case WizardStep::WifiPickScan: {
            const size_t n = wizard.scan.size();
            if (n == 0) {
                append_text("No networks found.\n");
            } else {
                append_text("Networks (strongest first):\n");
            }
            for (size_t i = 0; i < n; i++) {
                const WifiScanResult& r = wizard.scan[i];
                std::string line = " " + std::to_string(i + 1) + ") " + r.ssid + "  " + signal_bars(r.rssi);
                if (r.open) {
                    line += " open";
                }
                if (find_ssid(saved_networks, r.ssid) >= 0) {
                    line += " saved";
                }
                if (wifi_connected && r.ssid == wifi_ssid) {
                    line += " - connected";
                }
                append_text((line + "\n").c_str());
            }
            append_text((" " + std::to_string(n + 1) + ") Hidden network...\n").c_str());
            append_text((" " + std::to_string(n + 2) + ") Scan again\n").c_str());
            append_text(" 0) Back\n");
            text = "Select [1-" + std::to_string(n + 2) + ", 0=back]: ";
            for (size_t i = 1; i <= n + 2; i++) {
                wizard.choices.push_back(std::to_string(i));
            }
            wizard.choices.push_back("0");
            break;
        }

        case WizardStep::WifiHiddenSsid:
            text = "Network name (SSID, Enter = back): ";
            break;

        case WizardStep::WifiPassword:
            text = "Password for " + wizard.wifi_ssid +
                   (wizard.wifi_hidden ? " (Enter = open network): " : " (Enter = back): ");
            break;

        case WizardStep::WifiSave:
            text = find_ssid(saved_networks, wizard.wifi_ssid) >= 0
                 ? "Update saved password for " + wizard.wifi_ssid + "? (y/n) [y]: "
                 : "Save " + wizard.wifi_ssid + " so it auto-connects? (y/n) [y]: ";
            wizard.choices = {"y", "n"};
            break;

        case WizardStep::WifiPickSaved:
            list_saved_networks();
            text = std::string(wizard.action == WizardAction::WifiForget ? "Network to forget" : "Network to connect") +
                   " [1-" + std::to_string(saved_networks.size()) + ", 0=back]: ";
            for (size_t i = 1; i <= saved_networks.size(); i++) {
                wizard.choices.push_back(std::to_string(i));
            }
            wizard.choices.push_back("0");
            break;

        case WizardStep::WifiConfirmForget:
            text = "Forget " + wizard.wifi_ssid + " and its saved password? (y/n) [n]: ";
            wizard.choices = {"n", "y"};
            break;

        default:
            return false;
    }

    append_text(text.c_str());
    return true;
}

bool SSHTerminal::wifi_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::WifiMenu:
            if (input == "1") {
                wifi_scan_and_pick();
            } else if (input == "2" || input == "3") {
                if (saved_networks.empty()) {
                    append_text("No saved networks. Use 'Scan and connect' to add one.\n");
                    wizard_prompt();
                } else {
                    wizard.action = input == "2" ? WizardAction::WifiConnectSaved : WizardAction::WifiForget;
                    wizard_goto(WizardStep::WifiPickSaved);
                }
            } else if (input == "4") {
                disconnect_wifi();
                wizard_prompt();
            } else if (input == "0") {
                wizard_reset();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;

        case WizardStep::WifiPickScan: {
            const int n = wizard.scan.size();
            int choice = (!input.empty() && std::all_of(input.begin(), input.end(), ::isdigit))
                       ? std::atoi(input.c_str()) : -1;
            if (input == "0") {
                wizard_done();
            } else if (choice >= 1 && choice <= n) {
                WifiScanResult r = wizard.scan[choice - 1];
                wizard.wifi_hidden = false;
                wifi_choose_network(r.ssid, r.open);
            } else if (choice == n + 1) {
                wizard_goto(WizardStep::WifiHiddenSsid);
            } else if (choice == n + 2) {
                wifi_scan_and_pick();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            break;
        }

        case WizardStep::WifiHiddenSsid:
            if (raw_input.empty()) {
                wizard_goto(WizardStep::WifiPickScan);
            } else if (raw_input.size() > 32) {
                append_text("SSID is at most 32 characters.\n");
                wizard_prompt();
            } else {
                wizard.wifi_hidden = true;
                wifi_choose_network(raw_input, false);
            }
            break;

        case WizardStep::WifiPassword:
            if (raw_input.empty() && !wizard.wifi_hidden) {
                wizard_done();
            } else if (raw_input.size() > 63) {
                append_text("WiFi passwords are at most 63 characters.\n");
                wizard_prompt();
            } else {
                std::string password = raw_input;
                wifi_connect_with(password, false);
                vault::wipe(password);
            }
            break;

        case WizardStep::WifiSave:
            if (is_yes(input, true)) {
                wifi_remember_network();
            } else {
                wizard_reset();
            }
            break;

        case WizardStep::WifiPickSaved: {
            if (input == "0") {
                wizard_done();
                break;
            }
            bool numeric = !input.empty() && std::all_of(input.begin(), input.end(), ::isdigit);
            int n = numeric ? std::atoi(input.c_str()) : 0;
            if (n < 1 || n > (int)saved_networks.size()) {
                append_text("Invalid choice.\n");
                wizard_prompt();
                break;
            }
            const SavedNetwork net = saved_networks[n - 1];
            if (wizard.action == WizardAction::WifiForget) {
                wizard.wifi_ssid = net.ssid;
                wizard_goto(WizardStep::WifiConfirmForget);
            } else {
                wizard.wifi_hidden = false;
                wifi_choose_network(net.ssid, !net.secret_saved);
            }
            break;
        }

        case WizardStep::WifiConfirmForget:
            if (is_yes(input, false)) {
                int i = find_ssid(saved_networks, wizard.wifi_ssid);
                if (i >= 0) {
                    vault::remove(saved_networks[i].secret_id());
                    saved_networks.erase(saved_networks.begin() + i);
                    network_store::save(saved_networks);
                }
                append_text(("Forgot " + wizard.wifi_ssid + ".\n").c_str());
            } else {
                append_text("Kept.\n");
            }
            wizard_done();
            break;

        default:
            return false;
    }
    return true;
}
