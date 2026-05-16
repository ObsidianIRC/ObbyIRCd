/*
 * draft-ai-tools.c — obbyircd module for draft/ai-tools v0.4
 *
 * Advertises the draft/ai-tools IRCv3 capability and registers the single
 * +obsidianirc/ai-tools message tag, whose value is an IRC-tag-value-escaped
 * JSON object carrying all workflow/step/action payloads.
 *
 * Using one tag avoids clienttagdeny slot exhaustion that afflicted the old
 * 15-tag v0.3 design.  Tags are relayed only to clients that have negotiated
 * draft/ai-tools (enforced via clicap_handler); only clients with the
 * capability (or servers) may send them.
 *
 * Spec: draft-ai-tools.md in irc-ai-tag-framework
 */

#include "unrealircd.h"

ModuleHeader MOD_HEADER = {
	"draft-ai-tools",
	"0.4",
	"draft/ai-tools — AI workflow transparency tags (single-tag JSON)",
	"irc-ai-tag-framework",
	"unrealircd-6",
};

/* Allocated by ClientCapabilityAdd; used for fast bitmask checks. */
static long CAP_AI_TOOLS = 0L;

/* ── forward declarations ─────────────────────────────────────────────────── */

static const char *ai_tools_cap_parameter(Client *client);
static int         ai_tools_mtag_is_ok(Client *client, const char *name, const char *value);
static void        ai_tools_mtag_relay(Client *client, MessageTag *recv_mtags,
                                       MessageTag **mtag_list, const char *signature);
static void        register_ait_tag(Module *module, ClientCapability *cap, const char *name);

/* ── capability parameter ─────────────────────────────────────────────────── */

/*
 * Advertise the feature set supported by this server-side module.
 * Bots and clients read this to know which control signals to offer.
 */
static const char *ai_tools_cap_parameter(Client *client)
{
	return "interactive,thinking,approval";
}

/* ── tag sender validation ────────────────────────────────────────────────── */

/*
 * is_ok — called for every incoming +obby.world/ai-tools tag from a client
 * or server.  Per IRCv3 client-only-tag semantics (the leading "+"), the
 * server MUST relay these verbatim regardless of whether the sender has
 * negotiated the capability — recipients verify trust themselves.  We
 * therefore accept from anyone and just reject empty values.
 *
 * (Previously this required HasCapabilityFast(CAP_AI_TOOLS) on the sender,
 * which silently dropped every TAGMSG from bots that hadn't run CAP REQ.)
 */
static int ai_tools_mtag_is_ok(Client *client, const char *name, const char *value)
{
	if (BadPtr(value))
		return 0;
	return 1;
}

/* ── HOOKTYPE_NEW_MESSAGE relay ────────────────────────────────────────────── */

/*
 * Without a NEW_MESSAGE hook, the +obby.world/ai-tools tag survives the
 * incoming `is_ok` filter but never gets COPIED onto the outgoing message-
 * tag list `cmd_message` builds.  cmd_message then calls
 * has_client_mtags() on the outgoing list, finds no `+`-prefixed tags, and
 * silently drops the TAGMSG as "empty and useless".  Copying the tag from
 * recv_mtags into mtag_list is the standard pattern (see typing-indicator
 * and reply-tag modules).
 */
static void ai_tools_mtag_relay(Client *client, MessageTag *recv_mtags,
                                MessageTag **mtag_list, const char *signature)
{
	MessageTag *m;

	if (!IsUser(client))
		return;

	m = find_mtag(recv_mtags, "+obby.world/ai-tools");
	if (m)
	{
		m = duplicate_mtag(m);
		AddListItem(m, *mtag_list);
	}
}

/* ── tag registration helper ──────────────────────────────────────────────── */

static void register_ait_tag(Module *module, ClientCapability *cap, const char *name)
{
	MessageTagHandlerInfo mtag;

	memset(&mtag, 0, sizeof(mtag));
	mtag.name           = (char *)name;  /* const-cast safe; API doesn't modify */
	mtag.is_ok          = ai_tools_mtag_is_ok;
	mtag.clicap_handler = cap;           /* relay only to draft/ai-tools clients */
	/* should_send_to_client left NULL: clicap_handler already handles filtering */
	MessageTagHandlerAdd(module, &mtag);
}

/* ── module lifecycle ─────────────────────────────────────────────────────── */

MOD_TEST()
{
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ClientCapabilityInfo cap;
	ClientCapability *c;

	/* Register the draft/ai-tools capability. */
	memset(&cap, 0, sizeof(cap));
	cap.name      = "draft/ai-tools";
	cap.parameter = ai_tools_cap_parameter;
	/* flags = CLICAP_FLAGS_NONE: individual tags carry their own clicap_handler */
	c = ClientCapabilityAdd(modinfo->handle, &cap, &CAP_AI_TOOLS);
	if (!c)
	{
		config_error("draft-ai-tools: ClientCapabilityAdd failed");
		return MOD_FAILED;
	}

	/* Single JSON-envelope tag replaces all 15 v0.3 per-field tags.
	 * Value is an IRC-tag-value-escaped JSON object; see spec for schema. */
	register_ait_tag(modinfo->handle, c, "+obby.world/ai-tools");

	/* Copy the client-only tag from incoming to outgoing on every message --
	 * without this, cmd_message's has_client_mtags() check drops the TAGMSG. */
	HookAddVoid(modinfo->handle, HOOKTYPE_NEW_MESSAGE, 0, ai_tools_mtag_relay);

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}
