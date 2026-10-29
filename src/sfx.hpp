#pragma once
// Sons desativados.
struct SoftSfx {
    int port = -1; bool ready = false;
    bool init(int) { return false; }
    void tone(float, float, float = 0) {}
    void nav() {} void ok() {} void back() {} void err() {} void select() {}
};
