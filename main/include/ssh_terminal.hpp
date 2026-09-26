/*
 * SSHTerminal Header
 * Provides LVGL-based SSH terminal interface with WiFi connectivity.
 * Supports command history, battery monitoring, and interactive shell sessions.
 */

#ifndef SSH_TERMINAL_HPP
#define SSH_TERMINAL_HPP

#include "lvgl.h"
#include "esp_err.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "libssh2.h"
#include "battery_measurement.hpp"
#include "connection_profiles.hpp"

#define SSH_MAX_LINE_LENGTH 128
#define SSH_MAX_LINES 100
#define SSH_BUFFER_SIZE 4096

class SSHTerminal
{
public:
    SSHTerminal();
    ~SSHTerminal();

    lv_obj_t* create_terminal_screen();
    
    esp_err_t connect(const char* host, int port, const char* username, const char* password);
    esp_err_t connect_with_key(const char* host, int port, const char* username, const char* privkey_data, size_t privkey_len,
                               const char* passphrase = NULL);
    esp_err_t disconnect();
    bool is_connected();
    
    void append_text(const char* text);
    void clear_terminal();
    void handle_key_input(char key);
    void send_command(const char* cmd);
    void navigate_history(int direction);
    void delete_current_history_entry();
    void send_current_history_command();
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_home();
    void move_cursor_end();
    
    esp_err_t init_wifi(const char* ssid, const char* password);
    bool is_wifi_connected();
    void disconnect_wifi();
    void wifi_link_lost();
    
    // Runs once after the splash screen is dismissed (WiFi auto-connect)
    void run_startup_tasks();
    
    lv_obj_t* get_screen() { return terminal_screen; }
    
    void update_status_bar();
    
    // SSH key management
    void load_key_from_memory(const char* keyname, const char* key_data, size_t key_len);
    const char* get_loaded_key(const char* keyname, size_t* len);
    std::vector<std::string> get_loaded_key_names();
    
private:
    // Menu/wizard state shared by the profile, WiFi and vault menus
    // (profile_menu.cpp, wifi_menu.cpp, vault_menu.cpp)
    enum class WizardStep {
        None,
        // Connection profiles
        MainMenu, PickProfile, Name, Host, Port, User, AuthMethod, PickKey,
        Password, KeyPassphrase, Review, ConfirmDelete, ConnectSecret,
        // WiFi
        WifiMenu, WifiPickScan, WifiHiddenSsid, WifiPassword, WifiSave,
        WifiPickSaved, WifiConfirmForget,
        // Vault (master PIN)
        VaultUnlock, VaultOldPin, VaultNewPin, VaultConfirmPin, VaultConfirmReset
    };
    enum class WizardAction { None, Connect, Add, Edit, Delete, WifiConnectSaved, WifiForget, ChangePin };
    enum class SecretChange { Keep, Set, Clear };
    struct WifiScanResult {
        std::string ssid;
        int rssi;
        bool open;
    };
    struct ProfileWizard {
        WizardStep step = WizardStep::None;
        WizardAction action = WizardAction::None;
        WizardStep menu = WizardStep::None;  // Menu to return to when an action finishes
        std::vector<std::string> choices;    // Valid answers the trackball can cycle through
        
        ConnectionProfile draft;
        int index = -1;                      // Profile being edited/deleted/connected, -1 = new
        bool editing = false;                // Show current values as defaults
        SecretChange secret_change = SecretChange::Keep;
        std::string secret;                  // Plaintext password in flight (wiped on reset)
        
        std::string pin;                     // New PIN awaiting confirmation (wiped on reset)
        int pin_attempts = 0;
        std::function<void()> after_unlock;  // Continues the action once the vault is unlocked
        
        std::vector<WifiScanResult> scan;
        std::string wifi_ssid;
        bool wifi_open = false;
        bool wifi_hidden = false;
    };

    lv_obj_t* terminal_screen;
    lv_obj_t* terminal_output;
    lv_obj_t* input_label;
    lv_obj_t* status_bar;
    lv_obj_t* byte_counter_label;
    lv_obj_t* side_panel;
    
    std::string current_input;
    size_t cursor_pos;
    size_t bytes_received;
    std::vector<std::string> command_history;
    int history_index;
    
    lv_timer_t* cursor_blink_timer;
    bool cursor_visible;
    
    lv_timer_t* battery_update_timer;
    
    bool history_needs_save;
    lv_timer_t* history_save_timer;
    
    std::string text_buffer;
    int64_t last_display_update;
    
    bool wifi_connected;
    bool ssh_connected;
    bool battery_initialized;
    
    BatteryMeasurement battery;
    
    int ssh_socket;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    
    char* hostname;
    int port_number;
    
    // SSH key storage: keyname -> key content
    std::map<std::string, std::string> loaded_keys;
    
    std::vector<ConnectionProfile> profiles;
    std::vector<SavedNetwork> saved_networks;
    std::string wifi_ssid;
    ProfileWizard wizard;
    
    void update_terminal_display();
    void update_input_display();
    void process_received_data(const char* data, size_t len);
    void flush_display_buffer();
    
    void load_history_from_nvs();
    void save_history_to_nvs();
    void clear_history_nvs();
    std::string strip_ansi_codes(const char* data, size_t len);
    void send_special_key(const char* sequence);
    void create_side_panel();
    void toggle_side_panel();
    static void gesture_event_cb(lv_event_t* e);
    static void special_key_event_cb(lv_event_t* e);
    static void input_touch_event_cb(lv_event_t* e);
    static void cursor_blink_cb(lv_timer_t* timer);
    static void battery_update_cb(lv_timer_t* timer);
    static void history_save_cb(lv_timer_t* timer);
    static void ssh_receive_task(void* param);
    
    static int waitsocket(int socket_fd, LIBSSH2_SESSION *session);
    esp_err_t ssh_authenticate(const char* username, const char* password);
    esp_err_t ssh_authenticate_pubkey(const char* username, const char* privkey_data, size_t privkey_len,
                                      const char* passphrase);
    esp_err_t start_wifi_driver();
    esp_err_t scan_wifi(std::vector<WifiScanResult>& results);
    void refresh_display_now();
    esp_err_t ssh_open_channel();
    
    // Menu/wizard infrastructure and connection profiles (profile_menu.cpp)
    bool wizard_active() const { return wizard.step != WizardStep::None; }
    bool wizard_input_masked() const;
    std::string input_display_text() const;
    void wizard_goto(WizardStep step);
    void wizard_prompt();
    void wizard_handle_input(const std::string& input);
    void wizard_cycle_choice(int direction);
    void wizard_done();
    void wizard_reset();
    void handle_profile_command(const std::string& command);
    void list_profiles();
    int find_profile(const std::string& ref);
    bool key_is_encrypted(const std::string& key_name);
    std::string profile_summary(const ConnectionProfile& p, bool secret_saved);
    void begin_profile_action(WizardAction action, const std::string& ref);
    void on_profile_picked(int index);
    bool profile_step_prompt();
    bool profile_step_input(const std::string& raw, const std::string& input);
    void save_profile_draft(bool connect_after);
    void commit_profile_draft(bool connect_after);
    void start_profile_connect(int index);
    void connect_profile(const ConnectionProfile& profile, const std::string& secret);
    
    // WiFi menu and auto-connect (wifi_menu.cpp)
    void handle_wifi_command(const std::string& command);
    void list_saved_networks();
    int find_saved_network(const std::string& ref);
    void wifi_scan_and_pick();
    void wifi_choose_network(const std::string& ssid, bool open);
    void wifi_connect_with(const std::string& password, bool from_vault);
    void wifi_remember_network();
    void wifi_auto_connect();
    bool wifi_step_prompt();
    bool wifi_step_input(const std::string& raw, const std::string& input);
    
    // Master PIN / encrypted password vault (vault_menu.cpp)
    void handle_vault_command(const std::string& command);
    void with_vault(std::function<void()> then);
    void clear_all_saved_secret_flags();
    bool vault_step_prompt();
    bool vault_step_input(const std::string& raw, const std::string& input);
};

#endif
