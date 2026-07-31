#ifndef MOUSE_KMBOX_NET_INTERNAL_H
#define MOUSE_KMBOX_NET_INTERNAL_H

#include "mouse/mouse.h"

#include <memory>

namespace mouse::detail {

std::unique_ptr<IMouseController> create_kmbox_net_controller(
    const MouseConfig& config) noexcept;

} // namespace mouse::detail

#endif // MOUSE_KMBOX_NET_INTERNAL_H
