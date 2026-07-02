#include "GamepadManager.h"

#include <GameInput.h>
#include <iostream>

namespace next2d {

GamepadManager::GamepadManager() = default;

GamepadManager::~GamepadManager()
{
    if (game_input_) {
        game_input_->Release();
        game_input_ = nullptr;
    }
}

bool GamepadManager::Initialize()
{
    if (FAILED(GameInputCreate(&game_input_))) {
        std::cerr << "[Gamepad] GameInputCreate failed" << std::endl;
        return false;
    }
    snapshots_.resize(4);
    return true;
}

void GamepadManager::Poll(double now_ms)
{
    for (auto& snap : snapshots_) {
        snap.connected = false;
    }
    if (!game_input_) {
        return;
    }

    IGameInputReading* reading = nullptr;
    if (FAILED(game_input_->GetCurrentReading(GameInputKindGamepad, nullptr, &reading))) {
        return;
    }

    GameInputGamepadState state = {};
    if (reading->GetGamepadState(&state)) {
        GamepadSnapshot& snap = snapshots_[0];
        snap.connected = true;
        snap.timestamp = now_ms;

        // 軸 (Y は W3C 仕様に合わせ反転)
        snap.axes[0] = state.leftThumbstickX;
        snap.axes[1] = -state.leftThumbstickY;
        snap.axes[2] = state.rightThumbstickX;
        snap.axes[3] = -state.rightThumbstickY;

        auto set = [&](int index, bool down, float value) {
            snap.pressed[index] = down;
            snap.buttons[index] = value;
        };

        const auto btn = state.buttons;
        // standard mapping index 準拠
        set(0,  btn & GameInputGamepadA, (btn & GameInputGamepadA) ? 1.f : 0.f);
        set(1,  btn & GameInputGamepadB, (btn & GameInputGamepadB) ? 1.f : 0.f);
        set(2,  btn & GameInputGamepadX, (btn & GameInputGamepadX) ? 1.f : 0.f);
        set(3,  btn & GameInputGamepadY, (btn & GameInputGamepadY) ? 1.f : 0.f);
        set(4,  btn & GameInputGamepadLeftShoulder,  (btn & GameInputGamepadLeftShoulder) ? 1.f : 0.f);
        set(5,  btn & GameInputGamepadRightShoulder, (btn & GameInputGamepadRightShoulder) ? 1.f : 0.f);
        set(6,  state.leftTrigger  > 0.f, state.leftTrigger);
        set(7,  state.rightTrigger > 0.f, state.rightTrigger);
        set(8,  btn & GameInputGamepadView,  (btn & GameInputGamepadView) ? 1.f : 0.f);
        set(9,  btn & GameInputGamepadMenu,  (btn & GameInputGamepadMenu) ? 1.f : 0.f);
        set(10, btn & GameInputGamepadLeftThumbstick,  (btn & GameInputGamepadLeftThumbstick) ? 1.f : 0.f);
        set(11, btn & GameInputGamepadRightThumbstick, (btn & GameInputGamepadRightThumbstick) ? 1.f : 0.f);
        set(12, btn & GameInputGamepadDPadUp,    (btn & GameInputGamepadDPadUp) ? 1.f : 0.f);
        set(13, btn & GameInputGamepadDPadDown,  (btn & GameInputGamepadDPadDown) ? 1.f : 0.f);
        set(14, btn & GameInputGamepadDPadLeft,  (btn & GameInputGamepadDPadLeft) ? 1.f : 0.f);
        set(15, btn & GameInputGamepadDPadRight, (btn & GameInputGamepadDPadRight) ? 1.f : 0.f);
        set(16, false, 0.f); // guide ボタンは GameInput では非公開
    }

    reading->Release();
}

} // namespace next2d
