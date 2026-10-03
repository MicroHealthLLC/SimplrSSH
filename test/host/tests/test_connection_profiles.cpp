/*
 * profile_store / network_store: NVS records round-trip, stay bounded, never hold secrets.
 */

#include "connection_profiles.hpp"
#include "helpers.hpp"

static ConnectionProfile make_profile(int i)
{
    ConnectionProfile p;
    p.id = profile_store::new_id();
    p.name = "server" + std::to_string(i);
    p.host = "10.0.0." + std::to_string(i);
    p.port = 2200 + i;
    p.username = "user" + std::to_string(i);
    p.auth = i % 2 ? ConnectionProfile::Auth::Key : ConnectionProfile::Auth::Password;
    p.key_name = i % 2 ? "id_" + std::to_string(i) + ".pem" : "";
    p.secret_saved = i % 3 == 0;
    return p;
}

TEST(profiles_round_trip)
{
    factory_device();
    std::vector<ConnectionProfile> saved = {make_profile(1), make_profile(2), make_profile(3)};
    CHECK_EQ(profile_store::save(saved), ESP_OK);

    std::vector<ConnectionProfile> loaded = profile_store::load();
    CHECK_EQ(loaded.size(), saved.size());
    for (size_t i = 0; i < loaded.size() && i < saved.size(); i++) {
        CHECK_EQ(loaded[i].id, saved[i].id);
        CHECK_EQ(loaded[i].name, saved[i].name);
        CHECK_EQ(loaded[i].host, saved[i].host);
        CHECK_EQ(loaded[i].port, saved[i].port);
        CHECK_EQ(loaded[i].username, saved[i].username);
        CHECK(loaded[i].auth == saved[i].auth);
        CHECK_EQ(loaded[i].key_name, saved[i].key_name);
        CHECK_EQ(loaded[i].secret_saved, saved[i].secret_saved);
    }
    CHECK_EQ(fake::open_handles(), 0);
}

TEST(profiles_nothing_saved_loads_empty)
{
    factory_device();
    CHECK(profile_store::load().empty());
}

TEST(profiles_bounded_to_max_count)
{
    factory_device();
    std::vector<ConnectionProfile> many;
    for (int i = 0; i < PROFILE_MAX_COUNT + 5; i++) {
        many.push_back(make_profile(i));
    }
    CHECK_EQ(profile_store::save(many), ESP_OK);
    CHECK_EQ(profile_store::load().size(), (size_t)PROFILE_MAX_COUNT);
    CHECK_EQ(fake::store()["nvs"]["profiles"].count("p" + std::to_string(PROFILE_MAX_COUNT)), (size_t)0);
}

TEST(profiles_shrinking_removes_leftover_records)
{
    factory_device();
    std::vector<ConnectionProfile> five;
    for (int i = 0; i < 5; i++) {
        five.push_back(make_profile(i));
    }
    CHECK_EQ(profile_store::save(five), ESP_OK);
    five.resize(2);
    CHECK_EQ(profile_store::save(five), ESP_OK);
    auto& ns = fake::store()["nvs"]["profiles"];
    CHECK_EQ(ns.count("p1"), (size_t)1);
    CHECK_EQ(ns.count("p2"), (size_t)0);
    CHECK_EQ(ns.count("p4"), (size_t)0);
    CHECK_EQ(profile_store::load().size(), (size_t)2);
}

TEST(profiles_field_separator_keeps_fields_apart)
{
    factory_device();
    ConnectionProfile p = make_profile(1);
    p.name = "web server (prod) #1";
    p.username = "first.last";
    CHECK_EQ(profile_store::save({p}), ESP_OK);
    auto loaded = profile_store::load();
    CHECK_EQ(loaded.size(), (size_t)1);
    if (!loaded.empty()) {
        CHECK_EQ(loaded[0].name, p.name);
        CHECK_EQ(loaded[0].username, p.username);
    }
}

TEST(profiles_unreadable_records_skipped)
{
    factory_device();
    CHECK_EQ(profile_store::save({make_profile(1), make_profile(2)}), ESP_OK);
    fake::store()["nvs"]["profiles"]["p0"].bytes = "garbage";
    auto loaded = profile_store::load();
    CHECK_EQ(loaded.size(), (size_t)1);
    if (!loaded.empty()) {
        CHECK_EQ(loaded[0].name, std::string("server2"));
    }
}

TEST(profiles_save_failure_is_reported)
{
    factory_device();
    fake::fail_writes(true);
    CHECK(profile_store::save({make_profile(1)}) != ESP_OK);
    CHECK_EQ(fake::open_handles(), 0);
}

TEST(profiles_not_logged)
{
    factory_device();
    ConnectionProfile p = make_profile(7);
    CHECK_EQ(profile_store::save({p}), ESP_OK);
    profile_store::load();
    CHECK(!log_contains(p.host));
    CHECK(!log_contains(p.username));
    CHECK(!log_contains(p.name));
}

TEST(profile_ids_are_unique_hex)
{
    std::string a = profile_store::new_id();
    std::string b = profile_store::new_id();
    CHECK_EQ(a.size(), (size_t)8);
    CHECK(a != b);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST(networks_round_trip)
{
    factory_device();
    SavedNetwork open_net;
    open_net.ssid = "Cafe WiFi";
    SavedNetwork home;
    home.ssid = "HomeNet";
    home.secret_saved = true;
    home.hidden = true;
    CHECK_EQ(network_store::save({open_net, home}), ESP_OK);

    auto loaded = network_store::load();
    CHECK_EQ(loaded.size(), (size_t)2);
    if (loaded.size() == 2) {
        CHECK_EQ(loaded[0].ssid, std::string("Cafe WiFi"));
        CHECK(!loaded[0].secret_saved);
        CHECK(!loaded[0].hidden);
        CHECK_EQ(loaded[1].ssid, std::string("HomeNet"));
        CHECK(loaded[1].secret_saved);
        CHECK(loaded[1].hidden);
    }
}

TEST(networks_bounded_to_max_count)
{
    factory_device();
    std::vector<SavedNetwork> many;
    for (int i = 0; i < NETWORK_MAX_COUNT + 3; i++) {
        SavedNetwork n;
        n.ssid = "net" + std::to_string(i);
        many.push_back(n);
    }
    CHECK_EQ(network_store::save(many), ESP_OK);
    CHECK_EQ(network_store::load().size(), (size_t)NETWORK_MAX_COUNT);
}

TEST(secret_ids_are_namespaced)
{
    ConnectionProfile p;
    p.id = "1a2b3c4d";
    SavedNetwork n;
    n.ssid = "HomeNet";
    CHECK_EQ(p.secret_id(), std::string("profile:1a2b3c4d"));
    CHECK_EQ(n.secret_id(), std::string("wifi:HomeNet"));
}
