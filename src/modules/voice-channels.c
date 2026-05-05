/* src/modules/voice-channels.c
 *
 * Bridge between ObsidianIRC voice channels (`^`-prefixed) and the
 * hosted-backend's WebRTC SFU + TURN server.
 *
 * Wire shape:
 *
 *   ObsidianIRC client            ObbyIRCd                hosted-backend
 *   ─────────────────             ────────                ──────────────
 *   TAGMSG ^vc                    voice-channels.c        voice.go
 *   @+obsidianirc/rtc=…    ─────► (CAN_SEND_TO_CHANNEL    (Unix-socket
 *                                  hook intercepts        bridge JSON)
 *                                  +obsidianirc/rtc;
 *                                  forwards to bridge,
 *                                  denies broadcast so
 *                                  SDP doesn't leak)
 *
 *   :server.name TAGMSG vc        ◄── bridge frame ───
 *   @+obsidianirc/rtc=…             {"op":"signal",
 *                                    "to":"alice|^vc",
 *                                    "payload":…}
 *
 * The hosted-backend handles all the SFU work; this module just
 * shovels frames between IRC and the Unix-socket bridge.
 *
 *  License: GPLv3-or-later
 *  Copyright (c) 2026 ObsidianIRC Team
 */

#include "unrealircd.h"

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <jansson.h>

ModuleHeader MOD_HEADER = {
	"voice-channels",
	"1.0",
	"Voice/video channels (^prefix) bridged to hosted-backend SFU",
	"ObsidianIRC Team",
	"unrealircd-6",
};

#define VOICE_CHAN_PREFIX  '^'
#define VOICE_RTC_TAG      "+obsidianirc/rtc"
#define VOICE_DEFAULT_SOCK "/tmp/obbyirc-voice.sock"

/* Configurable via env var or set::voice-bridge-socket "<path>"; in obbyircd.conf. */
static char *cfg_bridge_socket = NULL;

/* ===================================================================
 * Bridge connection state
 * =================================================================== */
static int bridge_fd = -1;
static char bridge_inbuf[256 * 1024];
static size_t bridge_inbuf_len = 0;

static void bridge_disconnect(void)
{
	if (bridge_fd >= 0)
	{
		close(bridge_fd);
		bridge_fd = -1;
	}
	bridge_inbuf_len = 0;
}

static int bridge_connect(void)
{
	const char *path = cfg_bridge_socket ? cfg_bridge_socket : VOICE_DEFAULT_SOCK;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return -1;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	bridge_fd = fd;
	bridge_inbuf_len = 0;
	unreal_log(ULOG_INFO, "voice", "BRIDGE_CONNECTED", NULL,
	           "voice-channels: connected to bridge $path",
	           log_data_string("path", path));
	return 0;
}

/* Write a single newline-terminated frame.  Returns 0 on success,
 * -1 on connection failure (caller should reconnect). */
static int bridge_send_frame(json_t *frame)
{
	if (bridge_fd < 0 && bridge_connect() < 0)
		return -1;
	char *encoded = json_dumps(frame, JSON_COMPACT);
	if (!encoded)
		return -1;
	size_t len = strlen(encoded);
	struct iovec iov[2];
	iov[0].iov_base = encoded;
	iov[0].iov_len = len;
	char nl = '\n';
	iov[1].iov_base = &nl;
	iov[1].iov_len = 1;
	ssize_t w = writev(bridge_fd, iov, 2);
	free(encoded);
	if (w < 0)
	{
		bridge_disconnect();
		return -1;
	}
	return 0;
}

/* ===================================================================
 * Send-side: forward client signaling to the bridge.
 *
 * Called from the CAN_SEND_TO_CHANNEL hook when a client TAGMSG with
 * the +obsidianirc/rtc tag lands on a `^channel`.
 * =================================================================== */
static void bridge_forward_signal(Client *client, Channel *channel,
                                   const char *payload_json)
{
	json_error_t je;
	json_t *payload = json_loads(payload_json, 0, &je);
	if (!payload)
	{
		unreal_log(ULOG_DEBUG, "voice", "BAD_PAYLOAD", client,
		           "voice-channels: dropping malformed RTC payload");
		return;
	}
	json_t *frame = json_object();
	json_object_set_new(frame, "op", json_string("signal"));
	json_object_set_new(frame, "from", json_string(client->name));
	json_object_set_new(frame, "channel", json_string(channel->name));
	if (IsLoggedIn(client))
		json_object_set_new(frame, "account",
		                    json_string(client->user->account));
	json_object_set_new(frame, "payload", payload);
	bridge_send_frame(frame);
	json_decref(frame);
}

static void bridge_forward_part(Client *client, const char *channel_name)
{
	json_t *frame = json_object();
	json_object_set_new(frame, "op", json_string("part"));
	json_object_set_new(frame, "from", json_string(client->name));
	json_object_set_new(frame, "channel", json_string(channel_name));
	bridge_send_frame(frame);
	json_decref(frame);
}

static void bridge_forward_quit(Client *client)
{
	json_t *frame = json_object();
	json_object_set_new(frame, "op", json_string("quit"));
	json_object_set_new(frame, "from", json_string(client->name));
	bridge_send_frame(frame);
	json_decref(frame);
}

/* ===================================================================
 * Recv-side: pump the socket and emit TAGMSGs from the server.
 *
 * Called periodically by the EVENT timer.  We do non-blocking read
 * into bridge_inbuf, parse newline-delimited JSON frames, and emit
 * one TAGMSG per frame using me.name as the source.
 * =================================================================== */
static void emit_outbound_signal(const char *to, const char *payload_json)
{
	if (!to || !*to || !payload_json)
		return;

	/* Tag value escaping per IRCv3 message-tags:
	 *   ;  -> \:
	 *   space -> \s
	 *   \  -> \\
	 *   CR -> \r, LF -> \n
	 * The JSON is unlikely to contain CR/LF (single-line) but
	 * spaces are very likely.
	 */
	size_t plen = strlen(payload_json);
	char *escaped = safe_alloc(plen * 2 + 1);
	size_t ei = 0;
	for (size_t i = 0; i < plen; i++)
	{
		char c = payload_json[i];
		switch (c)
		{
		case ';':  escaped[ei++] = '\\'; escaped[ei++] = ':'; break;
		case ' ':  escaped[ei++] = '\\'; escaped[ei++] = 's'; break;
		case '\\': escaped[ei++] = '\\'; escaped[ei++] = '\\'; break;
		case '\r': escaped[ei++] = '\\'; escaped[ei++] = 'r'; break;
		case '\n': escaped[ei++] = '\\'; escaped[ei++] = 'n'; break;
		default:   escaped[ei++] = c; break;
		}
	}
	escaped[ei] = '\0';

	if (to[0] == VOICE_CHAN_PREFIX || to[0] == '#')
	{
		/* Channel target: server-sourced TAGMSG to every member. */
		Channel *channel = find_channel(to);
		if (!channel)
		{
			safe_free(escaped);
			return;
		}
		Member *m;
		for (m = channel->members; m; m = m->next)
		{
			if (!MyUser(m->client))
				continue;
			sendto_one(m->client, NULL,
			           "@" VOICE_RTC_TAG "=%s :%s TAGMSG %s",
			           escaped, me.name, channel->name);
		}
	}
	else
	{
		/* Direct to a specific user (nick lookup). */
		Client *target = find_user(to, NULL);
		if (target && MyUser(target))
		{
			sendto_one(target, NULL,
			           "@" VOICE_RTC_TAG "=%s :%s TAGMSG %s",
			           escaped, me.name, target->name);
		}
	}
	safe_free(escaped);
}

static void process_bridge_frame(const char *line, size_t len)
{
	json_error_t je;
	json_t *frame = json_loadb(line, len, 0, &je);
	if (!frame)
	{
		unreal_log(ULOG_DEBUG, "voice", "BRIDGE_BAD_FRAME", NULL,
		           "voice-channels: bad bridge frame: $err",
		           log_data_string("err", je.text));
		return;
	}
	json_t *op = json_object_get(frame, "op");
	if (!op || strcmp(json_string_value(op), "signal") != 0)
	{
		json_decref(frame);
		return;
	}
	json_t *to = json_object_get(frame, "to");
	json_t *payload = json_object_get(frame, "payload");
	if (!to || !payload)
	{
		json_decref(frame);
		return;
	}
	char *payload_json = json_dumps(payload, JSON_COMPACT);
	if (payload_json)
	{
		emit_outbound_signal(json_string_value(to), payload_json);
		free(payload_json);
	}
	json_decref(frame);
}

EVENT(bridge_pump_event)
{
	if (bridge_fd < 0)
	{
		/* Try to (re)connect every tick. */
		bridge_connect();
		return;
	}
	for (;;)
	{
		size_t avail = sizeof(bridge_inbuf) - bridge_inbuf_len - 1;
		if (avail == 0)
		{
			/* Buffer overflow: scrap and reconnect. */
			bridge_disconnect();
			return;
		}
		ssize_t n = read(bridge_fd, bridge_inbuf + bridge_inbuf_len, avail);
		if (n == 0)
		{
			bridge_disconnect();
			return;
		}
		if (n < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			bridge_disconnect();
			return;
		}
		bridge_inbuf_len += n;
	}

	/* Parse complete lines. */
	size_t consumed = 0;
	while (consumed < bridge_inbuf_len)
	{
		char *nl = memchr(bridge_inbuf + consumed, '\n',
		                  bridge_inbuf_len - consumed);
		if (!nl)
			break;
		size_t line_len = (size_t)(nl - (bridge_inbuf + consumed));
		process_bridge_frame(bridge_inbuf + consumed, line_len);
		consumed += line_len + 1; /* skip the \n */
	}
	if (consumed > 0)
	{
		memmove(bridge_inbuf, bridge_inbuf + consumed,
		        bridge_inbuf_len - consumed);
		bridge_inbuf_len -= consumed;
	}
}

/* ===================================================================
 * Hooks
 * =================================================================== */
/* PRE_CHANMSG fires before the channel TAGMSG is broadcast.  We
 * inspect the outgoing mtag list, forward the +obsidianirc/rtc payload
 * to the bridge, and *delete* the tag from the list -- the empty
 * resulting TAGMSG is dropped automatically by message.c's
 * has_client_mtags() guard, so the SDP never reaches other channel
 * members. */
static int voice_pre_chanmsg(Client *client, Channel *channel,
                              MessageTag **mtags,
                              const char *_text, SendType sendtype)
{
	if (!MyUser(client))
		return 0;
	if (channel->name[0] != VOICE_CHAN_PREFIX)
		return 0;
	if (sendtype != SEND_TYPE_TAGMSG)
		return 0;


	MessageTag *prev = NULL;
	for (MessageTag *m = *mtags; m; m = m->next)
	{
		if (m->name && !strcmp(m->name, VOICE_RTC_TAG) && m->value)
		{
			bridge_forward_signal(client, channel, m->value);
			/* Detach `m` from the list -- has_client_mtags() in
			 * message.c will then see an empty list and drop the
			 * TAGMSG before it broadcasts. */
			if (prev)
				prev->next = m->next;
			else
				*mtags = m->next;
			m->next = NULL;
			free_message_tags(m);
			return 0;
		}
		prev = m;
	}
	return 0;
}

static int voice_local_part(Client *client, Channel *channel,
                            MessageTag *_mtags, const char *_comment)
{
	if (channel->name[0] == VOICE_CHAN_PREFIX)
		bridge_forward_part(client, channel->name);
	return 0;
}

static int voice_local_quit(Client *client, MessageTag *_mtags,
                             const char *_comment)
{
	if (!IsUser(client))
		return 0;
	bridge_forward_quit(client);
	return 0;
}

/* Permit "+obsidianirc/rtc" from anyone -- clients send it, the
 * server module relays it back, and other servers may forward it. */
static int voice_rtc_mtag_is_ok(Client *_client, const char *_name,
                                 const char *_value)
{
	return 1;
}

/* HOOKTYPE_NEW_MESSAGE: copy "+obsidianirc/rtc" from the parsed recv
 * tag list onto the outgoing tag list so PRE_CHANMSG can see it.
 * Without this the tag is parsed and accepted but never propagated
 * past new_message(). */
static void voice_rtc_new_message(Client *_client, MessageTag *recv_mtags,
                                   MessageTag **mtag_list,
                                   const char *_signature)
{
	MessageTag *m = find_mtag(recv_mtags, VOICE_RTC_TAG);
	if (m)
	{
		m = duplicate_mtag(m);
		AddListItem(m, *mtag_list);
	}
}

/* ===================================================================
 * Config
 * =================================================================== */
static int voice_configtest(ConfigFile *_cf, ConfigEntry *ce, int type,
                             int *_errs)
{
	if (type != CONFIG_SET)
		return 0;
	if (!ce->name || strcmp(ce->name, "voice-bridge-socket"))
		return 0;
	return 1;
}

static int voice_configrun(ConfigFile *_cf, ConfigEntry *ce, int type)
{
	if (type != CONFIG_SET)
		return 0;
	if (!ce->name || strcmp(ce->name, "voice-bridge-socket"))
		return 0;
	safe_free(cfg_bridge_socket);
	safe_strdup(cfg_bridge_socket, ce->value);
	return 1;
}

/* ===================================================================
 * Module wiring
 * =================================================================== */
MOD_TEST()
{
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, voice_configtest);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, voice_configrun);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	const char *env = getenv("VOICE_BRIDGE_SOCKET");
	if (env && *env && !cfg_bridge_socket)
		safe_strdup(cfg_bridge_socket, env);

	/* Without registering a MessageTagHandler the parser silently
	 * drops "+obsidianirc/rtc" from incoming TAGMSGs (message_tag_ok()
	 * rejects unknown tags from local clients), and PRE_CHANMSG sees
	 * an empty mtag list. */
	MessageTagHandlerInfo mtag;
	memset(&mtag, 0, sizeof(mtag));
	mtag.name = VOICE_RTC_TAG;
	mtag.is_ok = voice_rtc_mtag_is_ok;
	mtag.flags = MTAG_HANDLER_FLAGS_NO_CAP_NEEDED;
	MessageTagHandlerAdd(modinfo->handle, &mtag);

	HookAddVoid(modinfo->handle, HOOKTYPE_NEW_MESSAGE, 0, voice_rtc_new_message);
	HookAdd(modinfo->handle, HOOKTYPE_PRE_CHANMSG, 0, voice_pre_chanmsg);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_PART, 0, voice_local_part);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_QUIT, 0, voice_local_quit);

	/* Pump the bridge socket on a 100ms cadence -- low enough that
	 * signaling RTT feels instant, high enough that idle CPU is
	 * negligible. */
	EventAdd(modinfo->handle, "voice_bridge_pump", bridge_pump_event,
	         NULL, 100, 0);

	/* CHANTYPES already advertises `#^` from src/api-isupport.c
	 * (see the patch alongside this module). */

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	bridge_connect(); /* best-effort; pump event will retry */
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	bridge_disconnect();
	safe_free(cfg_bridge_socket);
	return MOD_SUCCESS;
}
