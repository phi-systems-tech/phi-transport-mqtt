#include "model.h"

#include <algorithm>
#include <cctype>

namespace phi::endpoint {

namespace {

std::string strOf(const Json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int intOf(const Json &j, const char *key, int fallback = 0)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}

double numOf(const Json &j, const char *key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : 0.0;
}

bool sameRoom(const Room &a, const Room &b)
{
    return a.id == b.id && a.name == b.name && a.className == b.className && a.floorId == b.floorId;
}

bool sameScene(const Scene &a, const Scene &b)
{
    return a.id == b.id && a.name == b.name && a.roomId == b.roomId;
}

} // namespace

void Touched::merge(const Touched &other)
{
    devices.insert(other.devices.begin(), other.devices.end());
    rooms.insert(other.rooms.begin(), other.rooms.end());
    scenes.insert(other.scenes.begin(), other.scenes.end());
    topology = topology || other.topology;
}

// --- slugs -----------------------------------------------------------------

std::string slugFor(const std::string &externalId, int channelId)
{
    std::string slug;
    slug.reserve(externalId.size());
    for (const char raw : externalId) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const char lower = static_cast<char>(std::tolower(c));
        const bool ok = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') || lower == '_' || lower == '-';
        slug += ok ? lower : '_';
    }
    // A topic level cannot be empty; an adapter that names a channel with
    // nothing gets the id for a name.
    if (slug.empty()) slug = "channel_" + std::to_string(channelId);
    return slug;
}

bool reservedSlug(const std::string &slug)
{
    // `name`, `room` and `class` are the device JSON's own keys; `set` and
    // `availability` are the device topic's own levels.
    return slug == "set" || slug == "availability" || slug == "name" || slug == "room" || slug == "class";
}

void assignSlugs(std::vector<Channel> &channels)
{
    // The lowest channel id keeps the plain slug, so a channel that turns up
    // later never takes a topic a consumer already uses.
    std::sort(channels.begin(), channels.end(), [](const Channel &a, const Channel &b) { return a.id < b.id; });
    std::set<std::string> used;
    for (Channel &ch : channels) {
        std::string slug = slugFor(ch.externalId, ch.id);
        if (reservedSlug(slug) || used.count(slug)) slug += "_" + std::to_string(ch.id);
        while (used.count(slug)) slug += "_" + std::to_string(ch.id);
        used.insert(slug);
        ch.slug = std::move(slug);
    }
}

const Channel *Device::bySlug(const std::string &slug) const
{
    for (const Channel &ch : channels)
        if (ch.slug == slug) return &ch;
    return nullptr;
}

// --- parsing ---------------------------------------------------------------

Channel channelFromJson(const Json &json)
{
    Channel ch;
    if (!json.is_object()) return ch;
    ch.id = intOf(json, "id");
    ch.externalId = strOf(json, "externalId");
    ch.name = strOf(json, "name");
    ch.kind = intOf(json, "kind");
    ch.kindName = strOf(json, "kindName");
    ch.dataType = intOf(json, "dataType");
    ch.dataTypeName = strOf(json, "dataTypeName");
    ch.flags = static_cast<std::uint32_t>(intOf(json, "flags"));
    ch.unit = strOf(json, "unit");
    ch.min = numOf(json, "min");
    ch.max = numOf(json, "max");
    ch.step = numOf(json, "step");
    if (const auto meta = json.find("metaAdapter"); meta != json.end() && meta->is_object()) {
        if (const auto map = meta->find("enumMap"); map != meta->end() && map->is_object()) ch.enumMap = *map;
    }
    const auto has = json.find("hasValue");
    if (has != json.end() && has->is_boolean() && has->get<bool>()) {
        if (const auto v = json.find("lastValue"); v != json.end() && !v->is_null()) {
            ch.hasValue = true;
            ch.value = *v;
            ch.valueName = strOf(json, "lastValueName");
            if (const auto f = json.find("lastValueFlags"); f != json.end() && f->is_array()) ch.valueFlags = *f;
        }
    }
    return ch;
}

Device deviceFromJson(const Json &device, const Json &channels)
{
    Device d;
    if (!device.is_object()) return d;
    d.id = intOf(device, "id");
    d.adapterId = intOf(device, "adapterId");
    d.externalId = strOf(device, "externalId");
    d.name = strOf(device, "name");
    d.deviceClass = intOf(device, "deviceClass");
    d.className = strOf(device, "deviceClassName");
    d.userRoomId = intOf(device, "userRoomId");
    d.adapterRoomId = intOf(device, "adapterRoomId");
    d.userRoomName = strOf(device, "userRoomName");
    d.adapterRoomName = strOf(device, "adapterRoomName");
    if (channels.is_array()) {
        for (const Json &c : channels) {
            Channel ch = channelFromJson(c);
            if (ch.id > 0) d.channels.push_back(std::move(ch));
        }
        assignSlugs(d.channels);
    }
    return d;
}

Adapter adapterFromJson(const Json &json)
{
    Adapter a;
    if (!json.is_object()) return a;
    a.id = intOf(json, "id");
    a.pluginType = strOf(json, "pluginType");
    a.externalId = strOf(json, "externalId");
    if (const auto c = json.find("connected"); c != json.end() && c->is_boolean()) a.connected = c->get<bool>();
    return a;
}

Room roomFromJson(const Json &json)
{
    Room r;
    if (!json.is_object()) return r;
    r.id = intOf(json, "id");
    r.name = strOf(json, "name");
    r.className = strOf(json, "roomClassName");
    r.floorId = intOf(json, "floorId");
    return r;
}

Scene sceneFromJson(const Json &json)
{
    Scene s;
    if (!json.is_object()) return s;
    s.id = intOf(json, "id");
    s.name = strOf(json, "name");
    s.roomId = intOf(json, "roomId");
    return s;
}

// --- the model -------------------------------------------------------------

void Model::indexChannels(const Device &device)
{
    for (const Channel &ch : device.channels) m_deviceOfChannel[ch.id] = device.id;
}

void Model::unindexChannels(const Device &device)
{
    for (const Channel &ch : device.channels) m_deviceOfChannel.erase(ch.id);
}

const Device *Model::device(int id) const
{
    const auto it = m_devices.find(id);
    return it == m_devices.end() ? nullptr : &it->second;
}

const Room *Model::room(int id) const
{
    const auto it = m_rooms.find(id);
    return it == m_rooms.end() ? nullptr : &it->second;
}

const Scene *Model::scene(int id) const
{
    const auto it = m_scenes.find(id);
    return it == m_scenes.end() ? nullptr : &it->second;
}

const Adapter *Model::adapter(int id) const
{
    const auto it = m_adapters.find(id);
    return it == m_adapters.end() ? nullptr : &it->second;
}

bool Model::excluded(const Device &device) const
{
    const Adapter *a = adapter(device.adapterId);
    return m_config.excludes(a ? a->pluginType : std::string(), device.adapterId);
}

bool Model::adapterConnected(int adapterId) const
{
    const Adapter *a = adapter(adapterId);
    return a == nullptr || a->connected;
}

std::string Model::roomName(int roomId) const
{
    const Room *r = room(roomId);
    return r ? r->name : std::string();
}

std::string Model::roomNameOf(const Device &device) const
{
    const int id = device.roomId();
    if (id <= 0) return {};
    if (const Room *r = room(id)) return r->name;
    return device.userRoomId > 0 ? device.userRoomName : device.adapterRoomName;
}

Touched Model::devicesInRoom(int roomId) const
{
    Touched t;
    for (const auto &[id, d] : m_devices)
        if (d.roomId() == roomId) t.devices.insert(id);
    return t;
}

Touched Model::scenesInRoom(int roomId) const
{
    Touched t;
    for (const auto &[id, s] : m_scenes)
        if (s.roomId == roomId) t.scenes.insert(id);
    return t;
}

Touched Model::applyAdapters(const Json &adapters)
{
    Touched t;
    if (!adapters.is_array()) return t;
    std::map<int, Adapter> next;
    for (const Json &j : adapters) {
        Adapter a = adapterFromJson(j);
        if (a.id <= 0) continue;
        // core states the link only when it knows it.
        if (!j.contains("connected")) {
            if (const Adapter *old = adapter(a.id)) a.connected = old->connected;
        }
        next[a.id] = std::move(a);
    }
    const auto differs = [&](int id) {
        const auto o = m_adapters.find(id), n = next.find(id);
        if (o == m_adapters.end() || n == next.end()) return true;
        return o->second.pluginType != n->second.pluginType || o->second.connected != n->second.connected;
    };
    std::set<int> ids;
    for (const auto &[id, a] : m_adapters) ids.insert(id);
    for (const auto &[id, a] : next) ids.insert(id);
    std::set<int> changed;
    for (const int id : ids)
        if (differs(id)) changed.insert(id);
    m_adapters = std::move(next);
    for (const auto &[id, d] : m_devices)
        if (changed.count(d.adapterId)) t.devices.insert(id);
    return t;
}

Touched Model::upsertAdapter(const Json &json)
{
    Touched t;
    Adapter a = adapterFromJson(json);
    if (a.id <= 0) return t;
    const Adapter *old = adapter(a.id);
    if (old && !json.contains("connected")) a.connected = old->connected;
    const bool changed = !old || old->pluginType != a.pluginType || old->connected != a.connected;
    const int id = a.id;
    m_adapters[id] = std::move(a);
    if (changed)
        for (const auto &[did, d] : m_devices)
            if (d.adapterId == id) t.devices.insert(did);
    return t;
}

Touched Model::setAdapterConnected(int adapterId, bool connected)
{
    Touched t;
    if (adapterId <= 0) return t;
    Adapter &a = m_adapters[adapterId];
    const bool changed = a.id == 0 || a.connected != connected;
    a.id = adapterId;
    a.connected = connected;
    if (changed)
        for (const auto &[did, d] : m_devices)
            if (d.adapterId == adapterId) t.devices.insert(did);
    return t;
}

Touched Model::removeAdapter(int adapterId)
{
    // The record stays, marked not connected: the adapter's devices are
    // removed by their own events, and until then they must not lose the
    // plugin type an exclusion rule matches on.
    return setAdapterConnected(adapterId, false);
}

Touched Model::applyDevices(const Json &entries)
{
    Touched t;
    if (!entries.is_array()) return t;
    std::map<int, Device> next;
    for (const Json &e : entries) {
        if (!e.is_object()) continue;
        const auto d = e.find("device");
        if (d == e.end()) continue;
        const auto c = e.find("channels");
        Device device = deviceFromJson(*d, c == e.end() ? Json() : *c);
        if (device.id > 0) next[device.id] = std::move(device);
    }
    for (const auto &[id, d] : m_devices) {
        t.devices.insert(id);
        unindexChannels(d);
    }
    m_devices = std::move(next);
    for (const auto &[id, d] : m_devices) {
        t.devices.insert(id);
        indexChannels(d);
    }
    t.topology = true;
    return t;
}

Touched Model::upsertDevice(const Json &deviceJson, const Json &channels)
{
    Touched t;
    Device device = deviceFromJson(deviceJson, channels);
    if (device.id <= 0) return t;
    if (const auto old = m_devices.find(device.id); old != m_devices.end()) {
        // The channels are optional in a device event: absent means unchanged, not none.
        if (!channels.is_array()) device.channels = old->second.channels;
        unindexChannels(old->second);
    }
    indexChannels(device);
    t.devices.insert(device.id);
    t.topology = true;
    m_devices[device.id] = std::move(device);
    return t;
}

Touched Model::removeDevice(int deviceId)
{
    Touched t;
    const auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return t;
    unindexChannels(it->second);
    m_devices.erase(it);
    t.devices.insert(deviceId);
    t.topology = true;
    return t;
}

Touched Model::applyRooms(const Json &rooms)
{
    Touched t;
    if (!rooms.is_array()) return t;
    std::map<int, Room> next;
    for (const Json &j : rooms) {
        Room r = roomFromJson(j);
        if (r.id > 0) next[r.id] = std::move(r);
    }
    std::set<int> changed;
    for (const auto &[id, r] : m_rooms) {
        const auto n = next.find(id);
        if (n == next.end() || !sameRoom(r, n->second)) changed.insert(id);
    }
    for (const auto &[id, r] : next)
        if (!m_rooms.count(id)) changed.insert(id);
    for (const int id : changed) t.rooms.insert(id);
    m_rooms = std::move(next);
    for (const int id : changed) {
        t.merge(devicesInRoom(id));
        t.merge(scenesInRoom(id));
    }
    if (!changed.empty()) t.topology = true;
    return t;
}

Touched Model::upsertRoom(const Json &json)
{
    Touched t;
    Room r = roomFromJson(json);
    if (r.id <= 0) return t;
    const Room *old = room(r.id);
    if (old && sameRoom(*old, r)) return t;
    const int id = r.id;
    m_rooms[id] = std::move(r);
    t.rooms.insert(id);
    t.merge(devicesInRoom(id));
    t.merge(scenesInRoom(id));
    t.topology = true;
    return t;
}

Touched Model::removeRoom(int roomId)
{
    Touched t;
    if (m_rooms.erase(roomId) == 0) return t;
    t.rooms.insert(roomId);
    t.merge(devicesInRoom(roomId));
    t.merge(scenesInRoom(roomId));
    t.topology = true;
    return t;
}

Touched Model::applyScenes(const Json &scenes)
{
    Touched t;
    if (!scenes.is_array()) return t;
    std::map<int, Scene> next;
    for (const Json &j : scenes) {
        Scene s = sceneFromJson(j);
        if (s.id > 0) next[s.id] = std::move(s);
    }
    for (const auto &[id, s] : m_scenes) {
        const auto n = next.find(id);
        if (n == next.end() || !sameScene(s, n->second)) t.scenes.insert(id);
    }
    for (const auto &[id, s] : next)
        if (!m_scenes.count(id)) t.scenes.insert(id);
    m_scenes = std::move(next);
    if (!t.scenes.empty()) t.topology = true;
    return t;
}

Touched Model::upsertScene(const Json &json)
{
    Touched t;
    Scene s = sceneFromJson(json);
    if (s.id <= 0) return t;
    const Scene *old = scene(s.id);
    if (old && sameScene(*old, s)) return t;
    t.scenes.insert(s.id);
    m_scenes[s.id] = std::move(s);
    t.topology = true;
    return t;
}

Model::StateChange Model::findChannel(int channelId) const
{
    StateChange found;
    const auto owner = m_deviceOfChannel.find(channelId);
    if (owner == m_deviceOfChannel.end()) return found;
    const auto d = m_devices.find(owner->second);
    if (d == m_devices.end()) return found;
    for (const Channel &ch : d->second.channels) {
        if (ch.id == channelId) {
            found.device = &d->second;
            found.channel = &ch;
            break;
        }
    }
    return found;
}

Channel *Model::locate(int channelId)
{
    const auto owner = m_deviceOfChannel.find(channelId);
    if (owner == m_deviceOfChannel.end()) return nullptr;
    const auto d = m_devices.find(owner->second);
    if (d == m_devices.end()) return nullptr;
    for (Channel &ch : d->second.channels)
        if (ch.id == channelId) return &ch;
    return nullptr;
}

Model::StateChange Model::applyState(int channelId, const Json &value, const std::string &valueName,
                                     const Json &valueFlags)
{
    StateChange found = findChannel(channelId);
    if (!found.channel || found.channel->isEvent()) return found;
    Channel &ch = *locate(channelId);
    ch.hasValue = !value.is_null();
    ch.value = value;
    ch.valueName = valueName;
    ch.valueFlags = valueFlags.is_array() ? valueFlags : Json();
    return found;
}

Model::StateChange Model::applyRollback(int channelId, const Json &value)
{
    StateChange found = findChannel(channelId);
    if (!found.channel || found.channel->isEvent() || value.is_null()) return found;
    Channel &ch = *locate(channelId);
    // The names belong to the value they were sent with.
    if (!ch.hasValue || ch.value != value) {
        ch.valueName.clear();
        ch.valueFlags = Json();
    }
    ch.hasValue = true;
    ch.value = value;
    return found;
}

} // namespace phi::endpoint
