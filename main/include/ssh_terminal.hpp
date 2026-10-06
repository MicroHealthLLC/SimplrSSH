/*
 * SSHTerminal Header
 * Provides LVGL-based SSH terminal interface with WiFi connectivity.
 * Supports command history, battery monitoring, and interactive shell sessions.
 */

#ifndef SSH_TERMINAL_HPP
#define SSH_TERMINAL_HPP

#include "lvgl.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "libssh2.h"
#include "battery_measurement.hpp"
#include "connection_profiles.hpp"
#include "openai_client.hpp"
#include "term_screen.hpp"

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
    
    void append_text(const char* text);
    void clear_terminal();
    void handle_key_input(char key);
    void send_command(const char* cmd);
    void navigate_history(int direction);
    // Trackball up/down: scrolls the text on screen (terminal, menus or chat).
    // direction > 0 scrolls back toward older text, < 0 toward the newest.
    void scroll_screen(int direction);
    void delete_current_history_entry();
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_home();
    void move_cursor_end();
    void delete_at_cursor();   // Forward delete (a full keyboard's Del key)
    // Tab5 keyboard: F1-F12 (0-11) and Alt+key, for programs in the full-screen SSH terminal
    void function_key(int n);
    void alt_key(char key);
    
    esp_err_t init_wifi(const char* ssid, const char* password);
    void disconnect_wifi();
    void wifi_link_lost();
    
    // Runs once after boot, from the input task (WiFi auto-connect)
    void run_startup_tasks();
    // Called regularly from the input task: WiFi reconnect, return to menu after SSH ends
    void background_tick();
    // Trackball held down (start) / released (long_press: held >= 1 s)
    void on_trackball_hold(bool start, bool long_press);
    // Shows the home menu (startup, 'menu', 'exit')
    void go_home();
    // Boot message listing what was restored from the device's storage
    void print_saved_summary();
    
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
        WifiMenu, WifiPickScan, WifiHiddenSsid, WifiPassword,
        WifiPickSaved, WifiConfirmForget,
        // Vault (master PIN)
        VaultUnlock, VaultOldPin, VaultNewPin, VaultConfirmPin, VaultConfirmReset,
        // SSH host key trust (known_hosts.cpp)
        HostTrust, HostConfirmForget,
        // A key that turned out to need its passphrase, asked on the open connection
        SshKeyPassphrase,
        // Storage / SD card backup (storage_menu.cpp)
        StorageMenu, StorageConfirmBackup, StorageConfirmRestore, StorageConfirmErase,
        // Home and security menus (home_menu.cpp)
        HomeMenu, SecurityMenu,
        // ChatGPT (chat_app.cpp)
        ChatMenu, ChatKeySource, ChatKey, ChatKeyDelete, ChatModelWait, ChatModel, ChatModelOther
    };
    enum class WizardAction { None, Connect, Add, Edit, Delete, WifiConnectSaved, WifiForget, SetPin, ChangePin, RemovePin };
    enum class SecretChange { Keep, Set, Clear };
    struct WifiScanResult {
        std::string ssid;
        int rssi;
        bool open;
    };
    // A connection that has completed the SSH handshake and is waiting for the
    // user to trust the server's host key; credentials are wiped once used.
    struct PendingSsh {
        std::string host;
        int port = 22;
        std::string username;
        bool use_key = false;
        std::string password;          // Password auth
        const char* key_data = NULL;   // Points into loaded_keys (stable while connected)
        size_t key_len = 0;
        std::string passphrase;        // Key passphrase, empty if none
        int passphrase_prompts = 0;    // Times the passphrase was asked on this connection
        std::string fingerprint;       // "SHA256:..." of the server host key
        std::string key_type;
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
    SemaphoreHandle_t ssh_mutex;      // Serializes all libssh2 calls on session/channel
    uint32_t ssh_generation;          // Bumped on disconnect (under ssh_mutex) so a stale receive task exits
    PendingSsh pending_ssh;
    std::string forget_host;          // host:port awaiting 'hosts forget' confirmation
    
    // SSH key storage: keyname -> key content
    std::map<std::string, std::string> loaded_keys;
    
    std::vector<ConnectionProfile> profiles;
    std::vector<SavedNetwork> saved_networks;
    std::string wifi_ssid;
    ProfileWizard wizard;
    bool vault_was_reset = false;      // Passwords from another device were discarded at boot
    bool wifi_auto_enabled = true;     // Off after the user disconnects WiFi on purpose
    int64_t wifi_next_retry_ms = 0;    // When wifi_maintain() may try again
    int wifi_retry_delay_s = 0;
    int64_t last_input_ms = 0;         // Background reconnects wait until the user is idle
    volatile bool home_pending = false;  // Set by other tasks (SSH closed): show the home menu

    // ChatGPT (chat_app.cpp). History is touched only while holding the display lock.
    // Model names exactly as OpenAI gives them, saved. Until the user picks a chat model, and always
    // for voice, the newest ones from OpenAI's model list are used (refreshed once per boot).
    std::string chat_model = "gpt-4o-mini";
    bool chat_model_picked = false;          // Chosen in Model: never changed automatically
    std::string chat_stt_model = "whisper-1";
    std::string chat_tts_model = "gpt-4o-mini-tts";
    bool chat_models_checked = false;        // Model list fetched this boot
    bool chat_speak = false;           // Read replies aloud
    std::vector<openai::Message> chat_history;
    lv_obj_t* chat_view = NULL;
    bool chat_active = false;
    bool chat_open_after_key = false;
    bool chat_job_voice = false;
    volatile bool chat_busy = false;
    volatile bool chat_recording = false;
    volatile bool chat_cancel = false;      // Esc: the worker stops recording, answering or speaking
    volatile uint32_t chat_generation = 0;  // Bumped when the chat view closes
    std::vector<std::string> chat_models;  // From OpenAI, newest first (empty until asked)
    bool chat_models_for_menu = false;
    volatile bool chat_models_busy = false;
    std::string input_banner;          // Shown instead of the input line (e.g. recording)
    int pty_cols = 80;                 // Terminal size reported to SSH servers
    int pty_rows = 24;
    bool wifi_quiet_connect = false;   // Background reconnect: no progress dots

    // Full-screen SSH terminal (board::term_screen(), NULL on the T-Deck): shown while SSH is
    // connected; then every key goes to the server (raw_keys())
    TermScreen* term = NULL;
    bool raw_keys() const;
    void term_key(TermScreen::Key key);
    void show_term_view(bool show);
    
    // Scrolling: a view follows new text only while it shows the newest line, so text
    // the user scrolled back to (trackball or touch drag) stays put while output arrives
    bool terminal_follow = true;
    bool chat_follow = true;
    lv_obj_t* visible_view() const;
    void scroll_to_newest();
    static void scroll_end_cb(lv_event_t* e);
    static bool touch_scrolling(lv_obj_t* view);

    void update_input_display();
    void process_received_data(const char* data, size_t len);
    void flush_display_buffer();
    
    void load_history_from_nvs();
    void save_history_to_nvs();
    std::string strip_ansi_codes(const char* data, size_t len);
    void send_special_key(const char* sequence);
    void write_channel(const char* data, size_t len);
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
    esp_err_t ssh_begin(PendingSsh&& target);
    esp_err_t ssh_finish();
    void ssh_teardown();
    void wipe_pending_ssh();
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
    bool key_is_openssh_format(const std::string& key_name);
    bool wizard_holds_connection() const;
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
    void wifi_remember_network(const std::string& ssid, const std::string& password, bool hidden);
    bool wifi_auto_connect(bool quiet);
    bool wifi_step_prompt();
    bool wifi_step_input(const std::string& raw, const std::string& input);
    
    // SSH host key verification, trust on first use (known_hosts.cpp)
    enum class HostKeyStatus { Match, Unknown, Mismatch, Error };
    HostKeyStatus check_host_key(const std::string& host, int port, std::string& fingerprint, std::string& key_type);
    esp_err_t save_host_key(const std::string& host, int port, const std::string& key_type,
                            const std::string& fingerprint);
    void handle_hosts_command(const std::string& command);
    bool host_step_prompt();
    bool host_step_input(const std::string& raw, const std::string& input);
    
    // Storage: device vs SD card backup/restore (storage_menu.cpp)
    void handle_storage_command(const std::string& command);
    void storage_backup();
    void storage_restore();
    void storage_load_keys();
    void reload_saved_settings();
    bool storage_step_prompt();
    bool storage_step_input(const std::string& raw, const std::string& input);
    
    // Commands typed on the input line (ssh_terminal.cpp)
    void execute_command(const std::string& cmd);
    void wifi_maintain();

    // Home and security menus (home_menu.cpp)
    void show_menu_after(WizardStep menu);
    bool home_step_prompt();
    bool home_step_input(const std::string& raw, const std::string& input);

    // ChatGPT (chat_app.cpp)
    void chat_load_settings();
    void chat_save_settings();
    void handle_chat_command(const std::string& command);
    void open_chat();
    void close_chat();
    void chat_clear();
    bool chat_stop();
    void chat_submit(const std::string& text);
    void chat_start_job(bool voice);
    void chat_job();
    lv_obj_t* chat_add_bubble(int kind, const std::string& text);
    lv_obj_t* chat_ui_bubble(uint32_t generation, int kind, const std::string& text);
    void chat_ui_update(uint32_t generation, lv_obj_t* label, const std::string& text);
    void chat_ui_banner(const std::string& text);
    void chat_store_key(const std::string& key);
    static void chat_worker(void* param);
    void chat_request_models(bool for_menu);
    static void chat_models_worker(void* param);
    void chat_models_done(bool ok, const openai::Models& models, const std::string& error);
    std::string chat_apply_models(const openai::Models& m);
    bool chat_step_prompt();
    bool chat_step_input(const std::string& raw, const std::string& input);

    // Master PIN / encrypted password vault (vault_menu.cpp)
    void handle_vault_command(const std::string& command);
    void with_vault(std::function<void()> then);
    void clear_all_saved_secret_flags();
    bool vault_step_prompt();
    bool vault_step_input(const std::string& raw, const std::string& input);
};

#endif
