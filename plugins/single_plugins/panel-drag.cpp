#include <wayfire/core.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/view.hpp>
#include "wayfire/plugins/common/shared-core-data.hpp"
#include "wayfire/plugins/ipc/ipc-helpers.hpp"
#include "wayfire/plugins/ipc/ipc-method-repository.hpp"

class wayfire_panel_drag_t : public wf::plugin_interface_t
{
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

  public:
    void init() override
    {
        ipc_repo->register_method("panel-drag/start-move", ipc_start_move);
    }

    void fini() override
    {
        ipc_repo->unregister_method("panel-drag/start-move");
    }

    wf::ipc::method_callback ipc_start_move = [=] (const wf::json_t& params)
    {
        wayfire_toplevel_view view;
        auto view_id = wf::ipc::json_get_optional_view_id(params);

        if (view_id.has_value())
        {
            view = wf::toplevel_cast(wf::ipc::find_view_by_id(view_id.value()));
        } else
        {
            view = wf::toplevel_cast(wf::get_core().get_cursor_focus_view());
        }

        if (!view || !view->is_mapped())
        {
            return wf::ipc::json_error("toplevel view not found");
        }

        if (!(view->get_allowed_actions() & wf::VIEW_ALLOW_MOVE))
        {
            return wf::ipc::json_error("view does not allow move");
        }

        wf::get_core().default_wm->move_request(view);
        return wf::ipc::json_ok();
    };
};

DECLARE_WAYFIRE_PLUGIN(wayfire_panel_drag_t);
