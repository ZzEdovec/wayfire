#include "together-dock-dodge-v1-protocol.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <wayfire/core.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>

namespace
{
constexpr uint32_t TOGETHER_DOCK_DODGE_VERSION = 1;

bool is_attached_to_scenegraph(wf::scene::node_t *node)
{
    while (node)
    {
        if (node == wf::get_core().scene().get())
        {
            return true;
        }

        node = node->parent();
    }

    return false;
}

wf::scene::node_t *find_lca(wf::scene::node_t *a, wf::scene::node_t *b)
{
    std::vector<wf::scene::node_t*> ancestors;
    for (auto it = a; it; it = it->parent())
    {
        ancestors.push_back(it);
    }

    for (auto it = b; it; it = it->parent())
    {
        if (std::find(ancestors.begin(), ancestors.end(), it) != ancestors.end())
        {
            return it;
        }
    }

    return nullptr;
}

size_t find_index_in_parent(wf::scene::node_t *node, wf::scene::node_t *parent)
{
    while (node && node->parent() != parent)
    {
        node = node->parent();
    }

    if (!node || !parent)
    {
        return 0;
    }

    const auto& children = parent->get_children();
    auto it = std::find(children.begin(), children.end(), node->shared_from_this());
    return it == children.end() ? 0 : std::distance(children.begin(), it);
}

bool is_above_in_scenegraph(wayfire_view a, wayfire_view b)
{
    if (!a || !b)
    {
        return false;
    }

    auto *x = a->get_root_node().get();
    auto *y = b->get_root_node().get();
    if (!x || !y || !is_attached_to_scenegraph(x) || !is_attached_to_scenegraph(y))
    {
        return false;
    }

    auto *lca = find_lca(x, y);
    if (!lca || (lca == x) || (lca == y))
    {
        return false;
    }

    return find_index_in_parent(x, lca) > find_index_in_parent(y, lca);
}

bool can_obscure(wayfire_view view)
{
    if (!view || !view->is_mapped())
    {
        return false;
    }

    if (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT)
    {
        return false;
    }

    if (auto toplevel = wf::toplevel_cast(view); toplevel && toplevel->minimized)
    {
        return false;
    }

    return true;
}

class together_dock_dodge_protocol_impl;

class dock_dodge_surface_t
{
  public:
    dock_dodge_surface_t(together_dock_dodge_protocol_impl *manager,
        wl_client *client, uint32_t version, uint32_t id, wl_resource *surface_resource);
    ~dock_dodge_surface_t();

    void schedule_update();
    wl_resource *get_resource() const
    {
        return resource;
    }

    together_dock_dodge_protocol_impl *get_manager() const
    {
        return manager;
    }

  private:
    together_dock_dodge_protocol_impl *manager;
    wl_resource *resource = nullptr;
    wl_resource *surface_resource = nullptr;
    wlr_surface *surface = nullptr;
    bool last_obscured = false;
    bool has_sent_state = false;
    wf::wl_idle_call idle_update;
    wf::wl_listener_wrapper on_surface_destroy;
    wf::wl_listener_wrapper on_surface_commit;

    void update_state();
    void send_state(bool obscured);
    wayfire_view get_target_view() const;
};

class together_dock_dodge_protocol_impl : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        global = wl_global_create(wf::get_core().display,
            &together_dock_dodge_manager_v1_interface,
            TOGETHER_DOCK_DODGE_VERSION, this, bind_manager);

        wf::get_core().connect(&on_view_mapped);
        wf::get_core().connect(&on_view_unmapped);
        wf::get_core().connect(&on_view_geometry_changed);
        wf::get_core().connect(&on_view_set_output);
        wf::get_core().connect(&on_view_minimized);
        wf::get_core().connect(&on_view_activated);
    }

    void fini() override
    {
        tracked_surfaces.clear();
    }

    bool is_unloadable() override
    {
        return false;
    }

    void add_surface(std::unique_ptr<dock_dodge_surface_t> tracked)
    {
        tracked_surfaces[tracked->get_resource()] = std::move(tracked);
    }

    void remove_surface(wl_resource *resource)
    {
        tracked_surfaces.erase(resource);
    }

    void schedule_update_all()
    {
        for (auto& [_, tracked] : tracked_surfaces)
        {
            tracked->schedule_update();
        }
    }

  private:
    wl_global *global = nullptr;
    std::map<wl_resource*, std::unique_ptr<dock_dodge_surface_t>> tracked_surfaces;

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (auto*)
    {
        schedule_update_all();
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped = [=] (auto*)
    {
        schedule_update_all();
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed = [=] (auto*)
    {
        schedule_update_all();
    };

    wf::signal::connection_t<wf::view_set_output_signal> on_view_set_output = [=] (auto*)
    {
        schedule_update_all();
    };

    wf::signal::connection_t<wf::view_minimized_signal> on_view_minimized = [=] (auto*)
    {
        schedule_update_all();
    };

    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated = [=] (auto*)
    {
        schedule_update_all();
    };

    static void handle_manager_destroy(wl_resource*)
    {}

    static void handle_resource_destroy_request(wl_client*, wl_resource *resource)
    {
        wl_resource_destroy(resource);
    }

    static void handle_manager_get_surface(wl_client *client, wl_resource *resource,
        uint32_t id, wl_resource *surface)
    {
        auto *self = static_cast<together_dock_dodge_protocol_impl*>(wl_resource_get_user_data(resource));
        auto tracked = std::make_unique<dock_dodge_surface_t>(self, client,
            wl_resource_get_version(resource), id, surface);
        tracked->schedule_update();
        self->add_surface(std::move(tracked));
    }

    static const struct together_dock_dodge_manager_v1_interface manager_impl;

    static void bind_manager(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        auto *resource = wl_resource_create(client, &together_dock_dodge_manager_v1_interface,
            std::min<uint32_t>(version, TOGETHER_DOCK_DODGE_VERSION), id);
        wl_resource_set_implementation(resource, &manager_impl, data, handle_manager_destroy);
    }

    friend class dock_dodge_surface_t;
};

const struct together_dock_dodge_manager_v1_interface together_dock_dodge_protocol_impl::manager_impl = {
    .destroy = together_dock_dodge_protocol_impl::handle_resource_destroy_request,
    .get_dock_dodge_surface = together_dock_dodge_protocol_impl::handle_manager_get_surface,
};

void destroy_dock_dodge_surface_resource(wl_resource *resource)
{
    auto *tracked = static_cast<dock_dodge_surface_t*>(wl_resource_get_user_data(resource));
    if (!tracked)
    {
        return;
    }

    auto *manager = tracked->get_manager();
    wl_resource_set_user_data(resource, nullptr);
    manager->remove_surface(resource);
}

static void handle_dock_dodge_surface_destroy_request(wl_client*, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

const struct together_dock_dodge_surface_v1_interface dock_dodge_surface_impl = {
    .destroy = handle_dock_dodge_surface_destroy_request,
};

dock_dodge_surface_t::dock_dodge_surface_t(together_dock_dodge_protocol_impl *manager,
    wl_client *client, uint32_t version, uint32_t id, wl_resource *surface_resource)
    : manager(manager), surface_resource(surface_resource)
{
    resource = wl_resource_create(client, &together_dock_dodge_surface_v1_interface, version, id);
    wl_resource_set_implementation(resource, &dock_dodge_surface_impl, this,
        destroy_dock_dodge_surface_resource);

    surface = wlr_surface_from_resource(surface_resource);
    if (surface)
    {
        on_surface_destroy.set_callback([this] (void*)
        {
            wl_resource_destroy(resource);
        });
        on_surface_destroy.connect(&surface->events.destroy);

        on_surface_commit.set_callback([this] (void*)
        {
            schedule_update();
        });
        on_surface_commit.connect(&surface->events.commit);
    }
}

dock_dodge_surface_t::~dock_dodge_surface_t() = default;

wayfire_view dock_dodge_surface_t::get_target_view() const
{
    if (!surface_resource)
    {
        return nullptr;
    }

    return wf::wl_surface_to_wayfire_view(surface_resource);
}

void dock_dodge_surface_t::schedule_update()
{
    idle_update.run_once([this] ()
    {
        update_state();
    });
}

void dock_dodge_surface_t::send_state(bool obscured)
{
    if (has_sent_state && (last_obscured == obscured))
    {
        return;
    }

    together_dock_dodge_surface_v1_send_obscured(resource,
        obscured ? TOGETHER_DOCK_DODGE_SURFACE_V1_STATE_OBSCURED : TOGETHER_DOCK_DODGE_SURFACE_V1_STATE_CLEAR);
    has_sent_state = true;
    last_obscured  = obscured;
}

void dock_dodge_surface_t::update_state()
{
    auto target_view = get_target_view();
    if (!target_view || !target_view->is_mapped())
    {
        send_state(false);
        return;
    }

    auto *output = target_view->get_output();
    if (!output)
    {
        send_state(false);
        return;
    }

    wf::geometry_t target_box = target_view->get_bounding_box();
    if ((target_box.width <= 0) || (target_box.height <= 0))
    {
        send_state(false);
        return;
    }

    bool obscured = false;
    for (auto candidate : wf::get_core().get_all_views())
    {
        if ((candidate == target_view) || !can_obscure(candidate))
        {
            continue;
        }

        if (candidate->get_output() != output)
        {
            continue;
        }

        if (!is_above_in_scenegraph(candidate, target_view))
        {
            continue;
        }

        if (!(candidate->get_bounding_box() & target_box))
        {
            continue;
        }

        obscured = true;
        break;
    }

    send_state(obscured);
}

DECLARE_WAYFIRE_PLUGIN(together_dock_dodge_protocol_impl);
}
