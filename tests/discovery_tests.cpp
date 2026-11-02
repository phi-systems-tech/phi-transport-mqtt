// The Home Assistant discovery without a broker: golden payloads per kind of
// device (tests/golden/*.json, compared after parsing, so formatting and key
// order do not matter), and the behaviours around them - no component, no
// payload; a vanished component named before it is dropped; value-independent
// payloads; the ids.
//
// PHI_UPDATE_GOLDEN=1 rewrites the golden files from what the code produces.
// Read the diff before committing it: the files are the contract with Home
// Assistant, not a snapshot to accept.

#include "discovery.h"
#include "command.h"
#include "retained.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace phi::endpoint;

#ifndef PHI_GOLDEN_DIR
#error "PHI_GOLDEN_DIR is set by CMake"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                               \
        }                                                                               \
    } while (false)

constexpr int Readable = 1, Writable = 2, Reportable = 4, Momentary = 0x100;
constexpr int RW = Readable | Writable | Reportable, RO = Readable | Reportable;
constexpr int Bool = 1, Int = 2, Float = 3, String = 4, Color = 5, Enum = 6, Json_ = 7;

Json ch(int id, const char *externalId, const char *name, int kind, int dataType, int flags, const char *unit = "",
        double min = 0, double max = 0, double step = 0)
{
    return {{"id", id}, {"externalId", externalId}, {"name", name}, {"kind", kind}, {"kindName", "K"},
            {"dataType", dataType}, {"dataTypeName", "T"}, {"flags", flags}, {"unit", unit}, {"min", min},
            {"max", max}, {"step", step}, {"metaAdapter", Json::object()}, {"hasValue", false}};
}

Json entry(int id, int adapterId, const char *name, int deviceClass, const char *className, int roomId, Json channels,
           const char *manufacturer = "", const char *model = "", const char *firmware = "")
{
    Json d = {{"id", id}, {"adapterId", adapterId}, {"externalId", "ext" + std::to_string(id)}, {"name", name},
              {"manufacturer", manufacturer}, {"model", model}, {"firmware", firmware},
              {"deviceClass", deviceClass}, {"deviceClassName", className}, {"userRoomId", roomId}, {"adapterRoomId", 0},
              {"userRoomName", roomId ? Json("Kitchen") : Json()}, {"adapterRoomName", nullptr}};
    return Json{{"device", d}, {"channels", channels}};
}

// One of each device the discovery has to get right.
Json devices()
{
    return Json::array({
        // A Hue colour light.
        entry(10, 1, "Kitchen light", 1, "Light", 5,
              Json::array({ch(100, "power", "Power", 1, Bool, RW), ch(101, "brightness", "Brightness", 10, Int, RW, "%", 0, 100, 1),
                           ch(102, "color", "Colour", 12, Color, RW), ch(103, "ct", "Colour temperature", 11, Int, RW, "mired", 153, 500, 1),
                           ch(104, "reachable", "Link quality", 64, Int, RO, "%")}),
              "Signify", "LCT015", "1.65.9"),
        // A plug that measures.
        entry(11, 1, "Coffee plug", 5, "Plug", 5,
              Json::array({ch(110, "state", "Switch", 1, Bool, RW), ch(111, "power", "Power", 60, Float, RO, "W"),
                           ch(112, "energy", "Energy", 63, Float, RO, "kWh")}),
              "Hombli", "Smart Socket", ""),
        // A door contact with a battery.
        entry(12, 2, "Front door", 3, "Sensor", 0,
              Json::array({ch(120, "contact", "Contact", 66, Bool, RO), ch(121, "battery", "Battery", 54, Int, RO, "%", 0, 100)}),
              "Aqara", "MCCGQ11LM"),
        // A wall remote with two buttons.
        entry(13, 2, "Hall remote", 4, "Button", 0,
              Json::array({ch(130, "button1", "Button 1", 2, Int, RO), ch(131, "button2", "Button 2", 2, Int, RO),
                           ch(132, "battery", "Battery", 54, Int, RO, "%", 0, 100)})),
        // An infrared blaster's key and a chime pattern that needs a value.
        entry(14, 3, "TV", 0, "Unknown", 0,
              Json::array({ch(140, "power-key", "Power", 480, Bool, Writable | Momentary),
                           ch(141, "chime", "Chime", 400, Enum, Writable | Momentary)})),
        // A mailbox.
        entry(15, 4, "Marius (mail)", 16, "Messenger", 0, Json::array({ch(150, "mail", "Message", 460, Json_, Writable)})),
        // A thermostat: a reading, a setpoint, and an enum that is read only here.
        entry(16, 5, "Bath thermostat", 7, "Thermostat", 0,
              Json::array({ch(160, "temperature", "Temperature", 50, Float, RO, "C"),
                           ch(161, "setpoint", "Target", 0, Float, RW, "\xC2\xB0" "C", 5, 30, 0.5),
                           ch(162, "mode", "Mode", 0, Enum, RW), ch(163, "fault", "Fault", 72, 8, RO)})),
        // A doorbell with a camera: the ring is an event, the stream waits for phase (d).
        entry(17, 6, "Front doorbell", 14, "Doorbell", 0,
              Json::array({ch(170, "ring", "Ring", 422, Enum, RO), ch(171, "stream", "Stream", 420, Enum, RO),
                           ch(172, "privacy", "Privacy mode", 427, Bool, RW), ch(173, "detected", "Detected", 423, 8, RO)})),
        // A voice satellite.
        entry(18, 7, "Kitchen satellite", 0, "Unknown", 5,
              Json::array({ch(180, "say", "Announcement", 402, String, RW), ch(181, "voice", "Voice level", 401, Int, RO, "%", 0, 100),
                           ch(182, "mute", "Mute", 31, Bool, RW), ch(183, "volume", "Volume", 30, Int, RW, "%", 0, 100, 5)})),
        // Nothing Home Assistant could show: only an occurrence nobody declared.
        entry(19, 2, "Scene dial", 4, "Button", 0, Json::array({ch(190, "scene", "Scene", 300, Int, RO)})),
        // A device of an excluded adapter.
        entry(20, 9, "Hidden plug", 5, "Plug", 0, Json::array({ch(200, "state", "Switch", 1, Bool, RW)})),
    });
}

Model house()
{
    Config config;
    config.excludeAdapters = {"tuya"};
    Model m;
    m.setConfig(config);
    m.applyAdapters(Json::array({{{"id", 1}, {"pluginType", "hue"}, {"externalId", "hue1"}, {"connected", true}},
                                 {{"id", 9}, {"pluginType", "tuya"}, {"externalId", "tuya1"}, {"connected", true}}}));
    m.applyRooms(Json::array({{{"id", 5}, {"name", "Kitchen"}, {"roomClassName", "Kitchen"}, {"floorId", 0}}}));
    m.applyScenes(Json::array({{{"id", 20}, {"name", "Evening"}, {"roomId", 5}}, {{"id", 21}, {"name", "Movie"}, {"roomId", 0}}}));
    m.applyDevices(devices());
    return m;
}

DiscoveryContext context()
{
    DiscoveryContext ctx;
    ctx.topics = Topics("phi");
    ctx.boxId = "pi5_1";
    ctx.version = "9.9.9";
    return ctx;
}

// --- goldens ---------------------------------------------------------------------------------

std::string goldenPath(const std::string &name)
{
    return std::string(PHI_GOLDEN_DIR) + "/" + name + ".json";
}

void expectGolden(const std::string &name, const TopicMap &topics, const std::string &expectedTopic)
{
    const auto it = topics.find(expectedTopic);
    if (it == topics.end() || topics.size() != 1) {
        std::printf("FAIL golden %s: expected exactly the topic %s\n", name.c_str(), expectedTopic.c_str());
        ++g_failures;
        return;
    }
    const Json actual = Json::parse(it->second);
    if (std::getenv("PHI_UPDATE_GOLDEN")) {
        std::ofstream(goldenPath(name)) << actual.dump(2) << "\n";
        return;
    }
    std::ifstream in(goldenPath(name));
    if (!in) {
        std::printf("FAIL golden %s: %s is missing (PHI_UPDATE_GOLDEN=1 writes it)\n", name.c_str(), goldenPath(name).c_str());
        ++g_failures;
        return;
    }
    std::stringstream text;
    text << in.rdbuf();
    const Json expected = Json::parse(text.str(), nullptr, false);
    if (expected != actual) {
        std::printf("FAIL golden %s\n--- expected\n%s\n--- actual\n%s\n", name.c_str(), expected.dump(2).c_str(), actual.dump(2).c_str());
        ++g_failures;
    }
}

void testGoldens()
{
    const Model m = house();
    const DiscoveryContext ctx = context();
    const struct {
        int id;
        const char *name;
    } cases[] = {{10, "hue_light"}, {11, "plug"}, {12, "contact_sensor"}, {13, "remote"}, {14, "remote_key"},
                 {15, "messenger"}, {16, "thermostat"}, {17, "doorbell"}, {18, "satellite"}};
    for (const auto &c : cases) {
        const Device *device = m.device(c.id);
        CHECK(device != nullptr);
        if (!device) continue;
        expectGolden(c.name, deviceDiscovery(ctx, m, *device), "homeassistant/device/phi_pi5_1_" + std::to_string(c.id) + "/config");
    }
    expectGolden("scenes", sceneDiscovery(ctx, m), "homeassistant/device/phi_pi5_1_scenes/config");
}

// --- behaviours ---------------------------------------------------------------------------------

void testNothingToShow()
{
    const Model m = house();
    const DiscoveryContext ctx = context();
    // Only an undeclared event: no component, so no payload.
    CHECK(deviceDiscovery(ctx, m, *m.device(19)).empty());
    // An excluded adapter's device is left off.
    CHECK(deviceDiscovery(ctx, m, *m.device(20)).empty());
    // No scene, no scenes device.
    Model empty;
    CHECK(sceneDiscovery(ctx, empty).empty());
}

void testPayloadIgnoresValues()
{
    Model m = house();
    const DiscoveryContext ctx = context();
    const TopicMap before = deviceDiscovery(ctx, m, *m.device(10));
    m.applyState(100, true, "", Json());
    m.applyState(101, 42, "", Json());
    CHECK(deviceDiscovery(ctx, m, *m.device(10)) == before);
}

void testRoomAndRename()
{
    Model m = house();
    const DiscoveryContext ctx = context();
    const std::string topic = "homeassistant/device/phi_pi5_1_10/config";
    CHECK(Json::parse(deviceDiscovery(ctx, m, *m.device(10)).at(topic))["dev"]["sa"] == "Kitchen");
    m.upsertRoom({{"id", 5}, {"name", "Cooking"}, {"roomClassName", "Kitchen"}, {"floorId", 0}});
    CHECK(Json::parse(deviceDiscovery(ctx, m, *m.device(10)).at(topic))["dev"]["sa"] == "Cooking");
    // No room, no suggested area.
    CHECK(!Json::parse(deviceDiscovery(ctx, m, *m.device(12)).at("homeassistant/device/phi_pi5_1_12/config"))["dev"].contains("sa"));
}

void testRemoval()
{
    // The ledger clears the config topic when the device's payload goes.
    const Model m = house();
    const DiscoveryContext ctx = context();
    Retained r;
    r.replayAll();
    const TopicMap map = deviceDiscovery(ctx, m, *m.device(11));
    CHECK(r.set("discovery/11", map).size() == 1);
    const std::vector<Message> gone = r.drop("discovery/11");
    CHECK(gone.size() == 1 && gone[0].topic == "homeassistant/device/phi_pi5_1_11/config" && gone[0].payload.empty() && gone[0].retain);

    // A component that vanishes is named with its platform first.
    const std::string with = map.begin()->second;
    Json without = Json::parse(with);
    without["cmps"].erase("energy");
    const std::optional<std::string> step = removalStep(with, dumpJson(without));
    CHECK(step.has_value());
    if (step) {
        const Json j = Json::parse(*step);
        CHECK(j["cmps"]["energy"] == (Json{{"p", "sensor"}}));
        CHECK(j["cmps"].contains("power") && j["cmps"]["power"].size() > 1);
    }
    CHECK(!removalStep(with, with).has_value());
    // A new component is no removal.
    CHECK(!removalStep(dumpJson(without), with).has_value());
    CHECK(!removalStep("", with).has_value());
}

void testIds()
{
    CHECK(boxIdFor("Pi5", "1") == "pi5_1");
    CHECK(boxIdFor("my host.local", "1") == "my_host_local_1");
    CHECK(boxIdFor("taifun", "") == "taifun");
    CHECK(boxIdFor("", "2") == "box_2");
    CHECK(deviceObjectId("pi5_1", 42) == "phi_pi5_1_42");
    CHECK(scenesObjectId("pi5_1") == "phi_pi5_1_scenes");

    const DiscoveryContext ctx = context();
    CHECK(isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_42/config"));
    CHECK(isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_scenes/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_10_42/config")); // another box
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_2_42/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/zigbee_abc/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/sensor/phi_pi5_1_42/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_42/other"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_4/2/config"));
    CHECK(!isOwnConfigTopic(ctx, "homeassistant/device/phi_pi5_1_42/config/x"));
}

void testEventsAndCommandsThatBackTheMapping()
{
    const Model m = house();
    // A ring says `ring`, the one type the doorbell's event entity declares, whatever the code was.
    const Channel *ring = m.device(17)->bySlug("ring");
    CHECK(ring != nullptr);
    CHECK(Json::parse(eventPayload(*ring, 1, "InitialPress", Json(), 5)) == (Json{{"event_type", "ring"}, {"code", 1}, {"ts", 5}}));
    // A button keeps the name of its code, and every code is one of the declared types.
    const Channel *button = m.device(13)->bySlug("button1");
    const Json press = Json::parse(eventPayload(*button, 12, "ShortPressRelease", Json(), 5));
    CHECK(press["event_type"] == "short_press_release");
    bool declared = false;
    for (const std::string &type : buttonEventTypes()) declared = declared || press["event_type"] == type;
    CHECK(declared);

    // Home Assistant's notify entity publishes the bare text.
    const Channel *mail = m.device(15)->bySlug("mail");
    ParsedValue p = parseChannelPayload(*mail, "Dinner is ready");
    CHECK(p.ok && p.value == (Json{{"body", "Dinner is ready"}}));
    p = parseChannelPayload(*mail, "Dinner\n");
    CHECK(p.ok && p.value == (Json{{"body", "Dinner"}}));
    p = parseChannelPayload(*mail, "\"quoted\"");
    CHECK(p.ok && p.value == (Json{{"body", "quoted"}}));
    p = parseChannelPayload(*mail, "42"); // JSON, but not a message object: the text as sent
    CHECK(p.ok && p.value == (Json{{"body", "42"}}));
    p = parseChannelPayload(*mail, "{\"subject\":\"Hi\",\"body\":\"there\"}");
    CHECK(p.ok && p.value == (Json{{"subject", "Hi"}, {"body", "there"}}));
    CHECK(!parseChannelPayload(*mail, "").ok);
}

} // namespace

int main()
{
    testGoldens();
    testNothingToShow();
    testPayloadIgnoresValues();
    testRoomAndRename();
    testRemoval();
    testIds();
    testEventsAndCommandsThatBackTheMapping();
    if (g_failures) {
        std::printf("discovery_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("discovery_tests: ok\n");
    return 0;
}
