// GamepadManager: ゲームパッド状態を W3C Gamepad API (standard mapping) 相当の
// スナップショットとして保持する。
//
// 既定: GameInput でポーリングする。
// NEXT2D_GAMEPAD_EXTERNAL=1: ポーリングせず、埋め込み側が SetSnapshot で状態を
// 書き込む (入力 API をホスト本体の外に持つ構成用)。
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

    // 外部入力モード用: index 番のパッド状態を書き込む。範囲外は無視。
    void SetSnapshot(int index, const GamepadSnapshot& snapshot);

private:
    IGameInput* game_input_ = nullptr;
    std::vector<GamepadSnapshot> snapshots_;
};

} // namespace next2d
