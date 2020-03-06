#include <math.h>
#include <cairo.h>
#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-manager.hpp>

extern "C"
{
#include <wlr/types/wlr_output.h>
}

#define WIDGET_PADDING 10

class wayfire_bench_screen : public wf::plugin_interface_t
{
    int frames;
    cairo_t *cr;
    double text_y;
    double max_fps;
    double widget_xc;
    char fps_buf[128];
    uint32_t last_time;
    double current_fps;
    double widget_radius;
    bool hook_set = false;
    wf::framebuffer_t bench_tex;
    wf::geometry_t cairo_geometry;
    cairo_surface_t *cairo_surface;
    cairo_text_extents_t text_extents;
    wf::option_wrapper_t<std::string> position{"bench/position"};
    wf::option_wrapper_t<int> frames_per_update{"bench/frames_per_update"};

    public:
    void init() override
    {
        grab_interface->name = "bench";
        grab_interface->capabilities = 0;

        if (!hook_set)
        {
            hook_set = true;
            output->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);
            output->render->add_effect(&overlay_hook, wf::OUTPUT_EFFECT_OVERLAY);
            output->render->set_redraw_always();
        }

        last_time = wf::get_current_time();
        cr = nullptr;
        max_fps = 0;
        frames = 0;

        output->connect_signal("reserved-workarea", &workarea_changed);
        position.set_callback(position_changed);
        update_texture_position();
    }

    wf::config::option_base_t::updated_callback_t position_changed = [=] ()
    {
        update_texture_position();
    };

    void cairo_recreate()
    {
        auto og = output->get_relative_geometry();
        auto font_size = og.height * 0.05;

        if (!cr)
        {
            /* Setup dummy context to get initial font size */
            cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
            cr = cairo_create(cairo_surface);
        }

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, font_size);

        cairo_text_extents (cr, "234.5", &text_extents);

        widget_xc = text_extents.width / 2 + text_extents.x_bearing + WIDGET_PADDING;
        text_y = text_extents.height + WIDGET_PADDING;
        widget_radius = og.height * 0.04;

        cairo_geometry.width = text_extents.width + WIDGET_PADDING * 2;
        cairo_geometry.height = text_extents.height + widget_radius +
            (widget_radius * sin(M_PI / 8)) + WIDGET_PADDING * 2;

        /* Recreate surface based on font size */
        cairo_destroy(cr);
        cairo_surface_destroy(cairo_surface);

        cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
            cairo_geometry.width, cairo_geometry.height);
        cr = cairo_create(cairo_surface);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, font_size);
    }

    void update_texture_position()
    {
        auto workarea = output->workspace->get_workarea();

        cairo_recreate();

        if ((std::string) position == "top_left")
        {
            cairo_geometry.x = workarea.x;
            cairo_geometry.y = workarea.y;
        } else if ((std::string) position == "top_center")
        {
            cairo_geometry.x = workarea.x + (workarea.width / 2 - cairo_geometry.width / 2);
            cairo_geometry.y = workarea.y;
        } else if ((std::string) position == "top_right")
        {
            cairo_geometry.x = workarea.x + (workarea.width - cairo_geometry.width);
            cairo_geometry.y = workarea.y;
        } else if ((std::string) position == "center_left")
        {
            cairo_geometry.x = workarea.x;
            cairo_geometry.y = workarea.y + (workarea.height / 2 - cairo_geometry.height / 2);
        } else if ((std::string) position == "center")
        {
            cairo_geometry.x = workarea.x + (workarea.width / 2 - cairo_geometry.width / 2);
            cairo_geometry.y = workarea.y + (workarea.height / 2 - cairo_geometry.height / 2);
        } else if ((std::string) position == "center_right")
        {
            cairo_geometry.x = workarea.x + (workarea.width - cairo_geometry.width);
            cairo_geometry.y = workarea.y + (workarea.height / 2 - cairo_geometry.height / 2);
        } else if ((std::string) position == "bottom_left")
        {
            cairo_geometry.x = workarea.x;
            cairo_geometry.y = workarea.y + (workarea.height - cairo_geometry.height);
        } else if ((std::string) position == "bottom_center")
        {
            cairo_geometry.x = workarea.x + (workarea.width / 2 - cairo_geometry.width / 2);
            cairo_geometry.y = workarea.y + (workarea.height - cairo_geometry.height);
        } else if ((std::string) position == "bottom_right")
        {
            cairo_geometry.x = workarea.x + (workarea.width - cairo_geometry.width);
            cairo_geometry.y = workarea.y + (workarea.height - cairo_geometry.height);
        } else
        {
            cairo_geometry.x = workarea.x;
            cairo_geometry.y = workarea.y;
        }

        output->render->damage_whole();
    }

    wf::signal_connection_t workarea_changed{[this] (wf::signal_data_t *data)
    {
        update_texture_position();
    }};

    void cairo_clear(cairo_t *cr)
    {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_paint(cr);
    }

    /* GLESv2 doesn't support GL_BGRA */
    void cairo_set_source_rgba_swizzle(cairo_t *cr, double r, double g, double b, double a)
    {
        cairo_set_source_rgba(cr, b, g, r, a);
    }

    void render_bench()
    {
        double xc = widget_xc;
        double yc = widget_radius + WIDGET_PADDING;
        double radius = widget_radius;
        double min_angle = M_PI / 8;
        double max_angle = M_PI - M_PI / 8;
        double target_angle = 2 * M_PI - M_PI / 8;
        double fps_angle;

        if (output->handle->current_mode)
        {
            fps_angle = max_angle + (current_fps /
                ((double) output->handle->current_mode->refresh / 1000)) *
                (target_angle - max_angle);
        }
        else
        {
            fps_angle = max_angle + (current_fps / max_fps) * (target_angle - max_angle);
        }

        cairo_clear(cr);

        cairo_set_line_width(cr, 5.0);

        cairo_set_source_rgba_swizzle(cr, 0, 0, 0, 1);
        cairo_arc_negative(cr, xc, yc, radius, min_angle, max_angle);
        cairo_stroke(cr);

        cairo_set_source_rgba_swizzle(cr, 0.7, 0.7, 0.7, 0.7);
        cairo_move_to(cr, xc, yc);
        cairo_arc_negative(cr, xc, yc, radius, min_angle, max_angle);
        cairo_fill(cr);

        cairo_set_source_rgba_swizzle(cr, 1.0, 0.2, 0.2, 0.7);
        cairo_move_to(cr, xc, yc);
        cairo_arc_negative(cr, xc, yc, radius, fps_angle, max_angle);
        cairo_fill(cr);

        if (output->handle->current_mode)
            cairo_set_source_rgba_swizzle(cr, 0, 0, 1, 1);
        else
            cairo_set_source_rgba_swizzle(cr, 1, 1, 0, 1);

        cairo_text_extents (cr, fps_buf, &text_extents);
        cairo_move_to(cr,
            xc - text_extents.width / 2 + text_extents.x_bearing,
            text_y + yc);
        cairo_show_text(cr, fps_buf);
        cairo_stroke(cr);
    }

    void update_fps()
    {
        uint32_t elapsed;
        uint32_t current_time = wf::get_current_time();
        elapsed = current_time - last_time;

        last_time = current_time;

        current_fps = (double) 1000 / elapsed;

        if (current_fps > max_fps)
            max_fps = current_fps;
        else
            max_fps -= 1;

        sprintf(fps_buf, "%.1f", current_fps);
    }

    wf::effect_hook_t pre_hook = [=] ()
    {
        output->render->damage({
            cairo_geometry.x, cairo_geometry.y,
            cairo_geometry.width, cairo_geometry.height});
    };

    wf::effect_hook_t overlay_hook = [=] ()
    {
        update_fps();
        if (frames++ >= frames_per_update)
        {
             render_bench();
             frames = 0;
        }

        OpenGL::render_begin();
        bench_tex.allocate(cairo_geometry.width, cairo_geometry.height);
        GL_CALL(glBindTexture(GL_TEXTURE_2D, bench_tex.tex));
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            cairo_geometry.width, cairo_geometry.height,
            0, GL_RGBA, GL_UNSIGNED_BYTE,
            cairo_image_surface_get_data(cairo_surface)));
        gl_geometry src_geometry = {
            (float) cairo_geometry.x,
            (float) cairo_geometry.y,
            (float) cairo_geometry.x + cairo_geometry.width,
            (float) cairo_geometry.y + cairo_geometry.height};
        auto fb = output->render->get_target_framebuffer();
        fb.bind();
        OpenGL::render_transformed_texture(bench_tex.tex, src_geometry, {},
            fb.get_orthographic_projection(), glm::vec4(1.0),
            TEXTURE_TRANSFORM_INVERT_Y);
        OpenGL::render_end();
    };

    void unset_hook()
    {
        if (!hook_set)
            return;

        output->render->set_redraw_always(false);
        output->render->rem_effect(&pre_hook);
        output->render->rem_effect(&overlay_hook);
        hook_set = false;
    }

    void fini() override
    {
        unset_hook();
        cairo_surface_destroy(cairo_surface);
        cairo_destroy(cr);
        output->render->damage({
            cairo_geometry.x, cairo_geometry.y,
            cairo_geometry.width, cairo_geometry.height});
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_bench_screen);