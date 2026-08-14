#include "mcp_plugin.h"

#include "core/config/engine.h"
#include "core/math/color.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/time.h"
#include "core/string/string_name.h"
#include "editor/settings/editor_settings.h"
#include "mcp_server.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/spin_box.h"
#include "servers/text/text_server.h"

McpEditorPlugin::McpEditorPlugin() {
}

McpEditorPlugin::~McpEditorPlugin() {
}

void McpEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_toggle", "enabled"), &McpEditorPlugin::_on_toggle);
	ClassDB::bind_method(D_METHOD("_on_apply"), &McpEditorPlugin::_on_apply);
	ClassDB::bind_method(D_METHOD("_refresh_status"), &McpEditorPlugin::_refresh_status);
}

void McpEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			panel = memnew(VBoxContainer);
	panel->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	panel->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	panel->add_theme_constant_override("separation", 6);

	Label *title = memnew(Label);
	title->set_text("Godot MCP Server");
	title->add_theme_font_size_override("font_size", 16);
	panel->add_child(title);

	status_label = memnew(Label);
	status_label->set_text("...");
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	panel->add_child(status_label);

	url_label = memnew(Label);
	url_label->set_text("");
	url_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	url_label->add_theme_color_override("font_color", Color(0.5, 0.8, 1.0));
	panel->add_child(url_label);

	enabled_check = memnew(CheckButton);
	enabled_check->set_text("Aktifkan MCP server");
	enabled_check->connect(SNAME("toggled"), callable_mp(this, &McpEditorPlugin::_on_toggle));
	panel->add_child(enabled_check);

	HBoxContainer *port_row = memnew(HBoxContainer);
	Label *port_label = memnew(Label);
	port_label->set_text("Port:");
	port_row->add_child(port_label);
	port_spin = memnew(SpinBox);
	port_spin->set_min(1);
	port_spin->set_max(65535);
	port_spin->set_value(8766);
	port_spin->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	port_row->add_child(port_spin);
	panel->add_child(port_row);

	HBoxContainer *bind_row = memnew(HBoxContainer);
	Label *bind_label = memnew(Label);
	bind_label->set_text("Bind:");
	bind_row->add_child(bind_label);
	bind_edit = memnew(LineEdit);
	bind_edit->set_placeholder("0.0.0.0 (LAN + localhost)");
	bind_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bind_row->add_child(bind_edit);
	panel->add_child(bind_row);

	HBoxContainer *token_row = memnew(HBoxContainer);
	Label *token_label = memnew(Label);
	token_label->set_text("Token:");
	token_row->add_child(token_label);
	token_edit = memnew(LineEdit);
	token_edit->set_placeholder("(opsional)");
	token_edit->set_secret(true);
	token_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	token_row->add_child(token_edit);
	panel->add_child(token_row);

	Button *apply = memnew(Button);
	apply->set_text("Terapkan / Mulai ulang");
	apply->connect(SNAME("pressed"), callable_mp(this, &McpEditorPlugin::_on_apply));
	panel->add_child(apply);

	Label *hint = memnew(Label);
	hint->set_text("Client: RikkaHub, Codex CLI, Claude CLI, OpenCode CLI\nLAN: http://<ip-tablet>:8766/mcp");
	hint->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	hint->add_theme_color_override("font_color", Color(0.6, 0.6, 0.6));
	panel->add_child(hint);

	add_control_to_dock(EditorPlugin::DOCK_SLOT_RIGHT_BR, panel);

	McpServer *s = McpServer::get_singleton();
	enabled_check->set_pressed(s->get_enabled());
	port_spin->set_value(s->get_port());
	bind_edit->set_text(s->get_bind());
	token_edit->set_text(s->get_token());
	s->start_if_enabled();
	_refresh_status();
			set_process(true);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			set_process(false);
			if (panel) {
				remove_control_from_docks(panel);
				panel->queue_free();
				panel = nullptr;
			}
		} break;

		case NOTIFICATION_PROCESS: {
			if (Time::get_singleton()->get_ticks_msec() - last_refresh > 500) {
				last_refresh = Time::get_singleton()->get_ticks_msec();
				_refresh_status();
			}
		} break;
	}
}

void McpEditorPlugin::_refresh_status() {
	if (!status_label) {
		return;
	}
	McpServer *s = McpServer::get_singleton();
	if (s->is_running()) {
		status_label->set_text("Status: RUNNING");
		url_label->set_text(s->get_mcp_url());
		url_label->add_theme_color_override("font_color", Color(0.4, 0.9, 0.4));
	} else {
		status_label->set_text("Status: STOPPED");
		url_label->set_text("");
	}
	if (enabled_check) {
		enabled_check->set_pressed(s->get_enabled());
	}
}

void McpEditorPlugin::_on_toggle(bool p_enabled) {
	McpServer *s = McpServer::get_singleton();
	EditorSettings *es = EditorSettings::get_singleton();
	if (es) {
		es->set_setting("mcp/enabled", p_enabled);
		es->save();
	}
	if (p_enabled) {
		s->start_if_enabled();
	} else {
		s->stop_server();
	}
	_refresh_status();
}

void McpEditorPlugin::_on_apply() {
	McpServer *s = McpServer::get_singleton();
	EditorSettings *es = EditorSettings::get_singleton();
	if (es) {
		es->set_setting("mcp/port", (int)port_spin->get_value());
		es->set_setting("mcp/bind", bind_edit->get_text());
		es->set_setting("mcp/token", token_edit->get_text());
		es->set_setting("mcp/enabled", enabled_check->is_pressed());
		es->save();
	}
	s->apply_config();
	_refresh_status();
}