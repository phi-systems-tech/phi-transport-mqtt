#pragma once

// The tenant's house as this transport sees it: adapters, devices with their
// channels, rooms and scenes, parsed from what core's list calls and events
// carry. A plain in-memory model with no broker and no core in it - the
// topic mapping and the payloads are built from it elsewhere (payload.h), so
// a second publisher (the Home Assistant discovery) reads the same picture.

#include "config.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace phi::endpoint {

using Json = nlohmann::json;

// The numbers core's JSON carries, from phi-adapter-sdk/v1/types.h. Spelled
// out here because the wire is the contract and this package does not build
// against the SDK.
namespace kind {
inline constexpr int PowerOnOff = 1;
inline constexpr int ButtonEvent = 2;
inline constexpr int Brightness = 10;
inline constexpr int ColorTemperature = 11;
inline constexpr int ColorRGB = 12;
inline constexpr int Volume = 30;
// 31..40: Mute, HdmiInput, PlayPause and the Media* readings - the media player set (phase (d)).
inline constexpr int Mute = 31;
inline constexpr int MediaPlayedOn = 40;
inline constexpr int Temperature = 50;
inline constexpr int Humidity = 51;
inline constexpr int Illuminance = 52;
inline constexpr int Motion = 53;
inline constexpr int Battery = 54;
inline constexpr int CO2 = 55;
inline constexpr int ConnectivityStatus = 57;
inline constexpr int SignalStrength = 59;
inline constexpr int Power = 60;
inline constexpr int Voltage = 61;
inline constexpr int Current = 62;
inline constexpr int Energy = 63;
inline constexpr int LinkQuality = 64;
inline constexpr int Contact = 66;
inline constexpr int Tamper = 67;
inline constexpr int SceneTrigger = 300;
inline constexpr int VideoStream = 420;
inline constexpr int Snapshot = 421;
inline constexpr int DoorbellRing = 422;
inline constexpr int ObjectDetected = 423;
// 424..426: PtzMove, PtzPreset, NightVision - camera controls (phase (d)).
inline constexpr int PtzMove = 424;
inline constexpr int NightVision = 426;
inline constexpr int Presence = 440;
inline constexpr int Message = 460;
inline constexpr int RemoteKey = 480;
} // namespace kind

// DeviceClass numbers (v1/types.h).
namespace deviceClass {
inline constexpr int Light = 1;
inline constexpr int Plug = 5;
} // namespace deviceClass

namespace dataType {
inline constexpr int Unknown = 0;
inline constexpr int Bool = 1;
inline constexpr int Int = 2;
inline constexpr int Float = 3;
inline constexpr int String = 4;
inline constexpr int Color = 5;
inline constexpr int Enum = 6;
inline constexpr int Json = 7;
inline constexpr int Flags = 8;
} // namespace dataType

namespace channelFlag {
inline constexpr std::uint32_t Writable = 0x2;
inline constexpr std::uint32_t Momentary = 0x100;
} // namespace channelFlag

struct Channel {
    int id = 0;
    std::string externalId;
    std::string name;
    /// The key in the device JSON and the level in the channel topics.
    std::string slug;
    int kind = 0;
    std::string kindName;
    int dataType = 0;
    std::string dataTypeName;
    std::uint32_t flags = 0;
    std::string unit;
    double min = 0, max = 0, step = 0;
    /// metaAdapter.enumMap: the adapter's own token for each value -> number.
    Json enumMap = Json::object();

    bool hasValue = false;
    Json value;
    /// lastValueName / valueName: an enum value's name, empty when there is none.
    std::string valueName;
    /// lastValueFlags / valueFlags: the names of the flags set in a mask, null when core sent none.
    Json valueFlags;

    bool writable() const { return (flags & channelFlag::Writable) != 0; }
    bool momentary() const { return (flags & channelFlag::Momentary) != 0; }
    /// core's history class Event (historyclass.h): an occurrence, not a state.
    bool isEvent() const
    {
        return kind == kind::ButtonEvent || kind == kind::SceneTrigger || kind == kind::DoorbellRing
            || kind == kind::ObjectDetected;
    }
    /// Whether the channel has a retained state topic and a key in the device JSON.
    bool hasState() const { return !isEvent() && !momentary() && kind != kind::Message; }
};

struct Device {
    int id = 0;
    int adapterId = 0;
    std::string externalId;
    std::string name;
    /// What the device says about itself; any of them may be empty.
    std::string manufacturer, model, firmware;
    int deviceClass = 0;
    std::string className;
    int userRoomId = 0, adapterRoomId = 0;
    /// The names core resolved when it serialized the device; used while the room list does not know the room.
    std::string userRoomName, adapterRoomName;
    std::vector<Channel> channels; // by channel id

    /// The household's room wins over the adapter's.
    int roomId() const { return userRoomId > 0 ? userRoomId : adapterRoomId; }
    const Channel *bySlug(const std::string &slug) const;
};

struct Adapter {
    int id = 0;
    std::string pluginType;
    std::string externalId;
    bool connected = true; // core states the link only once it knows it
};

struct Room {
    int id = 0;
    std::string name;
    std::string className;
    int floorId = 0;
};

struct Scene {
    int id = 0;
    std::string name;
    int roomId = 0;
};

/// What a change touched, so a publisher refreshes exactly that.
struct Touched {
    std::set<int> devices, rooms, scenes;
    /// The shape of the house changed (bridge/devices is stale).
    bool topology = false;

    void merge(const Touched &other);
    bool empty() const { return devices.empty() && rooms.empty() && scenes.empty() && !topology; }
};

/// `externalId` lowercased, anything outside [a-z0-9_-] turned into `_`.
std::string slugFor(const std::string &externalId, int channelId);

/// Words the device JSON or the topic tree already uses: a channel slug may not take them.
bool reservedSlug(const std::string &slug);

/// Gives every channel its slug: the plain one for the lowest channel id, the
/// plain one plus `_<channelId>` for the rest of a collision.
void assignSlugs(std::vector<Channel> &channels);

Channel channelFromJson(const Json &json);
Device deviceFromJson(const Json &device, const Json &channels);
Adapter adapterFromJson(const Json &json);
Room roomFromJson(const Json &json);
Scene sceneFromJson(const Json &json);

class Model
{
public:
    void setConfig(const Config &config) { m_config = config; }

    // Snapshots (the list calls): what is not in them is gone.
    Touched applyAdapters(const Json &adapters);
    Touched applyDevices(const Json &entries);
    Touched applyRooms(const Json &rooms);
    Touched applyScenes(const Json &scenes);

    // Events.
    Touched upsertAdapter(const Json &adapter);
    Touched removeAdapter(int adapterId);
    Touched setAdapterConnected(int adapterId, bool connected);
    /// `channels` null: unchanged, not none.
    Touched upsertDevice(const Json &device, const Json &channels);
    Touched removeDevice(int deviceId);
    Touched upsertRoom(const Json &room);
    Touched removeRoom(int roomId);
    Touched upsertScene(const Json &scene);

    struct StateChange {
        const Device *device = nullptr;
        const Channel *channel = nullptr;
    };
    /// Records a channel's new value. An event kind keeps nothing (an occurrence is not a state), but is found all the same.
    StateChange applyState(int channelId, const Json &value, const std::string &valueName, const Json &valueFlags);
    /// A failed command's rollback value becomes the channel's value again.
    StateChange applyRollback(int channelId, const Json &value);
    StateChange findChannel(int channelId) const;

    const Device *device(int id) const;
    const Room *room(int id) const;
    const Scene *scene(int id) const;
    const Adapter *adapter(int id) const;
    const std::map<int, Device> &devices() const { return m_devices; }
    const std::map<int, Room> &rooms() const { return m_rooms; }
    const std::map<int, Scene> &scenes() const { return m_scenes; }

    /// Whether the device is left off the broker (excludeAdapters).
    bool excluded(const Device &device) const;
    bool adapterConnected(int adapterId) const;
    /// The name of the device's room; empty when it has none.
    std::string roomNameOf(const Device &device) const;
    std::string roomName(int roomId) const;

private:
    Channel *locate(int channelId);
    void indexChannels(const Device &device);
    void unindexChannels(const Device &device);
    Touched devicesInRoom(int roomId) const;
    Touched scenesInRoom(int roomId) const;

    Config m_config;
    std::map<int, Adapter> m_adapters;
    std::map<int, Device> m_devices;
    std::map<int, Room> m_rooms;
    std::map<int, Scene> m_scenes;
    std::unordered_map<int, int> m_deviceOfChannel;
};

} // namespace phi::endpoint
