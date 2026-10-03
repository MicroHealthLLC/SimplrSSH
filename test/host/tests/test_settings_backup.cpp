/*
 * settings_backup: SD card backup and restore. A backup is untrusted input: restore parses
 * the whole file before changing anything, and passwords only leave the device encrypted.
 */

#include "settings_backup.hpp"
#include "connection_profiles.hpp"
#include "secret_vault.hpp"
#include "helpers.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

static const std::string SECRET = "wifi-password-123";

static std::string temp_dir()
{
    char tmpl[] = "/tmp/simplrssh-backup-XXXXXX";
    char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    return dir ? dir : "/tmp";
}

static std::string read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write_file(const std::string& path, const std::string& data)
{
    std::ofstream f(path, std::ios::binary);
    f << data;
}

static void remove_dir(const std::string& dir)
{
    std::remove((dir + "/settings.dat").c_str());
    std::remove((dir + "/settings.new").c_str());
    rmdir(dir.c_str());
}

static void set_str(const char* ns, const char* key, const char* value)
{
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open(ns, NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_str(h, key, value), ESP_OK);
    nvs_close(h);
}

// A device with one of everything that is backed up
static void populate()
{
    ConnectionProfile p;
    p.id = "0a0b0c0d";
    p.name = "server";
    p.host = "10.0.0.5";
    p.username = "pi";
    p.secret_saved = true;
    CHECK_EQ(profile_store::save({p}), ESP_OK);
    SavedNetwork n;
    n.ssid = "HomeNet";
    n.secret_saved = true;
    CHECK_EQ(network_store::save({n}), ESP_OK);
    set_str("known_hosts", "h00112233445566", "10.0.0.5:22\x1Fssh-ed25519\x1F" "abc");
    set_str("storage", "hist_0", "ls -la");
    nvs_handle_t h;
    CHECK_EQ(settings_nvs::open("storage", NVS_READWRITE, &h), ESP_OK);
    CHECK_EQ(nvs_set_u32(h, "hist_count", 1), ESP_OK);
    nvs_close(h);
    CHECK(!vault::init());
    CHECK_EQ(vault::put(n.secret_id(), SECRET), ESP_OK);
}

TEST(backup_restore_round_trip_same_device)
{
    factory_device();
    populate();
    std::string dir = temp_dir();
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::backup(dir, counts), ESP_OK);
    CHECK_EQ(counts.profiles, 1);
    CHECK_EQ(counts.networks, 1);
    CHECK_EQ(counts.hosts, 1);
    CHECK_EQ(counts.history, 1);

    settings_backup::Counts in_file;
    CHECK(settings_backup::read_backup_counts(dir, in_file));
    CHECK_EQ(in_file.profiles, 1);
    CHECK_EQ(in_file.networks, 1);

    CHECK_EQ(settings_backup::erase_device(), ESP_OK);
    CHECK(profile_store::load().empty());
    CHECK(network_store::load().empty());

    settings_backup::Counts restored;
    CHECK_EQ(settings_backup::restore(dir, restored), ESP_OK);
    CHECK_EQ(restored.profiles, 1);
    CHECK_EQ(restored.hosts, 1);
    CHECK_EQ(profile_store::load().size(), (size_t)1);
    CHECK_EQ(network_store::load().size(), (size_t)1);

    CHECK(!vault::init());            // Same device: its secret still unwraps the vault
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
    CHECK_EQ(fake::open_handles(), 0);
    remove_dir(dir);
}

TEST(backup_holds_no_plaintext_secret_or_device_key)
{
    factory_device();
    populate();
    std::string dir = temp_dir();
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::backup(dir, counts), ESP_OK);
    std::string file = read_file(dir + "/settings.dat");
    CHECK_EQ(file.rfind("SIMPLRSSH-BACKUP 1 ", 0), (size_t)0);
    CHECK(file.find(SECRET) == std::string::npos);
    CHECK(file.find(hex_of(SECRET)) == std::string::npos);
    CHECK(file.find("vault_dev") == std::string::npos);
    CHECK(file.find(hex_of(fake::store()["nvs"]["vault_dev"]["secret"].bytes)) == std::string::npos);
    CHECK(access((dir + "/settings.new").c_str(), F_OK) != 0);
    remove_dir(dir);
}

TEST(backup_restored_on_other_device_discards_passwords)
{
    factory_device();
    populate();
    std::string dir = temp_dir();
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::backup(dir, counts), ESP_OK);

    fake::reset();                    // Another T-Deck
    fake::seed(98765);
    factory_device();
    CHECK(!vault::init());
    CHECK_EQ(settings_backup::restore(dir, counts), ESP_OK);
    CHECK_EQ(profile_store::load().size(), (size_t)1);
    CHECK(vault::init());             // Cannot unwrap: passwords discarded, new vault
    CHECK(vault::is_unlocked());
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_ERR_NOT_FOUND);
    remove_dir(dir);
}

TEST(backup_with_pin_moves_passwords_to_other_device)
{
    factory_device();
    populate();
    CHECK_EQ(vault::set_pin("2468"), ESP_OK);
    std::string dir = temp_dir();
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::backup(dir, counts), ESP_OK);

    fake::reset();                    // Another T-Deck
    fake::seed(98765);
    factory_device();
    CHECK_EQ(settings_backup::restore(dir, counts), ESP_OK);
    CHECK(!vault::init());
    CHECK(vault::has_pin());
    CHECK_EQ(vault::unlock("2468"), ESP_OK);
    std::string out;
    CHECK_EQ(vault::get("wifi:HomeNet", out), ESP_OK);
    CHECK_EQ(out, SECRET);
    remove_dir(dir);
}

TEST(backup_legacy_pocketssh_header_restores)
{
    factory_device();
    std::string dir = temp_dir();
    write_file(dir + "/settings.dat",
               "POCKETSSH-BACKUP 1 0 0 1 0\n"
               "known_hosts h00112233445566 s " + hex_of("host:22\x1Fssh-rsa\x1Fxyz") + "\n");
    settings_backup::Counts counts;
    CHECK(settings_backup::read_backup_counts(dir, counts));
    CHECK_EQ(counts.hosts, 1);
    CHECK_EQ(settings_backup::restore(dir, counts), ESP_OK);
    CHECK_EQ(counts.hosts, 1);
    CHECK_EQ(fake::store()["nvs"]["known_hosts"].size(), (size_t)1);
    remove_dir(dir);
}

// Each damaged backup must be refused without touching what is on the device
static void expect_rejected(const std::string& contents)
{
    fake::reset();
    factory_device();
    SavedNetwork n;
    n.ssid = "KeepMe";
    CHECK_EQ(network_store::save({n}), ESP_OK);
    std::string dir = temp_dir();
    write_file(dir + "/settings.dat", contents);
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::restore(dir, counts), ESP_ERR_INVALID_STATE);
    auto nets = network_store::load();
    CHECK_EQ(nets.size(), (size_t)1);
    remove_dir(dir);
}

TEST(restore_rejects_untrusted_backups)
{
    const std::string header = "SIMPLRSSH-BACKUP 1 0 0 0 0\n";
    expect_rejected("");                                                    // Empty
    expect_rejected("NOT-A-BACKUP 1\n");                                    // Wrong header
    expect_rejected(header + "vault_dev secret b 00112233\n");             // Device key namespace
    expect_rejected(header + "unknown key s 6869\n");                       // Unknown namespace
    expect_rejected(header + "storage this_key_is_too_long s 6869\n");     // NVS key > 15 chars
    expect_rejected(header + "storage bad-key s 6869\n");                   // Invalid key characters
    expect_rejected(header + "storage hist_0 s 6g69\n");                    // Not hex
    expect_rejected(header + "storage hist_0 s 686\n");                     // Odd hex length
    expect_rejected(header + "storage hist_0 s 680069\n");                  // NUL inside a string
    expect_rejected(header + "storage hist_0 s " + std::string(8002, '6') + "\n");  // String > 4000
    expect_rejected(header + "vault s0011 b " + std::string(2050, 'a') + "\n");     // Blob > 1024
    expect_rejected(header + "vault s0011 b \n");                           // Empty blob
    expect_rejected(header + "storage hist_count u 12x\n");                 // Not a number
    expect_rejected(header + "storage hist_count u 12345678901\n");        // Too many digits
    expect_rejected(header + "storage hist_0 x 6869\n");                    // Unknown type
    expect_rejected(header + "storage hist_0  s 6869\n");                   // Malformed spacing
    expect_rejected(header + std::string(9000, 'a') + "\n");               // Line too long

    std::string many = header;
    for (int i = 0; i < 1001; i++) {
        many += "storage k" + std::to_string(i) + " u 1\n";
    }
    expect_rejected(many);                                                  // Too many records
}

TEST(restore_missing_backup_fails_cleanly)
{
    factory_device();
    settings_backup::Counts counts;
    CHECK_EQ(settings_backup::restore("/nonexistent-simplrssh-dir", counts), ESP_ERR_INVALID_STATE);
    CHECK(!settings_backup::read_backup_counts("/nonexistent-simplrssh-dir", counts));
}

TEST(erase_device_clears_settings_and_vault)
{
    factory_device();
    populate();
    CHECK_EQ(settings_backup::erase_device(), ESP_OK);
    settings_backup::Counts c = settings_backup::count_device();
    CHECK_EQ(c.profiles, 0);
    CHECK_EQ(c.networks, 0);
    CHECK_EQ(c.hosts, 0);
    CHECK_EQ(c.history, 0);
    CHECK_EQ(vault::count(), 0);
}
