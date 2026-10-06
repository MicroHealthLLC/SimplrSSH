/*
 * Home Menu
 * The start screen, and where 'menu' / 'exit' / every "0) Back" lead. Numbers
 * open an app; anything else typed is run as a command. Also the Security menu and the
 * Power menu (sleep, turn off; carried out by keypad_task, deck_base.cpp).
 */

#include "ssh_terminal.hpp"
#include "secret_vault.hpp"
#include "board.hpp"
#include "esp_timer.h"

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

void SSHTerminal::leave_to_home()
{
    vault::wipe(current_input);   // A half-typed line may be a password
    cursor_pos = 0;
    history_index = -1;
    if (chat_active) {
        close_chat();
    }
    if (session || ssh_socket >= 0) {
        disconnect();             // Also a connection waiting for trust or a passphrase
    }
    append_text("\nBack to the menu.\n");
    go_home();
    update_input_display();
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
    if (!ssh_connected) {
        show_term_view(false);   // The session ended (by the server or the side panel)
    }
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
            append_text(("\n== SimplrSSH ==\n" + status + "\n").c_str());
            append_text(" 1) WiFi\n"
                        " 2) SSH servers\n"
                        " 3) ChatGPT\n"
                        " 4) Storage (device / SD card)\n"
                        " 5) Security (PIN, server keys)\n"
                        " 6) Power (sleep, turn off)\n"
                        "Select 1-6, or type a command: ");
            wizard.choices = {"1", "2", "3", "4", "5", "6"};
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
        case WizardStep::PowerMenu:
            append_text("\n== Power ==\n");
            append_text((std::string(" 1) Sleep (wake: ") + board::HINT_WAKE + ")\n").c_str());
            append_text(" 2) Turn off\n"
                        " 0) Back\n"
                        "Select: ");
            wizard.choices = {"1", "2", "0"};
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
            } else if (input == "6") {
                wizard_reset();
                wizard.menu = WizardStep::PowerMenu;
                wizard_goto(WizardStep::PowerMenu);
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

        case WizardStep::PowerMenu:
            if (input == "1") {
                request_power(PowerRequest::Sleep);
            } else if (input == "2") {
                request_power(PowerRequest::Off);
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

// Sleep / power off: recorded here, carried out by keypad_task once this input is handled
void SSHTerminal::request_power(PowerRequest request)
{
    wizard_reset();
    if (ssh_connected || session) {
        append_text("SSH is connected - type 'exit' to close it first.\n");
        return;
    }
    power_request = request;
}

SSHTerminal::PowerRequest SSHTerminal::take_power_request()
{
    PowerRequest request = power_request;
    power_request = PowerRequest::None;
    return request;
}

// Before sleeping or turning off: nothing left running, unsaved history saved, WiFi off.
// With a PIN, the vault locks (like a computer going to sleep); without one nothing is asked.
void SSHTerminal::power_prepare(PowerRequest request)
{
    if (chat_active) {
        close_chat();
    }
    wizard_reset();
    if (history_needs_save) {
        history_needs_save = false;
        save_history_to_nvs();
    }
    wifi_sleep();
    if (vault::has_pin()) {
        vault::lock();
    }
    if (request == PowerRequest::Off) {
        append_text((std::string("\nTurning off. ") + board::HINT_POWER_ON + "\n").c_str());
    } else {
        append_text((std::string("\nSleeping. Wake it with ") + board::HINT_WAKE + ".\n").c_str());
    }
    refresh_display_now();
}

// Awake again: WiFi reconnects as at startup (unless the user had turned it off), then the menu
void SSHTerminal::power_resume()
{
    last_input_ms = esp_timer_get_time() / 1000;
    append_text("Awake.\n");
    if (wifi_auto_enabled) {
        wifi_retry_delay_s = 0;
        wifi_next_retry_ms = last_input_ms + 30000;
        wifi_auto_connect(false);
    }
    if (!wizard_active()) {   // Not waiting for a PIN
        go_home();
    }
}

// The device stayed on after 'shutdown' (Tab5 on USB power): it sleeps instead
void SSHTerminal::power_off_failed()
{
    append_text((std::string("Still powered (USB keeps it on?) - sleeping instead. Wake it with ") +
                 board::HINT_WAKE + ".\n").c_str());
    refresh_display_now();
}
