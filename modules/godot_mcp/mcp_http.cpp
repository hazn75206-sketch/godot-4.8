#include "mcp_http.h"

#include "core/os/time.h"
#include "core/variant/json.h"
#include "mcp_server.h"

#include <chrono>
#include <thread>

struct MCPHttpServer::Connection {
	Ref<StreamPeerTCP> peer;
	String recv_buf;
	String method;
	String path;
	String query;
	Dictionary headers;
	String body;
	bool headers_done = false;
	int body_remaining = 0;
	bool chunked = false;
	bool request_ready = false;
	bool is_sse = false;
	String session_id;
	bool session_created = false;
	uint64_t last_activity = 0;
	uint64_t last_heartbeat = 0;
	std::mutex mu;
	std::vector<String> out_queue;
};

MCPHttpServer::MCPHttpServer(McpServer *p_owner) {
	owner = p_owner;
}

MCPHttpServer::~MCPHttpServer() {
	stop();
}

bool MCPHttpServer::start() {
	port = owner->get_port();
	bind = owner->get_bind();
	if (port <= 0 || port > 65535) {
		return false;
	}
	server.instantiate();
	Error err = server->listen(port, bind);
	if (err != OK) {
		return false;
	}
	stopping.store(false);
	accept_thread = std::make_unique<std::thread>([this] { _accept_loop(); });
	return true;
}

void MCPHttpServer::stop() {
	if (stopping.exchange(true)) {
		return;
	}
	if (accept_thread) {
		if (accept_thread->joinable()) {
			accept_thread->join();
		}
		accept_thread.reset();
	}
	if (server.is_valid()) {
		server->stop();
	}
	{
		std::lock_guard<std::mutex> lk(conn_threads_mu);
		for (auto &t : conn_threads) {
			if (t->joinable()) {
				t->join();
			}
		}
		conn_threads.clear();
	}
}

void MCPHttpServer::_accept_loop() {
	while (!stopping.load()) {
		if (server.is_valid() && server->is_connection_available()) {
			Ref<StreamPeerTCP> peer = server->take_connection();
			if (peer.is_valid()) {
				Connection *conn = memnew(Connection);
				conn->peer = peer;
				conn->last_activity = Time::get_singleton()->get_ticks_msec();
				conn->last_heartbeat = conn->last_activity;
				std::thread *t = new std::thread([this, conn] { _connection_loop(conn); });
				{
					std::lock_guard<std::mutex> lk(conn_threads_mu);
					conn_threads.push_back(std::unique_ptr<std::thread>(t));
				}
			}
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
}

void MCPHttpServer::_connection_loop(Connection *p_conn) {
	p_conn->peer->set_no_delay(true);
	while (!stopping.load() && p_conn->peer.is_valid() && p_conn->peer->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
		p_conn->peer->poll();

		if (!p_conn->request_ready && !p_conn->is_sse) {
			_read_request(p_conn);
		}
		if (p_conn->request_ready) {
			_handle_http(p_conn);
			if (!p_conn->is_sse) {
				// Reset connection state for keep-alive.
				p_conn->method = String();
				p_conn->path = String();
				p_conn->query = String();
				p_conn->headers = Dictionary();
				p_conn->body = String();
				p_conn->headers_done = false;
				p_conn->body_remaining = 0;
				p_conn->chunked = false;
				p_conn->request_ready = false;
				p_conn->recv_buf = String();
			}
		}

		if (p_conn->is_sse) {
			{
				std::lock_guard<std::mutex> lk(p_conn->mu);
				for (const String &f : p_conn->out_queue) {
					_write_sse(p_conn, f);
				}
				p_conn->out_queue.clear();
			}
			uint64_t now2 = Time::get_singleton()->get_ticks_msec();
			if (now2 - p_conn->last_heartbeat > 15000) {
				p_conn->last_heartbeat = now2;
				_write_sse(p_conn, ": ping\n\n");
			}
			if (Time::get_singleton()->get_ticks_msec() - p_conn->last_activity > 60000) {
				break;
			}
		} else if (p_conn->request_ready == false && p_conn->headers_done && p_conn->body_remaining == 0 && Time::get_singleton()->get_ticks_msec() - p_conn->last_activity > 30000) {
			// Idle persistent connection.
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	_close_conn(p_conn);
}

bool MCPHttpServer::_read_request(Connection *p_conn) {
	int avail = p_conn->peer->get_available_bytes();
	if (avail <= 0) {
		return false;
	}
	Vector<uint8_t> buf;
	buf.resize(avail);
	p_conn->peer->get_data(buf.ptrw(), avail);
	String chunk = String::utf8((const char *)buf.ptr(), avail);
	p_conn->recv_buf += chunk;

	if (!p_conn->headers_done) {
		int hdr_end = p_conn->recv_buf.find("\r\n\r\n");
		if (hdr_end == -1) {
			return false;
		}
		String head = p_conn->recv_buf.substr(0, hdr_end);
		p_conn->recv_buf = p_conn->recv_buf.substr(hdr_end + 4);
		Vector<String> lines = head.split("\n");
		if (!lines.is_empty()) {
			Vector<String> parts = lines[0].split(" ");
			if (parts.size() >= 2) {
				p_conn->method = parts[0];
				String full_path = parts[1];
				int q = full_path.find("?");
				if (q != -1) {
					p_conn->path = full_path.substr(0, q);
					p_conn->query = full_path.substr(q + 1);
				} else {
					p_conn->path = full_path;
				}
			}
		}
		for (int i = 1; i < lines.size(); i++) {
			String line = lines[i].strip_edges();
			if (line.is_empty()) {
				continue;
			}
			int colon = line.find(":");
			if (colon != -1) {
				String name = line.substr(0, colon).strip_edges().to_lower();
				String value = line.substr(colon + 1).strip_edges();
				p_conn->headers[name] = value;
			}
		}
		p_conn->headers_done = true;
		String clen = p_conn->headers.get("content-length", String());
		String te = p_conn->headers.get("transfer-encoding", String());
		if (!clen.is_empty() && clen.is_valid_int()) {
			p_conn->body_remaining = clen.to_int();
		} else if (te.to_lower().contains("chunked")) {
			p_conn->chunked = true;
			p_conn->body_remaining = -1;
		} else {
			p_conn->body_remaining = 0;
		}
	}

	if (p_conn->body_remaining > 0) {
		if ((int)p_conn->recv_buf.length() >= p_conn->body_remaining) {
			p_conn->body = p_conn->recv_buf.substr(0, p_conn->body_remaining);
			p_conn->recv_buf = p_conn->recv_buf.substr(p_conn->body_remaining);
			p_conn->body_remaining = 0;
		} else {
			return false;
		}
	} else if (p_conn->chunked) {
		while (true) {
			int crlf = p_conn->recv_buf.find("\r\n");
			if (crlf == -1) {
				return false;
			}
			String size_line = p_conn->recv_buf.substr(0, crlf);
			String hex = size_line.split(";")[0].strip_edges();
			uint64_t size = 0;
			for (int i = 0; i < hex.length(); i++) {
				uint32_t v = 0;
				char c = hex[i];
				if (c >= '0' && c <= '9') {
					v = c - '0';
				} else if (c >= 'a' && c <= 'f') {
					v = c - 'a' + 10;
				} else if (c >= 'A' && c <= 'F') {
					v = c - 'A' + 10;
				} else {
					return false;
				}
				size = size * 16 + v;
			}
			p_conn->recv_buf = p_conn->recv_buf.substr(crlf + 2);
			if (size == 0) {
				p_conn->body_remaining = 0;
				p_conn->chunked = false;
				p_conn->request_ready = true;
				return true;
			}
			if ((uint64_t)p_conn->recv_buf.length() < size + 2) {
				return false;
			}
			p_conn->body += p_conn->recv_buf.substr(0, size);
			p_conn->recv_buf = p_conn->recv_buf.substr(size + 2);
		}
	}

	if (p_conn->headers_done && p_conn->body_remaining == 0) {
		p_conn->request_ready = true;
	}
	return true;
}

bool MCPHttpServer::_check_auth(Connection *p_conn) {
	String token = owner->get_token();
	if (token.is_empty()) {
		return true;
	}
	String auth = p_conn->headers.get("authorization", String());
	String prefix = "Bearer ";
	if (auth.begins_with(prefix) && auth.substr(prefix.length()).strip_edges() == token) {
		return true;
	}
	return false;
}

void MCPHttpServer::_handle_http(Connection *p_conn) {
	if (p_conn->method == "GET" && p_conn->path == "/sse") {
		if (!_check_auth(p_conn)) {
			_send_response(p_conn, 401, "application/json", "{\"error\":\"unauthorized\"}", "");
			return;
		}
		p_conn->is_sse = true;
		String sid = p_conn->headers.get("mcp-session-id", String());
		if (sid.is_empty()) {
			Vector<String> params = p_conn->query.split("&");
			for (const String &p : params) {
				if (p.begins_with("sessionId=")) {
					sid = p.substr(String("sessionId=").length());
					break;
				}
			}
		}
		{
			std::lock_guard<std::mutex> lk(owner->sessions_mu);
			auto it = owner->sessions.find(sid);
			if (it != owner->sessions.end()) {
				it->second.sse_conn = p_conn;
			}
		}
		String resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
		CharString cs = resp.utf8();
		p_conn->peer->put_data((const uint8_t *)cs.get_data(), cs.length());
		p_conn->last_activity = Time::get_singleton()->get_ticks_msec();
		return;
	}

	if (p_conn->method == "GET") {
		String body = "Godot MCP server is running on port " + itos(port) + ".\nEndpoints:\n  POST /mcp (streamable HTTP)\n  GET /sse + POST /messages (SSE)\n";
		_send_response(p_conn, 200, "text/plain", body, "");
		return;
	}

	if (p_conn->method != "POST") {
		_send_response(p_conn, 405, "text/plain", "Method Not Allowed", "");
		return;
	}

	if (p_conn->path != "/mcp" && p_conn->path != "/messages") {
		_send_response(p_conn, 404, "application/json", "{\"error\":\"not found\"}", "");
		return;
	}

	if (!_check_auth(p_conn)) {
		_send_response(p_conn, 401, "application/json", "{\"error\":\"unauthorized\"}", "");
		return;
	}

	Variant json_variant = JSON::parse_string(p_conn->body);
	if (json_variant.get_type() != Variant::DICTIONARY) {
		_send_response(p_conn, 400, "application/json", "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}", "");
		return;
	}

	String sid = p_conn->headers.get("mcp-session-id", String());
	bool broadcast = false;
	String created;
	Dictionary response = owner->handle_jsonrpc(sid, json_variant, broadcast, created);
	if (!created.is_empty()) {
		p_conn->session_id = created;
	}
	if (response.is_empty()) {
		_send_response(p_conn, 202, "application/json", "{}", "");
		return;
	}
	String accept = p_conn->headers.get("accept", String());
	String body_json = JSON::stringify(response);
	String extra = String();
	if (!created.is_empty()) {
		extra = "Mcp-Session-Id: " + created + "\r\n";
	}
	if (accept.contains("text/event-stream")) {
		String resp_head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n" + extra + "\r\n";
		CharString cs = resp_head.utf8();
		p_conn->peer->put_data((const uint8_t *)cs.get_data(), cs.length());
		p_conn->is_sse = true;
		_write_sse(p_conn, "event: message\ndata: " + body_json + "\n\n");
		p_conn->last_activity = Time::get_singleton()->get_ticks_msec();
		{
			std::lock_guard<std::mutex> lk(owner->sessions_mu);
			auto it = owner->sessions.find(created.is_empty() ? sid : created);
			if (it != owner->sessions.end()) {
				it->second.sse_conn = p_conn;
			}
		}
		return;
	}

	_send_response(p_conn, 200, "application/json", body_json, extra);
	p_conn->last_activity = Time::get_singleton()->get_ticks_msec();
}

void MCPHttpServer::_send_response(Connection *p_conn, int p_status, const String &p_content_type, const String &p_body, const String &p_extra_headers) {
	String status_text = "OK";
	if (p_status == 400) {
		status_text = "Bad Request";
	} else if (p_status == 401) {
		status_text = "Unauthorized";
	} else if (p_status == 404) {
		status_text = "Not Found";
	} else if (p_status == 405) {
		status_text = "Method Not Allowed";
	}
	String resp = vformat("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nContent-Length: %d\r\n%s\r\n%s", p_status, status_text, p_content_type, p_body.length(), p_extra_headers, p_body);
	CharString cs = resp.utf8();
	p_conn->peer->put_data((const uint8_t *)cs.get_data(), cs.length());
}

void MCPHttpServer::_write_sse(Connection *p_conn, const String &p_frame) {
	if (!p_conn->peer.is_valid() || p_conn->peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		return;
	}
	CharString cs = p_frame.utf8();
	p_conn->peer->put_data((const uint8_t *)cs.get_data(), cs.length());
	p_conn->last_activity = Time::get_singleton()->get_ticks_msec();
}

void MCPHttpServer::_close_conn(Connection *p_conn) {
	if (p_conn->peer.is_valid()) {
		p_conn->peer->disconnect_from_host();
	}
	memdelete(p_conn);
}