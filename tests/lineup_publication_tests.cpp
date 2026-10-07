#include "lineup/control_publication_internal.h"
#include <iostream>
int main() {
    lineup::detail::ControlPublication cursor;
    lineup::control::Request idle, locate, observation;
    int failures = 0;
    const auto check = [&](bool ok, const char *message) { if (!ok) { ++failures; std::cerr << message << '\n'; } };
    check(cursor.wants(idle, false), "首次连接需要取消基线"); cursor.accepted(idle);
    check(!cursor.wants(idle, false), "空闲轮询不能重复取消刚按下的F9");
    check(!cursor.wants(idle, true), "等待首帧不是取消");
    locate.mode = lineup::control::Mode::LOCATE;
    locate.observation.sequence = 12; locate.observation.identity.selection_generation = 3;
    check(cursor.wants(locate, true), "显式定位到达后立即发布"); cursor.accepted(locate);
    observation = locate; observation.mode = lineup::control::Mode::OBSERVATION;
    check(!cursor.wants(observation, true), "同一帧不重复发布");
    ++observation.observation.sequence;
    check(cursor.wants(observation, true), "新鲜观察继续发布"); cursor.accepted(observation);
    check(!cursor.wants(idle, true), "再次定位准备首帧不撤销新的F9意图");
    check(cursor.wants(idle, false), "用户取消仍立即发布"); cursor.accepted(idle);
    check(!cursor.wants(idle, false), "真正取消也不重复发布");
    cursor.reset(); check(cursor.wants(idle, false), "重连必须重新建立取消基线");
    return failures ? 1 : 0;
}
