#ifndef GODOT_MCP_PLUGIN_H
#define GODOT_MCP_PLUGIN_H

#include "editor/plugins/editor_plugin.h"

class CheckButton;
class Label;
class LineEdit;
class SpinBox;
class Button;
class VBoxContainer;

class McpEditorPlugin : public EditorPlugin {
	GDCLASS(McpEditorPlugin, EditorPlugin);

public:
	McpEditorPlugin();
	~McpEditorPlugin();

protected:
	void _bind_methods();
	void _enter_tree() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

private:
	void _on_toggle(bool p_enabled);
	void _on_apply();
	void _refresh_status();

	VBoxContainer *panel = nullptr;
	Label *status_label = nullptr;
	Label *url_label = nullptr;
	CheckButton *enabled_check = nullptr;
	SpinBox *port_spin = nullptr;
	LineEdit *bind_edit = nullptr;
	LineEdit *token_edit = nullptr;
	uint64_t last_refresh = 0;
};

#endif // GODOT_MCP_PLUGIN_H