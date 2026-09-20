#pragma once

#include <waywallen_display_protocol_types.h>

#include <QColor>
#include <QRectF>

inline QRectF qtRect(const ww_rect_t& rect) {
    return { static_cast<qreal>(rect.x),
             static_cast<qreal>(rect.y),
             static_cast<qreal>(rect.w),
             static_cast<qreal>(rect.h) };
}

inline QColor qtColor(const waywallen_rgba_color_t& color) {
    return QColor::fromRgbF(static_cast<qreal>(color.r),
                            static_cast<qreal>(color.g),
                            static_cast<qreal>(color.b),
                            static_cast<qreal>(color.a));
}
