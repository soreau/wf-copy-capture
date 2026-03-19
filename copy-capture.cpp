/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <map>
#include <wayfire/plugin.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>

extern "C"
{
#include <wlr/types/wlr_xdg_shell.h>
#include "ext-image-copy-capture-v1-server-protocol.h"
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <drm_fourcc.h>
}

void *plugin_ptr;

void ext_image_capture_source_v1_init(wlr_ext_image_capture_source_v1 *source);
void ext_image_capture_source_v1_cursor_init(wlr_ext_image_capture_source_v1_cursor *source);

namespace wf
{
namespace copy_capture
{
class copy_capture_plugin : public wf::plugin_interface_t
{
    wf::option_wrapper_t<int> min_width{"copy-capture/min_width"};
    wf::option_wrapper_t<int> max_width{"copy-capture/max_width"};
    wf::option_wrapper_t<int> min_height{"copy-capture/min_height"};
    wf::option_wrapper_t<int> max_height{"copy-capture/max_height"};
    std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;
    std::map<wayfire_view, wlr_ext_foreign_toplevel_handle_v1*> toplevels;
    wf::wl_listener_wrapper on_new_request, on_new_session;
    wlr_ext_foreign_toplevel_handle_v1 *toplevel_handle;
    wlr_swapchain *swapchain;

  public:
    wf::option_wrapper_t<double> target_fps{"copy-capture/target_fps"};
    wlr_ext_image_capture_source_v1_cursor cursor_source;
    wlr_ext_image_capture_source_v1 toplevel_source;
    wayfire_view selected_view = nullptr;
    wl_resource *session_resource;
    wf::auxilliary_buffer_t dst;
    bool frame_fail = false;
    int64_t last_time;

    void init() override
    {
        on_new_request.set_callback([=] (void *data)
        {
            handle_new_request(data);
        });
        on_new_request.connect(
            &wf::get_core().protocols.foreign_toplevel_image_capture_source->events.new_request);

        on_new_session.set_callback([=] (void *data)
        {
            handle_new_session(data);
        });
        on_new_session.connect(&wf::get_core().protocols.image_copy_capture->events.new_session);

        for (auto& view : wf::get_core().get_all_views())
        {
            add_toplevel_view(view);
        }

        wf::get_core().connect(&on_view_mapped);
        wf::get_core().connect(&on_view_unmapped);
    }

    scene::damage_callback push_damage = [=] (wf::region_t region)
    {};

    void destroy_render_instance_manager()
    {
        if (!instance_manager)
        {
            return;
        }

        instance_manager.reset();
        instance_manager = nullptr;
    }

    void create_render_instance_manager()
    {
        if (instance_manager)
        {
            return;
        }

        wayfire_view view = get_view();
        if (!view)
        {
            return;
        }

        std::vector<scene::node_ptr> nodes;
        nodes.push_back(view->get_root_node());
        instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage,
            view->get_output());
        instance_manager->set_visibility_region(view->get_surface_root_node()->get_bounding_box());
    }

    wayfire_view get_view()
    {
        if (selected_view)
        {
            return selected_view;
        }

        for (auto & [view, toplevel] : toplevels)
        {
            if (toplevel == toplevel_handle)
            {
                return view;
            }
        }

        return nullptr;
    }

    void view_snapshot()
    {
        wayfire_view view = get_view();
        if (!view)
        {
            return;
        }

        auto root_node = view->get_surface_root_node();
        auto bbox   = view->get_root_node()->get_children_bounding_box();
        auto output = view->get_output();
        if (!output)
        {
            return;
        }

        /* Dimension Limits */
        bbox.width = std::max(int(min_width), std::min(bbox.width, int(max_width)));
        bbox.height = std::max(int(min_height), std::min(bbox.height, int(max_height)));

        if (toplevel_source.width != uint32_t(bbox.width) || toplevel_source.height != uint32_t(bbox.height))
        {
            toplevel_source.width  = bbox.width;
            toplevel_source.height = bbox.height;
            ext_image_copy_capture_session_v1_send_buffer_size(session_resource, bbox.width, bbox.height);
            if (swapchain->slots[0].acquired)
            {
                wl_signal_emit_mutable(&swapchain->slots[0].buffer->events.release, NULL);
            }
            dst.allocate({bbox.width, bbox.height});
            swapchain->slots[0].buffer = dst.get_buffer();
            swapchain->width = bbox.width;
            swapchain->height = bbox.height;
            wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&toplevel_source, swapchain,
                wf::get_core().renderer);
        }

        wf::render_target_t target = wf::render_target_t(dst.get_renderbuffer());

        target.geometry = bbox;
        target.scale    = 1.0f;

        std::vector<scene::render_instance_uptr> instances;

        auto views = wf::get_core().get_all_views();
        std::vector<wayfire_view> reversed_views_vector{views.rbegin(), views.rend()};
        for (auto& candidate : reversed_views_vector)
        {
            auto candidate_root_node = candidate->get_surface_root_node();
            if (!candidate->get_wlr_surface())
            {
                continue;
            }

            if (auto xdg_popup = wlr_xdg_popup_try_from_wlr_surface(candidate->get_wlr_surface()))
            {
                if (xdg_popup->parent == view->get_wlr_surface())
                {
                    candidate_root_node->gen_render_instances(instances, [] (auto) {}, view->get_output());
                }
            }
        }

        for (auto& candidate : wf::get_core().get_all_views())
        {
            auto candidate_root_node = candidate->get_surface_root_node();
            if (!candidate->get_wlr_surface())
            {
                continue;
            }

            if (candidate_root_node->parent() == dynamic_cast<wf::scene::node_t*>(root_node.get()))
            {
                candidate_root_node->gen_render_instances(instances, [] (auto) {}, view->get_output());
            }
        }

        auto children = view->get_root_node()->get_children();
        std::vector<std::shared_ptr<wf::scene::node_t>> reversed_node_vector{children.rbegin(),
            children.rend()};
        for (auto & node : reversed_node_vector)
        {
            node->gen_render_instances(instances, [] (auto) {}, view->get_output());
        }

        render_pass_params_t params;
        params.background_color = {0, 0, 0, 0};
        params.damage    = bbox;
        params.target    = target;
        params.instances = &instances;
        params.flags     = RPASS_CLEAR_BACKGROUND;
        auto pass = render_pass_t{params};
        pass.run_partial();

        wlr_output_cursor *cursor;
        wl_list_for_each(cursor, &output->handle->cursors, link)
        {
            if (!cursor->texture)
            {
                continue;
            }
            wf::geometry_t geometry{int(cursor->x - cursor->hotspot_x), int(cursor->y - cursor->hotspot_y), int(cursor->width), int(cursor->height)};
            pass.add_texture(wf::texture_t{cursor->texture}, target, geometry, wf::region_t{geometry}, 1.0);
        }
        pass.submit();
    }

    void handle_new_session(void *data)
    {
        wlr_ext_image_copy_capture_session_v1 *session =
            (wlr_ext_image_copy_capture_session_v1 *) data;
        session_resource = session->resource;
    }

    void handle_new_request(void *data)
    {
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request =
            (wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*)data;
        toplevel_handle = request->toplevel_handle;
        selected_view   = get_view();
        if (!selected_view)
        {
            LOGI("Could not find selected view to satisfy copy capture request.");
            return;
        }

        ext_image_capture_source_v1_init(&toplevel_source);
        ext_image_capture_source_v1_cursor_init(&cursor_source);
        wf::geometry_t bbox = selected_view->get_surface_root_node()->get_bounding_box();

        /* Dimension Limits */
        bbox.width = std::max(int(min_width), std::min(bbox.width, int(max_width)));
        bbox.height = std::max(int(min_height), std::min(bbox.height, int(max_height)));

        toplevel_source.width  = bbox.width;
        toplevel_source.height = bbox.height;
        wlr_drm_format format
        {
            .format = DRM_FORMAT_ABGR8888,
            .len    = 0,
        };
        swapchain = wlr_swapchain_create(wf::get_core().allocator, bbox.width, bbox.height, &format);
        wlr_swapchain_slot slot{};
        dst.allocate({bbox.width, bbox.height});
        slot.buffer = dst.get_buffer();
        swapchain->slots[0] = slot;
        wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&toplevel_source, swapchain,
            wf::get_core().renderer);
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, &toplevel_source);
    }

    void add_toplevel_view(wayfire_view view)
    {
        if (!view || (view->role != wf::VIEW_ROLE_TOPLEVEL))
        {
            return;
        }

        wlr_ext_foreign_toplevel_handle_v1_state state
        {
            strdup(view->get_title().c_str()),
            strdup(view->get_app_id().c_str()),
        };

        toplevels[view] = wlr_ext_foreign_toplevel_handle_v1_create(
            wf::get_core().protocols.foreign_toplevel_list, &state);
    }

    size_t remove_toplevel_view(wayfire_view view)
    {
        if (!view || (view->role != wf::VIEW_ROLE_TOPLEVEL))
        {
            return 0;
        }

        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevels[view]);
        return toplevels.erase(view);
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if (!ev->view)
        {
            return;
        }

        add_toplevel_view(ev->view);
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        if ((remove_toplevel_view(ev->view) > 0) && (selected_view == ev->view))
        {
            deactivate();
        }
    };

    void deactivate()
    {
        if (!selected_view)
        {
            return;
        }

        destroy_render_instance_manager();
        swapchain->slots[0].buffer = NULL;
        wlr_swapchain_destroy(swapchain);
        dst.free();
        selected_view = nullptr;
        session_resource = nullptr;
    }

    void fini() override
    {
        deactivate();
        on_view_mapped.disconnect();
        on_view_unmapped.disconnect();

        for (auto & [view, toplevel] : toplevels)
        {
            wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel);
        }

        plugin_ptr = nullptr;
    }
};
}
}

wf::copy_capture::copy_capture_plugin *plugin;

static void source_start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors)
{
    LOGI(__func__, ": Client started toplevel copy capture.");
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    plugin->destroy_render_instance_manager();
    plugin->create_render_instance_manager();
    plugin->last_time = wf::get_current_time();
    plugin->frame_fail = false;
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *source)
{
    LOGI(__func__, ": Client stopped toplevel copy capture.");
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    plugin->deactivate();
}

static bool event_looping;

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *source,
    bool schedule_frame)
{
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    if (event_looping)
    {
        return;
    }

    event_looping = true;
    int64_t elapsed = wf::get_current_time() - plugin->last_time;
    auto ms = 1000 / plugin->target_fps;
    while (elapsed < ms)
    {
        wl_event_loop_dispatch(wf::get_core().ev_loop, ms);
        wl_display_flush_clients(wf::get_core().display);
        elapsed = wf::get_current_time() - plugin->last_time;
    }
    event_looping = false;

    if (!plugin->session_resource)
    {
        return;
    }

    plugin->last_time += elapsed;
    if (!plugin->frame_fail)
    {
        plugin->view_snapshot();
    }
    wf::region_t damage = wf::region_t{wf::geometry_t{0, 0,
            int(plugin->toplevel_source.width),
            int(plugin->toplevel_source.height)}};
    wlr_ext_image_capture_source_v1_frame_event frame_event
    {
        .damage = damage.to_pixman(),
    };
    wl_signal_emit_mutable(&plugin->toplevel_source.events.frame, &frame_event);
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *source,
    struct wlr_ext_image_copy_capture_frame_v1 *frame,
    struct wlr_ext_image_capture_source_v1_frame_event *frame_event)
{
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    auto buffer = plugin->dst.get_buffer();
    if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer,
        wf::get_core().renderer))
    {
        LOGI("Failed to copy view buffer to client frame buffer!");
        if ((buffer->width != frame->buffer->width) || (buffer->height != frame->buffer->height))
        {
            LOGI("The src and dst sizes differ: ",
                buffer->width, "x", buffer->height, " : ",
                frame->buffer->width, "x", frame->buffer->height);
        }
        plugin->frame_fail = true;
        return;
    }

    ext_image_copy_capture_frame_v1_send_damage(frame->resource, 0, 0,
        plugin->toplevel_source.width, plugin->toplevel_source.height);

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &ts);
    plugin->frame_fail = false;
}

struct wlr_ext_image_capture_source_v1_cursor *get_pointer_cursor(
    struct wlr_ext_image_capture_source_v1 *source, struct wlr_seat *seat)
{
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return nullptr;
    }

    return &plugin->cursor_source;
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop  = source_stop,
    .request_frame = source_request_frame,
    .copy_frame    = source_copy_frame,
    .get_pointer_cursor = get_pointer_cursor,
};

void ext_image_capture_source_v1_init(wlr_ext_image_capture_source_v1 *source)
{
    wlr_ext_image_capture_source_v1_init(source, &source_impl);
}

void ext_image_capture_source_v1_cursor_init(wlr_ext_image_capture_source_v1_cursor *source)
{
    wlr_ext_image_capture_source_v1_cursor_init(source, &source_impl);
}

/* XXX: Hack: We need a copy of the class instance, so instead of using
 * the DECLARE_WAYFIRE_PLUGIN macro, we define our own. It shims in code
 * that provides us of a pointer to the class, whenever wayfire gets a
 * new instance. */
extern "C"
{
    wf::plugin_interface_t *newInstance()
    {
        auto p = new wf::copy_capture::copy_capture_plugin;
        plugin_ptr = (void*)p;
        return p;
    }

    uint32_t getWayfireVersion()
    {
        return WAYFIRE_API_ABI_VERSION;
    }
}
