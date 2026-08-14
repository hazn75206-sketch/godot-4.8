# Godot MCP Server (module: godot_mcp)

MCP (Model Context Protocol) server bawaan editor Godot 4.8-dev (build fork arm64).
AI agent bisa mengontrol editor dan membuat game: scene, node, script, run game, screenshot, dan lain-lain.

## Fitur

- **Transport**: Streamable HTTP (`POST /mcp`) dan SSE (`GET /sse` + `POST /messages`)
- **Jaringan**: `0.0.0.0` (LAN) dan localhost — client di perangkat lain bisa langsung konek
- **Auto-start**: server aktif otomatis saat aplikasi dibuka, **tanpa perlu buka project** (Project Manager), dan tetap tersedia di dalam project (dock "Godot MCP Server")
- **Notifikasi Android**: "Server MCP hidup" di status bar + tombol **"Matikan server"** untuk mematikan MCP tanpa membuka aplikasi (foreground service → server tetap jalan saat editor di-background)
- **Token auth opsional** untuk akses LAN
- **~25 tools**: project info, list/read/write file, project settings, scene tree, open/create/save scene, add/remove/rename/reparent node, get/set property (undoable), read/write/attach script, run/stop game, send input, screenshot (PNG), server info
- **Notifikasi MCP ke client**: `notifications/scene_changed`, `notifications/game_started`, `notifications/game_stopped`

## Konfigurasi (EditorSettings, tersimpan global)

| Setting | Default | Keterangan |
|---|---|---|
| `mcp/enabled` | `true` | aktif/tidak |
| `mcp/port` | `8766` | port HTTP |
| `mcp/bind` | `0.0.0.0` | `0.0.0.0` = LAN + localhost; `127.0.0.1` = localhost saja |
| `mcp/token` | (kosong) | jika diisi, client wajib kirim `Authorization: Bearer <token>` |

Bisa diubah dari dock editor (Project > Dock kanan bawah "Godot MCP Server") atau file settings editor.

## Endpoint

```
POST /mcp        Streamable HTTP (json-rpc; Accept: text/event-stream juga didukung)
GET  /sse        SSE stream  (session id via header Mcp-Session-Id atau query ?sessionId=)
POST /messages   SSE transport (kiim request client → server)
GET  /           info server
```

## Cara pakai dari client

Semua client MCP standar bisa pakai. Contoh:

**RikkaHub (di tablet, localhost):**
```json
{
  "mcpServers": {
    "godot": {
      "type": "streamable_http",
      "url": "http://127.0.0.1:8766/mcp"
    }
  }
}
```

**RikkaHub / perangkat lain (LAN):** ganti `127.0.0.1` dengan IP tablet.

**OpenCode CLI** (`opencode.json`):
```json
{
  "mcp": {
    "godot": {
      "type": "remote",
      "url": "http://<ip-tablet>:8766/mcp",
      "enabled": true
    }
  }
}
```

**Codex CLI:**
```
codex mcp add godot --transport http --url http://<ip-tablet>:8766/mcp
```

**Claude CLI:**
```
claude mcp add --transport http godot http://<ip-tablet>:8766/mcp
# atau transport SSE:
claude mcp add --transport sse godot http://<ip-tablet>:8766/sse
```

Dengan token:
```
claude mcp add --transport http godot http://<ip-tablet>:8766/mcp --header "Authorization: Bearer <token>"
```

## Contoh dialog AI

> "Buat scene gameplay.tscn dengan root Node2D bernama Game, tambahkan Sprite2D dan child script player.gd" → AI memanggil `create_scene`, `add_node`, `write_script`, `attach_script`
> "Jalankan main scene" → `run_main_scene`; "tangkapan layar" → `screenshot`

## Catatan

- Screenshot game berjalan: pada editor Android, game berjalan di proses terpisah → screenshot diambil dari viewport editor/simulasi, bukan proses game.
- `send_input` kontrol berjalan untuk game yang dijalankan dalam proses yang sama (desktop). Di Android gunakan `action`-based input.
- Saat MCP dimatikan lewat notifikasi, server berhenti sampai dinyalakan lagi (dari dock editor, aplikasi di-restart, atau tombol Enable).