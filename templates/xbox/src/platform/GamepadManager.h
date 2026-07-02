// GamepadManager: GameInput でゲームパッド状態をポーリングし、
// W3C Gamepad API (standard mapping) 相当のスナップショットへ変換する。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct IGameInput;

namespace next2d {

// standard mapping: 17 ボタン / 4 軸
struct GamepadSnapshot {
    bool connected = false;
    std::array<float, 4> axes = {0, 0, 0, 0};        // LX, LY, RX, RY
    std::array<float, 17> buttons = {};              // 押下量 (0.0-1.0)
    std::array<bool, 17> pressed = {};
    double timestamp = 0.0;
};

class GamepadManager {
public:
    GamepadManager();
    ~GamepadManager();

    bool Initialize();

    // 毎フレーム呼ぶ。now_ms は EventLoop::Now()。
    void Poll(double now_ms);

    // 現在の全パッドスナップショット (最大 4)
    const std::vector<GamepadSnapshot>& snapshots() const { return snapshots_; }

private:
    IGameInput* game_input_ = nullptr;
    std::vector<GamepadSnapshot> snapshots_;
};

} // namespace next2d
