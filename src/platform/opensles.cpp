#include "platform/opensles.h"

#include "core/log.h"
#include "shims/common.h"

#include <SDL.h>

#include <deque>
#include <atomic>
#include <map>
#include <set>

namespace rfu::platform {

namespace {

constexpr std::uint32_t kSuccess = 0, kParameterInvalid = 2, kFeatureUnsupported = 0x0C, kContentUnsupported = 0x0D;
constexpr std::uint32_t kStateRealized = 2;
constexpr std::uint32_t kPlayStopped = 1, kPlayPlaying = 3;  // SL_PLAYSTATE_PAUSED is 2
constexpr std::uint32_t kLocatorBufferQueue = 0x6, kLocatorAndroidSimpleBufferQueue = 0x800007BD;
constexpr std::uint32_t kFormatPcm = 2, kFormatAndroidPcmEx = 4;

enum class ItfType { Object, Engine, Play, BufferQueue, AndroidConfiguration };

struct SlObject;

// What a guest SLxxxItf points at: a block whose first word is the vtable.
struct Itf {
    ItfType type;
    SlObject* object;
};

struct Buffer {
    GuestAddr data;
    std::uint32_t size, offset;
};

struct SlObject {
    enum class Kind { Engine, OutputMix, Player } kind;
    Runtime* rt = nullptr;
    std::map<ItfType, GuestAddr> interfaces;
    std::uint32_t state = 1;  // SL_OBJECT_STATE_UNREALIZED

    // Players only.
    SDL_AudioSpec spec{};
    SDL_AudioDeviceID device = 0;
    std::mutex mutex;
    std::deque<Buffer> queue;
    std::uint32_t play_index = 0;
    std::uint32_t play_state = kPlayStopped;
    GuestAddr bq_callback = 0, bq_context = 0;
    GuestAddr play_callback = 0, play_context = 0;
    std::uint64_t frames_played = 0;
    int frame_bytes = 4;
};

struct SlState {
    std::mutex mutex;
    std::map<GuestAddr, Itf> interfaces;  // guest interface pointer -> what it is
    std::map<ItfType, GuestAddr> vtables;
    std::set<SlObject*> players;
    bool audio_ok = false;
    std::atomic<int> open_devices{0};
};

SlState& state(Runtime& rt) { return rt.state<SlState>(); }

// The SDL audio thread pulls PCM from the player's queue. When a buffer is used up the
// guest's callback runs right here (on a guest thread bound to SDL's audio thread), which is
// where the game enqueues the next one, as on Android.
void SDLCALL fill_audio(void* userdata, Uint8* out, int len) {
    auto* p = static_cast<SlObject*>(userdata);
    while (len > 0) {
        GuestAddr cb = 0, ctx = 0, itf = 0;
        {
            std::lock_guard lock(p->mutex);
            if (p->play_state != kPlayPlaying || p->queue.empty()) {
                std::memset(out, p->spec.silence, static_cast<std::size_t>(len));
                return;
            }
            Buffer& b = p->queue.front();
            std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(len), b.size - b.offset);
            std::memcpy(out, p->rt->mem().host(b.data + b.offset), n);
            b.offset += n;
            out += n;
            len -= static_cast<int>(n);
            p->frames_played += n / static_cast<std::uint32_t>(p->frame_bytes);
            if (b.offset < b.size) continue;
            p->queue.pop_front();
            ++p->play_index;
            cb = p->bq_callback;
            ctx = p->bq_context;
            itf = p->interfaces.count(ItfType::BufferQueue) ? p->interfaces.at(ItfType::BufferQueue) : 0;
        }
        if (!cb) continue;
        try {
            p->rt->call(cb, {itf, ctx});
        } catch (const std::exception& e) {
            log::error("OpenSL ES buffer queue callback failed: {}", e.what());
            std::lock_guard lock(p->mutex);
            p->bq_callback = 0;
        }
    }
}

GuestAddr vtable(Runtime& rt, ItfType type);

GuestAddr add_interface(Runtime& rt, SlObject* o, ItfType type) {
    GuestAddr block = rt.heap().alloc_zeroed(16);
    rt.mem().write<GuestAddr>(block, vtable(rt, type));
    auto& s = state(rt);
    std::lock_guard lock(s.mutex);
    s.interfaces[block] = {type, o};
    o->interfaces[type] = block;
    return block;
}

SlObject* object_of(CallContext& c) {
    auto& s = state(c.rt());
    std::lock_guard lock(s.mutex);
    auto it = s.interfaces.find(c.x(0));
    return it == s.interfaces.end() ? nullptr : it->second.object;
}

SlObject* new_object(Runtime& rt, SlObject::Kind kind) {
    auto* o = new SlObject;  // freed by Destroy
    o->kind = kind;
    o->rt = &rt;
    add_interface(rt, o, ItfType::Object);
    return o;
}

std::optional<ItfType> interface_for(Runtime& rt, GuestAddr iid) {
    static const std::pair<const char*, ItfType> known[] = {{"SL_IID_ENGINE", ItfType::Engine},
                                                             {"SL_IID_PLAY", ItfType::Play},
                                                             {"SL_IID_BUFFERQUEUE", ItfType::BufferQueue},
                                                             {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", ItfType::BufferQueue},
                                                             {"SL_IID_ANDROIDCONFIGURATION", ItfType::AndroidConfiguration}};
    for (const auto& [name, type] : known)
        if (auto a = rt.resolve_shim(name); a && rt.mem().read<GuestAddr>(*a) == iid) return type;
    return std::nullopt;
}

// --- SLObjectItf -------------------------------------------------------------------------

bool open_device(SlObject* p) {
    auto& s = state(*p->rt);
    if (!s.audio_ok) return false;
    SDL_AudioSpec have{};
    SDL_AudioSpec want = p->spec;
    want.callback = fill_audio;
    want.userdata = p;
    p->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!p->device && s.open_devices == 0) {
        // No sound card (or none free). The game's sound engine still waits for its buffers
        // to be played, and stalls the game without that, so play them to SDL's silent
        // driver, which takes them at the real rate.
        log::warn("OpenSL ES: couldn't open an audio device ({}); the game plays silently", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) p->device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    }
    if (!p->device) {
        log::warn("OpenSL ES: couldn't open an audio device: {}", SDL_GetError());
        return false;
    }
    ++s.open_devices;
    p->spec = have;
    SDL_PauseAudioDevice(p->device, 0);
    return true;
}

void object_realize(CallContext& c) {
    SlObject* o = object_of(c);
    if (!o) return c.ret(kParameterInvalid);
    if (o->kind == SlObject::Kind::Player && !o->device) open_device(o);
    o->state = kStateRealized;
    c.ret(kSuccess);
}

void object_get_state(CallContext& c) {
    SlObject* o = object_of(c);
    if (!o) return c.ret(kParameterInvalid);
    c.mem().write<std::uint32_t>(c.x(1), o->state);
    c.ret(kSuccess);
}

void object_get_interface(CallContext& c) {
    SlObject* o = object_of(c);
    auto type = interface_for(c.rt(), c.x(1));
    if (!o || !type) {
        log::warn("OpenSL ES: GetInterface for an interface rfu doesn't have");
        return c.ret(kFeatureUnsupported);
    }
    bool ok = (o->kind == SlObject::Kind::Engine && *type == ItfType::Engine) ||
              (o->kind == SlObject::Kind::Player && *type != ItfType::Engine);
    if (!ok) return c.ret(kFeatureUnsupported);
    GuestAddr itf;
    {
        auto& s = state(c.rt());
        std::lock_guard lock(s.mutex);
        auto it = o->interfaces.find(*type);
        itf = it == o->interfaces.end() ? 0 : it->second;
    }
    if (!itf) itf = add_interface(c.rt(), o, *type);
    c.mem().write<GuestAddr>(c.x(2), itf);
    c.ret(kSuccess);
}

void object_destroy(CallContext& c) {
    SlObject* o = object_of(c);
    if (!o) return;
    auto& s = state(c.rt());
    if (o->device) {
        SDL_CloseAudioDevice(o->device);
        --s.open_devices;
    }
    std::lock_guard lock(s.mutex);
    for (auto& [type, itf] : o->interfaces) s.interfaces.erase(itf);
    s.players.erase(o);
    delete o;
}

// --- SLEngineItf ---------------------------------------------------------------------------

void engine_create_output_mix(CallContext& c) {
    c.mem().write<GuestAddr>(c.x(1), new_object(c.rt(), SlObject::Kind::OutputMix)->interfaces[ItfType::Object]);
    c.ret(kSuccess);
}

void engine_create_audio_player(CallContext& c) {
    // (self, SLObjectItf* pPlayer, SLDataSource* src, SLDataSink* sink, numInterfaces, iids, required)
    GuestAddr src = c.x(2);
    GuestAddr locator = c.mem().read<GuestAddr>(src), format = c.mem().read<GuestAddr>(src + 8);
    std::uint32_t locator_type = c.mem().read<std::uint32_t>(locator);
    std::uint32_t format_type = c.mem().read<std::uint32_t>(format);
    if ((locator_type != kLocatorBufferQueue && locator_type != kLocatorAndroidSimpleBufferQueue) ||
        (format_type != kFormatPcm && format_type != kFormatAndroidPcmEx)) {
        log::warn("OpenSL ES: audio player with locator {:#x} / format {} isn't supported", locator_type, format_type);
        return c.ret(kContentUnsupported);
    }
    // SLDataFormat_PCM { formatType, numChannels, samplesPerSec (mHz), bitsPerSample,
    // containerSize, channelMask, endianness[, representation] }
    std::uint32_t channels = c.mem().read<std::uint32_t>(format + 4);
    std::uint32_t rate = c.mem().read<std::uint32_t>(format + 8) / 1000;
    std::uint32_t bits = c.mem().read<std::uint32_t>(format + 12);
    std::uint32_t representation = format_type == kFormatAndroidPcmEx ? c.mem().read<std::uint32_t>(format + 28) : 1;
    SDL_AudioFormat sdl_format = representation == 3 ? AUDIO_F32LSB : bits == 8 ? AUDIO_U8 : bits == 32 ? AUDIO_S32LSB : AUDIO_S16LSB;

    SlObject* p = new_object(c.rt(), SlObject::Kind::Player);
    p->spec.freq = static_cast<int>(rate);
    p->spec.format = sdl_format;
    p->spec.channels = static_cast<Uint8>(channels);
    p->spec.samples = 1024;
    p->frame_bytes = static_cast<int>(channels * std::max<std::uint32_t>(bits, 8) / 8);
    log::info("OpenSL ES: audio player, {} Hz, {} channel(s), {}-bit{}", rate, channels, bits, representation == 3 ? " float" : "");
    {
        auto& s = state(c.rt());
        std::lock_guard lock(s.mutex);
        s.players.insert(p);
        p->play_state = kPlayStopped;
    }
    c.mem().write<GuestAddr>(c.x(1), p->interfaces[ItfType::Object]);
    c.ret(kSuccess);
}

// --- SLPlayItf -----------------------------------------------------------------------------

void play_set_state(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    p->play_state = static_cast<std::uint32_t>(c.x(1));
    if (p->play_state == kPlayStopped) p->frames_played = 0;
    c.ret(kSuccess);
}

void play_get_state(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    c.mem().write<std::uint32_t>(c.x(1), p->play_state);
    c.ret(kSuccess);
}

void play_get_position(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    c.mem().write<std::uint32_t>(c.x(1), static_cast<std::uint32_t>(p->frames_played * 1000 / std::max(p->spec.freq, 1)));
    c.ret(kSuccess);
}

void play_register_callback(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    p->play_callback = c.x(1);
    p->play_context = c.x(2);
    c.ret(kSuccess);
}

// --- SLBufferQueueItf / SLAndroidSimpleBufferQueueItf --------------------------------------

void bq_enqueue(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    if (p->queue.size() >= 64) return c.ret(0x0E);  // SL_RESULT_BUFFER_INSUFFICIENT
    if (c.x(2)) p->queue.push_back({c.x(1), static_cast<std::uint32_t>(c.x(2)), 0});
    c.ret(kSuccess);
}

void bq_clear(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    p->queue.clear();
    c.ret(kSuccess);
}

void bq_get_state(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    c.mem().write<std::uint32_t>(c.x(1), static_cast<std::uint32_t>(p->queue.size()));
    c.mem().write<std::uint32_t>(c.x(1) + 4, p->play_index);
    c.ret(kSuccess);
}

void bq_register_callback(CallContext& c) {
    SlObject* p = object_of(c);
    if (!p) return c.ret(kParameterInvalid);
    std::lock_guard lock(p->mutex);
    p->bq_callback = c.x(1);
    p->bq_context = c.x(2);
    c.ret(kSuccess);
}

void ok(CallContext& c) { c.ret(kSuccess); }
void unsupported(CallContext& c) {
    if (c.thunk().calls.load() == 1) log::warn("OpenSL ES: {} isn't supported", c.thunk().name);
    c.ret(kFeatureUnsupported);
}

GuestAddr vtable(Runtime& rt, ItfType type) {
    auto& s = state(rt);
    {
        std::lock_guard lock(s.mutex);
        if (auto it = s.vtables.find(type); it != s.vtables.end()) return it->second;
    }
    std::vector<std::pair<const char*, ShimFn>> fns;
    switch (type) {
    case ItfType::Object:
        fns = {{"SLObject.Realize", object_realize},
               {"SLObject.Resume", ok},
               {"SLObject.GetState", object_get_state},
               {"SLObject.GetInterface", object_get_interface},
               {"SLObject.RegisterCallback", ok},
               {"SLObject.AbortAsyncOperation", [](CallContext&) {}},
               {"SLObject.Destroy", object_destroy},
               {"SLObject.SetPriority", ok},
               {"SLObject.GetPriority", unsupported},
               {"SLObject.SetLossOfControlInterfaces", ok}};
        break;
    case ItfType::Engine:
        fns = {{"SLEngine.CreateLEDDevice", unsupported},
               {"SLEngine.CreateVibraDevice", unsupported},
               {"SLEngine.CreateAudioPlayer", engine_create_audio_player},
               {"SLEngine.CreateAudioRecorder", unsupported},
               {"SLEngine.CreateMidiPlayer", unsupported},
               {"SLEngine.CreateListener", unsupported},
               {"SLEngine.Create3DGroup", unsupported},
               {"SLEngine.CreateOutputMix", engine_create_output_mix},
               {"SLEngine.CreateMetadataExtractor", unsupported},
               {"SLEngine.CreateExtensionObject", unsupported},
               {"SLEngine.QueryNumSupportedInterfaces", unsupported},
               {"SLEngine.QuerySupportedInterfaces", unsupported},
               {"SLEngine.QueryNumSupportedExtensions", unsupported},
               {"SLEngine.QuerySupportedExtension", unsupported},
               {"SLEngine.IsExtensionSupported", unsupported}};
        break;
    case ItfType::Play:
        fns = {{"SLPlay.SetPlayState", play_set_state},
               {"SLPlay.GetPlayState", play_get_state},
               {"SLPlay.GetDuration", [](CallContext& c) {
                    c.mem().write<std::uint32_t>(c.x(1), 0xFFFFFFFF);  // SL_TIME_UNKNOWN
                    c.ret(kSuccess);
                }},
               {"SLPlay.GetPosition", play_get_position},
               {"SLPlay.RegisterCallback", play_register_callback},
               {"SLPlay.SetCallbackEventsMask", ok},
               {"SLPlay.GetCallbackEventsMask", unsupported},
               {"SLPlay.SetMarkerPosition", ok},
               {"SLPlay.ClearMarkerPosition", ok},
               {"SLPlay.GetMarkerPosition", unsupported},
               {"SLPlay.SetPositionUpdatePeriod", ok},
               {"SLPlay.GetPositionUpdatePeriod", unsupported}};
        break;
    case ItfType::BufferQueue:
        fns = {{"SLBufferQueue.Enqueue", bq_enqueue},
               {"SLBufferQueue.Clear", bq_clear},
               {"SLBufferQueue.GetState", bq_get_state},
               {"SLBufferQueue.RegisterCallback", bq_register_callback}};
        break;
    case ItfType::AndroidConfiguration:
        fns = {{"SLAndroidConfiguration.SetConfiguration", ok}, {"SLAndroidConfiguration.GetConfiguration", unsupported}};
        break;
    }
    GuestAddr table = rt.heap().alloc_zeroed(8 * fns.size());
    for (std::size_t i = 0; i < fns.size(); ++i) rt.mem().write<GuestAddr>(table + 8 * i, rt.make_thunk(fns[i].first, fns[i].second));
    std::lock_guard lock(s.mutex);
    return s.vtables.emplace(type, table).first->second;
}

// SL_IID_xxx are `const SLInterfaceID`: a pointer to a 16-byte id, which here sits right
// after the pointer. Only the address matters to GetInterface.
DataShim interface_id() {
    return {24, [](Runtime& rt, GuestAddr addr) { rt.mem().write<GuestAddr>(addr, addr + 8); }};
}

}  // namespace

void register_opensles(ShimRegistry& reg) {
    for (const char* iid : {"SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_BUFFERQUEUE", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
                            "SL_IID_ANDROIDCONFIGURATION", "SL_IID_VOLUME"})
        reg.add_data(iid, interface_id());
    reg.add("slCreateEngine", [](CallContext& c) {
        // (SLObjectItf* pEngine, numOptions, options, numInterfaces, iids, required)
        auto& s = state(c.rt());
        if (!s.audio_ok) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
                log::warn("no audio device ({}); the game plays silently", SDL_GetError());
                SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
                SDL_InitSubSystem(SDL_INIT_AUDIO);
            }
            s.audio_ok = SDL_WasInit(SDL_INIT_AUDIO) != 0;
            if (s.audio_ok) log::info("audio: SDL driver {}", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
        }
        SlObject* engine = new_object(c.rt(), SlObject::Kind::Engine);
        c.mem().write<GuestAddr>(c.x(0), engine->interfaces[ItfType::Object]);
        c.ret(kSuccess);
    });
}

}  // namespace rfu::platform
