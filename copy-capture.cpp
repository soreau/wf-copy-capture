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
#include <algorithm>
#include <time.h>
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
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <drm_fourcc.h>
}

void *plugin_ptr;

void ext_image_capture_source_v1_init(wlr_ext_image_capture_source_v1 *source);

namespace wf
{
namespace copy_capture
{
class copy_capture_instance
{
    wf::option_wrapper_t<int> min_width{"copy-capture/min_width"};
    wf::option_wrapper_t<int> max_width{"copy-capture/max_width"};
    wf::option_wrapper_t<int> min_height{"copy-capture/min_height"};
    wf::option_wrapper_t<int> max_height{"copy-capture/max_height"};
    wf::option_wrapper_t<bool> enable_cursor_capture{"copy-capture/enable_cursor_capture"};

    std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;
    wlr_ext_foreign_toplevel_handle_v1 *toplevel_handle = nullptr;
    wlr_swapchain *swapchain = nullptr;

  public:
    wf::option_wrapper_t<double> max_fps{"copy-capture/max_fps"};
    wlr_ext_image_capture_source_v1 toplevel_source{};
    wayfire_view selected_view    = nullptr;
    wl_resource *session_resource = nullptr;
    bool render_cursors = false;
    bool event_looping  = false;
    wf::auxilliary_buffer_t dst;
    bool frame_fail   = false;
    int64_t last_time = 0;

    copy_capture_instance()
    {}

    ~copy_capture_instance()
    {
        deactivate();
    }

    scene::damage_callback push_damage = [=] (wf::regionf_t region)
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

        wayfire_view view = selected_view;
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

    void view_snapshot()
    {
        wayfire_view view = selected_view;
        if (!view)
        {
            return;
        }

        if (!view->is_mapped() || !view->get_root_node() || !view->get_surface_root_node())
        {
            return;
        }

        auto root_node = view->get_surface_root_node();
        wf::geometry_t bbox = view->get_surface_root_node()->get_bounding_box();
        auto output = view->get_output();
        if (!output)
        {
            return;
        }

        /* Dimension Limits */
        bbox.width  = std::max(double(min_width), std::min(bbox.width, double(max_width)));
        bbox.height = std::max(double(min_height), std::min(bbox.height, double(max_height)));

        if ((toplevel_source.width != uint32_t(bbox.width)) ||
            (toplevel_source.height != uint32_t(bbox.height)))
        {
            toplevel_source.width  = bbox.width;
            toplevel_source.height = bbox.height;

            if (session_resource)
            {
                ext_image_copy_capture_session_v1_send_buffer_size(session_resource, bbox.width, bbox.height);
            }

            if (swapchain && swapchain->slots[0].acquired && swapchain->slots[0].buffer)
            {
                wl_signal_emit_mutable(&swapchain->slots[0].buffer->events.release, NULL);
            }

            dst.allocate({int(bbox.width), int(bbox.height)});

            if (swapchain)
            {
                swapchain->slots[0].buffer = dst.get_buffer();
                swapchain->width  = bbox.width;
                swapchain->height = bbox.height;
                wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&toplevel_source, swapchain,
                    wf::get_core().renderer);
            }
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
            } else if (wlr_xwayland_surface_try_from_wlr_surface(candidate->get_wlr_surface()))
            {
                if (!wf::toplevel_cast(candidate) || (candidate->get_app_id() == view->get_app_id()))
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

        if (enable_cursor_capture && render_cursors)
        {
            wlr_output_cursor *cursor;
            wl_list_for_each(cursor, &output->handle->cursors, link)
            {
                if (!cursor->enabled || !cursor->visible || !cursor->texture)
                {
                    continue;
                }

                wf::geometry_t geometry{(cursor->x - cursor->hotspot_x) / output->handle->scale - bbox.x,
                    (cursor->y - cursor->hotspot_y) / output->handle->scale - bbox.y,
                    cursor->width / output->handle->scale, cursor->height / output->handle->scale};

                wlr_render_texture_options opts{};
                opts.texture   = cursor->texture;
                opts.transform = output->handle->transform;
                opts.src_box   = cursor->src_box;
                opts.dst_box   = wf::to_integer_box(geometry);
                wf::region_t clip_region{geometry};
                opts.clip = clip_region.to_pixman();
                wlr_render_pass_add_texture(pass.get_wlr_pass(), &opts);
            }
        }

        pass.submit();
    }

    wlr_ext_image_capture_source_v1 *handle_new_request(void *data)
    {
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request =
            (wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request*)data;
        toplevel_handle = request->toplevel_handle;
        selected_view.reset((wf::view_interface_t*)request->toplevel_handle->data);
        if (!selected_view)
        {
            LOGI("Could not find selected view to satisfy copy capture request.");
            return nullptr;
        }

        ext_image_capture_source_v1_init(&toplevel_source);

        wf::geometry_t bbox = selected_view->get_surface_root_node()->get_bounding_box();

        /* Dimension Limits */
        bbox.width  = std::max(double(min_width), std::min(bbox.width, double(max_width)));
        bbox.height = std::max(double(min_height), std::min(bbox.height, double(max_height)));

        toplevel_source.width  = bbox.width;
        toplevel_source.height = bbox.height;
        wlr_drm_format format
        {
            .format = DRM_FORMAT_ABGR8888,
            .len    = 0,
        };
        swapchain = wlr_swapchain_create(wf::get_core().allocator, bbox.width, bbox.height, &format);
        if (!swapchain)
        {
            LOGE("Failed to create swapchain for copy capture request.");
            return nullptr;
        }

        wlr_swapchain_slot slot{};
        dst.allocate({int(bbox.width), int(bbox.height)});
        slot.buffer = dst.get_buffer();
        swapchain->slots[0] = slot;
        wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&toplevel_source, swapchain,
            wf::get_core().renderer);
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, &toplevel_source);

        return &toplevel_source;
    }

    void deactivate()
    {
        if (!selected_view)
        {
            return;
        }

        destroy_render_instance_manager();

        if (swapchain)
        {
            swapchain->slots[0].buffer = nullptr;
            wlr_swapchain_destroy(swapchain);
            swapchain = nullptr;
        }

        dst.free();
        toplevel_handle  = nullptr;
        selected_view    = nullptr;
        session_resource = nullptr;
        render_cursors   = false;
        event_looping    = false;
        last_time = 0;
    }
};

class copy_capture_plugin : public wf::plugin_interface_t
{
    wf::wl_listener_wrapper on_new_request, on_new_session;
    wlr_ext_image_copy_capture_manager_v1 *image_copy_capture;
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *foreign_toplevel_image_capture_source;

  public:
    std::map<wlr_ext_image_capture_source_v1*, std::unique_ptr<copy_capture_instance>> sessions;

    void init() override
    {
        image_copy_capture = wlr_ext_image_copy_capture_manager_v1_create(wf::get_core().display, 1);
        foreign_toplevel_image_capture_source =
            wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(wf::get_core().display, 1);

        on_new_request.set_callback([=] (void *data)
        {
            auto instance = std::make_unique<copy_capture_instance>();
            auto source   = instance->handle_new_request(data);
            if (!source)
            {
                return;
            }

            sessions[source] = std::move(instance);
        });
        on_new_request.connect(
            &foreign_toplevel_image_capture_source->events.new_request);

        on_new_session.set_callback([=] (void *data)
        {
            handle_new_session(data);
        });
        on_new_session.connect(&image_copy_capture->events.new_session);
    }

    void handle_new_session(void *data)
    {
        wlr_ext_image_copy_capture_session_v1 *session =
            (wlr_ext_image_copy_capture_session_v1*)data;

        if (sessions.find(session->source) == sessions.end())
        {
            return;
        }

        sessions[session->source]->session_resource = session->resource;
    }

    bool is_unloadable() override
    {
        return false;
    }

    void fini() override
    {
        for (auto & [source, instance] : sessions)
        {
            instance.reset();
        }

        sessions.clear();

        on_new_request.disconnect();
        on_new_session.disconnect();

        plugin_ptr = nullptr;
    }
};
}
}

wf::copy_capture::copy_capture_plugin *plugin;

static void source_start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors)
{
    LOGD(__func__, ": Client started toplevel copy capture.");
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    auto it = plugin->sessions.find(source);
    if ((it == plugin->sessions.end()) || !it->second)
    {
        return;
    }

    it->second->render_cursors = with_cursors;
    it->second->destroy_render_instance_manager();
    it->second->create_render_instance_manager();
    it->second->last_time  = wf::get_current_time();
    it->second->frame_fail = false;
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *source)
{
    LOGD(__func__, ": Client stopped toplevel copy capture.");
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    auto it = plugin->sessions.find(source);
    if (it != plugin->sessions.end())
    {
        it->second.reset();
        plugin->sessions.erase(it);
    }
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *source,
    bool schedule_frame)
{
    plugin = (wf::copy_capture::copy_capture_plugin*)plugin_ptr;
    if (!plugin)
    {
        return;
    }

    /** Use .find(). [] silently add to the array. */
    auto it = plugin->sessions.find(source);
    if ((it == plugin->sessions.end()) || !it->second)
    {
        return;
    }

    if (it->second->event_looping)
    {
        return;
    }

    it->second->event_looping = true;

    /*
     * Throttle the capture rate.
     *
     * NOTE: The previous implementation called wl_event_loop_dispatch()
     * here to block until the next frame was due. That function is not
     * re-entrant — we are already inside an event dispatch, and recursing
     * corrupts libwayland's internal event-array state, causing a SIGSEGV
     * as soon as a queued event (e.g. cursor motion) is dispatched by the
     * inner loop. Block with nanosleep() instead; the outer event loop will
     * resume normally once we return.
     */
    auto last_time  = it->second->last_time;
    int64_t elapsed = wf::get_current_time() - last_time;
    int fps    = std::max(1, (int)it->second->max_fps);
    int64_t ms = 1000 / fps;

    if (elapsed < ms)
    {
        struct timespec ts
        {
            .tv_sec = 0, .tv_nsec = (ms - elapsed) * 1000000L
        };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        {
            /* interrupted by signal, resume */
        }
    }

    /* Re-lookup: session may have gone away while we slept. */
    it = plugin->sessions.find(source);
    if ((it == plugin->sessions.end()) || !it->second)
    {
        return;
    }

    it->second->last_time = wf::get_current_time();

    if (!it->second->frame_fail)
    {
        it->second->view_snapshot();
    }

    it = plugin->sessions.find(source);
    if ((it == plugin->sessions.end()) || !it->second)
    {
        return;
    }

    wf::region_t damage = wf::region_t{wf::geometry_t{0, 0,
            double(it->second->toplevel_source.width),
            double(it->second->toplevel_source.height)}};
    wlr_ext_image_capture_source_v1_frame_event frame_event
    {
        .damage = damage.to_pixman(),
    };
    wl_signal_emit_mutable(&it->second->toplevel_source.events.frame, &frame_event);

    auto it2 = plugin->sessions.find(source);
    if ((it2 != plugin->sessions.end()) && it2->second)
    {
        it2->second->event_looping = false;
    }
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

    auto it = plugin->sessions.find(source);
    if ((it == plugin->sessions.end()) || !it->second)
    {
        return;
    }

    auto buffer = it->second->dst.get_buffer();
    if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer,
        wf::get_core().renderer))
    {
        LOGD("Failed to copy view buffer to client frame buffer!");

        it->second->frame_fail = true;
        return;
    }

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &ts);
    it->second->frame_fail = false;
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
    .start = source_start,
    .stop  = source_stop,
    .request_frame = source_request_frame,
    .copy_frame    = source_copy_frame,
};

void ext_image_capture_source_v1_init(wlr_ext_image_capture_source_v1 *source)
{
    wlr_ext_image_capture_source_v1_init(source, &source_impl);
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
