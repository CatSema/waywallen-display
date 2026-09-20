#include "ww-presentation-widget.h"

#include "ww-shadow-paintable.h"

#include <math.h>
#include <unistd.h>

typedef struct {
    WwShadowPaintable* paintable;
    guint64            buffer_generation;
    guint64            content_token;
    guint64            presentation_config_generation;
    guint64            composition_generation;
} WwPresentationContent;

struct _WwPresentationWidget {
    GtkWidget parent_instance;

    WwDisplay* display;
    double     display_scale;
    gulong     display_handlers[5];

    WwPresentationContent current;
    WwPresentationContent candidate;
    WwShadowPaintable*    outgoing;
    GdkTexture*           retained_scene;

    guint64  submitted_token;
    guint64  committed_token;
    gboolean submission_pending;
    gboolean snapshot_submitted;

    guint64          transition_config_generation;
    WwTransitionKind transition_kind;
    guint            transition_duration_ms;
    guint            transition_angle;
    double           transition_origin_x;
    double           transition_origin_y;

    WwTransitionKind active_kind;
    guint            active_duration_ms;
    guint            active_angle;
    double           active_origin_x;
    double           active_origin_y;
    double           transition_progress;
    gint64           transition_start_us;
    gboolean         transition_pending;
    gboolean         transition_active;
    guint            tick_id;

    GdkFrameClock* frame_clock;
    gulong         after_paint_id;
};

G_DEFINE_FINAL_TYPE(WwPresentationWidget, ww_presentation_widget, GTK_TYPE_WIDGET)

enum
{
    SIGNAL_BINDING_STAGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void disconnect_display(WwPresentationWidget* self) {
    if (! self->display) return;
    for (guint i = 0; i < G_N_ELEMENTS(self->display_handlers); ++i) {
        if (self->display_handlers[i] != 0) {
            g_signal_handler_disconnect(self->display, self->display_handlers[i]);
            self->display_handlers[i] = 0;
        }
    }
    g_clear_object(&self->display);
}

static void content_clear(WwPresentationContent* content) {
    g_clear_object(&content->paintable);
    content->buffer_generation              = 0;
    content->content_token                  = 0;
    content->presentation_config_generation = 0;
    content->composition_generation         = 0;
}

static void clear_outgoing(WwPresentationWidget* self) {
    g_clear_object(&self->outgoing);
    g_clear_object(&self->retained_scene);
}

static double clamp_progress(double progress) { return CLAMP(progress, 0.0, 1.0); }

static double eased_progress(double progress) {
    progress = clamp_progress(progress);
    if (progress < 0.5) return 4.0 * progress * progress * progress;
    const double inverse = -2.0 * progress + 2.0;
    return 1.0 - inverse * inverse * inverse / 2.0;
}

static void transition_mask_stops(double progress, GskColorStop stops[6]) {
    const double feather   = 0.04;
    const double edge      = progress * (1.0 + feather);
    const double lower     = edge - feather;
    const double offsets[] = {
        0.0,
        CLAMP(lower, 0.0, 1.0),
        CLAMP(lower + feather / 3.0, 0.0, 1.0),
        CLAMP(lower + feather * 2.0 / 3.0, 0.0, 1.0),
        CLAMP(edge, 0.0, 1.0),
        1.0,
    };
    for (guint i = 0; i < G_N_ELEMENTS(offsets); ++i) {
        const double t     = CLAMP((offsets[i] - lower) / feather, 0.0, 1.0);
        const double alpha = 1.0 - t * t * (3.0 - 2.0 * t);
        stops[i]           = (GskColorStop) {
            .offset = (float)offsets[i],
            .color  = { 1.0, 1.0, 1.0, alpha },
        };
    }
}

static void snapshot_paintable(GtkSnapshot* snapshot, WwShadowPaintable* paintable, float width,
                               float height) {
    if (! paintable) return;
    gdk_paintable_snapshot(
        GDK_PAINTABLE(paintable), GDK_SNAPSHOT(snapshot), (double)width, (double)height);
}

static void snapshot_outgoing(WwPresentationWidget* self, GtkSnapshot* snapshot, float width,
                              float height) {
    if (self->retained_scene) {
        graphene_rect_t bounds;
        graphene_rect_init(&bounds, 0.0f, 0.0f, width, height);
        gtk_snapshot_append_texture(snapshot, self->retained_scene, &bounds);
        return;
    }
    snapshot_paintable(snapshot, self->outgoing, width, height);
}

static void append_wipe_mask(WwPresentationWidget* self, GtkSnapshot* snapshot,
                             const graphene_rect_t* bounds, double progress) {
    const double radians = (double)(self->active_angle % 360u) * G_PI / 180.0;
    const double vx      = cos(radians);
    const double vy      = sin(radians);
    const double dots[]  = {
        0.0,
        bounds->size.width * vx,
        bounds->size.height * vy,
        bounds->size.width * vx + bounds->size.height * vy,
    };
    double minimum = dots[0];
    double maximum = dots[0];
    for (guint i = 1; i < G_N_ELEMENTS(dots); ++i) {
        minimum = MIN(minimum, dots[i]);
        maximum = MAX(maximum, dots[i]);
    }
    const double     span = MAX(maximum - minimum, 1.0);
    graphene_point_t start;
    graphene_point_t end;
    const double     cx         = bounds->size.width * 0.5;
    const double     cy         = bounds->size.height * 0.5;
    const double     center_dot = cx * vx + cy * vy;
    graphene_point_init(&start,
                        (float)(cx + vx * (minimum - center_dot)),
                        (float)(cy + vy * (minimum - center_dot)));
    graphene_point_init(
        &end, (float)(cx + vx * (maximum - center_dot)), (float)(cy + vy * (maximum - center_dot)));

    GskColorStop stops[6];
    transition_mask_stops(progress, stops);
    gtk_snapshot_append_linear_gradient(snapshot, bounds, &start, &end, stops, G_N_ELEMENTS(stops));
}

static void append_grow_mask(WwPresentationWidget* self, GtkSnapshot* snapshot,
                             const graphene_rect_t* bounds, double progress) {
    graphene_point_t center;
    graphene_point_init(&center,
                        (float)(bounds->size.width * self->active_origin_x),
                        (float)(bounds->size.height * self->active_origin_y));
    const double distances[] = {
        hypot(center.x, center.y),
        hypot(bounds->size.width - center.x, center.y),
        hypot(center.x, bounds->size.height - center.y),
        hypot(bounds->size.width - center.x, bounds->size.height - center.y),
    };
    double reach = 1.0;
    for (guint i = 0; i < G_N_ELEMENTS(distances); ++i) reach = MAX(reach, distances[i]);

    GskColorStop stops[6];
    transition_mask_stops(progress, stops);
    gtk_snapshot_append_radial_gradient(snapshot,
                                        bounds,
                                        &center,
                                        (float)reach,
                                        (float)reach,
                                        0.0f,
                                        1.0f,
                                        stops,
                                        G_N_ELEMENTS(stops));
}

static void snapshot_scene(WwPresentationWidget* self, GtkSnapshot* snapshot, float width,
                           float height) {
    const gboolean transitioning = self->transition_pending || self->transition_active;
    if (! transitioning) {
        snapshot_paintable(snapshot, self->current.paintable, width, height);
        return;
    }

    snapshot_outgoing(self, snapshot, width, height);
    const double progress = clamp_progress(self->transition_progress);
    if (progress <= 0.0) return;
    if (progress >= 1.0) {
        snapshot_paintable(snapshot, self->current.paintable, width, height);
        return;
    }

    if (self->active_kind == WW_TRANSITION_KIND_FADE) {
        gtk_snapshot_push_opacity(snapshot, progress);
        snapshot_paintable(snapshot, self->current.paintable, width, height);
        gtk_snapshot_pop(snapshot);
        return;
    }

    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0.0f, 0.0f, width, height);
    gtk_snapshot_push_mask(snapshot, GSK_MASK_MODE_ALPHA);
    if (self->active_kind == WW_TRANSITION_KIND_WIPE)
        append_wipe_mask(self, snapshot, &bounds, progress);
    else
        append_grow_mask(self, snapshot, &bounds, progress);
    gtk_snapshot_pop(snapshot);
    snapshot_paintable(snapshot, self->current.paintable, width, height);
    gtk_snapshot_pop(snapshot);
}

static GdkTexture* capture_scene(WwPresentationWidget* self) {
    const int width  = gtk_widget_get_width(GTK_WIDGET(self));
    const int height = gtk_widget_get_height(GTK_WIDGET(self));
    if (width <= 0 || height <= 0) return NULL;

    GtkSnapshot* snapshot = gtk_snapshot_new();
    snapshot_scene(self, snapshot, (float)width, (float)height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    if (! node) return NULL;
    GtkNative*   native   = gtk_widget_get_native(GTK_WIDGET(self));
    GskRenderer* renderer = native ? gtk_native_get_renderer(native) : NULL;
    if (! renderer) {
        gsk_render_node_unref(node);
        return NULL;
    }
    graphene_rect_t viewport;
    graphene_rect_init(&viewport, 0.0f, 0.0f, (float)width, (float)height);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &viewport);
    gsk_render_node_unref(node);
    return texture;
}

static void cancel_transition(WwPresentationWidget* self) {
    self->transition_pending  = FALSE;
    self->transition_active   = FALSE;
    self->transition_progress = 1.0;
    self->transition_start_us = 0;
    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    clear_outgoing(self);
}

static gboolean transition_tick(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data) {
    WwPresentationWidget* self        = WW_PRESENTATION_WIDGET(user_data);
    const gint64          now         = gdk_frame_clock_get_frame_time(frame_clock);
    const double          duration_us = MAX((double)self->active_duration_ms * 1000.0, 1.0);
    self->transition_progress =
        eased_progress((double)(now - self->transition_start_us) / duration_us);
    gtk_widget_queue_draw(widget);
    if (self->transition_progress < 1.0) return G_SOURCE_CONTINUE;

    self->tick_id = 0;
    cancel_transition(self);
    return G_SOURCE_REMOVE;
}

static void after_paint(GdkFrameClock* frame_clock, WwPresentationWidget* self) {
    if (! self->submission_pending || ! self->snapshot_submitted) return;
    self->snapshot_submitted = FALSE;
    self->submission_pending = FALSE;
    self->committed_token    = self->submitted_token;

    if (! self->transition_pending) return;
    self->transition_pending  = FALSE;
    self->transition_active   = TRUE;
    self->transition_start_us = gdk_frame_clock_get_frame_time(frame_clock);
    if (self->tick_id == 0) {
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), transition_tick, self, NULL);
    }
}

static void presentation_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    WwPresentationWidget* self   = WW_PRESENTATION_WIDGET(widget);
    const int             width  = gtk_widget_get_width(widget);
    const int             height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0) return;
    snapshot_scene(self, snapshot, (float)width, (float)height);
    if (self->submission_pending) self->snapshot_submitted = TRUE;
}

static void presentation_measure(GtkWidget* widget, GtkOrientation orientation, int for_size,
                                 int* minimum, int* natural, int* minimum_baseline,
                                 int* natural_baseline) {
    (void)widget;
    (void)orientation;
    (void)for_size;
    *minimum          = 0;
    *natural          = 0;
    *minimum_baseline = -1;
    *natural_baseline = -1;
}

static void presentation_realize(GtkWidget* widget) {
    GTK_WIDGET_CLASS(ww_presentation_widget_parent_class)->realize(widget);
    WwPresentationWidget* self = WW_PRESENTATION_WIDGET(widget);
    self->frame_clock          = gtk_widget_get_frame_clock(widget);
    if (self->frame_clock) {
        self->after_paint_id =
            g_signal_connect(self->frame_clock, "after-paint", G_CALLBACK(after_paint), self);
    }
}

static void presentation_unrealize(GtkWidget* widget) {
    WwPresentationWidget* self = WW_PRESENTATION_WIDGET(widget);
    if (self->frame_clock && self->after_paint_id != 0) {
        g_signal_handler_disconnect(self->frame_clock, self->after_paint_id);
        self->after_paint_id = 0;
    }
    self->frame_clock = NULL;
    cancel_transition(self);
    self->submission_pending = FALSE;
    self->snapshot_submitted = FALSE;
    GTK_WIDGET_CLASS(ww_presentation_widget_parent_class)->unrealize(widget);
}

static void ww_presentation_widget_dispose(GObject* object) {
    WwPresentationWidget* self = WW_PRESENTATION_WIDGET(object);
    disconnect_display(self);
    ww_presentation_widget_clear(self);
    G_OBJECT_CLASS(ww_presentation_widget_parent_class)->dispose(object);
}

static void ww_presentation_widget_class_init(WwPresentationWidgetClass* klass) {
    GObjectClass*   object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    object_class->dispose        = ww_presentation_widget_dispose;
    widget_class->snapshot       = presentation_snapshot;
    widget_class->measure        = presentation_measure;
    widget_class->realize        = presentation_realize;
    widget_class->unrealize      = presentation_unrealize;
    gtk_widget_class_set_css_name(widget_class, "waywallen-presentation");

    signals[SIGNAL_BINDING_STAGED] = g_signal_new("binding-staged",
                                                  G_TYPE_FROM_CLASS(klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  5,
                                                  G_TYPE_UINT,
                                                  G_TYPE_UINT,
                                                  G_TYPE_UINT,
                                                  G_TYPE_UINT,
                                                  G_TYPE_INT);
}

static void ww_presentation_widget_init(WwPresentationWidget* self) {
    self->display_scale          = 1.0;
    self->transition_kind        = WW_TRANSITION_KIND_NONE;
    self->transition_duration_ms = 400;
    self->transition_origin_x    = 0.5;
    self->transition_origin_y    = 0.5;
    self->transition_progress    = 1.0;
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

WwPresentationWidget* ww_presentation_widget_new(void) {
    return g_object_new(WW_TYPE_PRESENTATION_WIDGET, NULL);
}

static void on_display_binding_ready(WwDisplay* display, guint64 buffer_generation,
                                     guint64 content_token, guint64 presentation_config_generation,
                                     guint64 composition_generation, guint count, guint width,
                                     guint height, guint fourcc, guint64 modifier, gint backend,
                                     gdouble sx, gdouble sy, gdouble sw, gdouble sh, gdouble dx,
                                     gdouble dy, gdouble dw, gdouble dh, guint transform,
                                     gdouble cr, gdouble cg, gdouble cb, gdouble ca,
                                     WwPresentationWidget* self) {
    (void)modifier;
    gint    fd              = -1;
    guint   n_planes        = 0;
    guint   strides[4]      = { 0 };
    guint64 offsets[4]      = { 0 };
    guint64 shadow_modifier = 0;
    if (! ww_display_get_shadow_export(
            display, &fd, &n_planes, strides, offsets, &shadow_modifier)) {
        g_warning("ww_presentation_widget: shadow export is unavailable");
        return;
    }
    const double scale = self->display_scale;
    if (! ww_presentation_widget_stage_shadow(self,
                                              fd,
                                              n_planes,
                                              width,
                                              height,
                                              fourcc,
                                              shadow_modifier,
                                              strides,
                                              offsets,
                                              buffer_generation,
                                              content_token,
                                              presentation_config_generation,
                                              composition_generation,
                                              sx,
                                              sy,
                                              sw,
                                              sh,
                                              dx / scale,
                                              dy / scale,
                                              dw / scale,
                                              dh / scale,
                                              transform,
                                              cr,
                                              cg,
                                              cb,
                                              ca)) {
        g_warning("ww_presentation_widget: failed to stage shadow");
        return;
    }
    g_signal_emit(self, signals[SIGNAL_BINDING_STAGED], 0, count, width, height, fourcc, backend);
}

static void on_display_textures_releasing(WwDisplay* display, guint64 buffer_generation,
                                          WwPresentationWidget* self) {
    (void)display;
    ww_presentation_widget_retire_binding(self, buffer_generation);
}

static void on_display_composition(WwDisplay* display, guint64 composition_generation,
                                   guint64 buffer_generation, gdouble sx, gdouble sy, gdouble sw,
                                   gdouble sh, gdouble dx, gdouble dy, gdouble dw, gdouble dh,
                                   guint transform, gdouble cr, gdouble cg, gdouble cb, gdouble ca,
                                   WwPresentationWidget* self) {
    (void)display;
    const double scale = self->display_scale;
    ww_presentation_widget_set_composition(self,
                                           composition_generation,
                                           buffer_generation,
                                           sx,
                                           sy,
                                           sw,
                                           sh,
                                           dx / scale,
                                           dy / scale,
                                           dw / scale,
                                           dh / scale,
                                           transform,
                                           cr,
                                           cg,
                                           cb,
                                           ca);
}

static void on_display_frame(WwDisplay* display, guint64 buffer_generation, guint buffer_index,
                             guint64 seq, gint release_fd, WwPresentationWidget* self) {
    (void)display;
    (void)buffer_index;
    (void)seq;
    ww_presentation_widget_frame_ready(self, buffer_generation);
    ww_display_close_fd(release_fd);
}

static void on_display_presentation(WwDisplay* display, guint64 config_generation,
                                    guint64 state_generation, guint pause_kind, guint blur_radius,
                                    gboolean pause_active, guint transition_kind, guint duration_ms,
                                    guint angle, gdouble origin_x, gdouble origin_y,
                                    WwPresentationWidget* self) {
    (void)display;
    (void)state_generation;
    (void)pause_kind;
    (void)blur_radius;
    (void)pause_active;
    ww_presentation_widget_set_transition(self,
                                          config_generation,
                                          (WwTransitionKind)transition_kind,
                                          duration_ms,
                                          angle,
                                          origin_x,
                                          origin_y);
}

void ww_presentation_widget_set_display(WwPresentationWidget* self, WwDisplay* display,
                                        gdouble scale) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    g_return_if_fail(display == NULL || WW_IS_DISPLAY(display));
    disconnect_display(self);
    ww_presentation_widget_clear(self);
    self->display_scale = isfinite(scale) && scale > 0.0 ? scale : 1.0;
    if (! display) return;

    self->display = g_object_ref(display);
    self->display_handlers[0] =
        g_signal_connect(display, "binding-ready", G_CALLBACK(on_display_binding_ready), self);
    self->display_handlers[1] = g_signal_connect(
        display, "textures-releasing", G_CALLBACK(on_display_textures_releasing), self);
    self->display_handlers[2] =
        g_signal_connect(display, "composition-config", G_CALLBACK(on_display_composition), self);
    self->display_handlers[3] =
        g_signal_connect(display, "frame-ready", G_CALLBACK(on_display_frame), self);
    self->display_handlers[4] = g_signal_connect(
        display, "presentation-snapshot", G_CALLBACK(on_display_presentation), self);
}

gboolean ww_presentation_widget_stage_shadow(
    WwPresentationWidget* self, gint fd, guint n_planes, guint width, guint height, guint fourcc,
    guint64 modifier, const guint strides[4], const guint64 offsets[4], guint64 buffer_generation,
    guint64 content_token, guint64 presentation_config_generation, guint64 composition_generation,
    gdouble sx, gdouble sy, gdouble sw, gdouble sh, gdouble dx, gdouble dy, gdouble dw, gdouble dh,
    guint transform, gdouble cr, gdouble cg, gdouble cb, gdouble ca) {
    g_return_val_if_fail(WW_IS_PRESENTATION_WIDGET(self), FALSE);
    if (fd < 0 || n_planes == 0 || n_planes > 4 || width == 0 || height == 0 || ! strides ||
        ! offsets || buffer_generation == 0 || content_token == 0) {
        if (fd >= 0) close(fd);
        return FALSE;
    }

    WwShadowPaintable* paintable = ww_shadow_paintable_new();
    if (! ww_shadow_paintable_set_shadow(
            paintable, fd, n_planes, width, height, fourcc, modifier, strides, offsets)) {
        g_object_unref(paintable);
        return FALSE;
    }
    ww_shadow_paintable_set_composition(
        paintable, sx, sy, sw, sh, dx, dy, dw, dh, transform, cr, cg, cb, ca);

    content_clear(&self->candidate);
    self->candidate.paintable                      = paintable;
    self->candidate.buffer_generation              = buffer_generation;
    self->candidate.content_token                  = content_token;
    self->candidate.presentation_config_generation = presentation_config_generation;
    self->candidate.composition_generation         = composition_generation;
    return TRUE;
}

void ww_presentation_widget_set_composition(WwPresentationWidget* self,
                                            guint64               composition_generation,
                                            guint64 buffer_generation, gdouble sx, gdouble sy,
                                            gdouble sw, gdouble sh, gdouble dx, gdouble dy,
                                            gdouble dw, gdouble dh, guint transform, gdouble cr,
                                            gdouble cg, gdouble cb, gdouble ca) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    WwPresentationContent* content = NULL;
    if (self->candidate.buffer_generation == buffer_generation)
        content = &self->candidate;
    else if (self->current.buffer_generation == buffer_generation)
        content = &self->current;
    if (! content || composition_generation <= content->composition_generation) return;

    content->composition_generation = composition_generation;
    ww_shadow_paintable_set_composition(
        content->paintable, sx, sy, sw, sh, dx, dy, dw, dh, transform, cr, cg, cb, ca);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void ww_presentation_widget_retire_binding(WwPresentationWidget* self, guint64 buffer_generation) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    if (self->candidate.buffer_generation == buffer_generation) content_clear(&self->candidate);
}

void ww_presentation_widget_frame_ready(WwPresentationWidget* self, guint64 buffer_generation) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    if (self->candidate.buffer_generation != buffer_generation) {
        if (self->current.buffer_generation == buffer_generation && self->current.paintable) {
            ww_shadow_paintable_refresh(self->current.paintable);
            gtk_widget_queue_draw(GTK_WIDGET(self));
        }
        return;
    }

    ww_shadow_paintable_refresh(self->candidate.paintable);
    const gboolean same_current = self->current.content_token != 0 &&
                                  self->current.content_token == self->candidate.content_token;
    const gboolean same_committed =
        self->committed_token != 0 && self->committed_token == self->candidate.content_token;
    const gboolean configured =
        self->transition_kind != WW_TRANSITION_KIND_NONE && self->transition_config_generation != 0;
    const gboolean should_transition = self->committed_token != 0 && ! same_committed &&
                                       ! same_current && configured && self->current.paintable;

    if (should_transition) {
        GdkTexture* retained = NULL;
        if (self->transition_pending || self->transition_active) retained = capture_scene(self);
        clear_outgoing(self);
        if (retained)
            self->retained_scene = retained;
        else {
            if (self->transition_pending || self->transition_active)
                g_warning("ww_presentation_widget: failed to retain interrupted scene");
            self->outgoing = g_object_ref(self->current.paintable);
        }

        self->active_kind         = self->transition_kind;
        self->active_duration_ms  = self->transition_duration_ms;
        self->active_angle        = self->transition_angle;
        self->active_origin_x     = self->transition_origin_x;
        self->active_origin_y     = self->transition_origin_y;
        self->transition_pending  = TRUE;
        self->transition_active   = FALSE;
        self->transition_progress = 0.0;
        if (self->tick_id != 0) {
            gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
            self->tick_id = 0;
        }
    } else if (! same_current) {
        cancel_transition(self);
    }

    content_clear(&self->current);
    self->current            = self->candidate;
    self->candidate          = (WwPresentationContent) { 0 };
    self->submitted_token    = self->current.content_token;
    self->submission_pending = TRUE;
    self->snapshot_submitted = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void ww_presentation_widget_set_transition(WwPresentationWidget* self, guint64 config_generation,
                                           WwTransitionKind kind, guint duration_ms, guint angle,
                                           gdouble origin_x, gdouble origin_y) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    self->transition_config_generation = config_generation;
    self->transition_kind              = kind;
    self->transition_duration_ms       = duration_ms;
    self->transition_angle             = angle;
    self->transition_origin_x          = origin_x;
    self->transition_origin_y          = origin_y;
    if (self->transition_pending && kind != WW_TRANSITION_KIND_NONE) {
        self->active_kind        = kind;
        self->active_duration_ms = duration_ms;
        self->active_angle       = angle;
        self->active_origin_x    = origin_x;
        self->active_origin_y    = origin_y;
    }
    if (kind == WW_TRANSITION_KIND_NONE) {
        cancel_transition(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

void ww_presentation_widget_clear(WwPresentationWidget* self) {
    g_return_if_fail(WW_IS_PRESENTATION_WIDGET(self));
    cancel_transition(self);
    content_clear(&self->candidate);
    content_clear(&self->current);
    self->submitted_token    = 0;
    self->committed_token    = 0;
    self->submission_pending = FALSE;
    self->snapshot_submitted = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
