#include "Bindings.h"

#include "HostContext.h"
#include "platform/GamepadManager.h"
#include "v8/V8Util.h"

namespace next2d {

using v8util::Str;

namespace {

v8::Local<v8::Object> BuildGamepad(v8::Isolate* isolate, v8::Local<v8::Context> ctx,
                                   const GamepadSnapshot& snap, int index)
{
    v8::Local<v8::Object> pad = v8::Object::New(isolate);

    v8util::SetValue(isolate, pad, "index", v8::Integer::New(isolate, index));
    v8util::SetValue(isolate, pad, "id", Str(isolate, "Xbox Wireless Controller (STANDARD GAMEPAD)"));
    v8util::SetValue(isolate, pad, "mapping", Str(isolate, "standard"));
    v8util::SetValue(isolate, pad, "connected", v8::Boolean::New(isolate, snap.connected));
    v8util::SetValue(isolate, pad, "timestamp", v8::Number::New(isolate, snap.timestamp));

    // axes
    v8::Local<v8::Array> axes = v8::Array::New(isolate, static_cast<int>(snap.axes.size()));
    for (size_t i = 0; i < snap.axes.size(); ++i) {
        axes->Set(ctx, static_cast<uint32_t>(i), v8::Number::New(isolate, snap.axes[i])).Check();
    }
    v8util::SetValue(isolate, pad, "axes", axes);

    // buttons: { pressed, touched, value }
    v8::Local<v8::Array> buttons = v8::Array::New(isolate, static_cast<int>(snap.buttons.size()));
    for (size_t i = 0; i < snap.buttons.size(); ++i) {
        v8::Local<v8::Object> b = v8::Object::New(isolate);
        v8util::SetValue(isolate, b, "pressed", v8::Boolean::New(isolate, snap.pressed[i]));
        v8util::SetValue(isolate, b, "touched", v8::Boolean::New(isolate, snap.pressed[i]));
        v8util::SetValue(isolate, b, "value", v8::Number::New(isolate, snap.buttons[i]));
        buttons->Set(ctx, static_cast<uint32_t>(i), b).Check();
    }
    v8util::SetValue(isolate, pad, "buttons", buttons);

    return pad;
}

// navigator.getGamepads()
void GetGamepads(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    GamepadManager* mgr = HostContext::From(isolate)->gamepad;

    const auto& snaps = mgr ? mgr->snapshots() : std::vector<GamepadSnapshot>{};
    v8::Local<v8::Array> result = v8::Array::New(isolate, static_cast<int>(snaps.size()));
    for (size_t i = 0; i < snaps.size(); ++i) {
        if (snaps[i].connected) {
            result->Set(ctx, static_cast<uint32_t>(i),
                        BuildGamepad(isolate, ctx, snaps[i], static_cast<int>(i))).Check();
        } else {
            result->Set(ctx, static_cast<uint32_t>(i), v8::Null(isolate)).Check();
        }
    }
    args.GetReturnValue().Set(result);
}

} // namespace

// navigator は DomShims が生成するため、ここでは navigator.getGamepads を後付けする。
void InstallGamepad(v8::Isolate* isolate, v8::Local<v8::Object> global, HostContext* /*host*/)
{
    v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::Local<v8::Value> nav_val;
    if (!global->Get(ctx, Str(isolate, "navigator")).ToLocal(&nav_val) || !nav_val->IsObject()) {
        return;
    }
    v8::Local<v8::Object> navigator = nav_val.As<v8::Object>();
    v8util::SetMethod(isolate, navigator, "getGamepads", GetGamepads);
}

} // namespace next2d
