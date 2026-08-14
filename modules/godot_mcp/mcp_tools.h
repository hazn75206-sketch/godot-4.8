#ifndef GODOT_MCP_TOOLS_H
#define GODOT_MCP_TOOLS_H

#include "core/variant/variant.h"
#include "core/string/ustring.h"

class McpServer;

void mcp_register_tools(McpServer *p_server);

Variant mcp_tool_ret_text(const String &p_text);
Variant mcp_tool_ret_error(const String &p_text);
Variant mcp_tool_ret_json(const Variant &p_value);

#endif // GODOT_MCP_TOOLS_H