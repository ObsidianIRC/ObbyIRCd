/*
 * obbyircd invitation module
 *
 * Adds two IRC commands and a small SQLite table for tracking
 * inviter -> invitee attribution:
 *
 *   INVITATION [<channel>]
 *       Issued by a logged-in user.  Mints a random share-id, stores
 *       it against the inviter's account, and replies with the
 *       full invite URL (uses set::invitation::base-url for the
 *       prefix).  Issuing user MUST have an account if
 *       set::invitation::require-registered is enabled (default).
 *
 *   INVCODE <share-id>
 *       Issued by a client BEFORE the REGISTER command, when the
 *       user is about to create an account from an invite link.
 *       Stashes the share-id on the client's connection as
 *       "pending attribution"; HOOKTYPE_ACCOUNT_LOGIN then writes a
 *       redemption row when that account first logs in.  Rejected
 *       if the user is already logged in (use a fresh connection).
 *
 * The invite-page module looks up share-ids via the
 * invitation_lookup_v1() function we export below so its
 * GET /i/<share-id> route can render "You've been invited by
 * <inviter>" with the channel hint (if any).
 *
 * Storage lives next to account-registration's accounts table in
 * obsidian.db (SQLite).  Two new tables:
 *
 *   invitations          (share_id, inviter_account, channel,
 *                         created_at, single_use)
 *   invitation_redemptions (share_id, account_name, redeemed_at)
 *
 * A share-id is valid for as long as the inviter's account exists
 * in the accounts table; lookups check that on every redeem so we
 * don't need to clean up rows on account deregistration.
 *
 * (C) 2026 obbyircd contributors. GPLv2+
 */
#include "unrealircd.h"
#include "obsidian.h"
#include <sqlite3.h>

/* Our own SQLite connection to the obsidian database file.  We don't
 * share account-registration's `obsidian_db` global because module
 * load order + cross-module symbol resolution would force a hard
 * link-time dependency on account-registration.so; opening our own
 * handle keeps invitation.c standalone and SQLite handles concurrent
 * connections to the same file natively.  The accounts table is
 * still authoritative for the "inviter still exists" check. */
static sqlite3 *inv_db = NULL;

ModuleHeader MOD_HEADER = {
	"invitation",
	"1.0",
	"INVITATION + INVCODE commands; tracks inviter -> invitee attribution",
	"obbyircd",
	"unrealircd-6",
};

/* ===================================================================
 * Config
 * =================================================================== */

#define INVITATION_SHARE_ID_LEN 12

static struct {
	int require_registered; /* gate INVITATION command behind login */
	int single_use_default;
	int max_per_account;
	char *base_url;         /* e.g. 'https://invite.example.com:6661/i/' */
} cfg = {
	.require_registered = 1,
	.single_use_default = 0,
	.max_per_account = 50,
	.base_url = NULL,
};

/* Per-client moddata: the share-id the client claimed via INVCODE,
 * pending application on HOOKTYPE_ACCOUNT_LOGIN. */
static ModDataInfo *pending_md = NULL;
static long CAP_INVITATION = 0L;

/* ===================================================================
 * Forwards
 * =================================================================== */
CMD_FUNC(cmd_invitation);
CMD_FUNC(cmd_invcode);
static int invitation_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int invitation_configrun(ConfigFile *cf, ConfigEntry *ce, int type);
static int invitation_account_login(Client *client, MessageTag *mtags);
static int invitation_local_quit(Client *client, MessageTag *mtags, const char *comment);
static int invitation_whois(Client *client, Client *target, NameValuePrioList **list);
static void pending_md_free(ModData *md);
static void invitation_create_tables(void);
static int generate_share_id(char *out, size_t n);
static int inviter_account_exists(const char *name);

/* Public, callable from invite-page.c via dlsym. */
int invitation_lookup_v1(const char *share_id, char *out_inviter, size_t out_inviter_sz,
                         char *out_channel, size_t out_channel_sz, int *out_valid);

/* ===================================================================
 * Module lifecycle
 * =================================================================== */
MOD_TEST()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, invitation_configtest);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ModDataInfo mdi;
	ClientCapabilityInfo cap;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, invitation_configrun);
	HookAdd(modinfo->handle, HOOKTYPE_ACCOUNT_LOGIN, 0, invitation_account_login);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_QUIT, 0, invitation_local_quit);
	HookAdd(modinfo->handle, HOOKTYPE_WHOIS, 0, invitation_whois);

	CommandAdd(modinfo->handle, "INVITATION", cmd_invitation, MAXPARA, CMD_USER);
	CommandAdd(modinfo->handle, "INVCODE", cmd_invcode, MAXPARA, CMD_USER | CMD_UNREGISTERED);

	memset(&mdi, 0, sizeof(mdi));
	mdi.name = "invitation_pending";
	mdi.type = MODDATATYPE_CLIENT;
	mdi.free = pending_md_free;
	pending_md = ModDataAdd(modinfo->handle, mdi);
	if (!pending_md)
	{
		config_error("invitation: could not register pending moddata");
		return MOD_FAILED;
	}

	/* Advertise capability so clients know the server supports the
	 * INVCODE / INVITATION protocol. */
	memset(&cap, 0, sizeof(cap));
	cap.name = "obby.world/invitation";
	ClientCapabilityAdd(modinfo->handle, &cap, &CAP_INVITATION);

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	if (!inv_db)
	{
		if (sqlite3_open(OBSIDIAN_DB, &inv_db) != SQLITE_OK)
		{
			config_error("invitation: cannot open SQLite database %s: %s",
			             OBSIDIAN_DB, inv_db ? sqlite3_errmsg(inv_db) : "open failed");
			if (inv_db) { sqlite3_close(inv_db); inv_db = NULL; }
			return MOD_FAILED;
		}
		/* WAL keeps reads/writes from competing handlers (account-
		 * registration's `obsidian_db` is also writing to the same
		 * file) from blocking each other. account-registration set
		 * up WAL on its open; this is a defensive set-once. */
		sqlite3_exec(inv_db, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
		sqlite3_exec(inv_db, "PRAGMA busy_timeout = 2000;", NULL, NULL, NULL);
	}
	invitation_create_tables();
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	if (inv_db)
	{
		sqlite3_close(inv_db);
		inv_db = NULL;
	}
	safe_free(cfg.base_url);
	return MOD_SUCCESS;
}

/* ===================================================================
 * Config: set::invitation { ... }
 * =================================================================== */

static int invitation_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	ConfigEntry *cep;
	int errors = 0;

	if (type != CONFIG_SET)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invitation"))
		return 0;

	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!cep->name)
			continue;
		if (!strcmp(cep->name, "require-registered") ||
		    !strcmp(cep->name, "single-use"))
		{
			if (!cep->value ||
			    (strcasecmp(cep->value, "yes") && strcasecmp(cep->value, "no")))
			{
				config_error("%s:%i: set::invitation::%s must be yes or no",
				             cep->file->filename, cep->line_number, cep->name);
				errors++;
			}
		} else if (!strcmp(cep->name, "max-per-account"))
		{
			int n = cep->value ? atoi(cep->value) : -1;
			if (n < 0 || n > 100000)
			{
				config_error("%s:%i: set::invitation::max-per-account must be 0..100000",
				             cep->file->filename, cep->line_number);
				errors++;
			}
		} else if (!strcmp(cep->name, "base-url"))
		{
			if (!cep->value || !*cep->value)
			{
				config_error("%s:%i: set::invitation::base-url requires a value",
				             cep->file->filename, cep->line_number);
				errors++;
			}
		} else
		{
			config_error("%s:%i: unknown directive set::invitation::%s",
			             cep->file->filename, cep->line_number, cep->name);
			errors++;
		}
	}

	*errs = errors;
	return errors ? -1 : 1;
}

static int invitation_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;
	if (type != CONFIG_SET)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invitation"))
		return 0;

	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!cep->name || !cep->value)
			continue;
		if (!strcmp(cep->name, "require-registered"))
			cfg.require_registered = !strcasecmp(cep->value, "yes");
		else if (!strcmp(cep->name, "single-use"))
			cfg.single_use_default = !strcasecmp(cep->value, "yes");
		else if (!strcmp(cep->name, "max-per-account"))
			cfg.max_per_account = atoi(cep->value);
		else if (!strcmp(cep->name, "base-url"))
			safe_strdup(cfg.base_url, cep->value);
	}
	return 1;
}

/* ===================================================================
 * Storage
 * =================================================================== */

static void invitation_create_tables(void)
{
	const char *sql =
	    "CREATE TABLE IF NOT EXISTS invitations ("
	    "  share_id TEXT PRIMARY KEY,"
	    "  inviter_account TEXT NOT NULL,"
	    "  channel TEXT,"
	    "  created_at INTEGER NOT NULL,"
	    "  single_use INTEGER NOT NULL DEFAULT 0"
	    ");"
	    "CREATE INDEX IF NOT EXISTS invitations_inviter "
	    "  ON invitations(inviter_account);"
	    "CREATE TABLE IF NOT EXISTS invitation_redemptions ("
	    "  share_id TEXT NOT NULL,"
	    "  account_name TEXT NOT NULL,"
	    "  redeemed_at INTEGER NOT NULL,"
	    "  PRIMARY KEY (share_id, account_name)"
	    ");"
	    "CREATE INDEX IF NOT EXISTS invitation_redemptions_account "
	    "  ON invitation_redemptions(account_name);";
	char *errmsg = NULL;
	if (sqlite3_exec(inv_db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
	{
		config_error("invitation: schema create failed: %s",
		             errmsg ? errmsg : "(unknown)");
		sqlite3_free(errmsg);
	}
}

/* Returns 1 if an account row exists in the accounts table.  Used to
 * implement the "share-id valid for as long as the inviter's account
 * is still registered" rule: when the inviter's account is gone, we
 * don't have to delete invitations rows -- the lookup just fails
 * naturally. */
static int inviter_account_exists(const char *name)
{
	sqlite3_stmt *stmt;
	int found = 0;

	if (!inv_db) return 0;
	if (sqlite3_prepare_v2(inv_db,
	    "SELECT 1 FROM accounts WHERE LOWER(name) = LOWER(?)", -1, &stmt, NULL)
	    != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		found = 1;
	sqlite3_finalize(stmt);
	return found;
}

/* Count outstanding share-ids issued by an account.  Used to enforce
 * set::invitation::max-per-account. */
static int invitation_count_for_account(const char *account)
{
	sqlite3_stmt *stmt;
	int n = 0;
	if (!inv_db) return 0;
	if (sqlite3_prepare_v2(inv_db,
	    "SELECT COUNT(*) FROM invitations WHERE LOWER(inviter_account) = LOWER(?)",
	    -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, account, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		n = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return n;
}

/* ===================================================================
 * Share-id generation
 * =================================================================== */

static int generate_share_id(char *out, size_t n)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	size_t i;
	if (n < INVITATION_SHARE_ID_LEN + 1) return 0;
	for (i = 0; i < INVITATION_SHARE_ID_LEN; i++)
		out[i] = alphabet[getrandom8() % (sizeof(alphabet) - 1)];
	out[INVITATION_SHARE_ID_LEN] = '\0';
	return 1;
}

/* ===================================================================
 * Public lookup (dlsym'd by invite-page)
 * =================================================================== */

/**
 * Look up a share-id.  Fills out_inviter / out_channel with the
 * stored values (channel may be empty for a network-wide invite).
 * Sets *out_valid to 1 iff the inviter's account currently exists
 * (i.e. the link still resolves) and 0 otherwise (link "expired"
 * because the inviter's account was deregistered).
 *
 * Returns 1 if the share-id was found in the table, 0 if not.
 */
int invitation_lookup_v1(const char *share_id, char *out_inviter, size_t out_inviter_sz,
                         char *out_channel, size_t out_channel_sz, int *out_valid)
{
	sqlite3_stmt *stmt;
	int rc, found = 0;

	if (out_inviter && out_inviter_sz) out_inviter[0] = '\0';
	if (out_channel && out_channel_sz) out_channel[0] = '\0';
	if (out_valid) *out_valid = 0;

	if (!inv_db || !share_id || !*share_id) return 0;

	if (sqlite3_prepare_v2(inv_db,
	    "SELECT inviter_account, COALESCE(channel,'') FROM invitations WHERE share_id = ?",
	    -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, share_id, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
	{
		const char *inv = (const char *)sqlite3_column_text(stmt, 0);
		const char *ch = (const char *)sqlite3_column_text(stmt, 1);
		if (inv && out_inviter && out_inviter_sz)
			strlcpy(out_inviter, inv, out_inviter_sz);
		if (ch && out_channel && out_channel_sz)
			strlcpy(out_channel, ch, out_channel_sz);
		found = 1;
		if (out_valid && inv && inviter_account_exists(inv))
			*out_valid = 1;
	}
	sqlite3_finalize(stmt);
	return found;
}

/* ===================================================================
 * INVITATION command dispatcher
 *
 * Syntax:
 *   INVITATION                       -- alias for LIST
 *   INVITATION LIST
 *   INVITATION CREATE [<channel>]
 *   INVITATION DELETE <share-id>
 *
 * Backward compat: `INVITATION <#channel>` (first arg starts with a
 * channel sigil) is treated as CREATE so existing scripts that
 * predate the subcommand split keep working.
 * =================================================================== */

static void inv_do_create(Client *client, const char *channel);
static void inv_do_list(Client *client);
static void inv_do_delete(Client *client, const char *share_id);

CMD_FUNC(cmd_invitation)
{
	const char *sub;

	if (!MyUser(client))
		return;

	/* Resolve the effective subcommand. */
	if (parc < 2 || BadPtr(parv[1]))
		sub = "LIST";
	else if (parv[1][0] == '#' || parv[1][0] == '&' ||
	         parv[1][0] == '^' || parv[1][0] == '$')
		sub = "CREATE_LEGACY"; /* dispatched below */
	else
		sub = parv[1];

	if (!strcasecmp(sub, "CREATE_LEGACY"))
	{
		inv_do_create(client, parv[1]);
		return;
	}
	if (!strcasecmp(sub, "CREATE"))
	{
		const char *channel = (parc >= 3 && !BadPtr(parv[2])) ? parv[2] : NULL;
		inv_do_create(client, channel);
		return;
	}
	if (!strcasecmp(sub, "LIST"))
	{
		inv_do_list(client);
		return;
	}
	if (!strcasecmp(sub, "DELETE") || !strcasecmp(sub, "DEL") ||
	    !strcasecmp(sub, "REMOVE"))
	{
		if (parc < 3 || BadPtr(parv[2]))
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION INVALID_PARAMS :Syntax: /INVITATION DELETE <share-id>",
			    me.name);
			return;
		}
		inv_do_delete(client, parv[2]);
		return;
	}

	sendto_one(client, NULL,
	    ":%s FAIL INVITATION INVALID_PARAMS :Subcommand must be CREATE, LIST, or DELETE.",
	    me.name);
}

/* Helper: return the calling client's "owner account" string for
 * invitation rows.  Logged-in users use their account; everyone else
 * uses their nick when require-registered is off.  Returns NULL when
 * the caller isn't allowed to mint/own invitations. */
static const char *inv_owner_for(Client *client)
{
	const char *acct = (client->user && IsLoggedIn(client)) ?
	    client->user->account : NULL;
	if (acct && *acct)
		return acct;
	if (cfg.require_registered)
		return NULL;
	return client->name;
}

static void inv_do_create(Client *client, const char *channel_raw)
{
	const char *account;
	const char *channel = NULL;
	char share_id[INVITATION_SHARE_ID_LEN + 1];
	char url_buf[512];
	sqlite3_stmt *stmt;

	account = inv_owner_for(client);
	if (!account)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION NOT_AUTHORISED :You must be logged in to create invitations.",
		    me.name);
		return;
	}

	if (channel_raw && *channel_raw)
	{
		if (*channel_raw != '#' && *channel_raw != '&' &&
		    *channel_raw != '^' && *channel_raw != '$')
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION INVALID_CHANNEL %s :Channel must start with # & ^ or $.",
			    me.name, channel_raw);
			return;
		}
		if (strlen(channel_raw) > CHANNELLEN)
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION INVALID_CHANNEL :Channel name too long.",
			    me.name);
			return;
		}
		channel = channel_raw;
	}

	if (cfg.max_per_account > 0 &&
	    invitation_count_for_account(account) >= cfg.max_per_account)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION QUOTA_EXCEEDED :You have reached the max %d invitations.",
		    me.name, cfg.max_per_account);
		return;
	}

	if (!inv_db)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION SERVER_BUG :Invitation database unavailable.",
		    me.name);
		return;
	}

	/* Retry a couple of times in the (vanishingly unlikely) event of
	 * a share-id collision.  72 bits of entropy collide at
	 * sqrt(2^72) ≈ 2^36 ≈ 70 billion rows, so this is paranoia. */
	{
		int tries;
		int inserted = 0;
		for (tries = 0; tries < 5 && !inserted; tries++)
		{
			generate_share_id(share_id, sizeof(share_id));

			if (sqlite3_prepare_v2(inv_db,
			    "INSERT INTO invitations (share_id, inviter_account, channel, created_at, single_use) "
			    "VALUES (?, ?, ?, ?, ?)",
			    -1, &stmt, NULL) != SQLITE_OK)
				break;
			sqlite3_bind_text(stmt, 1, share_id, -1, SQLITE_STATIC);
			sqlite3_bind_text(stmt, 2, account, -1, SQLITE_STATIC);
			if (channel)
				sqlite3_bind_text(stmt, 3, channel, -1, SQLITE_STATIC);
			else
				sqlite3_bind_null(stmt, 3);
			sqlite3_bind_int(stmt, 4, (int)TStime());
			sqlite3_bind_int(stmt, 5, cfg.single_use_default ? 1 : 0);
			if (sqlite3_step(stmt) == SQLITE_DONE)
				inserted = 1;
			sqlite3_finalize(stmt);
		}
		if (!inserted)
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION SERVER_BUG :Failed to record invitation.",
			    me.name);
			return;
		}
	}

	if (cfg.base_url && *cfg.base_url)
		snprintf(url_buf, sizeof(url_buf), "%s%s", cfg.base_url, share_id);
	else
		snprintf(url_buf, sizeof(url_buf), "%s", share_id);

	/* Machine-readable parameter-positional reply. */
	if (channel)
		sendto_one(client, NULL,
		    ":%s INVITATION %s %s :%s",
		    me.name, share_id, channel, url_buf);
	else
		sendto_one(client, NULL,
		    ":%s INVITATION %s * :%s",
		    me.name, share_id, url_buf);

	/* IRCv3 standard-replies NOTE so cap-aware clients can render
	 * the link nicely. */
	sendto_one(client, NULL,
	    ":%s NOTE INVITATION CREATED %s %s :Invitation link: %s",
	    me.name, share_id, channel ? channel : "*", url_buf);
}

/* INVITATION LIST: enumerate the caller's invitations.
 *
 * Wire format: one line per row, plus a final NOTE terminator.
 *
 *   :server INVITATION ENTRY <share-id> <channel|*> <created-iso8601> <redeem-count>
 *   ...
 *   :server NOTE INVITATION LIST_END * :End of invitation list. */
static void inv_do_list(Client *client)
{
	const char *account = inv_owner_for(client);
	sqlite3_stmt *stmt;
	int count = 0;

	if (!account)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION NOT_AUTHORISED :You must be logged in to list invitations.",
		    me.name);
		return;
	}
	if (!inv_db)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION SERVER_BUG :Invitation database unavailable.",
		    me.name);
		return;
	}

	if (sqlite3_prepare_v2(inv_db,
	    "SELECT i.share_id, COALESCE(i.channel,''), i.created_at, "
	    "       (SELECT COUNT(*) FROM invitation_redemptions r WHERE r.share_id = i.share_id) "
	    "FROM invitations i "
	    "WHERE LOWER(i.inviter_account) = LOWER(?) "
	    "ORDER BY i.created_at DESC",
	    -1, &stmt, NULL) != SQLITE_OK)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION SERVER_BUG :Database error.", me.name);
		return;
	}
	sqlite3_bind_text(stmt, 1, account, -1, SQLITE_STATIC);

	while (sqlite3_step(stmt) == SQLITE_ROW)
	{
		const char *share_id = (const char *)sqlite3_column_text(stmt, 0);
		const char *channel  = (const char *)sqlite3_column_text(stmt, 1);
		int         created  = sqlite3_column_int(stmt, 2);
		int         redeems  = sqlite3_column_int(stmt, 3);
		const char *iso      = timestamp_iso8601((time_t)created);
		sendto_one(client, NULL,
		    ":%s INVITATION ENTRY %s %s %s %d",
		    me.name, share_id,
		    (channel && *channel) ? channel : "*",
		    iso, redeems);
		count++;
	}
	sqlite3_finalize(stmt);

	sendto_one(client, NULL,
	    ":%s NOTE INVITATION LIST_END * :End of invitation list (%d %s).",
	    me.name, count, count == 1 ? "entry" : "entries");
}

/* INVITATION DELETE <share-id> -- caller must own the share-id
 * (LOWER-cased account match).  IRC operators can delete anyone's
 * invitation via the same command. */
static void inv_do_delete(Client *client, const char *share_id)
{
	const char *account = inv_owner_for(client);
	sqlite3_stmt *stmt;
	int affected = 0;

	if (!account)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION NOT_AUTHORISED :You must be logged in to delete invitations.",
		    me.name);
		return;
	}
	if (!inv_db)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION SERVER_BUG :Invitation database unavailable.",
		    me.name);
		return;
	}

	if (IsOper(client))
	{
		if (sqlite3_prepare_v2(inv_db,
		    "DELETE FROM invitations WHERE share_id = ?",
		    -1, &stmt, NULL) != SQLITE_OK)
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION SERVER_BUG :Database error.", me.name);
			return;
		}
		sqlite3_bind_text(stmt, 1, share_id, -1, SQLITE_STATIC);
	}
	else
	{
		if (sqlite3_prepare_v2(inv_db,
		    "DELETE FROM invitations WHERE share_id = ? AND LOWER(inviter_account) = LOWER(?)",
		    -1, &stmt, NULL) != SQLITE_OK)
		{
			sendto_one(client, NULL,
			    ":%s FAIL INVITATION SERVER_BUG :Database error.", me.name);
			return;
		}
		sqlite3_bind_text(stmt, 1, share_id, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, account, -1, SQLITE_STATIC);
	}

	if (sqlite3_step(stmt) == SQLITE_DONE)
		affected = sqlite3_changes(inv_db);
	sqlite3_finalize(stmt);

	if (affected <= 0)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVITATION NOT_FOUND %s :No matching invitation owned by you.",
		    me.name, share_id);
		return;
	}

	sendto_one(client, NULL,
	    ":%s NOTE INVITATION DELETED %s :Invitation deleted.",
	    me.name, share_id);

	/* Also delete the redemption history rows for tidy bookkeeping. */
	if (sqlite3_prepare_v2(inv_db,
	    "DELETE FROM invitation_redemptions WHERE share_id = ?",
	    -1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_text(stmt, 1, share_id, -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

/* ===================================================================
 * INVCODE command (pre-REGISTER)
 * =================================================================== */

static void pending_md_free(ModData *md)
{
	if (md && md->str)
	{
		safe_free(md->str);
		md->str = NULL;
	}
}

CMD_FUNC(cmd_invcode)
{
	const char *code;
	char inviter[NICKLEN + 1] = "";
	char channel[CHANNELLEN + 4] = "";
	int valid = 0;

	if (parc < 2 || BadPtr(parv[1]))
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVCODE INVALID_PARAMS :Syntax: /INVCODE <share-id>", me.name);
		return;
	}
	code = parv[1];

	if (IsLoggedIn(client))
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVCODE ALREADY_REGISTERED :Invite codes are only consumed during account registration.",
		    me.name);
		return;
	}

	if (!invitation_lookup_v1(code, inviter, sizeof(inviter),
	                          channel, sizeof(channel), &valid))
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVCODE INVALID_CODE %s :Unknown invitation code.",
		    me.name, code);
		return;
	}
	if (!valid)
	{
		sendto_one(client, NULL,
		    ":%s FAIL INVCODE EXPIRED %s :This invitation has expired (inviter account no longer registered).",
		    me.name, code);
		return;
	}

	/* Stash on the connection for HOOKTYPE_ACCOUNT_LOGIN. */
	if (pending_md)
	{
		ModData *md = &moddata_client(client, pending_md);
		safe_strdup(md->str, code);
	}

	sendto_one(client, NULL,
	    ":%s NOTE INVCODE ACCEPTED %s %s :Invitation code accepted; proceed with REGISTER.",
	    me.name, code, channel[0] ? channel : "*");
}

/* ===================================================================
 * Apply pending attribution on first login (== post-REGISTER)
 * =================================================================== */

static int invitation_account_login(Client *client, MessageTag *mtags)
{
	ModData *md;
	const char *code;
	const char *account;
	sqlite3_stmt *stmt;
	int already;

	if (!MyUser(client) || !pending_md) return 0;
	md = &moddata_client(client, pending_md);
	if (!md || !md->str || !*md->str) return 0;
	if (!client->user || !*client->user->account) return 0;

	code = md->str;
	account = client->user->account;

	/* Already recorded? Don't double-attribute. */
	already = 0;
	if (inv_db && sqlite3_prepare_v2(inv_db,
	    "SELECT 1 FROM invitation_redemptions WHERE share_id = ? AND LOWER(account_name) = LOWER(?)",
	    -1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, account, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW)
			already = 1;
		sqlite3_finalize(stmt);
	}

	if (!already && inv_db &&
	    sqlite3_prepare_v2(inv_db,
	    "INSERT INTO invitation_redemptions (share_id, account_name, redeemed_at) "
	    "VALUES (?, ?, ?)", -1, &stmt, NULL) == SQLITE_OK)
	{
		sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 2, account, -1, SQLITE_STATIC);
		sqlite3_bind_int (stmt, 3, (int)TStime());
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);

		/* If the share-id is single-use, delete it now so further
		 * redemptions fail at lookup time. */
		if (sqlite3_prepare_v2(inv_db,
		    "DELETE FROM invitations WHERE share_id = ? AND single_use = 1",
		    -1, &stmt, NULL) == SQLITE_OK)
		{
			sqlite3_bind_text(stmt, 1, code, -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}

		unreal_log(ULOG_INFO, "invitation", "INVITATION_REDEEMED", client,
		           "Account $account registered via invitation $code",
		           log_data_string("account", account),
		           log_data_string("code", code));
	}

	/* Clear the pending stash regardless. */
	safe_free(md->str);
	md->str = NULL;
	return 0;
}

static int invitation_local_quit(Client *client, MessageTag *mtags, const char *comment)
{
	if (pending_md)
	{
		ModData *md = &moddata_client(client, pending_md);
		if (md && md->str)
		{
			safe_free(md->str);
			md->str = NULL;
		}
	}
	return 0;
}

/* ===================================================================
 * WHOIS referral line
 *
 * Adds ":target :was referred by <inviter>" to the WHOIS reply when
 * the target has an attribution row in invitation_redemptions.
 * Gated through set::whois-details::referral.  Default policy in
 * whois.c is self + oper full (everyone none) -- the referral leaks
 * social-graph info so we don't broadcast it.
 * =================================================================== */
static int invitation_whois(Client *client, Client *target, NameValuePrioList **list)
{
	const char *target_account;
	sqlite3_stmt *stmt;
	char inviter[NICKLEN + 1] = "";

	if (!inv_db || !target->user) return 0;
	target_account = target->user->account;
	if (!target_account || !*target_account || !strcmp(target_account, "0"))
		return 0;
	if (whois_get_policy(client, target, "referral") <= WHOIS_CONFIG_DETAILS_NONE)
		return 0;

	if (sqlite3_prepare_v2(inv_db,
	    "SELECT i.inviter_account "
	    "FROM invitation_redemptions r "
	    "JOIN invitations i ON i.share_id = r.share_id "
	    "WHERE LOWER(r.account_name) = LOWER(?) "
	    "ORDER BY r.redeemed_at ASC LIMIT 1",
	    -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, target_account, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		const char *inv = (const char *)sqlite3_column_text(stmt, 0);
		if (inv) strlcpy(inviter, inv, sizeof(inviter));
	}
	sqlite3_finalize(stmt);

	if (*inviter)
	{
		add_nvplist_numeric_fmt(list, -10000, "referral", client, RPL_WHOISSPECIAL,
		                        "%s :was referred by %s",
		                        target->name, inviter);
	}
	return 0;
}
