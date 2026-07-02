// XMLHttpRequest / Blob / URL.createObjectURL / ImageData の最小実装。
//
// player は fetch ではなく XHR でアセットを読み込み(`DisplayObjectUtil`/`MediaUtil`)、
// Worker を Vite の `?worker&inline`(= URL.createObjectURL(new Blob([...]))) で生成する。
// ここではローカル(assets)読み込みを同期的に行い、onload をマイクロタスクで発火する。
#include "Bindings.h"

#include "HostContext.h"
#include "AssetLoader.h"
#include "v8/V8Util.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace next2d {

using v8util::Str;
using v8util::SetMethod;
using v8util::SetValue;
using v8util::ToStdString;

namespace {

// object URL レジストリ (createObjectURL で登録、Worker/XHR/Image が参照)
std::map<std::string, std::vector<uint8_t>>& ObjectURLs()
{
    static std::map<std::string, std::vector<uint8_t>> registry;
    return registry;
}
int g_object_url_seq = 0;

std::vector<uint8_t> ValueToBytes(v8::Isolate* isolate, v8::Local<v8::Value> v)
{
    std::vector<uint8_t> out;
    if (v->IsArrayBuffer()) {
        auto store = v.As<v8::ArrayBuffer>()->GetBackingStore();
        out.assign(static_cast<uint8_t*>(store->Data()),
                   static_cast<uint8_t*>(store->Data()) + store->ByteLength());
    } else if (v->IsArrayBufferView()) {
        auto view = v.As<v8::ArrayBufferView>();
        out.resize(view->ByteLength());
        view->CopyContents(out.data(), out.size());
    } else if (v->IsString()) {
        const std::string s = ToStdString(isolate, v);
        out.assign(s.begin(), s.end());
    }
    return out;
}

v8::Local<v8::ArrayBuffer> BytesToArrayBuffer(v8::Isolate* isolate, const std::vector<uint8_t>& bytes)
{
    auto store = v8::ArrayBuffer::NewBackingStore(isolate, bytes.size());
    if (!bytes.empty()) std::memcpy(store->Data(), bytes.data(), bytes.size());
    return v8::ArrayBuffer::New(isolate, std::move(store));
}

// --- Blob ----------------------------------------------------------------
// new Blob(parts[], options?) → { size, type, __bytes(External は使わず配列保持) }
void BlobConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (!args.IsConstructCall()) { v8util::ThrowTypeError(isolate, "Blob requires new"); return; }

    std::vector<uint8_t> bytes;
    if (args.Length() > 0 && args[0]->IsArray()) {
        auto arr = args[0].As<v8::Array>();
        for (uint32_t i = 0; i < arr->Length(); ++i) {
            v8::Local<v8::Value> part;
            if (arr->Get(ctx, i).ToLocal(&part)) {
                auto b = ValueToBytes(isolate, part);
                bytes.insert(bytes.end(), b.begin(), b.end());
            }
        }
    }
    v8::Local<v8::Object> self = args.This();
    // バイト列は base64 化せず ArrayBuffer プロパティで保持
    SetValue(isolate, self, "size", v8::Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(bytes.size())));
    SetValue(isolate, self, "__blobData", BytesToArrayBuffer(isolate, bytes));
    SetMethod(isolate, self, "arrayBuffer", [](const v8::FunctionCallbackInfo<v8::Value>& a){
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Context> c = iso->GetCurrentContext();
        auto r = v8::Promise::Resolver::New(c).ToLocalChecked();
        v8::Local<v8::Value> d;
        a.This()->Get(c, Str(iso, "__blobData")).ToLocal(&d);
        r->Resolve(c, d).Check();
        a.GetReturnValue().Set(r->GetPromise());
    });
    args.GetReturnValue().Set(self);
}

// --- URL.createObjectURL / revokeObjectURL -------------------------------
void CreateObjectURL(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    std::vector<uint8_t> bytes;
    if (args.Length() > 0 && args[0]->IsObject()) {
        v8::Local<v8::Value> d;
        if (args[0].As<v8::Object>()->Get(ctx, Str(isolate, "__blobData")).ToLocal(&d) &&
            d->IsArrayBuffer()) {
            bytes = ValueToBytes(isolate, d);
        }
    }
    const std::string url = "blob:next2d/" + std::to_string(++g_object_url_seq);
    ObjectURLs()[url] = std::move(bytes);
    args.GetReturnValue().Set(Str(isolate, url));
}

void RevokeObjectURL(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.Length() > 0) ObjectURLs().erase(ToStdString(isolate, args[0]));
}

// --- ImageData -----------------------------------------------------------
// new ImageData(Uint8ClampedArray, w, h) | new ImageData(w, h)
void ImageDataConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    if (!args.IsConstructCall()) { v8util::ThrowTypeError(isolate, "ImageData requires new"); return; }

    v8::Local<v8::Object> self = args.This();
    if (args.Length() >= 3 && args[0]->IsArrayBufferView()) {
        SetValue(isolate, self, "data", args[0]);
        SetValue(isolate, self, "width", args[1]);
        SetValue(isolate, self, "height", args[2]);
    } else {
        int w = args.Length() > 0 ? static_cast<int>(args[0].As<v8::Number>()->Value()) : 1;
        int h = args.Length() > 1 ? static_cast<int>(args[1].As<v8::Number>()->Value()) : 1;
        auto ab = v8::ArrayBuffer::New(isolate, static_cast<size_t>(w) * h * 4);
        self->Set(ctx, Str(isolate, "data"),
                  v8::Uint8ClampedArray::New(ab, 0, static_cast<size_t>(w) * h * 4)).Check();
        SetValue(isolate, self, "width", v8::Integer::New(isolate, w));
        SetValue(isolate, self, "height", v8::Integer::New(isolate, h));
    }
    args.GetReturnValue().Set(self);
}

// --- XMLHttpRequest ------------------------------------------------------
// open/send/setRequestHeader/responseType/response/status + load/error イベント
void XHRConstructor(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) { v8util::ThrowTypeError(isolate, "XMLHttpRequest requires new"); return; }
    v8::Local<v8::Object> self = args.This();
    SetValue(isolate, self, "readyState", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "status", v8::Integer::New(isolate, 0));
    SetValue(isolate, self, "responseType", Str(isolate, ""));
    SetValue(isolate, self, "__listeners", v8::Object::New(isolate));

    SetMethod(isolate, self, "open", [](const v8::FunctionCallbackInfo<v8::Value>& a){
        v8::Isolate* iso = a.GetIsolate();
        // open(method, url, async)
        SetValue(iso, a.This(), "__url", a.Length() > 1 ? a[1] : v8::Undefined(iso).As<v8::Value>());
    });
    SetMethod(isolate, self, "setRequestHeader", [](const v8::FunctionCallbackInfo<v8::Value>&){});
    SetMethod(isolate, self, "abort", [](const v8::FunctionCallbackInfo<v8::Value>&){});
    SetMethod(isolate, self, "getAllResponseHeaders", [](const v8::FunctionCallbackInfo<v8::Value>& a){
        a.GetReturnValue().Set(Str(a.GetIsolate(), ""));
    });
    // addEventListener(type, fn): __listeners[type] へ配列で保持
    SetMethod(isolate, self, "addEventListener", [](const v8::FunctionCallbackInfo<v8::Value>& a){
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Context> c = iso->GetCurrentContext();
        if (a.Length() < 2 || !a[1]->IsFunction()) return;
        const std::string type = v8util::ToStdString(iso, a[0]);
        v8::Local<v8::Value> lv;
        a.This()->Get(c, Str(iso, "__listeners")).ToLocal(&lv);
        v8::Local<v8::Object> listeners = lv.As<v8::Object>();
        v8::Local<v8::Value> arrv;
        v8::Local<v8::Array> arr;
        if (listeners->Get(c, Str(iso, type.c_str())).ToLocal(&arrv) && arrv->IsArray()) {
            arr = arrv.As<v8::Array>();
        } else {
            arr = v8::Array::New(iso, 0);
            listeners->Set(c, Str(iso, type.c_str()), arr).Check();
        }
        arr->Set(c, arr->Length(), a[1]).Check();
    });
    SetMethod(isolate, self, "removeEventListener", [](const v8::FunctionCallbackInfo<v8::Value>&){});

    // send(): assets/blob から同期読込 → response 準備 → load をマイクロタスクで発火
    SetMethod(isolate, self, "send", [](const v8::FunctionCallbackInfo<v8::Value>& a){
        v8::Isolate* iso = a.GetIsolate();
        v8::Local<v8::Context> c = iso->GetCurrentContext();
        v8::Local<v8::Object> self = a.This();
        HostContext* host = HostContext::From(iso);

        std::string url;
        v8::Local<v8::Value> uv;
        if (self->Get(c, Str(iso, "__url")).ToLocal(&uv)) url = ToStdString(iso, uv);

        std::vector<uint8_t> bytes;
        bool ok = false;
        auto it = ObjectURLs().find(url);
        if (it != ObjectURLs().end()) { bytes = it->second; ok = true; }
        else if (host->assets) {
            auto b = host->assets->ReadBinary(url);
            if (b) { bytes = std::move(*b); ok = true; }
        }

        SetValue(iso, self, "status", v8::Integer::New(iso, ok ? 200 : 404));
        SetValue(iso, self, "readyState", v8::Integer::New(iso, 4));

        // responseType に応じて response をセット
        const std::string rt = [&]{ v8::Local<v8::Value> v;
            return self->Get(c, Str(iso, "responseType")).ToLocal(&v) ? ToStdString(iso, v) : std::string(); }();
        v8::Local<v8::Value> response = v8::Null(iso);
        if (ok) {
            if (rt == "arraybuffer") {
                response = BytesToArrayBuffer(iso, bytes);
            } else {
                v8::Local<v8::String> s = v8::String::NewFromUtf8(
                    iso, reinterpret_cast<const char*>(bytes.data()),
                    v8::NewStringType::kNormal, static_cast<int>(bytes.size())).ToLocalChecked();
                if (rt == "json") {
                    v8::Local<v8::Value> parsed;
                    if (v8::JSON::Parse(c, s).ToLocal(&parsed)) response = parsed;
                } else {
                    response = s;
                    SetValue(iso, self, "responseText", s);
                }
            }
        }
        SetValue(iso, self, "response", response);

        // load / error イベントを発火 (on* + addEventListener)
        auto fire = [&](const char* type){
            v8::Local<v8::Object> ev = v8::Object::New(iso);
            SetValue(iso, ev, "type", Str(iso, type));
            SetValue(iso, ev, "target", self);
            v8::Local<v8::Value> args1[1] = { ev };
            std::string onname = std::string("on") + type;
            v8::Local<v8::Value> on;
            if (self->Get(c, Str(iso, onname.c_str())).ToLocal(&on) && on->IsFunction()) {
                v8::TryCatch tc(iso); (void) on.As<v8::Function>()->Call(c, self, 1, args1);
            }
            v8::Local<v8::Value> lv;
            self->Get(c, Str(iso, "__listeners")).ToLocal(&lv);
            v8::Local<v8::Value> arrv;
            if (lv->IsObject() && lv.As<v8::Object>()->Get(c, Str(iso, type)).ToLocal(&arrv) && arrv->IsArray()) {
                auto arr = arrv.As<v8::Array>();
                for (uint32_t i = 0; i < arr->Length(); ++i) {
                    v8::Local<v8::Value> fn;
                    if (arr->Get(c, i).ToLocal(&fn) && fn->IsFunction()) {
                        v8::TryCatch tc(iso); (void) fn.As<v8::Function>()->Call(c, self, 1, args1);
                    }
                }
            }
        };
        fire(ok ? "load" : "error");
        fire("loadend");
    });

    args.GetReturnValue().Set(self);
}

void InstallCtor(v8::Isolate* isolate, v8::Local<v8::Object> global,
                 const char* name, v8::FunctionCallback ctor)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate, ctor);
    t->SetClassName(Str(isolate, name));
    global->Set(ctx, Str(isolate, name), t->GetFunction(ctx).ToLocalChecked()).Check();
}

} // namespace

// blob: URL のバイト列を取得する (WorkerRuntime が inline worker の起動に使用)。
bool ResolveObjectURL(const std::string& url, std::string* out_text)
{
    auto it = ObjectURLs().find(url);
    if (it == ObjectURLs().end()) return false;
    out_text->assign(it->second.begin(), it->second.end());
    return true;
}

void InstallNetwork(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();

    InstallCtor(isolate, global, "XMLHttpRequest", XHRConstructor);
    InstallCtor(isolate, global, "Blob", BlobConstructor);
    InstallCtor(isolate, global, "ImageData", ImageDataConstructor);

    // URL: createObjectURL/revokeObjectURL を追加 (bootstrap.js の URL があれば拡張)
    v8::Local<v8::Value> url_val;
    v8::Local<v8::Object> url_obj;
    if (global->Get(ctx, Str(isolate, "URL")).ToLocal(&url_val) && url_val->IsObject()) {
        url_obj = url_val.As<v8::Object>();
    } else {
        url_obj = v8::Object::New(isolate);
        SetValue(isolate, global, "URL", url_obj);
    }
    SetMethod(isolate, url_obj, "createObjectURL", CreateObjectURL);
    SetMethod(isolate, url_obj, "revokeObjectURL", RevokeObjectURL);
}

} // namespace next2d
