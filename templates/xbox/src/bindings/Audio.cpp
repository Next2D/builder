#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "EventTarget.h"
#include "platform/AudioEngine.h"
#include "platform/DecodeQueue.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

// HTMLAudioElement (new Audio()) の状態。JS オブジェクトの内部フィールドに保持する。
struct AudioElementState {
    std::string src;                    // 現在の src (空 = 未設定)
    std::shared_ptr<AudioVoice> voice;  // デコード完了後に生成
    uint64_t generation = 0;            // src 変更 / load のたびに増やし、古いデコード結果を捨てる
    bool loading = false;
    bool ready = false;                 // デコード済みで再生可能
    bool paused = true;
    bool ended = false;
    bool want_play = false;             // デコード完了前に play() された
    bool tracked = false;               // PlayingElements に登録済み
    float volume = 1.0f;                // element.volume
    float gain = 1.0f;                  // createMediaElementSource 経由で接続した GainNode の値
    double duration = 0.0;

    void ApplyVolume() const
    {
        if (voice) {
            voice->SetVolume(volume * gain);
        }
    }
};

// GainNode.gain.value を保持し、接続済みボイスへ音量を反映する。
struct GainParam {
    float value = 1.0f;
    std::shared_ptr<AudioVoice> voice;  // source.connect(gain) 時にリンクされる
    // createMediaElementSource(el).connect(gain) でリンクされた audio 要素
    std::weak_ptr<AudioElementState> element;
};

// 再生中(非ループ)の audio 要素。毎フレーム終了を見て "ended" を発火する。
std::vector<v8::Global<v8::Object>>& PlayingElements()
{
    static std::vector<v8::Global<v8::Object>> elements;
    return elements;
}

// 再生中(非ループ)の source ノード。毎フレーム IsFinished を見て "ended" を発火する。
std::vector<v8::Global<v8::Object>>& PlayingSources()
{
    static std::vector<v8::Global<v8::Object>> sources;
    return sources;
}

// JS オブジェクトに shared_ptr を保持させるためのホルダ。GC で解放する。
template <typename T>
struct Holder {
    std::shared_ptr<T> ptr;
};

template <typename T>
void Attach(v8::Isolate* isolate, v8::Local<v8::Object> obj, std::shared_ptr<T> ptr)
{
    auto* holder = new Holder<T>{std::move(ptr)};
    obj->SetInternalField(0, v8::External::New(isolate, holder));
    v8util::AttachWeak(isolate, obj, holder);
}

template <typename T>
std::shared_ptr<T> Get(v8::Local<v8::Object> obj)
{
    auto* holder = static_cast<Holder<T>*>(
        obj->GetInternalField(0).As<v8::External>()->Value());
    return holder->ptr;
}

v8::Local<v8::ObjectTemplate> InternalTemplate(v8::Isolate* isolate)
{
    v8::Local<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
    tmpl->SetInternalFieldCount(1);
    return tmpl;
}

// --- AudioBufferSourceNode ------------------------------------------------
void SourceStart(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto voice = Get<AudioVoice>(args.This());
    bool loop = false;
    v8::Local<v8::Value> loop_val;
    if (args.This()->Get(args.GetIsolate()->GetCurrentContext(),
                         Str(args.GetIsolate(), "loop")).ToLocal(&loop_val)) {
        loop = loop_val->BooleanValue(args.GetIsolate());
    }
    if (voice) {
        voice->Start(loop);
        // 非ループ再生は終了時に "ended" を発火するため登録する。
        if (!loop) {
            PlayingSources().emplace_back(args.GetIsolate(), args.This());
        }
    }
}

void SourceStop(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto voice = Get<AudioVoice>(args.This());
    if (voice) {
        voice->Stop();
    }
}

// GainNode の connect: 実際に音量を反映するのは source.connect(gain) 側。ここは no-op。
void GainConnect(const v8::FunctionCallbackInfo<v8::Value>& /*args*/) {}

// source.disconnect(): player は stop() でこれを呼ぶ。ボイスを停止し登録からも外す
// (PlayingSources の強参照 Global がノードを固定し続けるのを防ぐ)。
void SourceDisconnect(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto voice = Get<AudioVoice>(args.This());
    if (voice) {
        voice->Stop();
    }
    auto& sources = PlayingSources();
    v8::Local<v8::Object> node = args.This();
    for (size_t i = 0; i < sources.size();) {
        if (sources[i].Get(isolate) == node) {
            sources.erase(sources.begin() + i);
        } else {
            ++i;
        }
    }
}

// source.connect(destination): destination が GainNode なら voice をリンクして音量を反映する。
void SourceConnect(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    auto voice = Get<AudioVoice>(args.This());
    if (!voice || args.Length() < 1 || !args[0]->IsObject()) {
        return;
    }
    // 接続先(GainNode)の gain オブジェクトから GainParam を取り出しリンクする。
    v8::Local<v8::Object> dest = args[0].As<v8::Object>();
    v8::Local<v8::Value> gain_val;
    if (!dest->Get(ctx, Str(isolate, "gain")).ToLocal(&gain_val) || !gain_val->IsObject()) {
        return;
    }
    v8::Local<v8::Object> gain = gain_val.As<v8::Object>();
    if (gain->InternalFieldCount() < 1) {
        return;
    }
    auto* param = static_cast<GainParam*>(
        gain->GetInternalField(0).As<v8::External>()->Value());
    if (param) {
        param->voice = voice;
        voice->SetVolume(param->value);  // 接続時点の音量を即時反映
    }
}

// createBufferSource() -> AudioBufferSourceNode
void CreateBufferSource(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    AudioEngine* engine = HostContext::From(isolate)->audio;

    v8::Local<v8::Object> node = InternalTemplate(isolate)->NewInstance(ctx).ToLocalChecked();
    // buffer は後から setter 経由で設定する想定 (ここでは空ボイス)。
    Attach<AudioVoice>(isolate, node, nullptr);

    v8util::SetValue(isolate, node, "loop", v8::Boolean::New(isolate, false));
    v8util::SetMethod(isolate, node, "start", SourceStart);
    v8util::SetMethod(isolate, node, "stop", SourceStop);
    v8util::SetMethod(isolate, node, "connect", SourceConnect);
    // source.addEventListener("ended", ...) を player が使うため EventTarget を設置する。
    InstallEventTarget(isolate, node);
    v8util::SetMethod(isolate, node, "disconnect", SourceDisconnect);

    // buffer プロパティ設定時に AudioVoice を生成するアクセサ
    // (SetAccessor は V8 12.9 で削除。SetNativeDataProperty を使う)
    node->SetNativeDataProperty(ctx, Str(isolate, "buffer"),
        nullptr,
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            v8::Isolate* iso = info.GetIsolate();
            if (!value->IsObject()) {
                return;
            }
            auto pcm = Get<PcmBuffer>(value.As<v8::Object>());
            AudioEngine* eng = HostContext::From(iso)->audio;
            auto voice = eng->CreateVoice(pcm);
            Attach<AudioVoice>(iso, info.This(), voice);
        }).Check();

    args.GetReturnValue().Set(node);
}

// --- GainNode -------------------------------------------------------------
void CreateGain(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Object> node = v8::Object::New(isolate);

    // gain オブジェクトは内部フィールドに GainParam を持ち、value アクセサで
    // リンク済みボイスへ音量を即時反映する (再生中のミュート/音量変更に対応)。
    v8::Local<v8::Object> gain = InternalTemplate(isolate)->NewInstance(ctx).ToLocalChecked();
    auto* param = new GainParam();
    gain->SetInternalField(0, v8::External::New(isolate, param));
    v8util::AttachWeak(isolate, gain, param);

    gain->SetNativeDataProperty(ctx, Str(isolate, "value"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto* p = static_cast<GainParam*>(
                info.This()->GetInternalField(0).As<v8::External>()->Value());
            info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), p ? p->value : 1.0));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            auto* p = static_cast<GainParam*>(
                info.This()->GetInternalField(0).As<v8::External>()->Value());
            if (!p || !value->IsNumber()) return;
            p->value = static_cast<float>(value.As<v8::Number>()->Value());
            if (p->voice) p->voice->SetVolume(p->value);  // 再生中も反映
            if (auto element = p->element.lock()) {
                element->gain = p->value;
                element->ApplyVolume();
            }
        }).Check();

    v8util::SetValue(isolate, node, "gain", gain);
    v8util::SetMethod(isolate, node, "connect", GainConnect);
    v8util::SetMethod(isolate, node, "disconnect",
        [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    args.GetReturnValue().Set(node);
}

// 毎フレーム呼ばれ、再生終了した source ノードへ "ended" を発火する (main.cpp のループから)。
} // namespace

// V8 破棄前に呼ぶ。再生追跡リストの v8::Global を解放する。
// (放置すると static デストラクタが V8 破棄後に走りアクセス違反になる)
void ShutdownAudioEvents()
{
    for (auto& g : PlayingSources()) {
        g.Reset();
    }
    PlayingSources().clear();
    for (auto& g : PlayingElements()) {
        g.Reset();
    }
    PlayingElements().clear();
}

// 再生が終わった audio 要素へ "ended" を発火する (source ノードと同じ 2 段階方式)。
static void PumpAudioElements(v8::Isolate* isolate)
{
    auto& elements = PlayingElements();
    if (elements.empty()) return;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    std::vector<v8::Global<v8::Object>> finished;
    for (size_t i = 0; i < elements.size();) {
        v8::Local<v8::Object> el = elements[i].Get(isolate);
        auto state = Get<AudioElementState>(el);
        const bool playing = state && !state->paused && state->voice;
        if (!playing) {
            // pause / src 変更などで再生が止まった: 追跡だけ外す (イベントなし)
            if (state) state->tracked = false;
            elements[i].Reset();
            elements.erase(elements.begin() + i);
        } else if (state->voice->IsFinished()) {
            state->tracked = false;
            state->paused = true;
            state->ended = true;
            finished.push_back(std::move(elements[i]));
            elements.erase(elements.begin() + i);
        } else {
            ++i;
        }
    }
    for (auto& g : finished) {
        v8::Local<v8::Object> el = g.Get(isolate);
        v8::Local<v8::Object> ev = v8::Object::New(isolate);
        ev->Set(ctx, Str(isolate, "type"), Str(isolate, "ended")).Check();
        DispatchEvent(isolate, el, ev);
        g.Reset();
    }
}

void PumpAudioEvents(v8::Isolate* isolate)
{
    PumpAudioElements(isolate);

    auto& sources = PlayingSources();
    if (sources.empty()) return;
    v8::HandleScope scope(isolate);
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    // 1) 終了した source を集めて先に登録リストから外す。ここでは JS を呼ばない。
    //    (この時点の erase による v8::Global の move は他の再入が無いので安全)
    std::vector<v8::Global<v8::Object>> finished;
    for (size_t i = 0; i < sources.size();) {
        v8::Local<v8::Object> node = sources[i].Get(isolate);
        auto voice = Get<AudioVoice>(node);
        if (!voice || voice->IsFinished()) {
            finished.push_back(std::move(sources[i]));
            sources.erase(sources.begin() + i);
        } else {
            ++i;
        }
    }

    // 2) リストが安定した状態で "ended" を配送する。
    //    "ended" ハンドラは source.disconnect() や新規再生で PlayingSources を
    //    再入的に変更しうる。以前は配送と erase を同一ループで行っていたため、
    //    配送中にベクタが変更されると直後の erase 中の v8::Global move が壊れ
    //    GlobalHandles::MoveGlobal でアクセス違反していた。独立した finished を
    //    走査することで再入変更から隔離する。
    for (auto& g : finished) {
        v8::Local<v8::Object> node = g.Get(isolate);
        auto voice = Get<AudioVoice>(node);
        if (voice) {
            v8::Local<v8::Object> ev = v8::Object::New(isolate);
            ev->Set(ctx, Str(isolate, "type"), Str(isolate, "ended")).Check();
            DispatchEvent(isolate, node, ev);
        }
        g.Reset();
    }
}

namespace {

// --- decodeAudioData ------------------------------------------------------
void DecodeAudioData(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    std::vector<uint8_t> input;
    if (args.Length() >= 1 && args[0]->IsArrayBuffer()) {
        auto ab = args[0].As<v8::ArrayBuffer>();
        auto store = ab->GetBackingStore();
        input.assign(static_cast<uint8_t*>(store->Data()),
                     static_cast<uint8_t*>(store->Data()) + store->ByteLength());
    }

    // Media Foundation デコードはバックグラウンドスレッドへ (Promise なので元々
    // 非同期契約)。BGM の同期デコードは画面遷移時の数百 ms ヒッチになっていた。
    auto resolver_ref =
        std::make_shared<v8::Global<v8::Promise::Resolver>>(isolate, resolver);
    auto pcm = std::make_shared<PcmBuffer>();
    auto decoded = std::make_shared<bool>(false);

    decodequeue::Submit(
        [input = std::move(input), pcm, decoded]() {
            *decoded = !input.empty() && AudioEngine::Decode(input, *pcm);
        },
        [isolate, resolver_ref, pcm, decoded]() {
            v8::HandleScope hs(isolate);
            v8::Local<v8::Promise::Resolver> r = resolver_ref->Get(isolate);
            v8::Local<v8::Context> c = r->GetCreationContextChecked();
            v8::Context::Scope cs(c);

            // 重要: デコード失敗でも Reject しない。@next2d/media の SoundDecodeService は
            // decodeAudioData の reject を catch して先頭バイトをスキップし再帰リトライするが、
            // そのループ条件 (`idx > byteLength`) が常に false のため同一バッファで無限再帰し、
            // マイクロタスクが枯渇してフレームワークのローディング画面ごとフリーズする。
            // ここで無音の空バッファを resolve すれば、そのサウンドは無音になるだけでアプリは
            // 進行できる (graceful degradation)。実データが壊れているか MF 非対応かは上のログで判別。
            if (!*decoded) {
                pcm->samples.clear();      // 0 サンプル = 無音
                pcm->channels = pcm->channels ? pcm->channels : 2;
                pcm->sample_rate = pcm->sample_rate ? pcm->sample_rate : 48000;
            }

            v8::Local<v8::Object> buffer =
                InternalTemplate(isolate)->NewInstance(c).ToLocalChecked();
            Attach<PcmBuffer>(isolate, buffer, pcm);
            v8util::SetValue(isolate, buffer, "sampleRate",
                             v8::Number::New(isolate, pcm->sample_rate));
            v8util::SetValue(isolate, buffer, "numberOfChannels",
                             v8::Integer::New(isolate, pcm->channels));
            v8util::SetValue(isolate, buffer, "duration",
                v8::Number::New(isolate, pcm->channels
                    ? static_cast<double>(pcm->samples.size()) / pcm->channels / pcm->sample_rate
                    : 0.0));
            r->Resolve(c, buffer).Check();
            resolver_ref->Reset();
        });
}

// --- Audio (HTMLAudioElement) ----------------------------------------------
// `new Audio(url)` / `new Audio()` + `src` 代入で音声を再生する経路。次の 2 つが使う:
//   - ゲームコードが効果音/BGM を直接鳴らす (`new Audio(url).play()`)
//   - player のストリーム再生 (`new Audio()` → preload="none" → src → load() →
//     loadedmetadata 待ち → createMediaElementSource(el).connect(gain) → play())
//
// 対応: src / load() / play() / pause() / paused / ended / loop / volume / duration /
//       getAttribute・setAttribute・hasAttribute・removeAttribute("src") /
//       loadstart・loadedmetadata・canplaythrough・ended・error イベント。
// 近似: ファイル全体をデコードしてから再生する (逐次ストリーミングではない)。
//       pause→play と currentTime への代入は先頭からの再生になる。

void FireElementEvent(v8::Isolate* isolate, v8::Local<v8::Object> el, const char* type)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> ev = v8::Object::New(isolate);
    ev->Set(ctx, Str(isolate, "type"), Str(isolate, type)).Check();
    DispatchEvent(isolate, el, ev);
}

bool ElementLoop(v8::Isolate* isolate, v8::Local<v8::Object> el)
{
    v8::Local<v8::Value> v;
    return el->Get(isolate->GetCurrentContext(), Str(isolate, "loop")).ToLocal(&v) &&
           v->BooleanValue(isolate);
}

// デコード済みの要素を先頭から再生する。
void StartElementVoice(v8::Isolate* isolate, v8::Local<v8::Object> el,
                       const std::shared_ptr<AudioElementState>& state)
{
    const bool loop = ElementLoop(isolate, el);
    state->want_play = false;
    state->paused = false;
    state->ended = false;
    if (!state->voice) {
        return;
    }
    state->voice->Stop();
    state->ApplyVolume();
    state->voice->Start(loop);
    if (!loop && !state->tracked) {
        PlayingElements().emplace_back(isolate, el);
        state->tracked = true;
    }
}

// 再生を止めて読み込み状態を破棄する (src 変更 / removeAttribute("src") / load() の前段)。
void ResetElement(const std::shared_ptr<AudioElementState>& state)
{
    ++state->generation;
    if (state->voice) {
        state->voice->Stop();
    }
    state->voice.reset();
    state->loading = false;
    state->ready = false;
    state->ended = false;
    state->duration = 0.0;
}

// src を読み込みバックグラウンドでデコードする。完了時に loadedmetadata を発火する。
void StartElementLoad(v8::Isolate* isolate, v8::Local<v8::Object> el,
                      const std::shared_ptr<AudioElementState>& state)
{
    if (state->src.empty() || state->loading || state->ready) {
        return;
    }
    HostContext* host = HostContext::From(isolate);
    state->loading = true;
    const uint64_t generation = state->generation;

    FireElementEvent(isolate, el, "loadstart");
    if (generation != state->generation) {
        return;   // loadstart ハンドラが src を変えた
    }

    auto bytes = host->assets->ReadBinary(state->src);
    auto self_ref = std::make_shared<v8::Global<v8::Object>>(isolate, el);
    auto pcm = std::make_shared<PcmBuffer>();
    auto found = std::make_shared<bool>(static_cast<bool>(bytes));
    auto input = std::make_shared<std::vector<uint8_t>>(
        bytes ? std::move(*bytes) : std::vector<uint8_t>());

    // イベントは必ず非同期 (次の Pump 以降) に発火する。呼び出し側が load() / play() の
    // 後にリスナを付けても拾えるようにするため。
    decodequeue::Submit(
        [input, pcm, found]() {
            if (*found && !AudioEngine::Decode(*input, *pcm)) {
                // デコード失敗は無音として扱う (アプリの進行を止めない)
                pcm->samples.clear();
                pcm->channels = pcm->channels ? pcm->channels : 2;
                pcm->sample_rate = pcm->sample_rate ? pcm->sample_rate : 48000;
            }
            input->clear();
        },
        [isolate, self_ref, state, pcm, found, host, generation]() {
            v8::HandleScope hs(isolate);
            v8::Local<v8::Object> element = self_ref->Get(isolate);
            self_ref->Reset();
            if (generation != state->generation) {
                return;   // 読み込み中に src が変わった / 破棄された
            }
            v8::Local<v8::Context> c = element->GetCreationContextChecked();
            v8::Context::Scope cs(c);
            state->loading = false;

            if (!*found) {
                v8::Local<v8::Object> error = v8::Object::New(isolate);
                v8util::SetValue(isolate, error, "code", v8::Integer::New(isolate, 4));
                v8util::SetValue(isolate, error, "message",
                                 Str(isolate, "audio source not found: " + state->src));
                v8util::SetValue(isolate, element, "error", error);
                state->want_play = false;
                state->paused = true;
                FireElementEvent(isolate, element, "error");
                return;
            }

            state->voice = host->audio->CreateVoice(pcm);
            state->ready = true;
            state->duration = (pcm->channels && pcm->sample_rate)
                ? static_cast<double>(pcm->samples.size()) / pcm->channels / pcm->sample_rate
                : 0.0;

            FireElementEvent(isolate, element, "loadedmetadata");
            if (generation != state->generation) {
                return;
            }
            FireElementEvent(isolate, element, "canplaythrough");
            if (generation != state->generation) {
                return;
            }
            if (state->want_play) {
                StartElementVoice(isolate, element, state);
            }
        });
}

// src を設定する。preload が "none" 以外ならすぐ読み込みを始める。
void SetElementSrc(v8::Isolate* isolate, v8::Local<v8::Object> el,
                   const std::shared_ptr<AudioElementState>& state, const std::string& src)
{
    ResetElement(state);
    state->src = src;
    if (src.empty()) {
        return;
    }
    v8::Local<v8::Value> preload;
    const bool none =
        el->Get(isolate->GetCurrentContext(), Str(isolate, "preload")).ToLocal(&preload) &&
        preload->IsString() && ToStdString(isolate, preload) == "none";
    if (!none) {
        StartElementLoad(isolate, el, state);
    }
}

void AudioElementPlay(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    auto state = Get<AudioElementState>(self);
    if (state) {
        if (state->ready) {
            StartElementVoice(isolate, self, state);
        } else {
            // デコード完了時に再生を始める
            state->want_play = true;
            state->paused = false;
            state->ended = false;
            StartElementLoad(isolate, self, state);
        }
    }
    resolver->Resolve(ctx, v8::Undefined(isolate)).Check();
}

void AudioElementPause(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto state = Get<AudioElementState>(args.This());
    if (!state) {
        return;
    }
    if (state->voice) {
        state->voice->Stop();
    }
    state->paused = true;
    state->want_play = false;
}

// load(): src があれば読み込み直す。無ければ状態を破棄するだけ。
void AudioElementLoad(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    auto state = Get<AudioElementState>(args.This());
    if (!state) {
        return;
    }
    ResetElement(state);
    state->paused = true;
    state->want_play = false;
    StartElementLoad(args.GetIsolate(), args.This(), state);
}

// getAttribute / setAttribute / hasAttribute / removeAttribute。src だけ要素の状態と
// 連動し、それ以外は __attrs に保持する。
v8::Local<v8::Object> ElementAttrs(v8::Isolate* isolate, v8::Local<v8::Object> el)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> v;
    if (el->Get(ctx, Str(isolate, "__attrs")).ToLocal(&v) && v->IsObject()) {
        return v.As<v8::Object>();
    }
    v8::Local<v8::Object> attrs = v8::Object::New(isolate);
    v8util::SetValue(isolate, el, "__attrs", attrs);
    return attrs;
}

void AudioElementGetAttribute(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    const std::string name = args.Length() > 0 ? ToStdString(isolate, args[0]) : "";
    auto state = Get<AudioElementState>(args.This());
    if (name == "src") {
        if (state && !state->src.empty()) {
            args.GetReturnValue().Set(Str(isolate, state->src));
        } else {
            args.GetReturnValue().SetNull();
        }
        return;
    }
    v8::Local<v8::Value> v;
    if (ElementAttrs(isolate, args.This())
            ->Get(isolate->GetCurrentContext(), Str(isolate, name)).ToLocal(&v) &&
        !v->IsUndefined()) {
        args.GetReturnValue().Set(v);
    } else {
        args.GetReturnValue().SetNull();
    }
}

void AudioElementHasAttribute(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    const std::string name = args.Length() > 0 ? ToStdString(isolate, args[0]) : "";
    auto state = Get<AudioElementState>(args.This());
    if (name == "src") {
        args.GetReturnValue().Set(state && !state->src.empty());
        return;
    }
    v8::Local<v8::Value> v;
    args.GetReturnValue().Set(
        ElementAttrs(isolate, args.This())
            ->Get(isolate->GetCurrentContext(), Str(isolate, name)).ToLocal(&v) &&
        !v->IsUndefined());
}

void AudioElementSetAttribute(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() < 2) {
        return;
    }
    const std::string name = ToStdString(isolate, args[0]);
    auto state = Get<AudioElementState>(args.This());
    if (name == "src") {
        if (state) {
            SetElementSrc(isolate, args.This(), state, ToStdString(isolate, args[1]));
        }
        return;
    }
    ElementAttrs(isolate, args.This())
        ->Set(isolate->GetCurrentContext(), Str(isolate, name), args[1]).Check();
}

void AudioElementRemoveAttribute(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    const std::string name = args.Length() > 0 ? ToStdString(isolate, args[0]) : "";
    auto state = Get<AudioElementState>(args.This());
    if (name == "src") {
        if (state) {
            ResetElement(state);
            state->src.clear();
            state->paused = true;
            state->want_play = false;
        }
        return;
    }
    ElementAttrs(isolate, args.This())
        ->Delete(isolate->GetCurrentContext(), Str(isolate, name)).Check();
}

void AudioElementConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "Audio must be called with new");
        return;
    }
    v8::Local<v8::Object> self = args.This();
    Attach<AudioElementState>(isolate, self, std::make_shared<AudioElementState>());

    v8util::SetValue(isolate, self, "loop", v8::Boolean::New(isolate, false));
    v8util::SetValue(isolate, self, "preload", Str(isolate, "auto"));
    v8util::SetValue(isolate, self, "crossOrigin", v8::Null(isolate));
    v8util::SetValue(isolate, self, "error", v8::Null(isolate));
    v8util::SetMethod(isolate, self, "play", AudioElementPlay);
    v8util::SetMethod(isolate, self, "pause", AudioElementPause);
    v8util::SetMethod(isolate, self, "load", AudioElementLoad);
    v8util::SetMethod(isolate, self, "getAttribute", AudioElementGetAttribute);
    v8util::SetMethod(isolate, self, "setAttribute", AudioElementSetAttribute);
    v8util::SetMethod(isolate, self, "hasAttribute", AudioElementHasAttribute);
    v8util::SetMethod(isolate, self, "removeAttribute", AudioElementRemoveAttribute);
    InstallEventTarget(isolate, self);

    // 状態と連動するプロパティ
    self->SetNativeDataProperty(ctx, Str(isolate, "src"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto s = Get<AudioElementState>(info.This());
            info.GetReturnValue().Set(Str(info.GetIsolate(), s ? s->src : std::string()));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            auto s = Get<AudioElementState>(info.This());
            if (s) {
                SetElementSrc(info.GetIsolate(), info.This(), s,
                              ToStdString(info.GetIsolate(), value));
            }
        }).Check();
    self->SetNativeDataProperty(ctx, Str(isolate, "volume"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto s = Get<AudioElementState>(info.This());
            info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), s ? s->volume : 1.0));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value> value,
           const v8::PropertyCallbackInfo<void>& info) {
            auto s = Get<AudioElementState>(info.This());
            if (s && value->IsNumber()) {
                s->volume = static_cast<float>(value.As<v8::Number>()->Value());
                s->ApplyVolume();
            }
        }).Check();
    self->SetNativeDataProperty(ctx, Str(isolate, "paused"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto s = Get<AudioElementState>(info.This());
            info.GetReturnValue().Set(s ? s->paused : true);
        }).Check();
    self->SetNativeDataProperty(ctx, Str(isolate, "ended"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto s = Get<AudioElementState>(info.This());
            info.GetReturnValue().Set(s ? s->ended : false);
        }).Check();
    self->SetNativeDataProperty(ctx, Str(isolate, "duration"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            auto s = Get<AudioElementState>(info.This());
            info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), s ? s->duration : 0.0));
        }).Check();
    // currentTime: 位置の取得/シークは未対応。代入は受け付けるだけ (再生は常に先頭から)。
    self->SetNativeDataProperty(ctx, Str(isolate, "currentTime"),
        [](v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
            info.GetReturnValue().Set(v8::Number::New(info.GetIsolate(), 0.0));
        },
        [](v8::Local<v8::Name>, v8::Local<v8::Value>,
           const v8::PropertyCallbackInfo<void>&) {}).Check();

    // `new Audio(url)`: すぐ読み込みを始める
    if (args.Length() > 0 && args[0]->IsString()) {
        SetElementSrc(isolate, self, Get<AudioElementState>(self),
                      ToStdString(isolate, args[0]));
    }
    args.GetReturnValue().Set(self);
}

// createMediaElementSource(el): audio 要素をグラフへつなぐノード。
// connect(gainNode) で要素の実効音量 (volume × gain) に GainNode の値が反映される。
void CreateMediaElementSource(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Object> node = v8::Object::New(isolate);
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8util::SetValue(isolate, node, "mediaElement", args[0]);
    }
    v8util::SetMethod(isolate, node, "connect",
        [](const v8::FunctionCallbackInfo<v8::Value>& a) {
            v8::Isolate* iso = a.GetIsolate();
            v8::Local<v8::Context> c = iso->GetCurrentContext();
            v8::Local<v8::Value> el_val, gain_val;
            if (a.Length() < 1 || !a[0]->IsObject() ||
                !a.This()->Get(c, Str(iso, "mediaElement")).ToLocal(&el_val) ||
                !el_val->IsObject() || el_val.As<v8::Object>()->InternalFieldCount() < 1) {
                return;
            }
            auto state = Get<AudioElementState>(el_val.As<v8::Object>());
            if (!state ||
                !a[0].As<v8::Object>()->Get(c, Str(iso, "gain")).ToLocal(&gain_val) ||
                !gain_val->IsObject() || gain_val.As<v8::Object>()->InternalFieldCount() < 1) {
                return;
            }
            auto* param = static_cast<GainParam*>(
                gain_val.As<v8::Object>()->GetInternalField(0).As<v8::External>()->Value());
            if (param) {
                param->element = state;
                state->gain = param->value;
                state->ApplyVolume();
            }
        });
    v8util::SetMethod(isolate, node, "disconnect",
        [](const v8::FunctionCallbackInfo<v8::Value>&) {});
    args.GetReturnValue().Set(node);
}

// AudioContext コンストラクタ
void AudioContextConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "AudioContext must be called with new");
        return;
    }
    v8::Local<v8::Object> self = args.This();
    v8util::SetValue(isolate, self, "sampleRate", v8::Number::New(isolate, 48000));
    v8util::SetValue(isolate, self, "destination", v8::Object::New(isolate));
    v8util::SetMethod(isolate, self, "createBufferSource", CreateBufferSource);
    v8util::SetMethod(isolate, self, "createGain", CreateGain);
    v8util::SetMethod(isolate, self, "createMediaElementSource", CreateMediaElementSource);
    v8util::SetMethod(isolate, self, "decodeAudioData", DecodeAudioData);

    // state / resume / suspend / close: ブラウザの AudioContext はユーザージェスチャまで
    // suspended だが、ホストの XAudio2 は常に有効なので "running" 固定。player の音声
    // アンロック (初回 pointerup で ctx.resume()) が未実装だと TypeError で throw し、
    // そのハンドラ(ミュート解除等)以降が中断する。解決済み Promise を返す no-op にする。
    v8util::SetValue(isolate, self, "state", v8util::Str(isolate, "running"));
    auto resolvedPromise = [](const v8::FunctionCallbackInfo<v8::Value>& a) {
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Context> c = iso->GetCurrentContext();
        auto r = v8::Promise::Resolver::New(c).ToLocalChecked();
        (void) r->Resolve(c, v8::Undefined(iso));
        a.GetReturnValue().Set(r->GetPromise());
    };
    v8util::SetMethod(isolate, self, "resume", resolvedPromise);
    v8util::SetMethod(isolate, self, "suspend", resolvedPromise);
    v8util::SetMethod(isolate, self, "close", resolvedPromise);

    args.GetReturnValue().Set(self);
}

} // namespace

void InstallAudio(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    v8::Local<v8::FunctionTemplate> tmpl =
        v8::FunctionTemplate::New(isolate, AudioContextConstructor);
    tmpl->SetClassName(Str(isolate, "AudioContext"));
    v8::Local<v8::Function> fn = tmpl->GetFunction(ctx).ToLocalChecked();

    global->Set(ctx, Str(isolate, "AudioContext"), fn).Check();
    global->Set(ctx, Str(isolate, "webkitAudioContext"), fn).Check();

    // Audio (HTMLAudioElement 最小): ゲームコードの `new Audio(url)` 用。
    // インスタンスは内部フィールドに AudioVoice ホルダを持つ。
    v8::Local<v8::FunctionTemplate> audio_tmpl =
        v8::FunctionTemplate::New(isolate, AudioElementConstructor);
    audio_tmpl->SetClassName(Str(isolate, "Audio"));
    audio_tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    global->Set(ctx, Str(isolate, "Audio"),
                audio_tmpl->GetFunction(ctx).ToLocalChecked()).Check();
}

} // namespace next2d
