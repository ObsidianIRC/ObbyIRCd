/*
 * pushbot -- Discord-style out-of-process bot framework for obbyircd.
 *
 * Phase 1: skeleton + config-defined bots + ghost clients + the
 * /PUSHBOT command with LIST and INFO subcommands.
 *
 * No event routing, no REST API, no gateway WebSocket yet -- those
 * land in subsequent phases.  Once Phase 1 is in, bots defined in
 * obbyircd.conf show up in /NAMES, /WHO, and /WHOIS but don't
 * actually do anything.  The point of this phase is to nail the
 * lifecycle: bot configured in conf -> persisted to SQLite ->
 * ghost client materialised at module load -> shows up in channels
 * -> survives /REHASH.
 *
 * Storage: pushbot.db (SQLite) in PERMDATADIR.  Schema covers all
 * the data the later phases will need (commands, channels, requests,
 * dead-letter queue) so we don't have to do a migration every time
 * we land a phase.
 *
 * Spec: doc/pushbot-spec.md (intentionally local-only, see the
 * private notes for the canonical version).
 *
 * (C) 2026 Valerie / ObbyIRCd Team
 * License: GPLv3 or later
 */

#include "unrealircd.h"
#include <sqlite3.h>
#include <jansson.h>

#define MYCONF "pushbot"
#define DEFAULT_DB "pushbot.db"

/* Gateway URL path. */
#define PB_GATEWAY_PATH "/pushbot/v1/gateway"

/* WS close codes (Discord-style). */
#define PB_CLOSE_AUTH_FAILED 4004
#define PB_CLOSE_INVALID_SESSION 4006
#define PB_CLOSE_TIMEOUT 4009
#define PB_CLOSE_QUEUE_OVERFLOW 4011

/* Default heartbeat interval (ms). */
#define PB_HEARTBEAT_INTERVAL_MS 30000
/* Two missed heartbeats -> session timeout. */
#define PB_HEARTBEAT_GRACE_MS (PB_HEARTBEAT_INTERVAL_MS * 2 + 5000)

/* Opcodes per spec §4.3. */
#define PB_OP_DISPATCH       0
#define PB_OP_HEARTBEAT      1
#define PB_OP_IDENTIFY       2
#define PB_OP_RESUME         6
#define PB_OP_RECONNECT      7
#define PB_OP_INVALID_SESSION 9
#define PB_OP_HELLO          10
#define PB_OP_HEARTBEAT_ACK  11

ModuleHeader MOD_HEADER = {
	"pushbot",
	"0.2",
	"Discord-style out-of-process bots (phase 1+2: skeleton + gateway)",
	"ObbyIRCd Team",
	"unrealircd-6"
};

/* ===================================================================
 * Internal state
 * =================================================================== */

typedef enum {
	PB_SCOPE_CHANNEL = 0,
	PB_SCOPE_SERVER  = 1,
} PbScope;

typedef enum {
	PB_TRANSPORT_GATEWAY = 0,
	PB_TRANSPORT_WEBHOOK = 1,
	PB_TRANSPORT_BOTH    = 2,
} PbTransport;

typedef enum {
	PB_STATUS_PENDING   = 0,
	PB_STATUS_ACTIVE    = 1,
	PB_STATUS_SUSPENDED = 2,
	PB_STATUS_DELETED   = 3,
} PbStatus;

struct PbSession;
typedef struct PbSession PbSession;

/* One serialized event waiting in a bot's outbound queue.  Kept in
 * memory for 60s (resume TTL); replayed on a successful RESUME. */
typedef struct PbQueuedEvent PbQueuedEvent;
struct PbQueuedEvent {
	PbQueuedEvent *prev, *next;
	long long seq;
	char *json;          /* serialized DISPATCH frame ready to send */
	time_t expires_at;   /* drop after this time */
};

/* Maximum queue depth per bot.  Exceeding this disconnects the slow
 * bot with PB_CLOSE_QUEUE_OVERFLOW; RESUME-fresh-IDENTIFY catches up
 * from the live state. */
#define PB_QUEUE_MAX 1024
/* Queued events outlive a disconnected session for this long. */
#define PB_RESUME_TTL_SEC 60

/* In-memory record for a bot.  Mirrors the SQLite row but with
 * resolved pointers and the live ghost client. */
typedef struct PbBot PbBot;
struct PbBot {
	PbBot *prev, *next;
	char *bot_id;          /* opaque id; matches SQLite row */
	char *nick;
	char *account;
	char *realname;
	PbScope scope;
	PbTransport transport;
	PbStatus status;
	char *webhook_url;     /* may be NULL */
	char *config_token;    /* plaintext token from config; NULL for self-reg */
	NameList *auto_join;   /* channels to auto-join after ghost creation */
	int from_config;       /* 1 = defined in obbyircd.conf this run */
	Client *ghost;         /* the virtual client, NULL when not materialised */
	PbSession *session;    /* current gateway session, NULL if not connected */

	/* Outbound event queue (for backpressure + RESUME). */
	PbQueuedEvent *queue_head, *queue_tail;
	int queue_count;
	long long next_seq;       /* next sequence number to assign */
	long long last_acked_seq; /* highest seq the bot has acked */
	char *resume_session_id;  /* id valid for RESUME (matches IDENTIFY result) */
	time_t resume_expires_at; /* when the resume window closes (0 = active) */
};

/* One gateway connection.  Created on WS upgrade, hung off the
 * connecting client via moddata.  Becomes "bound" to a PbBot once
 * IDENTIFY succeeds.  Sequence numbers + queue live on the bot,
 * not here, so they survive reconnects within the resume window. */
struct PbSession {
	Client *client;        /* the websocket-bearing client */
	PbBot *bot;            /* NULL until IDENTIFY succeeds */
	int identified;
	Event *heartbeat_ev;
	time_t last_heartbeat;
};

/* Pending config: collected during configrun, applied at MOD_LOAD. */
typedef struct PbConfigBot PbConfigBot;
struct PbConfigBot {
	PbConfigBot *prev, *next;
	char *nick;
	char *realname;
	char *token;
	char *webhook_url;
	PbScope scope;
	PbTransport transport;
	NameList *auto_join;
};

typedef struct {
	char *database_path;
	char *registration_mode;     /* "admin" | "approval" | "open" */
	int webhook_failure_suspend_after;
	PbConfigBot *pending_bots;   /* head of list */
} PbCfg;

static PbCfg cfg;
static sqlite3 *db = NULL;
static PbBot *bots = NULL;
static ModuleInfo *modinfo_ref = NULL;

/* Gateway moddata: per-client session pointer. */
static ModDataInfo *pb_session_md = NULL;
/* websocket_common's moddata, for inheriting WSU(). */
static ModDataInfo *pb_websocket_md = NULL;
/* webserver's moddata, for accessing WebRequest from client. */
static ModDataInfo *pb_webserver_md = NULL;

#define PB_SESS(c) ((PbSession *)moddata_client(c, pb_session_md).ptr)
#define PB_WEB(c)  ((WebRequest *)moddata_local_client(c, pb_webserver_md).ptr)

/* ===================================================================
 * Forward declarations
 * =================================================================== */

static int pb_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int pb_configrun(ConfigFile *cf, ConfigEntry *ce, int type);
static int pb_open_db(void);
static int pb_init_schema(void);
static int pb_apply_pending_bots(void);
static int pb_upsert_bot_row(const char *bot_id, PbConfigBot *b);
static int pb_load_active_bots_from_db(void);
static PbBot *pb_find_bot_by_nick(const char *nick);
static void pb_free_bot(PbBot *b);
static void pb_free_config(void);
static void pb_free_pending_bot(PbConfigBot *b);
static Client *pb_spawn_ghost(PbBot *b);
static void pb_destroy_ghost(PbBot *b, const char *reason);
static int pb_autojoin(PbBot *b);
static const char *pb_scope_str(PbScope s);
static const char *pb_transport_str(PbTransport t);
static const char *pb_status_str(PbStatus s);
static PbScope pb_parse_scope(const char *s);
static PbTransport pb_parse_transport(const char *s);
static void pb_generate_id(char *out, size_t outlen);

CMD_FUNC(cmd_pushbot);

/* Gateway -- forward decls */
static int pb_config_test_listen(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int pb_config_run_ex_listen(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr);
static int pb_config_listener(ConfigItem_listen *l);
static void pb_client_handshake(Client *client);
static int pb_handle_webrequest(Client *client, WebRequest *web);
static int pb_handle_webrequest_data(Client *client, WebRequest *web, const char *buf, int len);
static int pb_ws_handshake_send(Client *client);
static int pb_packet_in_websocket(Client *client, char *buf, int len);
static int pb_handle_body_websocket(Client *client, WebRequest *web, const char *buf, int len);
static void pb_handle_ws_message(Client *client, char *msg, int len);
static void pb_send_op(Client *client, json_t *frame);
static void pb_send_hello(Client *client);
static void pb_send_dispatch(Client *client, const char *event_name, json_t *data);
static void pb_handle_identify(Client *client, json_t *frame);
static void pb_handle_resume(Client *client, json_t *frame);
static void pb_handle_heartbeat(Client *client, json_t *frame);
static void pb_close_ws(Client *client, int code, const char *reason);
static void pb_session_free(PbSession *s);
static void pb_moddata_session_free(ModData *md);
EVENT(pb_heartbeat_check);

/* ===================================================================
 * Module lifecycle
 * =================================================================== */

MOD_TEST()
{
	memset(&cfg, 0, sizeof(cfg));
	safe_strdup(cfg.database_path, DEFAULT_DB);
	safe_strdup(cfg.registration_mode, "admin");
	cfg.webhook_failure_suspend_after = 86400;
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, pb_configtest);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, pb_config_test_listen);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ModDataInfo mreq;

	modinfo_ref = modinfo;
	MARK_AS_OFFICIAL_MODULE(modinfo);

	/* Register our per-client session moddata. */
	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "pushbot_session";
	mreq.type = MODDATATYPE_LOCAL_CLIENT;
	mreq.free = pb_moddata_session_free;
	pb_session_md = ModDataAdd(modinfo->handle, mreq);
	if (!pb_session_md) {
		config_error("[pushbot] ModDataAdd(pushbot_session) failed: %s",
		             ModuleGetErrorStr(modinfo->handle));
		return MOD_FAILED;
	}

	/* Borrow websocket_common's moddata for WSU(). */
	pb_websocket_md = findmoddata_byname("websocket", MODDATATYPE_CLIENT);
	pb_webserver_md = findmoddata_byname("web", MODDATATYPE_LOCAL_CLIENT);

	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, pb_configrun);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN_EX, 0, pb_config_run_ex_listen);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIG_LISTENER, 0, pb_config_listener);
	CommandAdd(modinfo->handle, "PUSHBOT", cmd_pushbot, MAXPARA, CMD_USER);

	/* Heartbeat watchdog: every 5s, kick sessions that missed too many. */
	EventAdd(modinfo->handle, "pb_heartbeat_check", pb_heartbeat_check, NULL, 5000, 0);
	return MOD_SUCCESS;
}

MOD_LOAD()
{
	if (pb_open_db() < 0) {
		config_error("[pushbot] cannot open database at %s", cfg.database_path);
		return MOD_FAILED;
	}
	if (pb_init_schema() < 0) {
		config_error("[pushbot] schema init failed");
		return MOD_FAILED;
	}

	/* Pull previously-active bots back into memory.  Self-registered
	 * bots (phase 8) come from this path; config-defined bots come
	 * from the pending list below. */
	pb_load_active_bots_from_db();

	/* Re-apply config-defined bots: upsert their row, materialise
	 * their ghost if status=active. */
	pb_apply_pending_bots();

	unreal_log(ULOG_INFO, "pushbot", "PUSHBOT_LOADED", NULL,
	           "pushbot module loaded (mode=$mode, db=$db)",
	           log_data_string("mode", cfg.registration_mode),
	           log_data_string("db", cfg.database_path));
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	PbBot *b, *n;
	for (b = bots; b; b = n) {
		n = b->next;
		pb_destroy_ghost(b, "pushbot module unloaded");
		pb_free_bot(b);
	}
	bots = NULL;

	if (db) {
		sqlite3_close(db);
		db = NULL;
	}
	pb_free_config();
	return MOD_SUCCESS;
}

/* ===================================================================
 * Config parsing
 * =================================================================== */

static int pb_test_bot_block(ConfigFile *cf, ConfigEntry *bot_ce, int *errs)
{
	int errors = 0;
	int has_token = 0;
	if (!bot_ce->value || !*bot_ce->value) {
		config_error("%s:%d: pushbot::bot block needs a nick",
		             bot_ce->file->filename, bot_ce->line_number);
		errors++;
	}
	for (ConfigEntry *cep = bot_ce->items; cep; cep = cep->next) {
		if (!cep->name) continue;
		if (!strcmp(cep->name, "token")) {
			if (!cep->value || !*cep->value) {
				config_error("%s:%d: pushbot::bot::token must have a value",
				             cep->file->filename, cep->line_number);
				errors++;
			} else has_token = 1;
		} else if (!strcmp(cep->name, "realname")
		        || !strcmp(cep->name, "scope")
		        || !strcmp(cep->name, "transport")
		        || !strcmp(cep->name, "webhook-url")
		        || !strcmp(cep->name, "webhook-secret")) {
			if (!cep->value) {
				config_error("%s:%d: pushbot::bot::%s needs a value",
				             cep->file->filename, cep->line_number, cep->name);
				errors++;
			}
		} else if (!strcmp(cep->name, "auto-join")) {
			/* Block of channel names; values inside are channels. */
			for (ConfigEntry *ch = cep->items; ch; ch = ch->next) {
				if (!ch->name || ch->name[0] != '#') {
					config_error("%s:%d: pushbot::bot::auto-join entries must be channel names",
					             ch->file->filename, ch->line_number);
					errors++;
				}
			}
		} else if (!strcmp(cep->name, "permissions")
		        || !strcmp(cep->name, "triggers")) {
			/* Reserved for later phases; quietly accept structure. */
		} else {
			config_error("%s:%d: unknown directive pushbot::bot::%s",
			             cep->file->filename, cep->line_number, cep->name);
			errors++;
		}
	}
	if (!has_token) {
		config_error("%s:%d: pushbot::bot %s missing token",
		             bot_ce->file->filename, bot_ce->line_number,
		             bot_ce->value ? bot_ce->value : "");
		errors++;
	}
	*errs += errors;
	return errors;
}

static int pb_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	if (type != CONFIG_MAIN) return 0;
	if (!ce || !ce->name || strcmp(ce->name, MYCONF)) return 0;

	for (ConfigEntry *cep = ce->items; cep; cep = cep->next) {
		if (!cep->name) continue;
		if (!strcmp(cep->name, "mode")) {
			if (!cep->value
			    || (strcmp(cep->value, "admin")
			        && strcmp(cep->value, "approval")
			        && strcmp(cep->value, "open"))) {
				config_error("%s:%d: pushbot::mode must be admin|approval|open",
				             cep->file->filename, cep->line_number);
				errors++;
			}
		} else if (!strcmp(cep->name, "database")) {
			if (!cep->value) {
				config_error("%s:%d: pushbot::database needs a value",
				             cep->file->filename, cep->line_number);
				errors++;
			}
		} else if (!strcmp(cep->name, "webhook-failure-suspend-after")) {
			if (!cep->value || atoi(cep->value) <= 0) {
				config_error("%s:%d: pushbot::webhook-failure-suspend-after needs positive seconds",
				             cep->file->filename, cep->line_number);
				errors++;
			}
		} else if (!strcmp(cep->name, "default-permissions")) {
			/* Reserved for later phases. */
		} else if (!strcmp(cep->name, "bot")) {
			pb_test_bot_block(cf, cep, &errors);
		} else {
			config_error("%s:%d: unknown directive pushbot::%s",
			             cep->file->filename, cep->line_number, cep->name);
			errors++;
		}
	}
	*errs = errors;
	return errors ? -1 : 1;
}

static void pb_parse_bot_block(ConfigEntry *bot_ce)
{
	PbConfigBot *b = safe_alloc(sizeof(*b));
	safe_strdup(b->nick, bot_ce->value);
	b->scope = PB_SCOPE_CHANNEL;
	b->transport = PB_TRANSPORT_GATEWAY;
	for (ConfigEntry *cep = bot_ce->items; cep; cep = cep->next) {
		if (!cep->name) continue;
		if (!strcmp(cep->name, "token")) safe_strdup(b->token, cep->value);
		else if (!strcmp(cep->name, "realname")) safe_strdup(b->realname, cep->value);
		else if (!strcmp(cep->name, "scope")) b->scope = pb_parse_scope(cep->value);
		else if (!strcmp(cep->name, "transport")) b->transport = pb_parse_transport(cep->value);
		else if (!strcmp(cep->name, "webhook-url")) safe_strdup(b->webhook_url, cep->value);
		else if (!strcmp(cep->name, "auto-join")) {
			for (ConfigEntry *ch = cep->items; ch; ch = ch->next) {
				if (ch->name && ch->name[0] == '#')
					add_name_list(b->auto_join, ch->name);
			}
		}
	}
	if (!b->realname) safe_strdup(b->realname, b->nick);
	b->next = cfg.pending_bots;
	if (cfg.pending_bots) cfg.pending_bots->prev = b;
	cfg.pending_bots = b;
}

static int pb_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	if (type != CONFIG_MAIN) return 0;
	if (!ce || !ce->name || strcmp(ce->name, MYCONF)) return 0;

	/* On /REHASH this may be called again; drop any previously-collected
	 * pending bots so we re-derive the set from the new config. */
	while (cfg.pending_bots) {
		PbConfigBot *next = cfg.pending_bots->next;
		pb_free_pending_bot(cfg.pending_bots);
		cfg.pending_bots = next;
	}

	for (ConfigEntry *cep = ce->items; cep; cep = cep->next) {
		if (!cep->name) continue;
		if (!strcmp(cep->name, "mode")) {
			safe_strdup(cfg.registration_mode, cep->value);
		} else if (!strcmp(cep->name, "database")) {
			safe_strdup(cfg.database_path, cep->value);
		} else if (!strcmp(cep->name, "webhook-failure-suspend-after")) {
			cfg.webhook_failure_suspend_after = atoi(cep->value);
		} else if (!strcmp(cep->name, "bot")) {
			pb_parse_bot_block(cep);
		}
	}
	return 1;
}

static void pb_free_pending_bot(PbConfigBot *b)
{
	if (!b) return;
	safe_free(b->nick);
	safe_free(b->realname);
	safe_free(b->token);
	safe_free(b->webhook_url);
	free_entire_name_list(b->auto_join);
	safe_free(b);
}

static void pb_free_config(void)
{
	while (cfg.pending_bots) {
		PbConfigBot *next = cfg.pending_bots->next;
		pb_free_pending_bot(cfg.pending_bots);
		cfg.pending_bots = next;
	}
	safe_free(cfg.database_path);
	safe_free(cfg.registration_mode);
}

/* ===================================================================
 * SQLite
 * =================================================================== */

static int pb_open_db(void)
{
	char *abspath = NULL;
	safe_strdup(abspath, cfg.database_path);
	convert_to_absolute_path(&abspath, PERMDATADIR);
	int rv = sqlite3_open(abspath, &db);
	if (rv != SQLITE_OK) {
		unreal_log(ULOG_ERROR, "pushbot", "DB_OPEN", NULL,
		           "Could not open pushbot DB at $path: $err",
		           log_data_string("path", abspath),
		           log_data_string("err", db ? sqlite3_errmsg(db) : "(no handle)"));
		if (db) { sqlite3_close(db); db = NULL; }
		safe_free(abspath);
		return -1;
	}
	safe_free(abspath);
	sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
	return 0;
}

static int pb_init_schema(void)
{
	static const char *schema =
		"CREATE TABLE IF NOT EXISTS pushbots ("
		"  bot_id TEXT PRIMARY KEY, nick TEXT UNIQUE NOT NULL,"
		"  account TEXT UNIQUE NOT NULL, realname TEXT NOT NULL,"
		"  scope TEXT NOT NULL CHECK(scope IN ('channel','server')),"
		"  token_hash TEXT NOT NULL, webhook_url TEXT, webhook_secret_hash TEXT,"
		"  transport TEXT NOT NULL CHECK(transport IN ('gateway','webhook','both')),"
		"  status TEXT NOT NULL CHECK(status IN ('pending','active','suspended','deleted')),"
		"  triggers TEXT NOT NULL DEFAULT '{}',"
		"  permissions TEXT NOT NULL DEFAULT '{}',"
		"  created_at INTEGER NOT NULL,"
		"  created_by TEXT NOT NULL,"
		"  approved_by TEXT, approved_at INTEGER, last_seen INTEGER"
		");"
		"CREATE TABLE IF NOT EXISTS pushbot_channels ("
		"  bot_id TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,"
		"  channel TEXT NOT NULL, PRIMARY KEY(bot_id, channel)"
		");"
		"CREATE TABLE IF NOT EXISTS pushbot_commands ("
		"  bot_id TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,"
		"  name TEXT NOT NULL, description TEXT NOT NULL,"
		"  visibility TEXT NOT NULL CHECK(visibility IN ('public','private')),"
		"  scopes TEXT NOT NULL, options TEXT NOT NULL,"
		"  requires TEXT, version INTEGER NOT NULL DEFAULT 1,"
		"  PRIMARY KEY(bot_id, name)"
		");"
		"CREATE TABLE IF NOT EXISTS pushbot_requests ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  bot_id TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,"
		"  channel TEXT NOT NULL, requested_by TEXT NOT NULL,"
		"  message TEXT,"
		"  status TEXT NOT NULL CHECK(status IN ('pending','approved','denied','expired')),"
		"  resolved_by TEXT, resolved_at INTEGER, deny_reason TEXT,"
		"  created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL"
		");"
		"CREATE TABLE IF NOT EXISTS pushbot_dead_letters ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  bot_id TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,"
		"  event_type TEXT NOT NULL, payload TEXT NOT NULL,"
		"  error TEXT, failed_at INTEGER NOT NULL"
		");";
	char *err = NULL;
	int rv = sqlite3_exec(db, schema, NULL, NULL, &err);
	if (rv != SQLITE_OK) {
		unreal_log(ULOG_ERROR, "pushbot", "DB_SCHEMA", NULL,
		           "Schema init failed: $err",
		           log_data_string("err", err ? err : "?"));
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

/* Trivial hash that's safe-enough for "did the config token change?"
 * we just store a SHA-256 hex digest.  We use the IRCd's built-in
 * sha2 functions where available.  For phase 1 we deliberately stash
 * the *plaintext* in memory for config-defined bots (`config_token`)
 * because the gateway needs to verify it on connect; the DB stores
 * the hash and we'd reject any external supplied token by comparing
 * its hash too.  Phase 2 will replace the plaintext-in-memory bit
 * with a constant-time bearer-comparison. */
static void pb_token_hash(const char *plaintext, char *out, size_t outlen)
{
	if (outlen < 65) { if (outlen) out[0] = '\0'; return; }
	unsigned char digest[32];
	sha256hash_binary((char *)digest, (char *)plaintext, strlen(plaintext));
	static const char *hex = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		out[i * 2]     = hex[(digest[i] >> 4) & 0xf];
		out[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	out[64] = '\0';
}

static int pb_upsert_bot_row(const char *bot_id, PbConfigBot *b)
{
	const char *sql =
		"INSERT INTO pushbots ("
		"  bot_id, nick, account, realname, scope, token_hash,"
		"  webhook_url, transport, status, created_at, created_by"
		") VALUES (?,?,?,?,?,?,?,?,'active',?,?)"
		" ON CONFLICT(bot_id) DO UPDATE SET"
		"  nick=excluded.nick, realname=excluded.realname,"
		"  scope=excluded.scope, token_hash=excluded.token_hash,"
		"  webhook_url=excluded.webhook_url,"
		"  transport=excluded.transport,"
		"  status=CASE WHEN pushbots.status='suspended' THEN 'suspended' ELSE 'active' END";

	char hash[65];
	pb_token_hash(b->token, hash, sizeof(hash));

	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(st, 1, bot_id, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, b->nick, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, b->nick, -1, SQLITE_STATIC); /* account == nick */
	sqlite3_bind_text(st, 4, b->realname ? b->realname : b->nick, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 5, pb_scope_str(b->scope), -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 6, hash, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 7, b->webhook_url, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 8, pb_transport_str(b->transport), -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 9, (long long)TStime());
	sqlite3_bind_text(st, 10, "config", -1, SQLITE_STATIC);

	int rv = sqlite3_step(st);
	sqlite3_finalize(st);
	return (rv == SQLITE_DONE) ? 0 : -1;
}

static int pb_load_active_bots_from_db(void)
{
	const char *sql =
		"SELECT bot_id, nick, account, realname, scope, transport,"
		"       status, webhook_url"
		"  FROM pushbots WHERE status IN ('active','suspended')";
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
		return -1;
	int loaded = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *bot_id   = (const char *)sqlite3_column_text(st, 0);
		const char *nick     = (const char *)sqlite3_column_text(st, 1);
		const char *account  = (const char *)sqlite3_column_text(st, 2);
		const char *realname = (const char *)sqlite3_column_text(st, 3);
		const char *scope    = (const char *)sqlite3_column_text(st, 4);
		const char *transp   = (const char *)sqlite3_column_text(st, 5);
		const char *status   = (const char *)sqlite3_column_text(st, 6);
		const char *whurl    = (const char *)sqlite3_column_text(st, 7);
		if (!bot_id || !nick) continue;
		if (pb_find_bot_by_nick(nick)) continue;

		PbBot *b = safe_alloc(sizeof(*b));
		safe_strdup(b->bot_id, bot_id);
		safe_strdup(b->nick, nick);
		safe_strdup(b->account, account);
		safe_strdup(b->realname, realname);
		b->scope = pb_parse_scope(scope);
		b->transport = pb_parse_transport(transp);
		b->status = !strcmp(status, "suspended") ? PB_STATUS_SUSPENDED : PB_STATUS_ACTIVE;
		if (whurl) safe_strdup(b->webhook_url, whurl);
		AddListItem(b, bots);
		loaded++;
	}
	sqlite3_finalize(st);
	if (loaded)
		unreal_log(ULOG_INFO, "pushbot", "DB_LOAD", NULL,
		           "Loaded $count bot row(s) from database",
		           log_data_integer("count", loaded));
	return 0;
}

static int pb_apply_pending_bots(void)
{
	for (PbConfigBot *pc = cfg.pending_bots; pc; pc = pc->next) {
		char bot_id[16];
		PbBot *existing = pb_find_bot_by_nick(pc->nick);
		if (existing) {
			strlcpy(bot_id, existing->bot_id, sizeof(bot_id));
		} else {
			pb_generate_id(bot_id, sizeof(bot_id));
		}

		if (pb_upsert_bot_row(bot_id, pc) < 0) {
			unreal_log(ULOG_ERROR, "pushbot", "DB_UPSERT", NULL,
			           "Failed to upsert bot $nick",
			           log_data_string("nick", pc->nick));
			continue;
		}

		PbBot *b = existing;
		if (!b) {
			b = safe_alloc(sizeof(*b));
			safe_strdup(b->bot_id, bot_id);
			safe_strdup(b->nick, pc->nick);
			safe_strdup(b->account, pc->nick);
			safe_strdup(b->realname, pc->realname ? pc->realname : pc->nick);
			b->scope = pc->scope;
			b->transport = pc->transport;
			b->status = PB_STATUS_ACTIVE;
			if (pc->webhook_url) safe_strdup(b->webhook_url, pc->webhook_url);
			AddListItem(b, bots);
		} else {
			/* Update mutable fields from new config */
			safe_strdup(b->realname, pc->realname ? pc->realname : pc->nick);
			b->scope = pc->scope;
			b->transport = pc->transport;
			safe_free(b->webhook_url);
			if (pc->webhook_url) safe_strdup(b->webhook_url, pc->webhook_url);
		}
		safe_strdup(b->config_token, pc->token);
		b->from_config = 1;

		/* Materialise ghost if not already up. */
		if (b->status == PB_STATUS_ACTIVE && !b->ghost)
			pb_spawn_ghost(b);

		/* Auto-join channels listed in config. */
		free_entire_name_list(b->auto_join);
		b->auto_join = NULL;
		for (NameList *n = pc->auto_join; n; n = n->next)
			add_name_list(b->auto_join, n->name);
		pb_autojoin(b);
	}
	return 0;
}

/* ===================================================================
 * Ghost client lifecycle
 * =================================================================== */

static Client *pb_spawn_ghost(PbBot *b)
{
	if (!b || b->ghost) return b ? b->ghost : NULL;

	/* If a real user already holds the nick we can't proceed. */
	if (find_user(b->nick, NULL)) {
		unreal_log(ULOG_WARNING, "pushbot", "NICK_TAKEN", NULL,
		           "Cannot materialise bot $nick: nick is in use by a real user",
		           log_data_string("nick", b->nick));
		return NULL;
	}

	Client *ghost = make_client(NULL, &me);
	ghost->local->fd = -2;
	/* Mirror persistence.c's spawn order exactly.  Various periodic
	 * IRCd paths (ping check, /STATS sweep, idle scan) walk client
	 * lists; missing ip / sockhost / away strings can NULL-deref
	 * them. */
	strlcpy(ghost->name, b->nick, sizeof(ghost->name));
	safe_strdup(ghost->ip, "127.0.0.1");
	strlcpy(ghost->local->sockhost, "bot.obby.world", sizeof(ghost->local->sockhost));

	make_user(ghost);
	/* user->server is the source-server name for the user record.
	 * For local users, src/modules/user.c sets it to me_hash (the
	 * pooled string for our own server name).  Persistence's
	 * ghost-revival code happens to dodge crashes because the field
	 * is overwritten when the real user reconnects, but for a ghost
	 * that NEVER becomes real (PushBot), leaving this NULL crashes
	 * /WHO * 0 (RPL_WHOREPLY formats acptr->user->server unchecked). */
	ghost->user->server = me_hash;
	strlcpy(ghost->ident, b->nick, sizeof(ghost->ident));
	strlcpy(ghost->info, b->realname, sizeof(ghost->info));
	strlcpy(ghost->user->username, b->nick, sizeof(ghost->user->username));
	strlcpy(ghost->user->realhost, "bot.obby.world", sizeof(ghost->user->realhost));
	strlcpy(ghost->user->cloakedhost, "bot.obby.world", sizeof(ghost->user->cloakedhost));
	safe_free(ghost->user->virthost);
	safe_strdup(ghost->user->virthost, "bot.obby.world");
	strlcpy(ghost->user->account, b->account, sizeof(ghost->user->account));

	long bot_bit = find_user_mode('B');
	long invis_bit = find_user_mode('i');
	if (bot_bit) ghost->umodes |= bot_bit;
	if (invis_bit) ghost->umodes |= invis_bit;
	SetUser(ghost);

	/* Mark away BEFORE adding to lists (matches persistence.c
	 * ordering, so no walker sees us mid-init without an away
	 * string). */
	safe_strdup(ghost->user->away, "bot offline");
	ghost->user->away_since = TStime();

	add_to_client_hash_table(ghost->name, ghost);
	add_client_to_list(ghost);
	irccounts.clients++;
	irccounts.me_clients++;
	if (IsInvisible(ghost))
		irccounts.invisible++;
	if (ghost->uplink && ghost->uplink->server)
		ghost->uplink->server->users++;

	b->ghost = ghost;
	unreal_log(ULOG_INFO, "pushbot", "GHOST_UP", ghost,
	           "Bot $nick materialised",
	           log_data_string("nick", b->nick));
	return ghost;
}

static void pb_destroy_ghost(PbBot *b, const char *reason)
{
	if (!b || !b->ghost) return;
	Client *ghost = b->ghost;
	b->ghost = NULL;

	Membership *mp;
	while ((mp = ghost->user->channel))
		remove_user_from_channel_withmb(ghost, mp->channel, mp, 1);

	moddata_free_client(ghost);
	if (*ghost->id) {
		del_from_id_hash_table(ghost->id, ghost);
		*ghost->id = '\0';
	}
	if (*ghost->name)
		del_from_client_hash_table(ghost->name, ghost);
	remove_client_from_list(ghost);
	irccounts.clients--;
	irccounts.me_clients--;
	if (IsInvisible(ghost))
		irccounts.invisible--;
	if (me.server)
		me.server->users--;
	free_client(ghost);

	if (reason)
		unreal_log(ULOG_INFO, "pushbot", "GHOST_DOWN", NULL,
		           "Bot $nick ghost destroyed: $reason",
		           log_data_string("nick", b->nick),
		           log_data_string("reason", reason));
}

static int pb_autojoin(PbBot *b)
{
	if (!b || !b->ghost) return 0;
	int joined = 0;
	for (NameList *n = b->auto_join; n; n = n->next) {
		Channel *ch = find_channel(n->name);
		if (!ch) ch = make_channel(n->name);
		if (!ch) continue;
		Membership *m;
		int already = 0;
		for (m = b->ghost->user->channel; m; m = m->next)
			if (m->channel == ch) { already = 1; break; }
		if (already) continue;
		add_user_to_channel(ch, b->ghost, "");
		joined++;
	}
	return joined;
}

/* ===================================================================
 * Helpers
 * =================================================================== */

static PbBot *pb_find_bot_by_nick(const char *nick)
{
	for (PbBot *b = bots; b; b = b->next)
		if (!strcasecmp(b->nick, nick)) return b;
	return NULL;
}

static void pb_free_bot(PbBot *b)
{
	if (!b) return;
	while (b->queue_head) {
		PbQueuedEvent *e = b->queue_head;
		safe_free(e->json);
		DelListItem(e, b->queue_head);
		safe_free(e);
	}
	safe_free(b->resume_session_id);
	safe_free(b->bot_id);
	safe_free(b->nick);
	safe_free(b->account);
	safe_free(b->realname);
	safe_free(b->webhook_url);
	safe_free(b->config_token);
	free_entire_name_list(b->auto_join);
	DelListItem(b, bots);
	safe_free(b);
}

static const char *pb_scope_str(PbScope s)
{
	return s == PB_SCOPE_SERVER ? "server" : "channel";
}

static PbScope pb_parse_scope(const char *s)
{
	return (s && !strcasecmp(s, "server")) ? PB_SCOPE_SERVER : PB_SCOPE_CHANNEL;
}

static const char *pb_transport_str(PbTransport t)
{
	switch (t) {
	case PB_TRANSPORT_WEBHOOK: return "webhook";
	case PB_TRANSPORT_BOTH:    return "both";
	default:                   return "gateway";
	}
}

static PbTransport pb_parse_transport(const char *s)
{
	if (!s) return PB_TRANSPORT_GATEWAY;
	if (!strcasecmp(s, "webhook")) return PB_TRANSPORT_WEBHOOK;
	if (!strcasecmp(s, "both"))    return PB_TRANSPORT_BOTH;
	return PB_TRANSPORT_GATEWAY;
}

static const char *pb_status_str(PbStatus s)
{
	switch (s) {
	case PB_STATUS_PENDING:   return "pending";
	case PB_STATUS_SUSPENDED: return "suspended";
	case PB_STATUS_DELETED:   return "deleted";
	default:                  return "active";
	}
}

static void pb_generate_id(char *out, size_t outlen)
{
	static unsigned long counter = 0;
	counter++;
	snprintf(out, outlen, "pb%lx%lx",
	         (unsigned long)TStime(),
	         counter ^ (unsigned long)getpid());
}

/* ===================================================================
 * /PUSHBOT command  (IRCop-only in this phase)
 * =================================================================== */

static void cmd_pushbot_list(Client *client)
{
	int count = 0;
	for (PbBot *b = bots; b; b = b->next) {
		sendnotice(client, "%-16s  %-8s  %-9s  %-10s  ghost:%s",
		           b->nick,
		           pb_scope_str(b->scope),
		           pb_transport_str(b->transport),
		           pb_status_str(b->status),
		           b->ghost ? "up" : "down");
		count++;
	}
	sendnotice(client, "--- %d bot(s)", count);
}

static void cmd_pushbot_info(Client *client, const char *nick)
{
	PbBot *b = pb_find_bot_by_nick(nick);
	if (!b) {
		sendnotice(client, "No such bot: %s", nick);
		return;
	}
	sendnotice(client, "Bot:        %s", b->nick);
	sendnotice(client, "  id:       %s", b->bot_id);
	sendnotice(client, "  account:  %s", b->account);
	sendnotice(client, "  realname: %s", b->realname);
	sendnotice(client, "  scope:    %s", pb_scope_str(b->scope));
	sendnotice(client, "  trans:    %s", pb_transport_str(b->transport));
	sendnotice(client, "  status:   %s", pb_status_str(b->status));
	sendnotice(client, "  source:   %s", b->from_config ? "config" : "self/db");
	if (b->webhook_url)
		sendnotice(client, "  webhook:  %s", b->webhook_url);
	if (b->ghost) {
		int chs = 0;
		for (Membership *m = b->ghost->user->channel; m; m = m->next) chs++;
		sendnotice(client, "  ghost:    up, %d channel(s)", chs);
		for (Membership *m = b->ghost->user->channel; m; m = m->next)
			sendnotice(client, "    - %s", m->channel->name);
	} else {
		sendnotice(client, "  ghost:    down");
	}
}

CMD_FUNC(cmd_pushbot)
{
	if (!MyConnect(client) || !IsUser(client)) return;

	if (!ValidatePermissionsForPath("server:pushbot", client, NULL, NULL, NULL)) {
		sendnumeric(client, ERR_NOPRIVILEGES);
		return;
	}
	if (parc < 2) {
		sendnotice(client, "Usage: PUSHBOT LIST | INFO <nick>");
		return;
	}

	const char *sub = parv[1];
	if (!strcasecmp(sub, "LIST")) {
		cmd_pushbot_list(client);
		return;
	}
	if (!strcasecmp(sub, "INFO")) {
		if (parc < 3) {
			sendnotice(client, "Usage: PUSHBOT INFO <nick>");
			return;
		}
		cmd_pushbot_info(client, parv[2]);
		return;
	}
	sendnotice(client, "Unknown PUSHBOT subcommand: %s", sub);
}

/* ===================================================================
 * Gateway -- Phase 2
 *
 * Listener registration:  listen { options { pushbot; }; } turns the
 * listener into a webserver-mode endpoint with our handlers attached.
 *
 * Path routing inside pb_handle_webrequest:
 *   GET /pushbot/v1/gateway  -> WebSocket upgrade (this phase)
 *   anything else            -> 404 for now (REST API: phase 4)
 *
 * On WS upgrade we:
 *   - require an Authorization: Bearer <token> header
 *   - allocate a PbSession and attach it to moddata
 *   - send op=10 HELLO with the heartbeat interval
 *
 * After upgrade the bot sends:
 *   - op=2 IDENTIFY  -> we look up the bot, attach session, send READY
 *   - op=1 HEARTBEAT -> we ack with op=11
 * =================================================================== */

static int pb_config_test_listen(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	if (type != CONFIG_LISTEN_OPTIONS)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "pushbot"))
		return 0;
	/* No sub-keys yet; this just claims the directive so the parser
	 * doesn't warn about an unknown listen option. */
	return 1;
}

static int pb_config_run_ex_listen(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr)
{
	ConfigItem_listen *l;
	if (type != CONFIG_LISTEN_OPTIONS) return 0;
	if (!ce || !ce->name || strcmp(ce->name, "pushbot")) return 0;
	l = (ConfigItem_listen *)ptr;
	l->options |= LISTENER_NO_CHECK_CONNECT_FLOOD;
	l->options |= LISTENER_NO_CHECK_ZLINED;
	/* Stash via reusing rpc_options? We can't -- it's RPC-specific.
	 * We'll detect in pb_config_listener by walking listen->config. */
	l->rpc_options = 0; /* unrelated; ensures we don't accidentally flip rpc */
	/* We need a distinguishing flag. Hijack a spare bit in listener
	 * options for pushbot. */
	l->options |= 0x800000; /* PUSHBOT marker -- unused upstream bit */
	return 1;
}

static int pb_is_pushbot_listener(ConfigItem_listen *l)
{
	return (l && (l->options & 0x800000)) ? 1 : 0;
}

static int pb_config_listener(ConfigItem_listen *l)
{
	if (!pb_is_pushbot_listener(l)) return 0;
	if (l->socket_type == SOCKET_TYPE_UNIX) {
		/* Not supported -- pushbot wants real TCP+TLS for external bots. */
		config_warn("[pushbot] Unix-socket pushbot listeners are not supported (yet).");
		return 0;
	}
	l->options |= LISTENER_TLS;
	/* Need a custom start_handshake so the default doesn't spam
	 * "*** Looking up your hostname..." NOTICEs into the connection
	 * BEFORE we discover it's an HTTP/WS request -- those bytes
	 * corrupt the WS upgrade response. */
	l->start_handshake = pb_client_handshake;
	l->webserver = safe_alloc(sizeof(WebServer));
	l->webserver->handle_request = pb_handle_webrequest;
	l->webserver->handle_body = pb_handle_webrequest_data;
	return 1;
}

/* Called on accept().  Replaces the default IRC handshake (which
 * would send "*** Looking up your hostname..." NOTICEs that
 * corrupt the WS handshake response).  We still need the bare
 * minimum: reset client status post-TLS and run HOOKTYPE_HANDSHAKE
 * so other modules (TLS cipher info, etc.) get notified.  Skip the
 * DNS lookup -- bots authenticate by Bearer, not by hostmask. */
static void pb_client_handshake(Client *client)
{
	client->status = CLIENT_STATUS_UNKNOWN;
	RunHook(HOOKTYPE_HANDSHAKE, client);
	if (!IsDead(client))
		fd_setselect(client->local->fd, FD_SELECT_READ, read_packet, client);
}

static int pb_check_bearer(const char *auth, char *token_out, size_t tlen)
{
	if (!auth) return 0;
	while (*auth == ' ') auth++;
	if (strncasecmp(auth, "Bearer ", 7)) return 0;
	auth += 7;
	while (*auth == ' ') auth++;
	strlcpy(token_out, auth, tlen);
	/* Strip trailing whitespace just in case. */
	size_t n = strlen(token_out);
	while (n > 0 && (token_out[n-1] == ' ' || token_out[n-1] == '\r' || token_out[n-1] == '\t'))
		token_out[--n] = '\0';
	return token_out[0] ? 1 : 0;
}

/* Look up a bot by plaintext bearer.  Returns NULL on no-match.
 * Phase 1 stashed config-tokens in PbBot->config_token; that's what
 * we compare here.  Self-registered bots will get hashed tokens; we'll
 * compare via the hash in a later phase. */
static PbBot *pb_find_bot_by_token(const char *token)
{
	if (!token || !*token) return NULL;
	for (PbBot *b = bots; b; b = b->next) {
		if (b->status != PB_STATUS_ACTIVE) continue;
		if (!b->config_token) continue;
		if (!strcmp(b->config_token, token))
			return b;
	}
	return NULL;
}

static int pb_handle_webrequest(Client *client, WebRequest *web)
{
	const char *auth = get_nvplist(web->headers, "Authorization");
	char token[512];
	if (!pb_check_bearer(auth, token, sizeof(token))) {
		webserver_send_response(client, 401, "Bearer token required\n");
		return 0;
	}

	/* Need to keep token around until the upgrade is complete so we
	 * can hand it to the session.  Stash on the WebSocketUser-equiv
	 * for now (we'll create our session below). */

	if (!strcmp(web->uri, PB_GATEWAY_PATH) &&
	    get_nvplist(web->headers, "Sec-WebSocket-Key")) {
		/* WebSocket upgrade path. */
		if (!pb_websocket_md) {
			webserver_send_response(client, 405,
			    "WebSocket support not loaded (websocket_common module missing).\n");
			return 0;
		}
		/* Look up bot by token BEFORE upgrading -- saves us a roundtrip
		 * on bad creds. */
		PbBot *b = pb_find_bot_by_token(token);
		if (!b) {
			webserver_send_response(client, 401, "Invalid bearer token\n");
			return 0;
		}

		/* Allocate WebSocketUser so websocket_common's frame parser
		 * picks up our connection. */
		moddata_client(client, pb_websocket_md).ptr = safe_alloc(sizeof(WebSocketUser));
		((WebSocketUser *)moddata_client(client, pb_websocket_md).ptr)->type = WEBSOCKET_TYPE_TEXT;

		const char *ws_key = get_nvplist(web->headers, "Sec-WebSocket-Key");
		if (strchr(ws_key, ':')) {
			webserver_send_response(client, 400, "Invalid Sec-WebSocket-Key\n");
			return 0;
		}
		safe_strdup(((WebSocketUser *)moddata_client(client, pb_websocket_md).ptr)->handshake_key, ws_key);

		/* Allocate our session, link to the bot. */
		PbSession *s = safe_alloc(sizeof(PbSession));
		s->client = client;
		s->bot = NULL; /* attached on IDENTIFY, not here */
		s->last_heartbeat = TStime();
		moddata_client(client, pb_session_md).ptr = s;

		/* Stash the bot we resolved so IDENTIFY can verify the token
		 * matches what was sent. We do this by storing bot_id in the
		 * client's local extended info -- simplest: set the session's
		 * `bot` field tentatively, but mark identified=0 so dispatch
		 * doesn't fire yet. */
		s->bot = b;

		pb_ws_handshake_send(client);
		pb_send_hello(client);
		return 1; /* accept */
	}

	/* TODO phase 4: REST routes here. */
	webserver_send_response(client, 404, "PushBot REST API not implemented yet (phase 4).\n");
	return 0;
}

static int pb_ws_handshake_send(Client *client)
{
	char buf[512], hashbuf[64], sha1out[20];
	WebSocketUser *wsu = moddata_client(client, pb_websocket_md).ptr;
	wsu->handshake_completed = 1;
	snprintf(buf, sizeof(buf), "%s%s", wsu->handshake_key, WEBSOCKET_MAGIC_KEY);
	sha1hash_binary(sha1out, buf, strlen(buf));
	b64_encode(sha1out, sizeof(sha1out), hashbuf, sizeof(hashbuf));
	snprintf(buf, sizeof(buf),
	         "HTTP/1.1 101 Switching Protocols\r\n"
	         "Upgrade: websocket\r\n"
	         "Connection: Upgrade\r\n"
	         "Sec-WebSocket-Accept: %s\r\n\r\n",
	         hashbuf);
	dbuf_put(&client->local->sendQ, buf, strlen(buf));
	send_queued(client);
	return 0;
}

static int pb_handle_webrequest_data(Client *client, WebRequest *web, const char *buf, int len)
{
	WebSocketUser *wsu = pb_websocket_md ? moddata_client(client, pb_websocket_md).ptr : NULL;
	if (wsu)
		return pb_handle_body_websocket(client, web, buf, len);
	/* TODO phase 4: REST body handling. */
	webserver_send_response(client, 404, "Page not found.\n");
	return 0;
}

static int pb_handle_body_websocket(Client *client, WebRequest *web, const char *buf, int len)
{
	WebSocketUser *wsu = moddata_client(client, pb_websocket_md).ptr;
	if (!wsu || !wsu->handshake_completed)
		return 0;
	return websocket_handle_websocket(client, web, buf, len, pb_packet_in_websocket);
}

static int pb_packet_in_websocket(Client *client, char *buf, int len)
{
	/* Each call is one fully-reassembled WS frame.  We expect text
	 * frames carrying a single JSON object per spec §4.2. */
	if (len <= 0) return 0;
	pb_handle_ws_message(client, buf, len);
	return 0;
}

static void pb_send_op(Client *client, json_t *frame)
{
	char *body = json_dumps(frame, JSON_COMPACT);
	if (!body) return;
	int len = strlen(body);
	char *out = body;
	if (websocket_create_packet(WSOP_TEXT, &out, &len) < 0) {
		free(body);
		return;
	}
	dbuf_put(&client->local->sendQ, out, len);
	send_queued(client);
	free(body);
}

static void pb_send_hello(Client *client)
{
	json_t *frame = json_object();
	json_t *d = json_object();
	json_object_set_new(d, "heartbeat_interval", json_integer(PB_HEARTBEAT_INTERVAL_MS));
	json_object_set_new(frame, "op", json_integer(PB_OP_HELLO));
	json_object_set_new(frame, "d", d);
	pb_send_op(client, frame);
	json_decref(frame);
}

/* Drop everything in the queue with seq <= ack. */
static void pb_queue_ack(PbBot *b, long long ack)
{
	if (!b) return;
	PbQueuedEvent *e = b->queue_head;
	while (e && e->seq <= ack) {
		PbQueuedEvent *next = e->next;
		safe_free(e->json);
		DelListItem(e, b->queue_head);
		if (e == b->queue_tail) b->queue_tail = NULL;
		safe_free(e);
		b->queue_count--;
		e = next;
	}
	if (ack > b->last_acked_seq) b->last_acked_seq = ack;
}

/* Walk the queue and drop entries past their TTL. */
static void pb_queue_expire(PbBot *b)
{
	if (!b) return;
	time_t now = TStime();
	PbQueuedEvent *e = b->queue_head;
	while (e) {
		PbQueuedEvent *next = e->next;
		if (e->expires_at && e->expires_at < now) {
			safe_free(e->json);
			DelListItem(e, b->queue_head);
			if (e == b->queue_tail) b->queue_tail = NULL;
			safe_free(e);
			b->queue_count--;
		}
		e = next;
	}
}

/* Stash a serialized DISPATCH frame in the bot's queue with the given
 * seq.  Used both for new events and for resumption replay (which
 * already has the JSON). */
static int pb_queue_push(PbBot *b, long long seq, const char *json)
{
	if (!b || !json) return -1;
	if (b->queue_count >= PB_QUEUE_MAX) {
		unreal_log(ULOG_WARNING, "pushbot", "QUEUE_OVERFLOW", NULL,
		           "Bot $nick queue overflowed; disconnecting",
		           log_data_string("nick", b->nick));
		if (b->session && b->session->client)
			pb_close_ws(b->session->client, PB_CLOSE_QUEUE_OVERFLOW,
			            "Outbound queue overflowed");
		return -1;
	}
	PbQueuedEvent *e = safe_alloc(sizeof(*e));
	e->seq = seq;
	e->json = strdup(json);
	e->expires_at = TStime() + PB_RESUME_TTL_SEC;
	AppendListItem(e, b->queue_head);
	b->queue_tail = e;
	b->queue_count++;
	return 0;
}

/* Send a DISPATCH frame to the bot's current session AND record it
 * in the per-bot queue for resume.  Called by event-routing code in
 * later phases (CHANMSG handler, slash-command dispatch, etc.). */
static void pb_dispatch_event(PbBot *b, const char *event_name, json_t *data)
{
	if (!b) { if (data) json_decref(data); return; }

	long long seq = ++b->next_seq;
	json_t *frame = json_object();
	json_object_set_new(frame, "op", json_integer(PB_OP_DISPATCH));
	json_object_set_new(frame, "t", json_string(event_name));
	json_object_set_new(frame, "s", json_integer(seq));
	json_object_set_new(frame, "d", data ? data : json_object());

	char *body = json_dumps(frame, JSON_COMPACT);
	json_decref(frame);
	if (!body) return;

	pb_queue_push(b, seq, body);

	if (b->session && b->session->client &&
	    !IsDead(b->session->client) && b->session->client->local &&
	    b->session->identified) {
		int len = strlen(body);
		char *out = body;
		if (websocket_create_packet(WSOP_TEXT, &out, &len) >= 0) {
			dbuf_put(&b->session->client->local->sendQ, out, len);
			send_queued(b->session->client);
		}
	}
	free(body);
}

/* Compat shim while older code paths still call the old per-session
 * sender.  Internally just defers to pb_dispatch_event. */
static void pb_send_dispatch(Client *client, const char *event_name, json_t *data)
{
	PbSession *s = PB_SESS(client);
	if (!s || !s->bot) { if (data) json_decref(data); return; }
	pb_dispatch_event(s->bot, event_name, data);
}

static void pb_handle_ws_message(Client *client, char *msg, int len)
{
	json_error_t err;
	json_t *frame = json_loadb(msg, len, 0, &err);
	if (!frame || !json_is_object(frame)) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION, "Frame is not a JSON object");
		if (frame) json_decref(frame);
		return;
	}
	json_t *opj = json_object_get(frame, "op");
	if (!json_is_integer(opj)) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION, "Frame missing op");
		json_decref(frame);
		return;
	}
	int op = (int)json_integer_value(opj);
	switch (op) {
	case PB_OP_IDENTIFY:   pb_handle_identify(client, frame); break;
	case PB_OP_HEARTBEAT:  pb_handle_heartbeat(client, frame); break;
	case PB_OP_RESUME:   pb_handle_resume(client, frame); break;
	default:
		unreal_log(ULOG_DEBUG, "pushbot", "WS_UNKNOWN_OP", client,
		           "Received unknown opcode $op",
		           log_data_integer("op", op));
		break;
	}
	json_decref(frame);
}

static void pb_handle_identify(Client *client, json_t *frame)
{
	PbSession *s = PB_SESS(client);
	if (!s || !s->bot) {
		pb_close_ws(client, PB_CLOSE_AUTH_FAILED, "No session context");
		return;
	}

	json_t *d = json_object_get(frame, "d");
	if (!json_is_object(d)) {
		pb_close_ws(client, PB_CLOSE_AUTH_FAILED, "IDENTIFY.d must be an object");
		return;
	}
	/* Verify token in IDENTIFY matches what authenticated the upgrade. */
	json_t *tokj = json_object_get(d, "token");
	if (!json_is_string(tokj) ||
	    !s->bot->config_token ||
	    strcmp(json_string_value(tokj), s->bot->config_token)) {
		pb_close_ws(client, PB_CLOSE_AUTH_FAILED,
		            "IDENTIFY token mismatch");
		return;
	}

	/* IDENTIFY (vs RESUME) explicitly starts a fresh session.  Any
	 * previous resume window is closed and queued events tossed. */
	while (s->bot->queue_head) {
		PbQueuedEvent *e = s->bot->queue_head;
		safe_free(e->json);
		DelListItem(e, s->bot->queue_head);
		safe_free(e);
		s->bot->queue_count--;
	}
	s->bot->queue_tail = NULL;
	s->bot->next_seq = 0;
	s->bot->last_acked_seq = 0;
	s->bot->resume_expires_at = 0;

	/* Mint a fresh resume session id. */
	char rid[64];
	snprintf(rid, sizeof(rid), "%s.%lx.%lx",
	         s->bot->bot_id, (unsigned long)TStime(), (unsigned long)getpid());
	safe_strdup(s->bot->resume_session_id, rid);

	s->identified = 1;
	s->bot->session = s;
	s->last_heartbeat = TStime();

	/* Update the ghost's away to "online" (drop the away flag). */
	if (s->bot->ghost && s->bot->ghost->user->away) {
		safe_free(s->bot->ghost->user->away);
		s->bot->ghost->user->away = NULL;
	}

	/* READY dispatch. */
	json_t *ready_d = json_object();
	json_object_set_new(ready_d, "session_id", json_string(s->bot->resume_session_id));
	json_object_set_new(ready_d, "bot_nick", json_string(s->bot->nick));
	json_object_set_new(ready_d, "scope", json_string(pb_scope_str(s->bot->scope)));
	json_t *channels = json_array();
	if (s->bot->ghost) {
		for (Membership *m = s->bot->ghost->user->channel; m; m = m->next)
			json_array_append_new(channels, json_string(m->channel->name));
	}
	json_object_set_new(ready_d, "channels", channels);
	pb_send_dispatch(client, "READY", ready_d);

	unreal_log(ULOG_INFO, "pushbot", "BOT_ONLINE", NULL,
	           "Bot $nick is now online on gateway",
	           log_data_string("nick", s->bot->nick));
}

static void pb_handle_heartbeat(Client *client, json_t *frame)
{
	PbSession *s = PB_SESS(client);
	if (!s) return;
	/* d carries the last seq the bot has seen; we use it to release
	 * everything in the queue at or below that watermark. */
	json_t *d = json_object_get(frame, "d");
	if (json_is_integer(d) && s->bot)
		pb_queue_ack(s->bot, json_integer_value(d));
	s->last_heartbeat = TStime();
	json_t *ack = json_object();
	json_object_set_new(ack, "op", json_integer(PB_OP_HEARTBEAT_ACK));
	pb_send_op(client, ack);
	json_decref(ack);
}

/* RESUME: bot reconnected within the TTL window and wants to replay
 * missed events. */
static void pb_handle_resume(Client *client, json_t *frame)
{
	PbSession *s = PB_SESS(client);
	if (!s || !s->bot) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION, "No session context for resume");
		return;
	}
	PbBot *b = s->bot;

	json_t *d = json_object_get(frame, "d");
	if (!json_is_object(d)) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION, "RESUME.d must be object");
		return;
	}
	json_t *sidj = json_object_get(d, "session_id");
	json_t *seqj = json_object_get(d, "seq");
	json_t *tokj = json_object_get(d, "token");
	if (!json_is_string(sidj) || !json_is_integer(seqj) || !json_is_string(tokj)) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION,
		            "RESUME requires session_id, seq, token");
		return;
	}
	const char *sid = json_string_value(sidj);
	long long seq = (long long)json_integer_value(seqj);
	const char *tok = json_string_value(tokj);

	/* Validate everything before touching state. */
	if (!b->config_token || strcmp(tok, b->config_token)) {
		pb_close_ws(client, PB_CLOSE_AUTH_FAILED, "RESUME token mismatch");
		return;
	}
	if (!b->resume_session_id || strcmp(sid, b->resume_session_id)) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION, "Unknown session_id");
		return;
	}
	pb_queue_expire(b);
	/* If the queue is empty AND seq < next_seq, we lost events that
	 * fell off the TTL window.  Reject; bot must IDENTIFY fresh. */
	if (b->queue_count == 0 && seq < b->next_seq) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION,
		            "Resume window expired -- IDENTIFY fresh");
		return;
	}
	if (seq > b->next_seq) {
		pb_close_ws(client, PB_CLOSE_INVALID_SESSION,
		            "seq ahead of server -- IDENTIFY fresh");
		return;
	}

	/* Attach to the bot.  Acknowledge what they have and replay
	 * everything beyond it.  We don't INCREMENT next_seq while
	 * replaying -- the seqs are already baked into each event. */
	pb_queue_ack(b, seq);
	b->session = s;
	b->resume_expires_at = 0;
	s->identified = 1;
	s->last_heartbeat = TStime();

	for (PbQueuedEvent *e = b->queue_head; e; e = e->next) {
		int len = strlen(e->json);
		char *out = e->json;
		if (websocket_create_packet(WSOP_TEXT, &out, &len) >= 0) {
			dbuf_put(&client->local->sendQ, out, len);
		}
	}
	send_queued(client);

	/* Final RESUMED dispatch so the bot knows the replay is done. */
	json_t *resumed_d = json_object();
	json_object_set_new(resumed_d, "replayed",
	                    json_integer(b->queue_count));
	pb_dispatch_event(b, "RESUMED", resumed_d);

	unreal_log(ULOG_INFO, "pushbot", "BOT_RESUMED", NULL,
	           "Bot $nick resumed session, replayed $count event(s)",
	           log_data_string("nick", b->nick),
	           log_data_integer("count", b->queue_count));
}

static void pb_close_ws(Client *client, int code, const char *reason)
{
	/* Send a WebSocket Close frame (opcode 0x08), then mark the
	 * connection dead.  Frame body is a 2-byte close code in network
	 * byte order optionally followed by a UTF-8 reason. */
	char buf[256];
	int rlen = reason ? strlen(reason) : 0;
	if (rlen > 250) rlen = 250;
	buf[0] = (code >> 8) & 0xff;
	buf[1] = code & 0xff;
	if (rlen) memcpy(buf + 2, reason, rlen);
	char *payload = buf;
	int total = 2 + rlen;
	if (websocket_create_packet(0x08 /* WSOP_CLOSE */, &payload, &total) >= 0) {
		dbuf_put(&client->local->sendQ, payload, total);
		send_queued(client);
	}
	dead_socket(client, reason ? reason : "Gateway closed");
}

static void pb_session_free(PbSession *s)
{
	if (!s) return;
	if (s->bot && s->bot->session == s) {
		PbBot *b = s->bot;
		b->session = NULL;
		/* Open a resume window: keep queued events + session id
		 * around for PB_RESUME_TTL_SEC.  pb_heartbeat_check garbage-
		 * collects expired windows. */
		b->resume_expires_at = TStime() + PB_RESUME_TTL_SEC;
		if (b->ghost && !b->ghost->user->away) {
			safe_strdup(b->ghost->user->away, "bot offline");
			b->ghost->user->away_since = TStime();
		}
	}
	if (s->heartbeat_ev) { EventDel(s->heartbeat_ev); s->heartbeat_ev = NULL; }
	safe_free(s);
}

static void pb_moddata_session_free(ModData *md)
{
	if (!md || !md->ptr) return;
	pb_session_free((PbSession *)md->ptr);
	md->ptr = NULL;
}

EVENT(pb_heartbeat_check)
{
	time_t now = TStime();
	for (PbBot *b = bots; b; b = b->next) {
		/* Live session: check heartbeat.  Guard against half-dead
		 * clients -- moddata free may not have run yet between
		 * dead_socket() and the actual teardown, so b->session can
		 * still point to an IsDead client that we MUST NOT touch. */
		PbSession *s = b->session;
		if (s && s->client && !IsDead(s->client) && s->client->local && s->identified) {
			if ((now - s->last_heartbeat) * 1000 > PB_HEARTBEAT_GRACE_MS) {
				unreal_log(ULOG_INFO, "pushbot", "HEARTBEAT_TIMEOUT", NULL,
				           "Bot $nick gateway timed out, closing",
				           log_data_string("nick", b->nick));
				pb_close_ws(s->client, PB_CLOSE_TIMEOUT, "Heartbeat missed");
			}
		}

		/* Detached resume window: expire queue + clear session id
		 * if the bot didn't come back in time. */
		if (!b->session && b->resume_expires_at && b->resume_expires_at < now) {
			pb_queue_expire(b);
			/* If everything's gone or expired, drop the resume id
			 * so the next IDENTIFY is treated as fresh. */
			if (b->queue_count == 0) {
				safe_free(b->resume_session_id);
				b->resume_session_id = NULL;
				b->next_seq = 0;
				b->last_acked_seq = 0;
				b->resume_expires_at = 0;
			}
		} else if (!b->session && b->queue_head) {
			/* Still inside the window -- just expire individual
			 * entries that timed out. */
			pb_queue_expire(b);
		}
	}
}
