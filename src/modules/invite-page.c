/*
 * obbyircd invite-page module
 *
 * Serves a tiny HTML splash so an obbyircd operator can hand out a
 * shareable HTTPS link that invites a friend to the network or to a
 * specific channel.  The splash's primary button is an `ircs://`
 * deep-link that the user's OS resolves to the registered IRC client
 * handler (e.g. ObsidianIRC's Tauri build registers itself as the
 * `ircs://` opener).  Browsers that don't have a registered handler
 * fall back to the optional `client-url` web build, and otherwise
 * just show the URL.
 *
 * Plumbing pattern: this module piggybacks on the in-tree webserver
 * dispatch by registering a `listen { ... options { invite-page; } }`
 * listen-options block.  Mount it on a dedicated port (typically
 * behind a reverse proxy with TLS) and point your invite shorturl
 * at it.
 *
 * Routes:
 *   GET /              ->  general "join the network" splash
 *   GET /c/<channel>   ->  channel-specific splash (channel name
 *                          minus its #/& prefix; the page reattaches
 *                          # for display + ircs:// link)
 *
 * (C) 2026 Valerie Pond / ObsidianIRC contributors. GPLv2+
 */
#include "unrealircd.h"

ModuleHeader MOD_HEADER = {
	"invite-page",
	"1.0",
	"HTTP splash page that invites users to the network/a channel",
	"obbyircd",
	"unrealircd-6",
};

/* ===================================================================
 * Config
 * =================================================================== */

struct {
	char *network_name;     /* display name, e.g. "ObbyNet" */
	char *irc_host;         /* hostname for ircs:// link */
	int irc_port;           /* port for ircs:// link (typically WSS) */
	int native_port;        /* port shown for "connect manually with a
	                         * traditional client" — TLS-IRC. Falls
	                         * back to irc_port when unset. */
	char *default_channel;  /* optional: auto-prefill the channel form */
	char *accent_color;     /* optional hex accent, defaults to #5865F2 */
} cfg = { 0 };

/* ===================================================================
 * Forwards
 * =================================================================== */

static int invite_configtest_set(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int invite_configrun_set(ConfigFile *cf, ConfigEntry *ce, int type);
static int invite_configtest_listen(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int invite_configrun_listen_ex(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr);
static void invite_client_handshake(Client *client);
static int invite_handle_request(Client *client, WebRequest *web);
static int invite_handle_body(Client *client, WebRequest *web, const char *buf, int len);
static void invite_send_html(Client *client, int status, const char *html);
static void invite_send_redirect(Client *client, const char *target);
static char *build_invite_html(const char *channel /* NULL or "#foo" */);

/* ===================================================================
 * Module lifecycle
 * =================================================================== */
MOD_TEST()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, invite_configtest_set);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, invite_configtest_listen);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, invite_configrun_set);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN_EX, 0, invite_configrun_listen_ex);
	return MOD_SUCCESS;
}

MOD_LOAD() { return MOD_SUCCESS; }

MOD_UNLOAD()
{
	safe_free(cfg.network_name);
	safe_free(cfg.irc_host);
	safe_free(cfg.default_channel);
	safe_free(cfg.accent_color);
	return MOD_SUCCESS;
}

/* ===================================================================
 * Config: set::invite-page { ... }
 * =================================================================== */

static int invite_configtest_set(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	ConfigEntry *cep;
	int errors = 0;

	if (type != CONFIG_SET)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invite-page"))
		return 0;

	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!cep->name)
			continue;
		if (!strcmp(cep->name, "network-name") ||
		    !strcmp(cep->name, "irc-host") ||
		    !strcmp(cep->name, "default-channel") ||
		    !strcmp(cep->name, "accent-color"))
		{
			if (!cep->value || !*cep->value)
			{
				config_error("%s:%i: set::invite-page::%s requires a value",
				             cep->file->filename, cep->line_number, cep->name);
				errors++;
			}
		} else
		if (!strcmp(cep->name, "irc-port") ||
		    !strcmp(cep->name, "native-port"))
		{
			int p = cep->value ? atoi(cep->value) : 0;
			if (p <= 0 || p > 65535)
			{
				config_error("%s:%i: set::invite-page::%s must be 1..65535",
				             cep->file->filename, cep->line_number, cep->name);
				errors++;
			}
		} else
		{
			config_error("%s:%i: unknown directive set::invite-page::%s",
			             cep->file->filename, cep->line_number, cep->name);
			errors++;
		}
	}

	*errs = errors;
	return errors ? -1 : 1;
}

static int invite_configrun_set(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep;

	if (type != CONFIG_SET)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invite-page"))
		return 0;

	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!cep->name || !cep->value)
			continue;
		if (!strcmp(cep->name, "network-name"))
			safe_strdup(cfg.network_name, cep->value);
		else if (!strcmp(cep->name, "irc-host"))
			safe_strdup(cfg.irc_host, cep->value);
		else if (!strcmp(cep->name, "default-channel"))
			safe_strdup(cfg.default_channel, cep->value);
		else if (!strcmp(cep->name, "accent-color"))
			safe_strdup(cfg.accent_color, cep->value);
		else if (!strcmp(cep->name, "irc-port"))
			cfg.irc_port = atoi(cep->value);
		else if (!strcmp(cep->name, "native-port"))
			cfg.native_port = atoi(cep->value);
	}
	return 1;
}

/* ===================================================================
 * Config: listen { ... options { invite-page; } }
 * =================================================================== */

static int invite_configtest_listen(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	if (type != CONFIG_LISTEN_OPTIONS)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invite-page"))
		return 0;
	/* No sub-items expected; this is a marker option. */
	return 1;
}

static int invite_configrun_listen_ex(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr)
{
	ConfigItem_listen *l;

	if (type != CONFIG_LISTEN_OPTIONS)
		return 0;
	if (!ce || !ce->name || strcmp(ce->name, "invite-page"))
		return 0;

	l = (ConfigItem_listen *)ptr;
	if (!l->webserver)
		l->webserver = safe_alloc(sizeof(WebServer));
	l->webserver->handle_request = invite_handle_request;
	l->webserver->handle_body = invite_handle_body;
	/* Replace the default IRC handshake so it doesn't emit the
	 * "*** Looking up your hostname..." / "*** Found your hostname"
	 * NOTICEs into the TCP stream BEFORE the HTTP request comes in.
	 * Those raw bytes land in front of our HTTP/1.1 status line and
	 * curl reports "Received HTTP/0.9 when not allowed".  Browsers
	 * may render them as garbage or simply fail to parse the page.
	 * Same fix pushbot uses for its WS listener. */
	l->start_handshake = invite_client_handshake;
	return 1;
}

static void invite_client_handshake(Client *client)
{
	client->status = CLIENT_STATUS_UNKNOWN;
	RunHook(HOOKTYPE_HANDSHAKE, client);
	if (!IsDead(client))
		fd_setselect(client->local->fd, FD_SELECT_READ, read_packet, client);
}

/* ===================================================================
 * HTTP helpers
 * =================================================================== */

static void invite_send_html(Client *client, int status, const char *html)
{
	char header[512];
	size_t body_len = strlen(html);
	const char *msg =
	    (status == 200) ? "OK" :
	    (status == 404) ? "Not Found" :
	    (status == 400) ? "Bad Request" :
	    (status == 503) ? "Service Unavailable" : "Internal Server Error";

	snprintf(header, sizeof(header),
	         "HTTP/1.1 %d %s\r\n"
	         "Content-Type: text/html; charset=utf-8\r\n"
	         "Content-Length: %zu\r\n"
	         "Cache-Control: no-cache\r\n"
	         "Connection: close\r\n"
	         "X-Content-Type-Options: nosniff\r\n"
	         "Referrer-Policy: no-referrer\r\n"
	         "\r\n",
	         status, msg, body_len);

	dbuf_put(&client->local->sendQ, header, strlen(header));
	dbuf_put(&client->local->sendQ, html, body_len);
	webserver_close_client(client);
}

static void invite_send_redirect(Client *client, const char *target)
{
	char buf[1024];
	snprintf(buf, sizeof(buf),
	         "HTTP/1.1 302 Found\r\n"
	         "Location: %s\r\n"
	         "Content-Length: 0\r\n"
	         "Connection: close\r\n"
	         "\r\n",
	         target);
	dbuf_put(&client->local->sendQ, buf, strlen(buf));
	webserver_close_client(client);
}

/** URL-decode src into dst (NUL-terminated, dst large enough). */
static void url_decode(char *dst, const char *src, size_t maxlen)
{
	size_t di = 0;
	while (*src && di + 1 < maxlen)
	{
		if (src[0] == '%' && isxdigit((unsigned char)src[1]) &&
		    isxdigit((unsigned char)src[2]))
		{
			unsigned int v;
			sscanf(src + 1, "%2x", &v);
			dst[di++] = (char)v;
			src += 3;
		} else if (*src == '+') {
			dst[di++] = ' ';
			src++;
		} else {
			dst[di++] = *src++;
		}
	}
	dst[di] = '\0';
}

/** Minimal HTML escape.  Returns a pointer into a rotating ring of
 * static buffers so it's safe to call multiple times within a single
 * printf-family expression (which evaluates its args in unspecified
 * order).  4 slots is enough for the snprintfs in this module. */
static const char *html_escape(const char *s)
{
	static char rings[4][1024];
	static int slot = 0;
	char *buf = rings[slot];
	size_t i = 0;
	slot = (slot + 1) & 3;
	if (!s) { buf[0] = '\0'; return buf; }
	while (*s && i + 7 < sizeof(rings[0]))
	{
		switch (*s)
		{
			case '<': memcpy(buf+i, "&lt;", 4); i += 4; break;
			case '>': memcpy(buf+i, "&gt;", 4); i += 4; break;
			case '&': memcpy(buf+i, "&amp;", 5); i += 5; break;
			case '"': memcpy(buf+i, "&quot;", 6); i += 6; break;
			case '\'': memcpy(buf+i, "&#x27;", 6); i += 6; break;
			default: buf[i++] = *s; break;
		}
		s++;
	}
	buf[i] = '\0';
	return buf;
}

/* ===================================================================
 * Request handling
 * =================================================================== */

static int invite_handle_request(Client *client, WebRequest *web)
{
	const char *uri = web->uri ? web->uri : "/";
	char path[512];
	char *qmark;

	/* Strip query string if any. */
	strlcpy(path, uri, sizeof(path));
	qmark = strchr(path, '?');
	if (qmark) *qmark = '\0';

	if (!cfg.irc_host || !cfg.irc_port)
	{
		invite_send_html(client, 503,
		    "<!DOCTYPE html><meta charset=utf-8>"
		    "<title>invite-page not configured</title>"
		    "<p>The invite-page module is loaded but "
		    "<code>set::invite-page::irc-host</code> / "
		    "<code>set::invite-page::irc-port</code> are not set.</p>");
		return 0;
	}

	if (!strcmp(path, "/"))
	{
		char *html = build_invite_html(NULL);
		invite_send_html(client, 200, html);
		safe_free(html);
		return 0;
	}

	/* /c/<channel> -- channel-specific invite. The channel name in
	 * the URL has its # / & sigil stripped; we reattach a sensible
	 * default ('#') unless the URL has e.g. /c/%26private for a &
	 * channel.  URL decoding lets the admin link /c/%23weather too. */
	if (!strncmp(path, "/c/", 3) && path[3])
	{
		char decoded[CHANNELLEN + 8];
		char channel[CHANNELLEN + 4];

		url_decode(decoded, path + 3, sizeof(decoded));
		/* Strip any trailing slash. */
		{
			size_t L = strlen(decoded);
			while (L && (decoded[L-1] == '/' || decoded[L-1] == '\n' || decoded[L-1] == '\r'))
				decoded[--L] = '\0';
		}
		if (!*decoded || strchr(decoded, ' ') || strchr(decoded, ',') ||
		    strchr(decoded, '\0'+1))
		{
			invite_send_html(client, 400,
			    "<!DOCTYPE html><meta charset=utf-8>"
			    "<title>Invalid channel</title>"
			    "<p>Channel name is empty or malformed.</p>");
			return 0;
		}

		if (decoded[0] == '#' || decoded[0] == '&' || decoded[0] == '^' ||
		    decoded[0] == '$')
			strlcpy(channel, decoded, sizeof(channel));
		else
			snprintf(channel, sizeof(channel), "#%s", decoded);

		{
			char *html = build_invite_html(channel);
			invite_send_html(client, 200, html);
			safe_free(html);
		}
		return 0;
	}

	/* Anything else 404s — we intentionally don't serve favicons /
	 * /robots.txt etc to avoid stale browser cache surprises. */
	invite_send_html(client, 404,
	    "<!DOCTYPE html><meta charset=utf-8><title>Not found</title>"
	    "<p>Try the <a href=\"/\">main invite page</a>.</p>");
	return 0;
}

static int invite_handle_body(Client *client, WebRequest *web, const char *buf, int len)
{
	/* No POST handling. */
	return 0;
}

/* ===================================================================
 * HTML
 * =================================================================== */

/* Allocate ~4KB into a fresh buffer; caller frees. Argument is the
 * channel including its # / & / ^ / $ prefix, or NULL for the
 * generic "join the network" view. */
static char *build_invite_html(const char *channel)
{
	const char *net = cfg.network_name ? cfg.network_name : "this network";
	const char *host = cfg.irc_host;
	const char *accent = cfg.accent_color ? cfg.accent_color : "#5865F2";
	/* Shared network logo from set::network-icon (draft/ICON
	 * ISUPPORT). Already validated by core to be an https:// URL
	 * without spaces. */
	const char *icon = NETWORK_ICON;
	int port = cfg.irc_port;
	int native_port = cfg.native_port > 0 ? cfg.native_port : cfg.irc_port;
	char ircs_link[256];
	char join_line[CHANNELLEN + 16] = "";
	char *out;
	size_t cap = 8192;
	int n;

	/* Build ircs:// link.  Per ircs://host[:port][/channel] -- the
	 * channel form omits the leading # (some clients accept it with
	 * or without). We URL-encode the # because clients differ. */
	if (channel)
	{
		const char *raw = channel;
		char encoded_chan[CHANNELLEN * 4];
		size_t i = 0;
		const char *p;
		for (p = raw; *p && i + 4 < sizeof(encoded_chan); p++)
		{
			if (*p == '#' || *p == '&' || *p == '^' || *p == '$')
			{
				snprintf(encoded_chan + i, sizeof(encoded_chan) - i,
				         "%%%02X", (unsigned char)*p);
				i += 3;
			} else {
				encoded_chan[i++] = *p;
			}
		}
		encoded_chan[i] = '\0';
		snprintf(ircs_link, sizeof(ircs_link),
		         "ircs://%s:%d/%s", host, port, encoded_chan);
		snprintf(join_line, sizeof(join_line), "/join %s", channel);
	} else {
		snprintf(ircs_link, sizeof(ircs_link),
		         "ircs://%s:%d/", host, port);
	}

	out = safe_alloc(cap);
	n = snprintf(out, cap,
	    "<!DOCTYPE html>\n"
	    "<html lang=\"en\"><head>\n"
	    "<meta charset=\"utf-8\">\n"
	    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
	    "<meta name=\"referrer\" content=\"no-referrer\">\n"
	    "<title>Join %s%s%s</title>\n"
	    "<style>\n"
	    " *{box-sizing:border-box}\n"
	    " body{margin:0;font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:#0d0e10;color:#e8e8ea;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px}\n"
	    " .card{max-width:480px;width:100%%;background:#181a1f;border-radius:16px;padding:36px 32px;box-shadow:0 20px 60px rgba(0,0,0,.4);border-top:3px solid %s;text-align:center}\n"
	    " h1{margin:0 0 8px;font-size:24px;font-weight:600}\n"
	    " h1 .net{color:%s}\n"
	    " .sub{color:#9a9aa3;margin:0 0 28px;font-size:14px;line-height:1.5}\n"
	    " .chan{display:inline-flex;align-items:center;gap:6px;background:#222530;color:#fff;font-family:ui-monospace,'SF Mono',Menlo,monospace;font-size:14px;padding:4px 10px;border-radius:8px;margin:0 4px}\n"
	    " .icon{width:72px;height:72px;border-radius:16px;display:block;margin:0 auto 16px;object-fit:cover;background:#222530;box-shadow:0 4px 12px rgba(0,0,0,.3)}\n"
	    " .btn{display:block;width:100%%;text-align:center;background:%s;color:#fff;text-decoration:none;font-weight:600;font-size:16px;padding:14px 18px;border-radius:10px;margin:0 0 12px;transition:filter .15s}\n"
	    " .btn:hover{filter:brightness(1.1)}\n"
	    " details{margin-top:18px;background:#11131a;border-radius:10px;border:1px solid #232631;text-align:left}\n"
	    " summary{cursor:pointer;list-style:none;padding:12px 16px;font-size:13px;color:#a9aab2;user-select:none}\n"
	    " summary::-webkit-details-marker{display:none}\n"
	    " summary::after{content:'\\203A';float:right;color:#5d5e66;transition:transform .15s}\n"
	    " details[open] summary::after{transform:rotate(90deg)}\n"
	    " summary:hover{color:#fff}\n"
	    " .manual{padding:4px 16px 14px;font-size:13px}\n"
	    " .manual dl{margin:0;display:grid;grid-template-columns:max-content 1fr;gap:6px 14px}\n"
	    " .manual dt{color:#6b6c75;font-size:11px;text-transform:uppercase;letter-spacing:.05em;align-self:center}\n"
	    " .manual dd{margin:0;color:#e8e8ea;font-family:ui-monospace,'SF Mono',Menlo,monospace;font-size:13px;word-break:break-all}\n"
	    " .manual .note{margin-top:12px;color:#6b6c75;font-family:inherit;font-size:12px;line-height:1.5}\n"
	    " .footer{margin-top:24px;text-align:center;font-size:11px;color:#5d5e66}\n"
	    "</style>\n"
	    "</head><body>\n"
	    "<div class=\"card\">\n",
	    /* title */ html_escape(net),
	    channel ? " &middot; " : "",
	    channel ? html_escape(channel) : "",
	    accent, accent, accent);

	if (n < 0 || (size_t)n >= cap) return out;

	/* Network icon (set::network-icon).  Rendered above the heading
	 * when configured; omitted otherwise so the card layout stays
	 * tight.  Server-validated to be https:// + no spaces, but we
	 * still HTML-escape defensively. */
	if (icon && *icon)
	{
		n += snprintf(out + n, cap - n,
		    "<img class=\"icon\" src=\"%s\" alt=\"%s\" referrerpolicy=\"no-referrer\">\n",
		    html_escape(icon), html_escape(net));
	}

	n += snprintf(out + n, cap - n,
	    "<h1>You're invited to <span class=\"net\">%s</span></h1>\n",
	    html_escape(net));

	if (channel)
	{
		n += snprintf(out + n, cap - n,
		    "<p class=\"sub\">Open the channel <span class=\"chan\">%s</span> in your IRC client to join the conversation.</p>\n",
		    html_escape(channel));
	} else {
		n += snprintf(out + n, cap - n,
		    "<p class=\"sub\">Connect to <code>%s</code> in your IRC client.</p>\n",
		    html_escape(host));
	}

	/* Primary button: triggers the ircs:// handler ONLY on click.
	 * No auto-redirect on load -- the user opted in to follow the
	 * invite link, but the IRC client launch is a separate explicit
	 * action so e.g. previewing the invite in a browser tab doesn't
	 * yank focus to the desktop IRC app. */
	n += snprintf(out + n, cap - n,
	    "<a class=\"btn\" href=\"%s\">Open in IRC client</a>\n",
	    ircs_link);

	/* Manual-connect details: collapsed by default. Shows everything
	 * a user would need to connect with another IRC client of their
	 * choice (HexChat, irssi, weechat, etc.). */
	n += snprintf(out + n, cap - n,
	    "<details>\n"
	    "<summary>Connect manually</summary>\n"
	    "<div class=\"manual\">\n"
	    "<dl>\n"
	    "<dt>Server</dt><dd>%s</dd>\n"
	    "<dt>Port</dt><dd>%d</dd>\n"
	    "<dt>TLS</dt><dd>Required</dd>\n",
	    html_escape(host), native_port);

	if (channel)
	{
		n += snprintf(out + n, cap - n,
		    "<dt>Channel</dt><dd>%s</dd>\n"
		    "<dt>Join</dt><dd>%s</dd>\n",
		    html_escape(channel), html_escape(join_line));
	}

	n += snprintf(out + n, cap - n,
	    "</dl>\n"
	    "<p class=\"note\">Point your client at <code>%s</code> on port %d with TLS. "
	    "Once connected%s, you're in.</p>\n"
	    "</div>\n"
	    "</details>\n"
	    "<div class=\"footer\">powered by obbyircd</div>\n"
	    "</div>\n"
	    "</body></html>\n",
	    html_escape(host), native_port,
	    channel ? ", run the join command above" : "");

	return out;
}
