// The mapping without a broker: slugs, payload shapes per data type, what is
// state and what is an event, the device JSON, command parsing, the topic
// parser, the config, the model's events and the retained ledger.

#include "command.h"
#include "config.h"
#include "managementcard.h"
#include "model.h"
#include "payload.h"
#include "retained.h"
#include "topics.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <set>

using namespace phi::endpoint;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++g_failures;                                                               \
        }                                                                               \
    } while (false)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        const auto va = (a);                                                            \
        const auto vb = (b);                                                            \
        if (!(va == vb)) {                                                              \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);          \
            std::cout << "  left:  " << Json(va).dump() << "\n  right: " << Json(vb).dump() << "\n"; \
            ++g_failures;                                                               \
        }                                                                               \
    } while (false)

constexpr int Readable = 1, Writable = 2, Reportable = 4, Momentary = 0x100;

Json ch(int id, const char *externalId, int kind, int dataType, int flags, Json lastValue = nullptr,
        const char *valueName = nullptr)
{
    Json j = {{"id", id}, {"externalId", externalId}, {"name", externalId}, {"kind", kind},
              {"kindName", "Kind" + std::to_string(kind)}, {"dataType", dataType},
              {"dataTypeName", "Type" + std::to_string(dataType)}, {"flags", flags},
              {"unit", ""}, {"min", 0.0}, {"max", 0.0}, {"step", 0.0}, {"metaAdapter", Json::object()}};
    j["hasValue"] = !lastValue.is_null();
    if (!lastValue.is_null()) {
        j["lastValue"] = lastValue;
        if (valueName) j["lastValueName"] = valueName;
    }
    return j;
}

Json deviceEntry(int id, int adapterId, const char *name, int roomId, const char *roomName, Json channels)
{
    Json d = {{"id", id}, {"adapterId", adapterId}, {"externalId", "ext" + std::to_string(id)}, {"name", name},
              {"deviceClass", 1}, {"deviceClassName", "Light"}, {"userRoomId", roomId}, {"adapterRoomId", 0},
              {"userRoomName", roomId ? Json(roomName) : Json()}, {"adapterRoomName", nullptr}};
    return Json{{"device", d}, {"channels", channels}};
}

Json adapterJson(int id, const char *plugin, bool connected)
{
    return {{"id", id}, {"pluginType", plugin}, {"externalId", std::string(plugin) + "1"}, {"connected", connected}};
}

// A kitchen light with a bit of everything, a sensor without a room, and a
// device of a plugin the config leaves out.
Json house()
{
    Json colour = {{"r", 1.0}, {"g", 0.5}, {"b", 0.0}};
    Json power = ch(100, "power", 1, 1, Readable | Writable | Reportable, true);
    Json bri = ch(101, "brightness", 10, 2, Readable | Writable | Reportable, 80);
    bri["unit"] = "%";
    bri["max"] = 100.0;
    bri["step"] = 1.0;
    Json enumCh = ch(103, "power-on behaviour", 70, 6, Readable | Writable | Reportable, 2, "On");
    enumCh["metaAdapter"]["enumMap"] = {{"Off", 0}, {"On", 2}, {"Previous", 3}};
    Json fault = ch(107, "fault", 71, 8, Readable | Reportable, 5);
    fault["lastValueFlags"] = {"OverCurrent", "OverTemperature"};
    Json mail = ch(109, "mail", 460, 7, Writable);
    Json light = Json::array({power, bri, ch(102, "color", 12, 5, Readable | Writable | Reportable, colour), enumCh,
                              ch(104, "button1", 2, 2, Readable | Reportable, 12, "ShortPressRelease"),
                              ch(105, "Power", 60, 3, Readable | Reportable, 12.5),
                              ch(106, "identify", 480, 1, Writable | Momentary),
                              fault,
                              ch(108, "set", 66, 1, Readable | Reportable, false),
                              mail,
                              ch(111, "label", 402, 4, Readable | Writable | Reportable, "hello")});
    return {
        {"adapters", Json::array({adapterJson(1, "hue", true), adapterJson(2, "z2m", true), adapterJson(3, "tuya", true)})},
        {"rooms", Json::array({{{"id", 5}, {"name", "Kitchen"}, {"roomClassName", "Kitchen"}, {"floorId", 1}}})},
        {"scenes", Json::array({{{"id", 20}, {"name", "Evening"}, {"roomId", 5}}})},
        {"devices", Json::array({deviceEntry(10, 1, "Kitchen light", 5, "Kitchen", light),
                                 deviceEntry(11, 2, "Hall sensor", 0, "", Json::array({ch(110, "temperature", 50, 3, Readable | Reportable, 21.5)})),
                                 deviceEntry(12, 3, "Tuya plug", 0, "", Json::array({ch(120, "power", 1, 1, Readable | Writable | Reportable, true)}))})},
    };
}

Config configWith(std::vector<std::string> excluded = {})
{
    Config c;
    c.excludeAdapters = std::move(excluded);
    return c;
}

Model modelOf(const Json &h, const Config &config = configWith({"tuya"}))
{
    Model m;
    m.setConfig(config);
    m.applyAdapters(h["adapters"]);
    m.applyRooms(h["rooms"]);
    m.applyScenes(h["scenes"]);
    m.applyDevices(h["devices"]);
    return m;
}

const Channel *channelOf(const Model &m, int deviceId, const std::string &slug)
{
    const Device *d = m.device(deviceId);
    return d ? d->bySlug(slug) : nullptr;
}

// --- slugs ---------------------------------------------------------------

void testSlugs()
{
    CHECK_EQ(slugFor("Color Temp", 1), std::string("color_temp"));
    CHECK_EQ(slugFor("power-on_behaviour", 1), std::string("power-on_behaviour"));
    CHECK_EQ(slugFor("a/b#c+d", 1), std::string("a_b_c_d"));
    CHECK_EQ(slugFor("\xC3\x84x", 1), std::string("__x")); // each byte of a non-ASCII letter
    CHECK_EQ(slugFor("", 7), std::string("channel_7"));

    const Model m = modelOf(house());
    const Device *light = m.device(10);
    CHECK(light != nullptr);
    CHECK(light->bySlug("power") && light->bySlug("power")->id == 100);   // lowest id keeps the plain slug
    CHECK(light->bySlug("power_105") && light->bySlug("power_105")->id == 105);
    CHECK(light->bySlug("set") == nullptr);                               // the command level is not a channel
    CHECK(light->bySlug("set_108") && light->bySlug("set_108")->id == 108);
    CHECK(light->bySlug("power-on_behaviour") != nullptr);

    std::vector<Channel> clash(3);
    clash[0].id = 9; clash[0].externalId = "name";
    clash[1].id = 3; clash[1].externalId = "x";
    clash[2].id = 5; clash[2].externalId = "X";
    assignSlugs(clash);
    std::set<std::string> slugs;
    for (const Channel &c : clash) slugs.insert(c.slug);
    CHECK(slugs.size() == 3);
    CHECK(slugs.count("x") && slugs.count("x_5") && slugs.count("name_9"));
}

void testSnakeCase()
{
    CHECK_EQ(snakeCase("ShortPressRelease"), std::string("short_press_release"));
    CHECK_EQ(snakeCase("InitialPress"), std::string("initial_press"));
    CHECK_EQ(snakeCase("LongPressRelease"), std::string("long_press_release"));
    CHECK_EQ(snakeCase("NFCTag"), std::string("nfc_tag"));
    CHECK_EQ(snakeCase("already_snake"), std::string("already_snake"));
    CHECK_EQ(snakeCase("Two Words"), std::string("two_words"));
}

// --- payloads ------------------------------------------------------------

void testStateShapes()
{
    const Model m = modelOf(house());
    const Topics t("phi");
    const TopicMap topics = deviceTopics(t, m, *m.device(10));

    CHECK_EQ(topics.at("phi/device/10/power"), std::string("true"));
    CHECK_EQ(topics.at("phi/device/10/brightness"), std::string("80"));
    CHECK_EQ(topics.at("phi/device/10/power_105"), std::string("12.5"));
    CHECK_EQ(topics.at("phi/device/10/label"), std::string("\"hello\""));
    CHECK_EQ(Json::parse(topics.at("phi/device/10/color")), (Json{{"r", 1.0}, {"g", 0.5}, {"b", 0.0}}));
    // An enum is its name when core has one ...
    CHECK_EQ(topics.at("phi/device/10/power-on_behaviour"), std::string("\"On\""));
    // ... and a flags mask the names of its flags.
    CHECK_EQ(Json::parse(topics.at("phi/device/10/fault")), (Json::array({"OverCurrent", "OverTemperature"})));
    CHECK_EQ(topics.at("phi/device/10/availability"), std::string("online"));

    // Without a name an enum is the number, and a mask without names too.
    Channel e;
    e.dataType = dataType::Enum;
    e.hasValue = true;
    e.value = 3;
    CHECK_EQ(stateValue(e), Json(3));
    Channel f;
    f.dataType = dataType::Flags;
    f.hasValue = true;
    f.value = 6;
    CHECK_EQ(stateValue(f), Json(6));
}

void testEventsAreNotState()
{
    const Model m = modelOf(house());
    const Topics t("phi");
    const TopicMap topics = deviceTopics(t, m, *m.device(10));
    // The button, the momentary key and the message have no state topic and no key in the device JSON.
    for (const char *slug : {"button1", "identify", "mail"}) {
        CHECK(topics.count(std::string("phi/device/10/") + slug) == 0);
        CHECK(!Json::parse(topics.at("phi/device/10")).contains(slug));
    }
    CHECK(channelOf(m, 10, "button1")->isEvent());
    CHECK(channelOf(m, 10, "identify")->momentary());
    CHECK(!channelOf(m, 10, "identify")->hasState());
    CHECK(!channelOf(m, 10, "mail")->hasState());
    CHECK(channelOf(m, 10, "power")->hasState());

    // Core's history class Event: these four kinds.
    for (const int kind : {2, 300, 422, 423}) {
        Channel c;
        c.kind = kind;
        CHECK(c.isEvent());
    }
    for (const int kind : {1, 66, 53, 480, 460}) {
        Channel c;
        c.kind = kind;
        CHECK(!c.isEvent());
    }

    const Channel &button = *channelOf(m, 10, "button1");
    CHECK_EQ(Json::parse(eventPayload(button, 12, "ShortPressRelease", Json(), 1730000000123)),
             (Json{{"event_type", "short_press_release"}, {"code", 12}, {"ts", 1730000000123}}));
    CHECK_EQ(Json::parse(eventPayload(button, 1, "InitialPress", Json(), 5)),
             (Json{{"event_type", "initial_press"}, {"code", 1}, {"ts", 5}}));
    // A doorbell is the same code set; an object detection names the classes it saw; a trigger without a name is just an event.
    CHECK_EQ(Json::parse(eventPayload(button, 12, "ShortPressRelease", Json(), 5))["event_type"], Json("short_press_release"));
    CHECK_EQ(Json::parse(eventPayload(button, 3, "Person", Json::array({"Person", "Vehicle"}), 9)),
             (Json{{"event_type", "person"}, {"code", 3}, {"flags", Json::array({"Person", "Vehicle"})}, {"ts", 9}}));
    CHECK_EQ(Json::parse(eventPayload(button, 7, "", Json(), 9)), (Json{{"event_type", "event"}, {"code", 7}, {"ts", 9}}));
    CHECK_EQ(Json::parse(eventPayload(button, "movie night", "", Json(), 9)), (Json{{"event_type", "movie_night"}, {"ts", 9}}));
}

void testDeviceJson()
{
    const Model m = modelOf(house());
    const Json j = deviceJson(m, *m.device(10));
    CHECK_EQ(j["name"], Json("Kitchen light"));
    CHECK_EQ(j["room"], Json("Kitchen"));
    CHECK_EQ(j["roomId"], Json(5));
    CHECK_EQ(j["class"], Json("Light"));
    CHECK_EQ(j["brightness"], Json(80));
    CHECK_EQ(j["power"], Json(true));
    CHECK_EQ(j["set_108"], Json(false));
    CHECK_EQ(j["fault"], (Json::array({"OverCurrent", "OverTemperature"})));
    CHECK_EQ(j.size(), std::size_t(3 + 1 + 8)); // name, room, roomId, class + 8 state channels (power, brightness, color, behaviour, power_105, fault, set_108, label)

    // A device without a room says null, not an empty name.
    const Json hall = deviceJson(m, *m.device(11));
    CHECK(hall["room"].is_null() && hall["roomId"].is_null());
    CHECK_EQ(hall["temperature"], Json(21.5));

    // A channel that has not reported has no key and no topic.
    Json h = house();
    h["devices"][1]["channels"][0].erase("lastValue");
    h["devices"][1]["channels"][0]["hasValue"] = false;
    const Model m2 = modelOf(h);
    CHECK(!deviceJson(m2, *m2.device(11)).contains("temperature"));
    CHECK(deviceTopics(Topics("phi"), m2, *m2.device(11)).count("phi/device/11/temperature") == 0);
}

void testRoomSceneBridge()
{
    const Model m = modelOf(house());
    const Topics t("phi");
    CHECK_EQ(Json::parse(roomTopics(t, m, *m.room(5)).at("phi/room/5")), (Json{{"name", "Kitchen"}, {"class", "Kitchen"}, {"floor", 1}}));
    CHECK_EQ(Json::parse(sceneTopics(t, m, *m.scene(20)).at("phi/scene/20")), (Json{{"name", "Evening"}, {"room", "Kitchen"}, {"roomId", 5}}));

    BridgeInfo info{"0.1.0", "2.2.0", "Home", "1"};
    const TopicMap bridge = bridgeTopics(t, m, info);
    CHECK_EQ(Json::parse(bridge.at("phi/bridge/info")), (Json{{"version", "0.1.0"}, {"sdk", "2.2.0"}, {"instance", "Home"}, {"tenant", "1"}}));
    const Json devices = Json::parse(bridge.at("phi/bridge/devices"));
    CHECK(devices.is_array());
    CHECK_EQ(devices.size(), std::size_t(2)); // the tuya plug is excluded
    CHECK_EQ(devices[0]["id"], Json(10));
    CHECK_EQ(devices[0]["room"], Json("Kitchen"));
    const Json &channels = devices[0]["channels"];
    const auto find = [&](int id) -> const Json * {
        for (const Json &c : channels)
            if (c["id"] == id) return &c;
        return nullptr;
    };
    const Json *bri = find(101);
    CHECK(bri != nullptr);
    CHECK_EQ((*bri)["slug"], Json("brightness"));
    CHECK_EQ((*bri)["unit"], Json("%"));
    CHECK_EQ((*bri)["min"], Json(0.0));
    CHECK_EQ((*bri)["max"], Json(100.0));
    CHECK_EQ((*bri)["step"], Json(1.0));
    CHECK_EQ((*bri)["writable"], Json(true));
    CHECK_EQ((*bri)["momentary"], Json(false));
    CHECK_EQ((*bri)["event"], Json(false));
    CHECK_EQ((*find(104))["event"], Json(true));
    CHECK_EQ((*find(104))["writable"], Json(false));
    CHECK_EQ((*find(106))["momentary"], Json(true));
    CHECK((*find(105))["min"].is_null() && (*find(105))["unit"].is_null());
}

// --- exclusion and removal --------------------------------------------------

void testExclusionClearsEverythingPublished()
{
    const Topics t("phi");
    Json h = house();
    // The tuya plug is published when nothing is excluded ...
    const Model open = modelOf(h, configWith());
    Retained ledger;
    ledger.replayAll(); // online
    const std::vector<Message> first = ledger.set("device/12", deviceTopics(t, open, *open.device(12)));
    CHECK_EQ(first.size(), std::size_t(3)); // device JSON, availability, power
    for (const Message &msg : first) CHECK(msg.retain && !msg.payload.empty());

    // ... and when it is excluded, the same group is desired empty: every topic is cleared.
    const Model closed = modelOf(h, configWith({"tuya"}));
    CHECK(deviceTopics(t, closed, *closed.device(12)).empty());
    const std::vector<Message> cleared = ledger.set("device/12", deviceTopics(t, closed, *closed.device(12)));
    CHECK_EQ(cleared.size(), std::size_t(3));
    std::set<std::string> topics;
    for (const Message &msg : cleared) {
        CHECK(msg.retain && msg.payload.empty());
        topics.insert(msg.topic);
    }
    CHECK((topics == std::set<std::string>{"phi/device/12", "phi/device/12/availability", "phi/device/12/power"}));
    CHECK(ledger.topicCount() == 0);

    // Excluding by adapter id works the same way.
    const Model byId = modelOf(h, configWith({"3"}));
    CHECK(byId.excluded(*byId.device(12)) && !byId.excluded(*byId.device(10)));
}

void testRemovalClearsChannelsThatDisappear()
{
    const Topics t("phi");
    Model m = modelOf(house());
    Retained ledger;
    ledger.replayAll();
    ledger.set("device/10", deviceTopics(t, m, *m.device(10)));

    // The device event brings fewer channels: the topics of the ones that went are cleared.
    Json fewer = house()["devices"][0];
    fewer["channels"] = Json::array({fewer["channels"][0]});
    m.upsertDevice(fewer["device"], fewer["channels"]);
    const std::vector<Message> messages = ledger.set("device/10", deviceTopics(t, m, *m.device(10)));
    std::size_t clears = 0;
    for (const Message &msg : messages) {
        if (msg.payload.empty()) {
            ++clears;
            CHECK(msg.topic != "phi/device/10" && msg.topic != "phi/device/10/power");
        }
    }
    CHECK_EQ(clears, std::size_t(7)); // brightness, color, power-on_behaviour, power_105, fault, set_108, label

    // The device itself removed: everything goes.
    m.removeDevice(10);
    const std::vector<Message> gone = ledger.set("device/10", m.device(10) ? deviceTopics(t, m, *m.device(10)) : TopicMap());
    for (const Message &msg : gone) CHECK(msg.payload.empty() && msg.retain);
    CHECK(ledger.topicCount() == 0);
}

// --- commands ----------------------------------------------------------------

void testCommandParsing()
{
    const Model m = modelOf(house());
    const Channel &power = *channelOf(m, 10, "power");
    const Channel &bri = *channelOf(m, 10, "brightness");
    const Channel &watts = *channelOf(m, 10, "power_105");
    const Channel &label = *channelOf(m, 10, "label");
    const Channel &colour = *channelOf(m, 10, "color");
    const Channel &behaviour = *channelOf(m, 10, "power-on_behaviour");
    const Channel &fault = *channelOf(m, 10, "fault");
    const Channel &mail = *channelOf(m, 10, "mail");

    // Bool: JSON, bare text in any case, numbers.
    for (const char *on : {"true", "ON", "on", "On", "1", "\"ON\"", " true\n", "2"}) {
        const ParsedValue p = parseChannelPayload(power, on);
        CHECK(p.ok && p.value == Json(true));
    }
    for (const char *off : {"false", "OFF", "off", "0", "\"off\"", "0.0"}) {
        const ParsedValue p = parseChannelPayload(power, off);
        CHECK(p.ok && p.value == Json(false));
    }
    CHECK(!parseChannelPayload(power, "maybe").ok);
    CHECK(!parseChannelPayload(power, "").ok);
    CHECK(!parseChannelPayload(power, "{\"a\":1}").ok);

    // Int and Float.
    CHECK(parseChannelPayload(bri, "50").ok && parseChannelPayload(bri, "50").value == Json(50));
    CHECK(parseChannelPayload(bri, "50.0").ok && parseChannelPayload(bri, "50.0").value == Json(50));
    CHECK(parseChannelPayload(bri, "\"50\"").ok);
    CHECK(!parseChannelPayload(bri, "50.5").ok);
    CHECK(!parseChannelPayload(bri, "ON").ok);
    CHECK(parseChannelPayload(watts, "12.25").ok && parseChannelPayload(watts, "12.25").value == Json(12.25));
    CHECK(parseChannelPayload(watts, "-3").ok);
    CHECK(!parseChannelPayload(watts, "hot").ok);

    // String: bare text is the string, JSON strings are unquoted, a number stays its text.
    CHECK(parseChannelPayload(label, "Dinner is ready").value == Json("Dinner is ready"));
    CHECK(parseChannelPayload(label, "\"Dinner\"").value == Json("Dinner"));
    CHECK(parseChannelPayload(label, "42").value == Json("42"));
    CHECK(parseChannelPayload(label, "line\n").value == Json("line"));

    // Colour: the object core delivers, "r,g,b", both in 0..1.
    const Json warm = {{"r", 1.0}, {"g", 0.5}, {"b", 0.25}};
    CHECK(parseChannelPayload(colour, "{\"r\":1,\"g\":0.5,\"b\":0.25}").value == warm);
    CHECK(parseChannelPayload(colour, "1,0.5,0.25").value == warm);
    CHECK(parseChannelPayload(colour, "1, 0.5, 0.25").value == warm);
    CHECK(parseChannelPayload(colour, "\"1,0.5,0.25\"").value == warm);
    CHECK(!parseChannelPayload(colour, "1,0.5").ok);
    CHECK(!parseChannelPayload(colour, "255,0,0").ok);
    CHECK(!parseChannelPayload(colour, "{\"r\":1,\"g\":0.5}").ok);
    CHECK(!parseChannelPayload(colour, "red").ok);

    // Enum: the number, or a name from the adapter's enumMap.
    CHECK(parseChannelPayload(behaviour, "3").value == Json(3));
    CHECK(parseChannelPayload(behaviour, "Previous").value == Json(3));
    CHECK(parseChannelPayload(behaviour, "\"previous\"").value == Json(3));
    CHECK(!parseChannelPayload(behaviour, "Sideways").ok);
    // Flags: the mask.
    CHECK(parseChannelPayload(fault, "5").value == Json(5));
    CHECK(!parseChannelPayload(fault, "-1").ok);
    CHECK(!parseChannelPayload(fault, "OverCurrent").ok);
    // A composite value is an object.
    CHECK(parseChannelPayload(mail, "{\"subject\":\"Hi\",\"body\":\"There\"}").ok);
    // ... except a Message, which takes the bare text as its body (a Home Assistant notify entity sends nothing else).
    CHECK(parseChannelPayload(mail, "Hi there").value == (Json{{"body", "Hi there"}}));

    // device/<id>/set keeps the sender's order.
    std::vector<std::pair<std::string, Json>> items;
    std::string error;
    CHECK(parseDeviceSet("{\"power\":true,\"brightness\":40,\"color\":{\"r\":1,\"g\":0,\"b\":0},\"a\":1}", &items, &error));
    CHECK_EQ(items.size(), std::size_t(4));
    CHECK(items[0].first == "power" && items[1].first == "brightness" && items[2].first == "color" && items[3].first == "a");
    CHECK(parseChannelJson(power, items[0].second).ok);
    CHECK(parseChannelJson(colour, items[2].second).ok);
    items.clear();
    CHECK(!parseDeviceSet("[1,2]", &items, &error) && !error.empty());
    CHECK(!parseDeviceSet("ON", &items, &error));

    CHECK(parseSceneSet("ON") && parseSceneSet("on") && parseSceneSet("true") && parseSceneSet("\"ON\"") && parseSceneSet(" ON\n"));
    CHECK(!parseSceneSet("OFF") && !parseSceneSet("") && !parseSceneSet("1x"));
}

void testTopics()
{
    const Topics t("phi");
    CHECK_EQ(t.bridgeState(), std::string("phi/bridge/state"));
    CHECK_EQ(t.channelSet(10, "power"), std::string("phi/device/10/power/set"));
    CHECK_EQ(t.sceneSet(20), std::string("phi/scene/20/set"));
    CHECK((t.subscriptions() == std::vector<std::string>{"phi/device/+/+/set", "phi/device/+/set", "phi/scene/+/set"}));

    auto c = t.parseCommand("phi/device/10/power/set");
    CHECK(c.kind == Topics::Command::Kind::ChannelSet && c.id == 10 && c.slug == "power");
    c = t.parseCommand("phi/device/10/set");
    CHECK(c.kind == Topics::Command::Kind::DeviceSet && c.id == 10);
    c = t.parseCommand("phi/scene/20/set");
    CHECK(c.kind == Topics::Command::Kind::SceneSet && c.id == 20);

    // What is not a command: state topics, other bases, hostile ids.
    for (const char *topic : {"phi/device/10", "phi/device/10/power", "phi/device/10/availability", "phi/bridge/state",
                              "phi/device/+/set", "phi/device/0/set", "phi/device/-1/set", "phi/device/1e3/set",
                              "phi/device/ 5/set", "phi/device/5x/set", "phi/device/99999999999/set",
                              "phi/device//set", "phi/device/10//set", "phi/device/10/a/b/set", "phi/room/5/set",
                              "other/device/10/set", "phi", "phi/", "phiX/device/10/set", "phi/device/10/../set"}) {
        CHECK(t.parseCommand(topic).kind == Topics::Command::Kind::None);
    }
    // A nested base works.
    const Topics nested("home/phi");
    CHECK(nested.parseCommand("home/phi/device/3/set").kind == Topics::Command::Kind::DeviceSet);
    CHECK(nested.parseCommand("phi/device/3/set").kind == Topics::Command::Kind::None);
}

// --- config -------------------------------------------------------------------

void testConfig()
{
    Config c;
    std::string error;
    CHECK(parseConfig("{\"hosting\":\"sidecar\",\"stateDir\":\"/var/lib/phi/@1/transports/mqtt/state\",\"baseTopic\":\"phi\",\"readOnly\":false,\"excludeAdapters\":[]}", &c, &error));
    CHECK(c.baseTopic == "phi" && !c.readOnly && c.excludeAdapters.empty() && !c.broker && c.discovery.homeAssistant);
    CHECK(c.discovery.prefix == "homeassistant");

    CHECK(parseConfig("{}", &c, &error) && c.baseTopic == "phi");
    CHECK(parseConfig("{\"readOnly\":true,\"excludeAdapters\":[\"tuya\",7]}", &c, &error));
    CHECK(c.readOnly && c.excludes("tuya", 1) && c.excludes("hue", 7) && !c.excludes("hue", 1));

    CHECK(parseConfig("{\"baseTopic\":\"home/phi\",\"broker\":{\"host\":\"192.168.1.5\",\"username\":\"u\",\"password\":\"p\"},\"discovery\":{\"homeAssistant\":true,\"prefix\":\"ha\"}}", &c, &error));
    CHECK(c.broker && c.broker->host == "192.168.1.5" && c.broker->port == 1883 && !c.broker->tls);
    CHECK(c.broker->username == "u" && c.broker->password == "p");
    CHECK(c.baseTopic == "home/phi" && c.discovery.homeAssistant && c.discovery.prefix == "ha");

    CHECK(parseConfig("{\"broker\":{\"host\":\"b\",\"tls\":{\"caFile\":\"/etc/ca.pem\"}}}", &c, &error));
    CHECK(c.broker->tls && c.broker->port == 8883 && c.broker->caFile == "/etc/ca.pem");
    CHECK(parseConfig("{\"broker\":{\"host\":\"b\",\"port\":9000,\"tls\":{}}}", &c, &error) && c.broker->port == 9000 && c.broker->tls);

    // Refusals say why.
    for (const char *bad : {"[]", "not json", "{\"baseTopic\":\"\"}", "{\"baseTopic\":\"a/#\"}", "{\"baseTopic\":\"/a\"}",
                            "{\"baseTopic\":\"a//b\"}", "{\"baseTopic\":\"$SYS\"}", "{\"readOnly\":\"yes\"}",
                            "{\"excludeAdapters\":\"tuya\"}", "{\"excludeAdapters\":[true]}", "{\"broker\":{}}",
                            "{\"broker\":{\"host\":\"b\",\"port\":0}}", "{\"broker\":\"b\"}",
                            "{\"discovery\":{\"prefix\":\"a/+\"}}", "{\"stateDir\":3}",
                            // a prefix of its own on the box broker would be refused every publish
                            "{\"baseTopic\":\"mine\"}"}) {
        error.clear();
        CHECK(!parseConfig(bad, &c, &error));
        CHECK(!error.empty());
    }

    // core's own switch rides in the merged config and is never a reason to refuse it.
    CHECK(parseConfig("{\"enabled\":false,\"somethingNew\":{\"a\":1}}", &c, &error));
    CHECK(parseConfig("{\"enabled\":true,\"baseTopic\":\"phi\"}", &c, &error));
}

// --- the management card ---------------------------------------------------------

void testManagementSummary()
{
    ManagementState s;
    CHECK_EQ(managementSummary(s), std::string("not running"));
    s.running = true;
    CHECK_EQ(managementSummary(s), std::string("waiting for the broker account"));
    s.foreignBroker = true;
    CHECK_EQ(managementSummary(s), std::string("starting"));

    s.hasLink = true;
    s.host = "127.0.0.1";
    s.port = 1883;
    s.lastError = "connection refused";
    CHECK_EQ(managementSummary(s), std::string("broker not reachable: connection refused"));
    s.lastError.clear();
    CHECK_EQ(managementSummary(s), std::string("broker not reachable: no answer yet"));

    s.connected = true;
    s.username = "mqtt_main";
    s.devices = 76;
    s.topics = 741;
    CHECK_EQ(managementSummary(s), std::string("on the broker 127.0.0.1:1883 as mqtt_main, 76 devices, 741 topics, Home Assistant discovery on"));
    s.homeAssistant = false;
    s.readOnly = true;
    s.username.clear();
    s.devices = 1;
    s.topics = 1;
    CHECK_EQ(managementSummary(s), std::string("on the broker 127.0.0.1:1883, 1 device, 1 topic, Home Assistant discovery off, read-only"));
}

void testSettingsForm()
{
    Config c;
    std::string error;
    CHECK(parseConfig("{\"readOnly\":true,\"baseTopic\":\"home/phi\",\"broker\":{\"host\":\"b\",\"port\":9000,\"username\":\"u\",\"password\":\"secret\",\"tls\":{\"caFile\":\"/ca.pem\"}}}", &c, &error));
    const Json form = settingsForm(c);
    std::map<std::string, Json> byKey;
    std::vector<std::string> order;
    for (const Json &f : form["fields"]) {
        byKey[f["key"]] = f;
        order.push_back(f["key"]);
    }
    CHECK_EQ(order, (std::vector<std::string>{"homeAssistant", "discoveryPrefix", "readOnly", "baseTopic", "useBroker", "brokerHost",
                                              "brokerPort", "brokerUsername", "brokerPassword", "brokerTls", "brokerCaFile"}));
    CHECK_EQ(byKey["homeAssistant"]["type"], Json("boolean"));
    CHECK_EQ(byKey["homeAssistant"]["default"], Json(true));
    CHECK_EQ(byKey["discoveryPrefix"]["default"], Json("homeassistant"));
    CHECK_EQ(byKey["readOnly"]["default"], Json(true));
    CHECK_EQ(byKey["baseTopic"]["default"], Json("home/phi"));
    CHECK_EQ(byKey["useBroker"]["default"], Json(true));
    CHECK_EQ(byKey["brokerPort"]["type"], Json("integer"));
    CHECK_EQ(byKey["brokerPort"]["default"], Json(9000));
    CHECK_EQ(byKey["brokerPassword"]["type"], Json("password"));
    CHECK_EQ(byKey["brokerPassword"]["default"], Json(""));      // the stored one is never sent to a browser
    CHECK(form.dump().find("secret") == std::string::npos);
    CHECK_EQ(byKey["brokerTls"]["default"], Json(true));
    CHECK_EQ(byKey["brokerCaFile"]["default"], Json("/ca.pem"));

    // Without a broker of its own the broker fields are blank, the port the usual one.
    CHECK(parseConfig("{}", &c, &error));
    const Json plain = settingsForm(c);
    for (const Json &f : plain["fields"]) {
        if (f["key"] == "useBroker") CHECK_EQ(f["default"], Json(false));
        if (f["key"] == "brokerPort") CHECK_EQ(f["default"], Json(1883));
        if (f["key"] == "brokerHost") CHECK_EQ(f["default"], Json(""));
    }
}

void testSettingsMerge()
{
    const Json base = {{"hosting", "sidecar"}, {"baseTopic", "phi"}, {"readOnly", false}, {"discovery", {{"homeAssistant", true}, {"prefix", "homeassistant"}}}};
    Json out;
    std::string error;

    // Only the keys the form owns change; core's keys and unknown ones stay in the override.
    const Json override1 = {{"enabled", false}, {"stateDir", "/x"}, {"excludeAdapters", {"tuya"}}, {"future", {{"a", 1}}}};
    Json effective1 = base;
    effective1.merge_patch(override1);
    CHECK(mergeSettings(override1, effective1, {{"homeAssistant", false}, {"readOnly", true}, {"useBroker", false}}, &out, &error));
    CHECK_EQ(out["enabled"], Json(false));
    CHECK_EQ(out["stateDir"], Json("/x"));
    CHECK_EQ(out["excludeAdapters"], (Json{"tuya"}));
    CHECK_EQ(out["future"], (Json{{"a", 1}}));
    CHECK_EQ(out["readOnly"], Json(true));
    CHECK_EQ(out["discovery"]["homeAssistant"], Json(false));
    CHECK_EQ(out["discovery"]["prefix"], Json("homeassistant"));
    CHECK(!out.contains("broker") && !out.contains("hosting"));

    // A foreign broker, typed in: port as text, the base topic free now.
    CHECK(mergeSettings(Json::object(), base,
                        {{"useBroker", true}, {"brokerHost", " 10.0.0.5 "}, {"brokerPort", "8883"}, {"brokerUsername", "ha"},
                         {"brokerPassword", "pw"}, {"brokerTls", true}, {"brokerCaFile", "/ca.pem"}, {"baseTopic", "home/phi"}},
                        &out, &error));
    CHECK_EQ(out["broker"], (Json{{"host", "10.0.0.5"}, {"port", 8883}, {"username", "ha"}, {"password", "pw"}, {"tls", {{"caFile", "/ca.pem"}}}}));
    CHECK_EQ(out["baseTopic"], Json("home/phi"));

    // The password the form leaves empty is the stored one; the other keys of the block follow the form.
    const Json stored = {{"baseTopic", "home/phi"}, {"broker", {{"host", "b"}, {"port", 1884}, {"username", "u"}, {"password", "keep"}, {"future", 1}}}};
    Json effective2 = base;
    effective2.merge_patch(stored);
    CHECK(mergeSettings(stored, effective2, {{"useBroker", true}, {"brokerHost", "b2"}, {"brokerPort", 1885}, {"brokerUsername", ""}, {"brokerPassword", ""}, {"brokerTls", false}}, &out, &error));
    CHECK_EQ(out["broker"], (Json{{"host", "b2"}, {"port", 1885}, {"password", "keep"}, {"future", 1}}));
    // ... also when the block came from the base layer and the override has none yet.
    Json effective3 = base;
    effective3["broker"] = {{"host", "base"}, {"password", "fromBase"}};
    effective3["baseTopic"] = "x";
    CHECK(mergeSettings(Json::object(), effective3, {{"brokerPassword", ""}, {"brokerHost", "base"}}, &out, &error));
    CHECK_EQ(out["broker"]["password"], Json("fromBase"));
    // A typed password replaces it.
    CHECK(mergeSettings(stored, effective2, {{"useBroker", true}, {"brokerPassword", "new"}}, &out, &error));
    CHECK_EQ(out["broker"]["password"], Json("new"));
    CHECK_EQ(out["broker"]["host"], Json("b"));

    // The broker switched off: the block goes, the base topic must be phi again.
    CHECK(!mergeSettings(stored, effective2, {{"useBroker", false}}, &out, &error));
    CHECK(error.find("phi") != std::string::npos);
    CHECK(mergeSettings(stored, effective2, {{"useBroker", false}, {"baseTopic", "phi"}}, &out, &error));
    CHECK(!out.contains("broker"));
    CHECK_EQ(out["baseTopic"], Json("phi"));

    // Refusals, each with a sentence.
    const Json on = {{"useBroker", true}, {"brokerHost", "b"}};
    for (const Json &bad : {Json{{"useBroker", true}, {"brokerHost", "  "}}, Json{{"useBroker", true}, {"brokerHost", ""}},
                            Json{{"useBroker", true}, {"brokerHost", "b"}, {"brokerPort", 0}},
                            Json{{"useBroker", true}, {"brokerHost", "b"}, {"brokerPort", 70000}},
                            Json{{"useBroker", true}, {"brokerHost", "b"}, {"brokerPort", "http"}},
                            Json{{"baseTopic", "a/#"}}, Json{{"baseTopic", ""}}, Json{{"discoveryPrefix", "a//b"}},
                            Json{{"baseTopic", "mine"}}, Json(3)}) {
        error.clear();
        CHECK(!mergeSettings(Json::object(), base, bad, &out, &error));
        CHECK(!error.empty());
    }
    CHECK(mergeSettings(Json::object(), base, on, &out, &error));
}

// --- the model -----------------------------------------------------------------

void testModelEvents()
{
    const Topics t("phi");
    Model m = modelOf(house());

    // A state change touches the value only.
    Model::StateChange change = m.applyState(101, 30, "", Json());
    CHECK(change.channel && change.device && change.device->id == 10);
    CHECK_EQ(deviceTopics(t, m, *m.device(10)).at("phi/device/10/brightness"), std::string("30"));
    // An event changes nothing it keeps.
    change = m.applyState(104, 1, "InitialPress", Json());
    CHECK(change.channel && change.channel->isEvent());
    CHECK(m.applyState(99999, 1, "", Json()).channel == nullptr);

    // An enum value's name travels with it.
    m.applyState(103, 3, "Previous", Json());
    CHECK_EQ(deviceTopics(t, m, *m.device(10)).at("phi/device/10/power-on_behaviour"), std::string("\"Previous\""));

    // The adapter going away is the device's availability.
    Touched touched = m.setAdapterConnected(1, false);
    CHECK(touched.devices.count(10) == 1 && touched.devices.count(11) == 0);
    CHECK_EQ(deviceTopics(t, m, *m.device(10)).at("phi/device/10/availability"), std::string("offline"));
    touched = m.setAdapterConnected(1, false);
    CHECK(touched.empty());
    m.setAdapterConnected(1, true);

    // A room rename reaches the devices and scenes in it.
    touched = m.upsertRoom(Json{{"id", 5}, {"name", "Kitchenette"}, {"roomClassName", "Kitchen"}, {"floorId", 1}});
    CHECK(touched.rooms.count(5) && touched.devices.count(10) && touched.scenes.count(20) && !touched.devices.count(11));
    CHECK_EQ(deviceJson(m, *m.device(10))["room"], Json("Kitchenette"));
    CHECK(m.upsertRoom(Json{{"id", 5}, {"name", "Kitchenette"}, {"roomClassName", "Kitchen"}, {"floorId", 1}}).empty());
    touched = m.removeRoom(5);
    CHECK(touched.devices.count(10));
    // The name core resolved with the device is what is left.
    CHECK_EQ(deviceJson(m, *m.device(10))["room"], Json("Kitchen"));

    // A device event without channels keeps the ones it had.
    Json d = house()["devices"][0]["device"];
    d["name"] = "Renamed";
    m.upsertDevice(d, Json());
    CHECK(m.device(10)->name == "Renamed" && m.device(10)->channels.size() == 11);
    CHECK(m.findChannel(101).channel != nullptr);

    // A snapshot that no longer lists a device removes it, and its channels.
    Json shrunk = house()["devices"];
    shrunk.erase(shrunk.begin());
    touched = m.applyDevices(shrunk);
    CHECK(m.device(10) == nullptr && touched.devices.count(10) && touched.topology);
    CHECK(m.findChannel(101).channel == nullptr);

    // A snapshot of scenes that lacks one removes it (core says nothing when a scene goes).
    touched = m.applyScenes(Json::array());
    CHECK(touched.scenes.count(20) && m.scene(20) == nullptr);
}

void testRollback()
{
    Model m = modelOf(house());
    // The rollback value becomes the channel's value again; its old name goes with the old value.
    m.applyState(103, 3, "Previous", Json());
    m.applyRollback(103, 2);
    const Channel *c = channelOf(m, 10, "power-on_behaviour");
    CHECK(c->value == Json(2) && c->valueName.empty());
    m.applyRollback(101, 80);
    CHECK(channelOf(m, 10, "brightness")->value == Json(80));
    // A rollback of the value it already holds keeps the name.
    m.applyState(103, 2, "On", Json());
    m.applyRollback(103, 2);
    CHECK(channelOf(m, 10, "power-on_behaviour")->valueName == "On");
}

void testRetainedLedger()
{
    Retained r;
    // Offline: nothing to send, but the state is kept.
    CHECK(r.set("g", {{"a", "1"}, {"b", "2"}}).empty());
    std::vector<Message> all = r.replayAll();
    CHECK_EQ(all.size(), std::size_t(2));
    // Online: only what changed.
    CHECK(r.set("g", {{"a", "1"}, {"b", "2"}}).empty());
    std::vector<Message> m = r.set("g", {{"a", "1"}, {"b", "3"}});
    CHECK(m.size() == 1 && m[0].topic == "b" && m[0].payload == "3" && m[0].retain);
    // Forced: everything again, even unchanged.
    CHECK_EQ(r.set("g", {{"a", "1"}, {"b", "3"}}, true).size(), std::size_t(2));
    // A topic that leaves the group is cleared.
    m = r.set("g", {{"a", "1"}});
    CHECK(m.size() == 1 && m[0].topic == "b" && m[0].payload.empty() && m[0].retain);
    // A clear while the broker is away is sent at the next connect.
    r.setOffline();
    CHECK(r.drop("g").empty());
    all = r.replayAll();
    CHECK(all.size() == 1 && all[0].topic == "a" && all[0].payload.empty());
    CHECK(r.topicCount() == 0 && r.replayAll().empty());
    // ... unless the topic came back before.
    r.setOffline();
    r.set("h", {{"x", "1"}});
    r.drop("h");
    r.set("h", {{"x", "2"}});
    all = r.replayAll();
    CHECK(all.size() == 1 && all[0].topic == "x" && all[0].payload == "2");
}

} // namespace

int main()
{
    testSlugs();
    testSnakeCase();
    testStateShapes();
    testEventsAreNotState();
    testDeviceJson();
    testRoomSceneBridge();
    testExclusionClearsEverythingPublished();
    testRemovalClearsChannelsThatDisappear();
    testCommandParsing();
    testTopics();
    testConfig();
    testManagementSummary();
    testSettingsForm();
    testSettingsMerge();
    testModelEvents();
    testRollback();
    testRetainedLedger();
    if (g_failures) {
        std::printf("mqtt_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mqtt_tests: ok\n");
    return 0;
}
