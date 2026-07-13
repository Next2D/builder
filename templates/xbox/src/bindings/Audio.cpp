#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "EventTarget.h"
#include "platform/AudioEngine.h"
#include "platform/DecodeQueue.h"
#include "v8/V8Util.h"
#include "v8/WeakHandle.h"

#include <memory>
#include <utility>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::ToStdString;

namespace {

// GainNode.gain.value を保持し、接続済みボイスへ音量を反映する。
struct GainParam {
    float value = 1.0f;
    std::shared_ptr<AudioVoice> voice;  // source.connect(gain) 時にリンクされる
};

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
}

void PumpAudioEvents(v8::Isolate* isolate)
{
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

// --- Audio (HTMLAudioElement 最小実装) -------------------------------------
// ゲームコードが `new Audio(url)` で効果音/BGM を再生する経路。
// play/pause/paused/loop/volume/currentTime/preload をサポートする。
// pause→play は先頭からの再生になる (位置レジュームは未対応の近似)。

void AudioElementPlay(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Object> self = args.This();

    auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
    args.GetReturnValue().Set(resolver->GetPromise());

    auto voice = Get<AudioVoice>(self);
    if (voice) {
        bool loop = false;
        double volume = 1.0;
        v8::Local<v8::Value> v;
        if (self->Get(ctx, Str(isolate, "loop")).ToLocal(&v)) {
            loop = v->BooleanValue(isolate);
        }
        if (self->Get(ctx, Str(isolate, "volume")).ToLocal(&v) && v->IsNumber()) {
            volume = v.As<v8::Number>()->Value();
        }
        voice->SetVolume(static_cast<float>(volume));
        voice->Start(loop);
    }
    v8util::SetValue(isolate, self, "paused", v8::Boolean::New(isolate, false));
    resolver->Resolve(ctx, v8::Undefined(isolate)).Check();
}

void AudioElementPause(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    auto voice = Get<AudioVoice>(args.This());
    if (voice) {
        voice->Stop();
    }
    v8util::SetValue(isolate, args.This(), "paused", v8::Boolean::New(isolate, true));
}

void AudioElementConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
        v8util::ThrowTypeError(isolate, "Audio must be called with new");
        return;
    }
    v8::Local<v8::Object> self = args.This();
    Attach<AudioVoice>(isolate, self, nullptr);

    // URL 指定があればロードし、デコードはバックグラウンドスレッドへ (失敗時は
    // 無音の no-op)。デコード完了前に play() が呼ばれていた場合 (paused=false) は
    // 完了時に再生を開始する (同期デコード時代の「即時再生」挙動を維持)。
    if (args.Length() > 0 && args[0]->IsString()) {
        const std::string src = ToStdString(isolate, args[0]);
        v8util::SetValue(isolate, self, "src", args[0]);
        HostContext* host = HostContext::From(isolate);
        auto bytes = host->assets->ReadBinary(src);
        if (bytes) {
            auto self_ref = std::make_shared<v8::Global<v8::Object>>(isolate, self);
            auto pcm = std::make_shared<PcmBuffer>();
            auto decoded = std::make_shared<bool>(false);
            decodequeue::Submit(
                [input = std::move(*bytes), pcm, decoded]() {
                    *decoded = AudioEngine::Decode(input, *pcm);
                },
                [isolate, self_ref, pcm, decoded, host]() {
                    v8::HandleScope hs(isolate);
                    v8::Local<v8::Object> el = self_ref->Get(isolate);
                    v8::Local<v8::Context> c = el->GetCreationContextChecked();
                    v8::Context::Scope cs(c);
                    if (*decoded) {
                        auto voice = host->audio->CreateVoice(pcm);
                        Attach<AudioVoice>(isolate, el, voice);
                        // デコード中に play() 済みなら開始する
                        v8::Local<v8::Value> paused_v, loop_v, vol_v;
                        const bool paused =
                            !el->Get(c, Str(isolate, "paused")).ToLocal(&paused_v) ||
                            paused_v->BooleanValue(isolate);
                        if (!paused && voice) {
                            if (el->Get(c, Str(isolate, "volume")).ToLocal(&vol_v) &&
                                vol_v->IsNumber()) {
                                voice->SetVolume(static_cast<float>(
                                    vol_v.As<v8::Number>()->Value()));
                            }
                            const bool loop =
                                el->Get(c, Str(isolate, "loop")).ToLocal(&loop_v) &&
                                loop_v->BooleanValue(isolate);
                            voice->Start(loop);
                        }
                    }
                    self_ref->Reset();
                });
        }
    }

    v8util::SetValue(isolate, self, "loop", v8::Boolean::New(isolate, false));
    v8util::SetValue(isolate, self, "volume", v8::Number::New(isolate, 1.0));
    v8util::SetValue(isolate, self, "paused", v8::Boolean::New(isolate, true));
    v8util::SetValue(isolate, self, "preload", Str(isolate, "auto"));
    v8util::SetValue(isolate, self, "currentTime", v8::Number::New(isolate, 0));
    v8util::SetMethod(isolate, self, "play", AudioElementPlay);
    v8util::SetMethod(isolate, self, "pause", AudioElementPause);
    InstallEventTarget(isolate, self);
    args.GetReturnValue().Set(self);
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
