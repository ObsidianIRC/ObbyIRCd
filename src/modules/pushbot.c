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

#define MYCONF "pushbot"
#define DEFAULT_DB "pushbot.db"

ModuleHeader MOD_HEADER = {
	"pushbot",
	"0.1",
	"Discord-style out-of-process bots (skeleton, phase 1)",
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
	unreal_log(ULOG_DEBUG, "pushbot", "MOD_TEST", NULL,
	           "pushbot MOD_TEST ran, CONFIGTEST hook registered");
	return MOD_SUCCESS;
}

MOD_INIT()
{
	modinfo_ref = modinfo;
	MARK_AS_OFFICIAL_MODULE(modinfo);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, pb_configrun);
	CommandAdd(modinfo->handle, "PUSHBOT", cmd_pushbot, MAXPARA, CMD_USER);
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
