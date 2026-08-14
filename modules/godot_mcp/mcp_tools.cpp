#include "mcp_tools.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "editor/editor_undo_redo_manager.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/io/json.h"
#include "editor/editor_interface.h"
#include "editor/settings/editor_settings.h"
#include "mcp_server.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "core/input/input_event.h"
#include "core/object/class_db.h"

#if defined(TOOLS_ENABLED)

Variant mcp_tool_ret_text(const String &p_text) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", p_text } } } }, { "isError", false } };
}

Variant mcp_tool_ret_error(const String &p_text) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", p_text } } } }, { "isError", true } };
}

Variant mcp_tool_ret_json(const Variant &p_value) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", JSON::stringify(p_value) } } } }, { "isError", false } };
}

static Node *_scene_root() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return nullptr;
	}
	return ei->get_edited_scene_root();
}

static Node *_resolve_node(const String &p_path) {
	Node *root = _scene_root();
	if (!root) {
		return nullptr;
	}
	if (p_path.is_empty() || p_path == "." || p_path == "/") {
		return root;
	}
	return root->get_node_or_null(NodePath(p_path));
}

static bool _has_project() {
	return ProjectSettings::get_singleton()->has_setting("application/config/name");
}

static bool _wildcard_match(const String &p_pattern, const String &p_str) {
	if (!p_pattern.contains("*")) {
		return p_pattern == p_str;
	}
	Vector<String> parts = p_pattern.split("*");
	int idx = 0;
	for (int i = 0; i < parts.size(); i++) {
		String part = parts[i];
		if (part.is_empty()) {
			continue;
		}
		int found = p_str.find(part, idx);
		if (found == -1) {
			return false;
		}
		if (i == 0 && !p_str.begins_with(part)) {
			return false;
		}
		idx = found + part.length();
	}
	if (!p_pattern.ends_with("*") && !p_str.ends_with(parts[parts.size() - 1])) {
		return false;
	}
	return true;
}

static void _walk_assets(const String &p_dir, Array &r_out, const String &p_pattern, bool p_recursive, int p_depth) {
	if (p_depth > 32) {
		return;
	}
	Ref<DirAccess> d = DirAccess::open(p_dir);
	if (d.is_null()) {
		return;
	}
	d->list_dir_begin();
	String f = d->get_next();
	while (!f.is_empty()) {
		if (f == "." || f == "..") {
			f = d->get_next();
			continue;
		}
		String full = p_dir.path_join(f);
		if (d->current_is_dir()) {
			if (f == ".godot") {
				f = d->get_next();
				continue;
			}
			if (p_recursive) {
				_walk_assets(full, r_out, p_pattern, true, p_depth + 1);
			}
		} else {
			if (_wildcard_match(p_pattern, full.replace("res://", ""))) {
				Dictionary entry;
				entry["path"] = full.replace("res://", "");
				entry["size"] = FileAccess::get_size(full);
				r_out.append(entry);
			}
		}
		f = d->get_next();
	}
	d->list_dir_end();
	d.unref();
}

static void _walk_scene(Node *p_node, Node *p_root, Dictionary &r_out) {
	r_out["name"] = p_node->get_name();
	r_out["type"] = p_node->get_class();
	r_out["path"] = p_root->get_path_to(p_node);
	if (p_node->get_script_instance()) {
		Ref<Script> scr = p_node->get_script();
		if (scr.is_valid()) {
			r_out["script"] = scr->get_path();
		}
	}
	Dictionary props;
	if (p_node->has_method("get")) {
		bool ok = false;
		Variant v = p_node->get("position", &ok);
		if (ok && (v.get_type() == Variant::VECTOR2 || v.get_type() == Variant::VECTOR3)) {
			props["position"] = v;
		}
		v = p_node->get("rotation", &ok);
		if (ok) {
			props["rotation"] = v;
		}
		v = p_node->get("scale", &ok);
		if (ok && (v.get_type() == Variant::VECTOR2 || v.get_type() == Variant::VECTOR3)) {
			props["scale"] = v;
		}
		v = p_node->get("visible", &ok);
		if (ok && v.get_type() == Variant::BOOL) {
			props["visible"] = v;
		}
		v = p_node->get("text", &ok);
		if (ok && (v.get_type() == Variant::STRING || v.get_type() == Variant::STRING_NAME)) {
			props["text"] = v;
		}
	}
	if (!props.is_empty()) {
		r_out["properties"] = props;
	}
	Array children;
	int n = p_node->get_child_count();
	for (int i = 0; i < n; i++) {
		Node *c = p_node->get_child(i);
		if (c->get_owner() != p_root && c != p_root) {
			continue;
		}
		if (String(c->get_name()).to_lower() == "editorpaint" && c->get_class() == "SubViewport") {
			continue;
		}
		Dictionary child;
		_walk_scene(c, p_root, child);
		children.append(child);
	}
	if (!children.is_empty()) {
		r_out["children"] = children;
	}
}

static void _append_icon(Dictionary &r_ret, const Ref<Image> &p_image) {
	PackedByteArray png = p_image->save_png_to_buffer();
	String b64 = CryptoCore::b64_encode_str(png.ptr(), png.size());
	Dictionary img;
	img["type"] = "image";
	img["data"] = b64;
	img["mimeType"] = "image/png";
	r_ret["content"] = Array{ img };
	r_ret["isError"] = false;
}

// ----------------------------------------------------------------- Tools

static Variant _tool_project_info(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("No project is currently open.");
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	Dictionary info;
	info["name"] = ps->get_setting("application/config/name", String());
	info["main_scene"] = ps->get_setting("application/run/main_scene", String());
	info["path"] = ProjectSettings::get_singleton()->globalize_path("res://");
	info["version"] = String("4.8-dev");
	return mcp_tool_ret_json(info);
}

static Variant _tool_list_assets(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("No project is currently open.");
	}
	String pattern = p_args.get("pattern", "*");
	bool recursive = p_args.get("recursive", true);
	Array out;
	_walk_assets("res://", out, pattern, recursive, 0);
	return mcp_tool_ret_json(out);
}

static Variant _tool_read_file(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Missing 'path'.");
	}
	if (!FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("File not found: %s", path));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot open file: %s", path));
	}
	String content = f->get_as_text();
	if (p_args.get("json", false)) {
		Variant parsed = JSON::parse_string(content);
		if (parsed.get_type() != Variant::NIL) {
			return mcp_tool_ret_json(parsed);
		}
	}
	return mcp_tool_ret_text(content);
}

static Variant _tool_write_file(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String content = p_args.get("content", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Missing 'path'.");
	}
	// Ensure parent dirs exist.
	String dir = path.get_base_dir();
	if (!dir.is_empty() && dir != "." && !dir.begins_with("res://")) {
		DirAccess::make_dir_recursive_absolute(dir);
	} else if (dir.begins_with("res://")) {
		DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(dir));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot write file: %s", path));
	}
	f->store_string(content);
	f->close();
	return mcp_tool_ret_text(vformat("Wrote %d bytes to %s", content.utf8().length(), path));
}

static Variant _tool_get_project_setting(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("No project is currently open.");
	}
	String name = p_args.get("name", String());
	if (name.is_empty()) {
		return mcp_tool_ret_error("Missing 'name'.");
	}
	Variant value = ProjectSettings::get_singleton()->get_setting(name);
	if (value.get_type() == Variant::NIL) {
		return mcp_tool_ret_error(vformat("Setting not found: %s", name));
	}
	Dictionary out;
	out["name"] = name;
	out["value"] = value;
	return mcp_tool_ret_json(out);
}

static Variant _tool_set_project_setting(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("No project is currently open.");
	}
	String name = p_args.get("name", String());
	if (name.is_empty()) {
		return mcp_tool_ret_error("Missing 'name'.");
	}
	Variant value = p_args.get("value", Variant());
	ProjectSettings::get_singleton()->set_setting(name, value);
	if (p_args.get("save", true)) {
		ProjectSettings::get_singleton()->save();
	}
	return mcp_tool_ret_text(vformat("Setting '%s' updated.", name));
}

static Variant _tool_get_scene_tree(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	Node *root = _scene_root();
	if (!root) {
		return mcp_tool_ret_error("No scene open.");
	}
	Dictionary tree;
	tree["scene_path"] = root->get_scene_file_path().is_empty() ? String("<untitled>") : root->get_scene_file_path();
	Dictionary r;
	_walk_scene(root, root, r);
	tree["root"] = r;
	return mcp_tool_ret_json(tree);
}

static Variant _tool_open_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	String path = p_args.get("path", String());
	if (path.is_empty() || !path.ends_with(".tscn") || !FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene not found: %s", path));
	}
	ei->open_scene_from_path(path);
	return mcp_tool_ret_text(vformat("Opened scene %s", path));
}

static Variant _tool_save_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	String path = p_args.get("path", String());
	if (!path.is_empty()) {
		if (path.ends_with(".tscn") || path.ends_with(".scn")) {
			ei->save_scene_as(path);
		} else {
			return mcp_tool_ret_error("Path must end with .tscn or .scn");
		}
	} else {
		ei->save_scene();
	}
	return mcp_tool_ret_text("Scene saved.");
}

static Variant _tool_create_scene(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String root_class = p_args.get("root_class", "Node");
	String root_name = p_args.get("root_name", "Root");
	if (path.is_empty() || !path.ends_with(".tscn")) {
		return mcp_tool_ret_error("Missing 'path' (must end with .tscn).");
	}
	if (FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene already exists: %s", path));
	}
	String content = vformat("[gd_scene format=3]\n\n[node name=\"%s\" type=\"%s\"]\n", root_name, root_class);
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot write scene: %s", path));
	}
	f->store_string(content);
	f->close();
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei) {
		ei->open_scene_from_path(path);
	}
	return mcp_tool_ret_text(vformat("Created scene %s", path));
}

static Variant _tool_add_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("No scene open.");
	}
	String class_name = p_args.get("class", "Node");
	String name = p_args.get("name", String());
	String parent_path = p_args.get("parent", String());
	Node *parent = parent_path.is_empty() || parent_path == "." ? root : _resolve_node(parent_path);
	if (!parent) {
		return mcp_tool_ret_error(vformat("Parent not found: %s", parent_path));
	}
	if (!ClassDB::class_exists(class_name)) {
		return mcp_tool_ret_error(vformat("Class not found: %s", class_name));
	}
	Object *obj = ClassDB::instantiate(class_name);
	Node *node = Object::cast_to<Node>(obj);
	if (!node) {
		memdelete(obj);
		return mcp_tool_ret_error(vformat("Class is not a Node: %s", class_name));
	}
	String final_name = name.is_empty() ? class_name : name;
	node->set_name(final_name);
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: add node %s", final_name));
	ur->add_do_method(parent, "add_child", node, true);
	ur->add_do_method(node, "set_owner", root, true);
	ur->add_undo_method(node, "set_owner", (Object *)nullptr, true);
	ur->add_undo_method(parent, "remove_child", node, true);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Added %s '%s' under %s", class_name, final_name, parent->get_path()));
}

static Variant _tool_remove_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("No scene open.");
	}
	String path = p_args.get("path", String());
	Node *node = _resolve_node(path);
	if (!node || node == root) {
		return mcp_tool_ret_error(vformat("Node not found: %s", path));
	}
	Node *parent = node->get_parent();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: remove node %s", node->get_name()));
	ur->add_do_method(parent, "remove_child", node);
	ur->add_undo_method(parent, "add_child", node, true);
	ur->add_undo_method(node, "set_owner", root, true);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Removed node %s", path));
}

static Variant _tool_rename_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	String path = p_args.get("path", String());
	String new_name = p_args.get("name", String());
	Node *node = _resolve_node(path);
	if (!node) {
		return mcp_tool_ret_error(vformat("Node not found: %s", path));
	}
	String old_name = node->get_name();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: rename node %s", old_name));
	ur->add_do_method(node, "set_name", new_name);
	ur->add_undo_method(node, "set_name", old_name);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Renamed %s -> %s", path, new_name));
}

static Variant _tool_get_node_property(const Dictionary &p_args) {
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!node) {
		return mcp_tool_ret_error(vformat("Node not found: %s", p_args.get("path", String())));
	}
	String prop = p_args.get("property", String());
	if (prop.is_empty()) {
		return mcp_tool_ret_error("Missing 'property'.");
	}
	bool ok = false;
	Variant v = node->get(prop, &ok);
	if (!ok) {
		return mcp_tool_ret_error(vformat("Property not found: %s", prop));
	}
	Dictionary out;
	out["path"] = p_args.get("path", String());
	out["property"] = prop;
	out["value"] = v;
	return mcp_tool_ret_json(out);
}

static Variant _tool_set_node_property(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!ei || !node) {
		return mcp_tool_ret_error(vformat("Node not found: %s", p_args.get("path", String())));
	}
	String prop = p_args.get("property", String());
	if (prop.is_empty()) {
		return mcp_tool_ret_error("Missing 'property'.");
	}
	Variant value = p_args.get("value", Variant());
	bool ok = false;
	Variant old = node->get(prop, &ok);
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: set %s.%s", p_args.get("path", String()), prop));
	ur->add_do_method(node, "set", prop, value);
	ur->add_undo_method(node, "set", prop, old);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Set %s.%s", p_args.get("path", String()), prop));
}

static Variant _tool_reparent_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("No scene open.");
	}
	Node *node = _resolve_node(p_args.get("path", String()));
	Node *new_parent = _resolve_node(p_args.get("new_parent", String()));
	if (!node || node == root) {
		return mcp_tool_ret_error(vformat("Node not found: %s", p_args.get("path", String())));
	}
	if (!new_parent) {
		return mcp_tool_ret_error(vformat("Parent not found: %s", p_args.get("new_parent", String())));
	}
	Node *old_parent = node->get_parent();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: reparent %s", node->get_name()));
	ur->add_do_method(new_parent, "add_child", node, true);
	ur->add_do_method(node, "set_owner", root, true);
	ur->add_undo_method(old_parent, "add_child", node, true);
	ur->add_undo_method(node, "set_owner", root, true);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Reparented %s under %s", node->get_name(), new_parent->get_path()));
}

static Variant _tool_read_script(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Missing 'path'.");
	}
	if (!FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Script not found: %s", path));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot open script: %s", path));
	}
	return mcp_tool_ret_text(f->get_as_text());
}

static Variant _tool_write_script(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String content = p_args.get("content", String());
	if (path.is_empty() || !path.ends_with(".gd")) {
		return mcp_tool_ret_error("'path' must end with .gd");
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot write script: %s", path));
	}
	f->store_string(content);
	f->close();
	return mcp_tool_ret_text(vformat("Wrote script %s (%d bytes)", path, content.utf8().length()));
}

static Variant _tool_attach_script(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!ei || !node) {
		return mcp_tool_ret_error(vformat("Node not found: %s", p_args.get("path", String())));
	}
	String script_path = p_args.get("script", String());
	if (script_path.is_empty()) {
		return mcp_tool_ret_error("Missing 'script' path.");
	}
	if (!FileAccess::exists(script_path)) {
		Ref<FileAccess> f = FileAccess::open(script_path, FileAccess::WRITE);
		if (f.is_null()) {
			return mcp_tool_ret_error(vformat("Cannot create script: %s", script_path));
		}
		f->store_string("extends Node\n");
		f->close();
	}
	Ref<Script> script = ResourceLoader::load(script_path);
	if (script.is_null()) {
		return mcp_tool_ret_error(vformat("Cannot load script: %s", script_path));
	}
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: attach %s to %s", script_path, node->get_path()));
	ur->add_do_method(node, "set_script", script);
	ur->add_undo_method(node, "set_script", (Object *)nullptr);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Attached script %s to %s", script_path, node->get_path()));
}

static Variant _tool_run_main_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	if (ei->is_playing_scene()) {
		return mcp_tool_ret_error("Game is already running.");
	}
	ei->play_main_scene();
	return mcp_tool_ret_text("Starting main scene.");
}

static Variant _tool_run_custom_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	String path = p_args.get("path", String());
	if (path.is_empty() || !FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene not found: %s", path));
	}
	if (ei->is_playing_scene()) {
		return mcp_tool_ret_error("Game is already running.");
	}
	ei->play_custom_scene(path);
	return mcp_tool_ret_text(vformat("Starting scene %s", path));
}

static Variant _tool_stop_game(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	if (!ei->is_playing_scene()) {
		return mcp_tool_ret_text("Game is not running.");
	}
	ei->stop_playing_scene();
	return mcp_tool_ret_text("Game stopped.");
}

static Variant _tool_game_state(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	Dictionary st;
	st["playing"] = ei->is_playing_scene();
	Node *root = _scene_root();
	if (root) {
		st["current_scene"] = root->get_scene_file_path();
	}
	return mcp_tool_ret_json(st);
}

static Key _key_from_name(const String &p_name) {
	if (p_name.is_empty()) {
		return Key::NONE;
	}
	if (p_name.is_valid_int()) {
		return (Key)p_name.to_int();
	}
	String n = p_name.to_upper();
	if (n.length() == 1 && n[0] >= 'A' && n[0] <= 'Z') {
		return (Key)(Key::A + (n[0] - 'A'));
	}
	if (n.length() == 1 && n[0] >= '0' && n[0] <= '9') {
		return (Key)(Key::KEY_0 + (n[0] - '0'));
	}
	struct KeyName {
		const char *name;
		Key key;
	};
	static const KeyName names[] = {
		{ "SPACE", Key::SPACE }, { "ENTER", Key::ENTER }, { "RETURN", Key::ENTER }, { "ESCAPE", Key::ESCAPE },
		{ "TAB", Key::TAB }, { "SHIFT", Key::SHIFT }, { "CTRL", Key::CTRL }, { "CONTROL", Key::CTRL },
		{ "ALT", Key::ALT }, { "LEFT", Key::LEFT }, { "RIGHT", Key::RIGHT }, { "UP", Key::UP }, { "DOWN", Key::DOWN },
		{ "BACKSPACE", Key::BACKSPACE }, { "DELETE", Key::KEY_DELETE }, { "HOME", Key::HOME }, { "END", Key::END },
		{ "PAGEUP", Key::PAGEUP }, { "PAGEDOWN", Key::PAGEDOWN }, { "F1", Key::F1 }, { "F2", Key::F2 },
		{ "F3", Key::F3 }, { "F4", Key::F4 }, { "F5", Key::F5 }, { "F6", Key::F6 }, { "F7", Key::F7 },
		{ "F8", Key::F8 }, { "F9", Key::F9 }, { "F10", Key::F10 }, { "F11", Key::F11 }, { "F12", Key::F12 },
	};
	for (const KeyName &kn : names) {
		if (n == kn.name) {
			return kn.key;
		}
	}
	return Key::NONE;
}

static Variant _tool_send_input(const Dictionary &p_args) {
	Input *in = Input::get_singleton();
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!in || !ei) {
		return mcp_tool_ret_error("Input not available.");
	}
	String kind = p_args.get("kind", "action");
	if (kind == "action") {
		String action = p_args.get("action", String());
		if (action.is_empty()) {
			return mcp_tool_ret_error("Missing 'action'.");
		}
		if (p_args.get("pressed", true)) {
			in->action_press(action);
		} else {
			in->action_release(action);
		}
		return mcp_tool_ret_text(vformat("Action %s %s", action, p_args.get("pressed", true) ? "pressed" : "released"));
	}
	if (kind == "key") {
		Key keycode = _key_from_name(p_args.get("key", String()));
		if (keycode == Key::NONE) {
			return mcp_tool_ret_error(vformat("Unknown key name: %s", p_args.get("key", String())));
		}
		Ref<InputEventKey> ev = memnew(InputEventKey);
		ev->set_keycode(keycode);
		ev->set_physical_keycode(keycode);
		ev->set_pressed(p_args.get("pressed", true));
		in->parse_input_event(ev);
		return mcp_tool_ret_text(vformat("Key %s %s", p_args.get("key", String()), p_args.get("pressed", true) ? "pressed" : "released"));
	}
	if (kind == "mouse_button") {
		Ref<InputEventMouseButton> ev = memnew(InputEventMouseButton);
		ev->set_button_index((MouseButton)(int)p_args.get("button", 1));
		ev->set_pressed(p_args.get("pressed", true));
		if (p_args.has("position")) {
			Array pos = p_args["position"];
			if (pos.size() >= 2) {
				ev->set_position(Vector2(pos[0], pos[1]));
				ev->set_global_position(Vector2(pos[0], pos[1]));
			}
		}
		in->parse_input_event(ev);
		return mcp_tool_ret_text("Mouse button sent.");
	}
	return mcp_tool_ret_error(vformat("Unknown input kind: %s", kind));
}

static Variant _tool_screenshot(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor is not available.");
	}
	String source = p_args.get("source", "editor");
	Ref<Image> img;
	if (source == "game" && ei->is_playing_scene()) {
		Window *w = SceneTree::get_singleton()->get_root();
		if (w) {
			img = w->get_texture()->get_image();
		}
	}
	if (img.is_null()) {
		SubViewport *vp = source == "2d" ? ei->get_editor_viewport_2d() : ei->get_editor_viewport_3d();
		if (vp) {
			img = vp->get_texture()->get_image();
		}
	}
	if (img.is_null()) {
		return mcp_tool_ret_error("Cannot capture screenshot (is the viewport available?).");
	}
	Dictionary ret;
	_append_icon(ret, img);
	return ret;
}

static Variant _tool_server_info(const Dictionary &p_args) {
	McpServer *s = McpServer::get_singleton();
	Dictionary info;
	info["running"] = s->is_running();
	info["port"] = s->get_port();
	info["bind"] = s->get_bind();
	info["enabled"] = s->get_enabled();
	info["token_set"] = !s->get_token().is_empty();
	info["url"] = s->get_mcp_url();
	Dictionary endpoints;
	endpoints["streamable_http"] = "POST /mcp";
	endpoints["sse"] = "GET /sse + POST /messages";
	info["endpoints"] = endpoints;
	return mcp_tool_ret_json(info);
}

static Dictionary _schema(bool p_required, const Vector<String> &p_props) {
	Dictionary props;
	for (const String &p : p_props) {
		props[p] = Dictionary{ { "type", "string" } };
	}
	Dictionary s;
	s["type"] = "object";
	s["properties"] = props;
	if (p_required) {
		Array req;
		for (const String &p : p_props) {
			req.append(p);
		}
		s["required"] = req;
	}
	return s;
}

static Dictionary _schema_any(const Vector<String> &p_optional) {
	Dictionary props;
	for (const String &p : p_optional) {
		props[p] = Dictionary{ { "type", "string" } };
	}
	Dictionary s;
	s["type"] = "object";
	s["properties"] = props;
	return s;
}

void mcp_register_tools(McpServer *p_server) {
	p_server->register_tool("project_info", "Get information about the currently open project (name, main scene, path).", _schema_any(Vector<String>()), _tool_project_info);
	p_server->register_tool("list_assets", "Recursively list asset files under res://. Args: pattern (wildcard), recursive (bool).", _schema_any(Vector<String>{ "pattern", "recursive" }), _tool_list_assets);
	p_server->register_tool("read_file", "Read a file as text. Args: path, json (bool, parse as JSON).", _schema(true, Vector<String>{ "path" }), _tool_read_file);
	p_server->register_tool("write_file", "Write text content to a file. Args: path, content.", _schema(true, Vector<String>{ "path", "content" }), _tool_write_file);
	p_server->register_tool("get_project_setting", "Read a project setting by name. Args: name.", _schema(true, Vector<String>{ "name" }), _tool_get_project_setting);
	p_server->register_tool("set_project_setting", "Set a project setting by name. Args: name, value, save (bool).", _schema(true, Vector<String>{ "name" }), _tool_set_project_setting);
	p_server->register_tool("get_scene_tree", "Get the current scene tree as JSON (nodes, types, paths, key properties).", _schema_any(Vector<String>()), _tool_get_scene_tree);
	p_server->register_tool("open_scene", "Open a .tscn scene in the editor. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_open_scene);
	p_server->register_tool("save_scene", "Save the current scene. Args: path (optional, save-as).", _schema_any(Vector<String>{ "path" }), _tool_save_scene);
	p_server->register_tool("create_scene", "Create a new .tscn scene and open it. Args: path, root_class, root_name.", _schema(true, Vector<String>{ "path" }), _tool_create_scene);
	p_server->register_tool("add_node", "Add a node to the scene. Args: class, name, parent (relative path).", _schema(true, Vector<String>{ "class" }), _tool_add_node);
	p_server->register_tool("remove_node", "Remove a node from the scene. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_remove_node);
	p_server->register_tool("rename_node", "Rename a node. Args: path, name.", _schema(true, Vector<String>{ "path", "name" }), _tool_rename_node);
	p_server->register_tool("get_node_property", "Read a node property. Args: path, property.", _schema(true, Vector<String>{ "path", "property" }), _tool_get_node_property);
	p_server->register_tool("set_node_property", "Set a node property (undoable). Args: path, property, value.", _schema(true, Vector<String>{ "path", "property" }), _tool_set_node_property);
	p_server->register_tool("reparent_node", "Move a node under a new parent. Args: path, new_parent.", _schema(true, Vector<String>{ "path", "new_parent" }), _tool_reparent_node);
	p_server->register_tool("read_script", "Read a GDScript file. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_read_script);
	p_server->register_tool("write_script", "Write a GDScript file. Args: path, content.", _schema(true, Vector<String>{ "path", "content" }), _tool_write_script);
	p_server->register_tool("attach_script", "Attach a script to a node (creates it if missing). Args: path (node), script.", _schema(true, Vector<String>{ "path", "script" }), _tool_attach_script);
	p_server->register_tool("run_main_scene", "Run the project's main scene (play).", _schema_any(Vector<String>()), _tool_run_main_scene);
	p_server->register_tool("run_custom_scene", "Run a specific scene. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_run_custom_scene);
	p_server->register_tool("stop_game", "Stop the running game.", _schema_any(Vector<String>()), _tool_stop_game);
	p_server->register_tool("game_state", "Get play state (running?, current scene).", _schema_any(Vector<String>()), _tool_game_state);
	p_server->register_tool("send_input", "Send input to the running game. Args: kind (action|key|mouse_button), action/key/button, pressed, position.", _schema(false, Vector<String>{ "kind", "action", "key", "button", "pressed", "position" }), _tool_send_input);
	p_server->register_tool("screenshot", "Capture a screenshot of the editor viewport (or the game when running). Returns a PNG image. Args: source (editor|game|2d).", _schema_any(Vector<String>{ "source" }), _tool_screenshot);
	p_server->register_tool("server_info", "Get MCP server status and endpoints.", _schema_any(Vector<String>()), _tool_server_info);
}

#endif // TOOLS_ENABLED