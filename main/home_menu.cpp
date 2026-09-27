/*
 * Home Menu
 * The start screen, and where 'menu' / 'exit' / every "0) Back" lead. Numbers
 * open an app; anything else typed is run as a command.
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"

void SSHTerminal::go_home()
{
    if (chat_active) {
        close_chat();
    }
    wizard_reset();
    if (ssh_connected) {
        append_text("SSH is connected - type 'exit' to close it and go to the menu.\n");
        return;
    }
    wizard.menu = WizardStep::HomeMenu;
    wizard_goto(WizardStep::HomeMenu);
}

// After a command started from a menu: return to that menu when it finishes
void SSHTerminal::show_menu_after(WizardStep menu)
{
    wizard.menu = menu;
    if (!wizard_active()) {
        wizard_goto(menu);
    }
}

void SSHTerminal::background_tick()
{
    if (home_pending) {
        home_pending = false;
        go_home();
    }
    wifi_maintain();
}

bool SSHTerminal::home_step_prompt()
{
    switch (wizard.step) {
        case WizardStep::HomeMenu: {
            std::string status = wifi_connected ? "WiFi: " + wifi_ssid : std::string("WiFi: off");
            status += " | Saved: " + std::to_string(saved_networks.size()) + " WiFi, " +
                      std::to_string(profiles.size()) + " SSH";
            append_text(("\n== PocketSSH ==\n" + status + "\n").c_str());
            append_text(" 1) WiFi\n"
                        " 2) SSH servers\n"
                        " 3) ChatGPT\n"
                        " 4) Storage (device / SD card)\n"
                        " 5) Security (PIN, server keys)\n"
                        "Select 1-5, or type a command: ");
            wizard.choices = {"1", "2", "3", "4", "5"};
            return true;
        }
        case WizardStep::SecurityMenu:
            append_text("\n== Security ==\n");
            append_text(("Saved passwords: " + std::to_string(vault::count()) +
                         (vault::has_pin() ? " (PIN protected)\n" : " (no PIN)\n")).c_str());
            append_text(vault::has_pin() ? " 1) Change PIN\n 2) Remove PIN\n" : " 1) Set a PIN\n");
            append_text(" 3) Trusted servers\n"
                        " 4) Erase saved passwords\n"
                        " 0) Back\n"
                        "Select: ");
            wizard.choices = {"1", "2", "3", "4", "0"};
            return true;
        default:
            return false;
    }
}

bool SSHTerminal::home_step_input(const std::string& raw_input, const std::string& input)
{
    switch (wizard.step) {
        case WizardStep::HomeMenu:
            if (input == "1") {
                handle_wifi_command("wifi");
            } else if (input == "2") {
                handle_profile_command("profile");
            } else if (input == "3") {
                handle_chat_command("chat settings");
            } else if (input == "4") {
                handle_storage_command("storage");
            } else if (input == "5") {
                wizard_reset();
                wizard.menu = WizardStep::SecurityMenu;
                wizard_goto(WizardStep::SecurityMenu);
            } else if (input.empty()) {
                wizard_prompt();
            } else {
                wizard_reset();
                execute_command(raw_input);
            }
            return true;

        case WizardStep::SecurityMenu:
            if (input == "1") {
                handle_vault_command("vault pin");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "2" && vault::has_pin()) {
                handle_vault_command("vault nopin");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "3") {
                handle_hosts_command("hosts");
                append_text("  (remove one with: hosts forget <number>)\n");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "4") {
                handle_vault_command("vault reset");
                show_menu_after(WizardStep::SecurityMenu);
            } else if (input == "0") {
                go_home();
            } else {
                append_text("Invalid choice.\n");
                wizard_prompt();
            }
            return true;

        default:
            return false;
    }
}
