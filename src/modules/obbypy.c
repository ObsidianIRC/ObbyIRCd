/*
 * obbypy -- server-side Python scripting for ObbyIRCd
 * (C) 2026 Valerie / ObbyIRCd
 * License: GPLv3 or later
 *
 * Mirrors obbyscript (JS via Duktape) but embeds CPython.  Drops .py
 * files in <conf>/scripts/python/ and they're imported on load.  The
 * `obby` module is preinstalled into every script and exposes the
 * hooks / commands / IRC primitives a script needs to do useful work.
 *
 * MVP surface (extend as needed):
 *
 *   obby.register_hook(name, fn)       hooks below
 *   obby.register_command(name, fn,    server-side IRC command
 *                         params=1)
 *
 *   obby.send_notice(target, msg)      NOTICE
 *   obby.send_msg(target, msg)         PRIVMSG
 *   obby.send_raw(client, line)        :server LINE -> client
 *   obby.log(msg, level='info')        unreal_log
 *
 *   obby.find_client(nick)             -> Client or None
 *   obby.find_channel(name)            -> Channel or None
 *   obby.is_user(c) / is_server(c)     basic predicates
 *   obby.is_oper(c) / is_logged_in(c)
 *
 *   Client.name, .host, .ip, .account, .realname, .umodes, .id, .info
 *   Channel.name, .topic, .modes, .users (list[str])
 *
 *   Hook names accepted (the obvious uppercase short forms):
 *     LOCAL_CONNECT, LOCAL_QUIT, REMOTE_QUIT, LOCAL_JOIN,
 *     LOCAL_PART, LOCAL_KICK, CHANMSG, USERMSG, NICK_CHANGE,
 *     WELCOME
 *
 * Hook handler signature:
 *   def handler(event): ...
 * where `event` is a dict carrying whichever of these are relevant
 * to the hook: client, channel, target, msg, mtags, reason, oldnick.
 * Return an int (HOOK_CONTINUE = 0, HOOK_DENY = 1); None == 0.
 *
 * Command handler signature:
 *   def handler(client, params): ...
 * where `params` is list[str] of the trailing args (parv[1..]).
 *
 * Co-exists with obbyscript -- load both modules and you get JS + Py.
 */

/* Python.h MUST come before any other includes (per Python docs):
 * Py_PYTHON_H pre-defines some macros that collide with libc/ctype.h
 * otherwise -- you'll get cryptic "expected ']'" errors inside
 * common.h's ctype-using macros if anything else gets in first. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "unrealircd.h"
#include <dirent.h>
#include <limits.h>

ModuleHeader MOD_HEADER = {
	"obbypy",
	"0.1",
	"Server-side Python scripting (embedded CPython)",
	"Valerie",
	"unrealircd-6"
};

#define MYCONF "obbypy"
#define DEFAULT_SCRIPTS_DIR "scripts/python"

/* ===================================================================
 * Internal state
 * =================================================================== */

typedef struct PyHook PyHook;
struct PyHook {
	PyHook *prev, *next;
	int hooktype;
	PyObject *fn;       /* owned ref */
	char *script_name;  /* for diagnostics */
};

typedef struct PyCmd PyCmd;
struct PyCmd {
	PyCmd *prev, *next;
	char *name;
	int params;
	PyObject *fn;       /* owned ref */
	Command *cmd;       /* CommandAdd handle */
	char *script_name;
};

static PyHook *registered_hooks = NULL;
static PyCmd *registered_cmds = NULL;
static int hooks_added[256] = {0};  /* dedupe HookAdd per hooktype */

static struct {
	char *scripts_dir;
} cfg;

static ModuleInfo *modinfo_ref = NULL;

/* ===================================================================
 * Forward declarations
 * =================================================================== */

static int obbypy_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
static int obbypy_configrun(ConfigFile *cf, ConfigEntry *ce, int type);
static void load_scripts(void);
static void cleanup_scripts(void);
static int hook_name_to_type(const char *name);
static PyObject *py_client_new(Client *c);
static PyObject *py_channel_new(Channel *c);
static PyObject *mtags_to_dict(MessageTag *mtags);
static PyObject *parv_to_list(const char *parv[], int parc, int start);
static Client *unwrap_client_arg(PyObject *o);
static int call_handler_with_event(PyObject *fn, PyObject *event);
static const char *script_label(PyObject *fn);

/* Hook trampolines */
static int hook_local_connect(Client *client);
static int hook_local_quit(Client *client, MessageTag *mtags, const char *comment);
static int hook_remote_quit(Client *client, MessageTag *mtags, const char *comment);
static int hook_local_join(Client *client, Channel *channel, MessageTag *mtags);
static int hook_local_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment);
static int hook_local_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment);
static int hook_chanmsg(Client *client, Channel *channel, int sendflags, const char *prefix, const char *target,
                       MessageTag *mtags, const char *text, SendType sendtype);
static int hook_usermsg(Client *client, Client *to, MessageTag *mtags, const char *text, SendType sendtype);
static int hook_local_nickchange(Client *client, MessageTag *mtags, const char *newnick);
static int hook_welcome(Client *client, int after_authenticated);

/* Command dispatcher (single function used for all registered commands;
 * looks up the PyCmd by name). */
CMD_FUNC(cmd_py_dispatch);

/* ===================================================================
 * Client / Channel Python wrapper types
 * =================================================================== */

typedef struct {
	PyObject_HEAD
	Client *c;
} PyClientObject;

typedef struct {
	PyObject_HEAD
	Channel *ch;
} PyChannelObject;

static PyObject *PyClient_getattro(PyObject *self, PyObject *name_obj)
{
	PyClientObject *o = (PyClientObject *)self;
	const char *name = PyUnicode_AsUTF8(name_obj);
	if (!name)
		return NULL;
	if (!o->c)
		Py_RETURN_NONE;

	if (!strcmp(name, "name"))
		return PyUnicode_FromString(o->c->name ? o->c->name : "");
	if (!strcmp(name, "id"))
		return PyUnicode_FromString(o->c->id ? o->c->id : "");
	if (!strcmp(name, "info"))
		return PyUnicode_FromString(o->c->info ? o->c->info : "");
	if (!strcmp(name, "host")) {
		const char *h = NULL;
		if (o->c->user)
			h = o->c->user->virthost ? o->c->user->virthost : o->c->user->realhost;
		if (!h)
			h = GetHost(o->c);
		return PyUnicode_FromString(h ? h : "");
	}
	if (!strcmp(name, "realhost"))
		return PyUnicode_FromString(o->c->user && o->c->user->realhost ? o->c->user->realhost : "");
	if (!strcmp(name, "ip"))
		return PyUnicode_FromString(o->c->ip ? o->c->ip : "");
	if (!strcmp(name, "account")) {
		if (o->c->user && o->c->user->account && strcmp(o->c->user->account, "0"))
			return PyUnicode_FromString(o->c->user->account);
		Py_RETURN_NONE;
	}
	if (!strcmp(name, "realname")) {
		if (o->c->user)
			return PyUnicode_FromString(o->c->info ? o->c->info : "");
		Py_RETURN_NONE;
	}
	if (!strcmp(name, "umodes")) {
		if (!o->c->user)
			return PyUnicode_FromString("");
		char buf[64];
		get_usermode_string_r(o->c, buf, sizeof(buf));
		return PyUnicode_FromString(buf);
	}
	if (!strcmp(name, "is_user"))
		return PyBool_FromLong(IsUser(o->c) ? 1 : 0);
	if (!strcmp(name, "is_server"))
		return PyBool_FromLong(IsServer(o->c) ? 1 : 0);
	if (!strcmp(name, "is_oper"))
		return PyBool_FromLong(IsOper(o->c) ? 1 : 0);
	if (!strcmp(name, "is_secure"))
		return PyBool_FromLong(IsSecure(o->c) ? 1 : 0);
	if (!strcmp(name, "is_logged_in"))
		return PyBool_FromLong((o->c->user && IsLoggedIn(o->c)) ? 1 : 0);

	/* Fallthrough to default object attribute lookup so dunders work. */
	return PyObject_GenericGetAttr(self, name_obj);
}

static PyObject *PyClient_repr(PyObject *self)
{
	PyClientObject *o = (PyClientObject *)self;
	if (!o->c)
		return PyUnicode_FromString("<Client (gone)>");
	return PyUnicode_FromFormat("<Client %s>", o->c->name ? o->c->name : "?");
}

static PyTypeObject PyClient_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "obby.Client",
	.tp_basicsize = sizeof(PyClientObject),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_getattro = PyClient_getattro,
	.tp_repr = PyClient_repr,
};

static PyObject *py_client_new(Client *c)
{
	if (!c) Py_RETURN_NONE;
	PyClientObject *o = PyObject_New(PyClientObject, &PyClient_Type);
	if (!o) return NULL;
	o->c = c;
	return (PyObject *)o;
}

static PyObject *PyChannel_getattro(PyObject *self, PyObject *name_obj)
{
	PyChannelObject *o = (PyChannelObject *)self;
	const char *name = PyUnicode_AsUTF8(name_obj);
	if (!name)
		return NULL;
	if (!o->ch)
		Py_RETURN_NONE;

	if (!strcmp(name, "name"))
		return PyUnicode_FromString(o->ch->name ? o->ch->name : "");
	if (!strcmp(name, "topic"))
		return PyUnicode_FromString(o->ch->topic ? o->ch->topic : "");
	if (!strcmp(name, "modes")) {
		char mbuf[128], pbuf[256];
		channel_modes(&me, mbuf, pbuf, sizeof(mbuf), sizeof(pbuf), o->ch, 0);
		if (pbuf[0]) {
			char both[512];
			snprintf(both, sizeof(both), "%s %s", mbuf, pbuf);
			return PyUnicode_FromString(both);
		}
		return PyUnicode_FromString(mbuf);
	}
	if (!strcmp(name, "users")) {
		PyObject *list = PyList_New(0);
		if (!list) return NULL;
		Member *m;
		for (m = o->ch->members; m; m = m->next) {
			PyObject *n = PyUnicode_FromString(m->client->name ? m->client->name : "");
			if (n) {
				PyList_Append(list, n);
				Py_DECREF(n);
			}
		}
		return list;
	}
	if (!strcmp(name, "users_count")) {
		int count = 0;
		Member *m;
		for (m = o->ch->members; m; m = m->next) count++;
		return PyLong_FromLong(count);
	}

	return PyObject_GenericGetAttr(self, name_obj);
}

static PyObject *PyChannel_repr(PyObject *self)
{
	PyChannelObject *o = (PyChannelObject *)self;
	if (!o->ch)
		return PyUnicode_FromString("<Channel (gone)>");
	return PyUnicode_FromFormat("<Channel %s>", o->ch->name ? o->ch->name : "?");
}

static PyTypeObject PyChannel_Type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "obby.Channel",
	.tp_basicsize = sizeof(PyChannelObject),
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_getattro = PyChannel_getattro,
	.tp_repr = PyChannel_repr,
};

static PyObject *py_channel_new(Channel *c)
{
	if (!c) Py_RETURN_NONE;
	PyChannelObject *o = PyObject_New(PyChannelObject, &PyChannel_Type);
	if (!o) return NULL;
	o->ch = c;
	return (PyObject *)o;
}

/* Resolve a Python arg to a Client*: either a Client wrapper or a
 * string (nick) that we'll find_user(). Returns NULL on miss; raises
 * TypeError if the arg is the wrong shape. */
static Client *unwrap_client_arg(PyObject *o)
{
	if (!o || o == Py_None)
		return NULL;
	if (PyObject_TypeCheck(o, &PyClient_Type))
		return ((PyClientObject *)o)->c;
	if (PyUnicode_Check(o)) {
		const char *s = PyUnicode_AsUTF8(o);
		return s ? find_user(s, NULL) : NULL;
	}
	PyErr_SetString(PyExc_TypeError, "expected Client or nick string");
	return NULL;
}

/* Same shape but for channel targets: Channel wrapper, name string,
 * or fall-through. */
static Channel *unwrap_channel_arg(PyObject *o)
{
	if (!o || o == Py_None)
		return NULL;
	if (PyObject_TypeCheck(o, &PyChannel_Type))
		return ((PyChannelObject *)o)->ch;
	if (PyUnicode_Check(o)) {
		const char *s = PyUnicode_AsUTF8(o);
		return s ? find_channel(s) : NULL;
	}
	return NULL;
}

/* ===================================================================
 * Built-in `obby` module functions
 * =================================================================== */

static PyObject *api_log(PyObject *self, PyObject *args, PyObject *kw)
{
	const char *msg = NULL;
	const char *level = "info";
	static char *kwlist[] = {"msg", "level", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kw, "s|s", kwlist, &msg, &level))
		return NULL;
	int lv = ULOG_INFO;
	if (!strcasecmp(level, "debug")) lv = ULOG_DEBUG;
	else if (!strcasecmp(level, "warn") || !strcasecmp(level, "warning")) lv = ULOG_WARNING;
	else if (!strcasecmp(level, "error")) lv = ULOG_ERROR;
	unreal_log(lv, "obbypy", "PY_LOG", NULL, "$msg", log_data_string("msg", msg));
	Py_RETURN_NONE;
}

static PyObject *api_send_notice(PyObject *self, PyObject *args)
{
	PyObject *target_obj;
	const char *msg;
	if (!PyArg_ParseTuple(args, "Os", &target_obj, &msg))
		return NULL;
	Client *c = unwrap_client_arg(target_obj);
	if (PyErr_Occurred()) return NULL;
	if (!c) {
		PyErr_SetString(PyExc_LookupError, "target client not found");
		return NULL;
	}
	sendnotice(c, "%s", msg);
	Py_RETURN_NONE;
}

static PyObject *api_send_msg(PyObject *self, PyObject *args)
{
	PyObject *target_obj;
	const char *msg;
	if (!PyArg_ParseTuple(args, "Os", &target_obj, &msg))
		return NULL;

	Client *c = NULL;
	Channel *ch = NULL;
	if (PyUnicode_Check(target_obj)) {
		const char *s = PyUnicode_AsUTF8(target_obj);
		if (s && s[0] == '#')
			ch = find_channel(s);
		else if (s)
			c = find_user(s, NULL);
	} else if (PyObject_TypeCheck(target_obj, &PyClient_Type)) {
		c = ((PyClientObject *)target_obj)->c;
	} else if (PyObject_TypeCheck(target_obj, &PyChannel_Type)) {
		ch = ((PyChannelObject *)target_obj)->ch;
	}

	if (ch) {
		sendto_channel(ch, &me, NULL, 0, 0, SEND_LOCAL, NULL,
		               "PRIVMSG %s :%s", ch->name, msg);
		Py_RETURN_NONE;
	}
	if (c) {
		sendto_one(c, NULL, ":%s PRIVMSG %s :%s",
		           me.name, c->name, msg);
		Py_RETURN_NONE;
	}
	PyErr_SetString(PyExc_LookupError, "target not found");
	return NULL;
}

static PyObject *api_send_raw(PyObject *self, PyObject *args)
{
	PyObject *target_obj;
	const char *line;
	if (!PyArg_ParseTuple(args, "Os", &target_obj, &line))
		return NULL;
	Client *c = unwrap_client_arg(target_obj);
	if (PyErr_Occurred()) return NULL;
	if (!c) {
		PyErr_SetString(PyExc_LookupError, "target client not found");
		return NULL;
	}
	sendto_one(c, NULL, "%s", line);
	Py_RETURN_NONE;
}

static PyObject *api_find_client(PyObject *self, PyObject *args)
{
	const char *name;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	Client *c = find_user(name, NULL);
	return py_client_new(c);
}

static PyObject *api_find_channel(PyObject *self, PyObject *args)
{
	const char *name;
	if (!PyArg_ParseTuple(args, "s", &name))
		return NULL;
	Channel *ch = find_channel(name);
	return py_channel_new(ch);
}

static PyObject *api_is_oper(PyObject *self, PyObject *args)
{
	PyObject *o;
	if (!PyArg_ParseTuple(args, "O", &o)) return NULL;
	Client *c = unwrap_client_arg(o);
	if (PyErr_Occurred()) return NULL;
	return PyBool_FromLong(c && IsOper(c) ? 1 : 0);
}

static PyObject *api_is_user(PyObject *self, PyObject *args)
{
	PyObject *o;
	if (!PyArg_ParseTuple(args, "O", &o)) return NULL;
	Client *c = unwrap_client_arg(o);
	if (PyErr_Occurred()) return NULL;
	return PyBool_FromLong(c && IsUser(c) ? 1 : 0);
}

static PyObject *api_is_server(PyObject *self, PyObject *args)
{
	PyObject *o;
	if (!PyArg_ParseTuple(args, "O", &o)) return NULL;
	Client *c = unwrap_client_arg(o);
	if (PyErr_Occurred()) return NULL;
	return PyBool_FromLong(c && IsServer(c) ? 1 : 0);
}

static PyObject *api_is_logged_in(PyObject *self, PyObject *args)
{
	PyObject *o;
	if (!PyArg_ParseTuple(args, "O", &o)) return NULL;
	Client *c = unwrap_client_arg(o);
	if (PyErr_Occurred()) return NULL;
	return PyBool_FromLong(c && c->user && IsLoggedIn(c) ? 1 : 0);
}

static PyObject *api_register_hook(PyObject *self, PyObject *args)
{
	const char *name;
	PyObject *fn;
	if (!PyArg_ParseTuple(args, "sO", &name, &fn))
		return NULL;
	if (!PyCallable_Check(fn)) {
		PyErr_SetString(PyExc_TypeError, "handler must be callable");
		return NULL;
	}
	int hooktype = hook_name_to_type(name);
	if (hooktype < 0) {
		PyErr_Format(PyExc_ValueError, "unknown hook name: %s", name);
		return NULL;
	}

	PyHook *h = safe_alloc(sizeof(*h));
	Py_INCREF(fn);
	h->fn = fn;
	h->hooktype = hooktype;
	h->script_name = strdup(script_label(fn));
	AddListItem(h, registered_hooks);

	/* Add the C trampoline at most once per hook type. */
	if (!hooks_added[hooktype % 256]) {
		switch (hooktype) {
		case HOOKTYPE_LOCAL_CONNECT:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_CONNECT, 0, hook_local_connect); break;
		case HOOKTYPE_LOCAL_QUIT:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_QUIT, 0, hook_local_quit); break;
		case HOOKTYPE_REMOTE_QUIT:
			HookAdd(modinfo_ref->handle, HOOKTYPE_REMOTE_QUIT, 0, hook_remote_quit); break;
		case HOOKTYPE_LOCAL_JOIN:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_JOIN, 0, hook_local_join); break;
		case HOOKTYPE_LOCAL_PART:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_PART, 0, hook_local_part); break;
		case HOOKTYPE_LOCAL_KICK:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_KICK, 0, hook_local_kick); break;
		case HOOKTYPE_CHANMSG:
			HookAdd(modinfo_ref->handle, HOOKTYPE_CHANMSG, 0, hook_chanmsg); break;
		case HOOKTYPE_USERMSG:
			HookAdd(modinfo_ref->handle, HOOKTYPE_USERMSG, 0, hook_usermsg); break;
		case HOOKTYPE_LOCAL_NICKCHANGE:
			HookAdd(modinfo_ref->handle, HOOKTYPE_LOCAL_NICKCHANGE, 0, hook_local_nickchange); break;
		case HOOKTYPE_WELCOME:
			HookAdd(modinfo_ref->handle, HOOKTYPE_WELCOME, 0, hook_welcome); break;
		}
		hooks_added[hooktype % 256] = 1;
	}
	Py_RETURN_NONE;
}

static PyObject *api_register_command(PyObject *self, PyObject *args, PyObject *kw)
{
	const char *name;
	PyObject *fn;
	int params = 1;
	static char *kwlist[] = {"name", "fn", "params", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|i", kwlist,
	                                 &name, &fn, &params))
		return NULL;
	if (!PyCallable_Check(fn)) {
		PyErr_SetString(PyExc_TypeError, "handler must be callable");
		return NULL;
	}
	/* Refuse duplicate names from Python; UnrealIRCd would refuse too
	 * but we'd leak the safe_alloc. */
	for (PyCmd *p = registered_cmds; p; p = p->next) {
		if (!strcasecmp(p->name, name)) {
			PyErr_Format(PyExc_ValueError,
			             "command %s already registered", name);
			return NULL;
		}
	}

	PyCmd *c = safe_alloc(sizeof(*c));
	c->name = strdup(name);
	c->params = params;
	Py_INCREF(fn);
	c->fn = fn;
	c->script_name = strdup(script_label(fn));
	c->cmd = CommandAdd(modinfo_ref->handle, name, cmd_py_dispatch,
	                    (unsigned char)params, CMD_USER);
	if (!c->cmd) {
		Py_DECREF(c->fn);
		safe_free(c->name);
		safe_free(c->script_name);
		safe_free(c);
		PyErr_Format(PyExc_RuntimeError,
		             "CommandAdd failed for %s", name);
		return NULL;
	}
	AddListItem(c, registered_cmds);
	Py_RETURN_NONE;
}

static PyMethodDef obby_methods[] = {
	{"log",              (PyCFunction)api_log,             METH_VARARGS | METH_KEYWORDS,
	 "Write a message to the IRCd log. level= debug|info|warn|error"},
	{"send_notice",      api_send_notice,     METH_VARARGS,
	 "Send a NOTICE to a Client or nick."},
	{"send_msg",         api_send_msg,        METH_VARARGS,
	 "Send a PRIVMSG to a Client, Channel, nick, or '#chan'."},
	{"send_raw",         api_send_raw,        METH_VARARGS,
	 "Send a raw IRC line to a client (already-formatted, no ':server' prefix needed)."},
	{"find_client",      api_find_client,     METH_VARARGS,
	 "Look up a Client by nick. Returns None if not found."},
	{"find_channel",     api_find_channel,    METH_VARARGS,
	 "Look up a Channel by name. Returns None if not found."},
	{"is_oper",          api_is_oper,         METH_VARARGS,    "Check IRC operator status."},
	{"is_user",          api_is_user,         METH_VARARGS,    "Is the given Client a registered user?"},
	{"is_server",        api_is_server,       METH_VARARGS,    "Is the given Client a server peer?"},
	{"is_logged_in",     api_is_logged_in,    METH_VARARGS,    "Is the given Client logged into a services account?"},
	{"register_hook",    api_register_hook,   METH_VARARGS,
	 "Register a hook: obby.register_hook(name, fn). Names: LOCAL_CONNECT, LOCAL_QUIT, REMOTE_QUIT, LOCAL_JOIN, LOCAL_PART, LOCAL_KICK, CHANMSG, USERMSG, LOCAL_NICKCHANGE, WELCOME."},
	{"register_command", (PyCFunction)api_register_command, METH_VARARGS | METH_KEYWORDS,
	 "Register a user command. obby.register_command(name, fn, params=1)."},
	{NULL, NULL, 0, NULL}
};

static struct PyModuleDef obby_module_def = {
	PyModuleDef_HEAD_INIT,
	"obby",
	"ObbyIRCd scripting API",
	-1,
	obby_methods
};

static PyObject *PyInit_obby(void)
{
	if (PyType_Ready(&PyClient_Type) < 0) return NULL;
	if (PyType_Ready(&PyChannel_Type) < 0) return NULL;
	PyObject *m = PyModule_Create(&obby_module_def);
	if (!m) return NULL;
	Py_INCREF(&PyClient_Type);
	PyModule_AddObject(m, "Client", (PyObject *)&PyClient_Type);
	Py_INCREF(&PyChannel_Type);
	PyModule_AddObject(m, "Channel", (PyObject *)&PyChannel_Type);
	return m;
}

/* ===================================================================
 * Hook-name -> hooktype table
 * =================================================================== */

static struct {
	const char *name;
	int type;
} hook_table[] = {
	{"LOCAL_CONNECT",    HOOKTYPE_LOCAL_CONNECT},
	{"LOCAL_QUIT",       HOOKTYPE_LOCAL_QUIT},
	{"REMOTE_QUIT",      HOOKTYPE_REMOTE_QUIT},
	{"LOCAL_JOIN",       HOOKTYPE_LOCAL_JOIN},
	{"LOCAL_PART",       HOOKTYPE_LOCAL_PART},
	{"LOCAL_KICK",       HOOKTYPE_LOCAL_KICK},
	{"CHANMSG",          HOOKTYPE_CHANMSG},
	{"USERMSG",          HOOKTYPE_USERMSG},
	{"LOCAL_NICKCHANGE", HOOKTYPE_LOCAL_NICKCHANGE},
	{"NICK_CHANGE",      HOOKTYPE_LOCAL_NICKCHANGE},
	{"WELCOME",          HOOKTYPE_WELCOME},
	{NULL, 0}
};

static int hook_name_to_type(const char *name)
{
	for (int i = 0; hook_table[i].name; i++)
		if (!strcasecmp(hook_table[i].name, name))
			return hook_table[i].type;
	return -1;
}

/* ===================================================================
 * Event-dict + handler dispatch
 * =================================================================== */

static PyObject *mtags_to_dict(MessageTag *mtags)
{
	PyObject *d = PyDict_New();
	if (!d) return NULL;
	for (MessageTag *m = mtags; m; m = m->next) {
		if (!m->name) continue;
		PyObject *v = PyUnicode_FromString(m->value ? m->value : "");
		if (v) {
			PyDict_SetItemString(d, m->name, v);
			Py_DECREF(v);
		}
	}
	return d;
}

static PyObject *parv_to_list(const char *parv[], int parc, int start)
{
	PyObject *l = PyList_New(0);
	if (!l) return l;
	for (int i = start; i < parc; i++) {
		if (!parv[i]) continue;
		PyObject *s = PyUnicode_FromString(parv[i]);
		if (s) {
			PyList_Append(l, s);
			Py_DECREF(s);
		}
	}
	return l;
}

static const char *script_label(PyObject *fn)
{
	PyObject *mod = PyObject_GetAttrString(fn, "__module__");
	const char *name = "?";
	if (mod && PyUnicode_Check(mod))
		name = PyUnicode_AsUTF8(mod);
	if (mod) Py_DECREF(mod);
	return name;
}

/* Calls fn(event). Returns the int the script returned (None -> 0).
 * On exception, logs and returns 0 (HOOK_CONTINUE). */
static int call_handler_with_event(PyObject *fn, PyObject *event)
{
	PyObject *args = PyTuple_Pack(1, event ? event : Py_None);
	if (!args) {
		PyErr_Print();
		return 0;
	}
	PyObject *rv = PyObject_Call(fn, args, NULL);
	Py_DECREF(args);
	if (!rv) {
		PyObject *type, *val, *tb;
		PyErr_Fetch(&type, &val, &tb);
		PyErr_NormalizeException(&type, &val, &tb);
		PyObject *vstr = val ? PyObject_Str(val) : NULL;
		const char *vs = vstr ? PyUnicode_AsUTF8(vstr) : "";
		unreal_log(ULOG_ERROR, "obbypy", "PY_HOOK_ERROR", NULL,
		           "Python hook raised: $err",
		           log_data_string("err", vs ? vs : "?"));
		Py_XDECREF(vstr);
		Py_XDECREF(type);
		Py_XDECREF(val);
		Py_XDECREF(tb);
		return 0;
	}
	int r = 0;
	if (PyLong_Check(rv))
		r = (int)PyLong_AsLong(rv);
	else if (PyBool_Check(rv))
		r = (rv == Py_True) ? 1 : 0;
	Py_DECREF(rv);
	return r;
}

/* Walk the hook list and dispatch to every Python handler for `htype`.
 * `event` is the prebuilt dict (we Py_DECREF it once at the end). */
static int dispatch_hook(int htype, PyObject *event)
{
	int rv = 0;
	for (PyHook *h = registered_hooks; h; h = h->next) {
		if (h->hooktype != htype) continue;
		int r = call_handler_with_event(h->fn, event);
		if (r) rv = r;
	}
	Py_XDECREF(event);
	return rv;
}

/* ===================================================================
 * Hook trampolines (C -> Python)
 * =================================================================== */

static int hook_local_connect(Client *client)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	return dispatch_hook(HOOKTYPE_LOCAL_CONNECT, e);
}

static int hook_local_quit(Client *client, MessageTag *mtags, const char *comment)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "reason", PyUnicode_FromString(comment ? comment : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_LOCAL_QUIT, e);
}

static int hook_remote_quit(Client *client, MessageTag *mtags, const char *comment)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "reason", PyUnicode_FromString(comment ? comment : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_REMOTE_QUIT, e);
}

static int hook_local_join(Client *client, Channel *channel, MessageTag *mtags)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "channel", py_channel_new(channel));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_LOCAL_JOIN, e);
}

static int hook_local_part(Client *client, Channel *channel, MessageTag *mtags, const char *comment)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "channel", py_channel_new(channel));
	PyDict_SetItemString(e, "reason", PyUnicode_FromString(comment ? comment : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_LOCAL_PART, e);
}

static int hook_local_kick(Client *client, Client *victim, Channel *channel, MessageTag *mtags, const char *comment)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "victim", py_client_new(victim));
	PyDict_SetItemString(e, "channel", py_channel_new(channel));
	PyDict_SetItemString(e, "reason", PyUnicode_FromString(comment ? comment : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_LOCAL_KICK, e);
}

static int hook_chanmsg(Client *client, Channel *channel, int sendflags, const char *prefix, const char *target,
                       MessageTag *mtags, const char *text, SendType sendtype)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "channel", py_channel_new(channel));
	PyDict_SetItemString(e, "msg", PyUnicode_FromString(text ? text : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	PyDict_SetItemString(e, "is_notice",
	                     PyBool_FromLong(sendtype == SEND_TYPE_NOTICE ? 1 : 0));
	PyDict_SetItemString(e, "is_tagmsg",
	                     PyBool_FromLong(sendtype == SEND_TYPE_TAGMSG ? 1 : 0));
	return dispatch_hook(HOOKTYPE_CHANMSG, e);
}

static int hook_usermsg(Client *client, Client *to, MessageTag *mtags, const char *text, SendType sendtype)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "target", py_client_new(to));
	PyDict_SetItemString(e, "msg", PyUnicode_FromString(text ? text : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	PyDict_SetItemString(e, "is_notice",
	                     PyBool_FromLong(sendtype == SEND_TYPE_NOTICE ? 1 : 0));
	PyDict_SetItemString(e, "is_tagmsg",
	                     PyBool_FromLong(sendtype == SEND_TYPE_TAGMSG ? 1 : 0));
	return dispatch_hook(HOOKTYPE_USERMSG, e);
}

static int hook_local_nickchange(Client *client, MessageTag *mtags, const char *newnick)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "oldnick", PyUnicode_FromString(client && client->name ? client->name : ""));
	PyDict_SetItemString(e, "newnick", PyUnicode_FromString(newnick ? newnick : ""));
	PyDict_SetItemString(e, "mtags", mtags_to_dict(mtags));
	return dispatch_hook(HOOKTYPE_LOCAL_NICKCHANGE, e);
}

static int hook_welcome(Client *client, int after_authenticated)
{
	PyObject *e = PyDict_New();
	if (!e) return 0;
	PyDict_SetItemString(e, "client", py_client_new(client));
	PyDict_SetItemString(e, "after_authenticated", PyBool_FromLong(after_authenticated ? 1 : 0));
	return dispatch_hook(HOOKTYPE_WELCOME, e);
}

/* ===================================================================
 * Command dispatcher
 * =================================================================== */

CMD_FUNC(cmd_py_dispatch)
{
	/* parv[0] is the command name. Walk the registered list. */
	const char *cmdname = NULL;
	if (parc > 0 && parv[0])
		cmdname = parv[0];
	if (!cmdname) return;

	PyCmd *pc = NULL;
	for (PyCmd *p = registered_cmds; p; p = p->next) {
		if (!strcasecmp(p->name, cmdname)) {
			pc = p;
			break;
		}
	}
	if (!pc) return;

	PyObject *cli = py_client_new(client);
	PyObject *params = parv_to_list(parv, parc, 1);
	PyObject *args = PyTuple_Pack(2, cli, params);
	Py_XDECREF(cli);
	Py_XDECREF(params);
	if (!args) {
		PyErr_Print();
		return;
	}
	PyObject *rv = PyObject_Call(pc->fn, args, NULL);
	Py_DECREF(args);
	if (!rv) {
		PyObject *type, *val, *tb;
		PyErr_Fetch(&type, &val, &tb);
		PyErr_NormalizeException(&type, &val, &tb);
		PyObject *vstr = val ? PyObject_Str(val) : NULL;
		const char *vs = vstr ? PyUnicode_AsUTF8(vstr) : "";
		unreal_log(ULOG_ERROR, "obbypy", "PY_CMD_ERROR", client,
		           "Python command $cmd raised: $err",
		           log_data_string("cmd", cmdname),
		           log_data_string("err", vs ? vs : "?"));
		Py_XDECREF(vstr);
		Py_XDECREF(type);
		Py_XDECREF(val);
		Py_XDECREF(tb);
		return;
	}
	Py_DECREF(rv);
}

/* ===================================================================
 * Script loader
 * =================================================================== */

static void load_one_script(const char *dirpath, const char *fname)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", dirpath, fname);

	FILE *fp = fopen(path, "rb");
	if (!fp) {
		unreal_log(ULOG_WARNING, "obbypy", "PY_OPEN_FAILED", NULL,
		           "Failed to open script $path: $err",
		           log_data_string("path", path),
		           log_data_string("err", strerror(errno)));
		return;
	}

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0 || size > 4 * 1024 * 1024) {  /* 4MB cap */
		fclose(fp);
		unreal_log(ULOG_WARNING, "obbypy", "PY_INVALID_SIZE", NULL,
		           "Script $path has implausible size, skipping",
		           log_data_string("path", path));
		return;
	}
	char *buf = safe_alloc(size + 1);
	if (fread(buf, 1, size, fp) != (size_t)size) {
		fclose(fp);
		safe_free(buf);
		unreal_log(ULOG_WARNING, "obbypy", "PY_READ_FAILED", NULL,
		           "Failed to read script $path",
		           log_data_string("path", path));
		return;
	}
	fclose(fp);
	buf[size] = '\0';

	/* Strip the .py extension to get a module name (used for tracebacks). */
	char modname[256];
	strlcpy(modname, fname, sizeof(modname));
	char *dot = strrchr(modname, '.');
	if (dot) *dot = '\0';

	/* Compile + exec into a fresh module namespace so scripts don't
	 * stomp on each other's globals. */
	PyObject *code = Py_CompileString(buf, fname, Py_file_input);
	safe_free(buf);
	if (!code) {
		PyObject *type, *val, *tb;
		PyErr_Fetch(&type, &val, &tb);
		PyErr_NormalizeException(&type, &val, &tb);
		PyObject *vstr = val ? PyObject_Str(val) : NULL;
		const char *vs = vstr ? PyUnicode_AsUTF8(vstr) : "";
		unreal_log(ULOG_ERROR, "obbypy", "PY_COMPILE_ERROR", NULL,
		           "Failed to compile $path: $err",
		           log_data_string("path", path),
		           log_data_string("err", vs ? vs : "?"));
		Py_XDECREF(vstr);
		Py_XDECREF(type);
		Py_XDECREF(val);
		Py_XDECREF(tb);
		return;
	}

	PyObject *mod = PyImport_ExecCodeModule(modname, code);
	Py_DECREF(code);
	if (!mod) {
		PyObject *type, *val, *tb;
		PyErr_Fetch(&type, &val, &tb);
		PyErr_NormalizeException(&type, &val, &tb);
		PyObject *vstr = val ? PyObject_Str(val) : NULL;
		const char *vs = vstr ? PyUnicode_AsUTF8(vstr) : "";
		unreal_log(ULOG_ERROR, "obbypy", "PY_EXEC_ERROR", NULL,
		           "Failed to run $path: $err",
		           log_data_string("path", path),
		           log_data_string("err", vs ? vs : "?"));
		Py_XDECREF(vstr);
		Py_XDECREF(type);
		Py_XDECREF(val);
		Py_XDECREF(tb);
		return;
	}
	Py_DECREF(mod);
	unreal_log(ULOG_INFO, "obbypy", "PY_SCRIPT_LOADED", NULL,
	           "Loaded $path", log_data_string("path", path));
}

static void load_scripts(void)
{
	char dirpath[PATH_MAX];
	snprintf(dirpath, sizeof(dirpath), "%s/%s", CONFDIR,
	         cfg.scripts_dir ? cfg.scripts_dir : DEFAULT_SCRIPTS_DIR);
	DIR *d = opendir(dirpath);
	if (!d) {
		unreal_log(ULOG_INFO, "obbypy", "PY_NO_SCRIPTS_DIR", NULL,
		           "Scripts dir not found at $path (create it to add scripts)",
		           log_data_string("path", dirpath));
		return;
	}
	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;
		size_t n = strlen(ent->d_name);
		if (n < 3 || strcmp(ent->d_name + n - 3, ".py")) continue;
		load_one_script(dirpath, ent->d_name);
	}
	closedir(d);
}

static void cleanup_scripts(void)
{
	/* Drop references; Python's atexit / Py_Finalize will reclaim. */
	for (PyHook *h = registered_hooks; h; ) {
		PyHook *next = h->next;
		Py_XDECREF(h->fn);
		safe_free(h->script_name);
		safe_free(h);
		h = next;
	}
	registered_hooks = NULL;

	for (PyCmd *c = registered_cmds; c; ) {
		PyCmd *next = c->next;
		if (c->cmd) CommandDel(c->cmd);
		Py_XDECREF(c->fn);
		safe_free(c->name);
		safe_free(c->script_name);
		safe_free(c);
		c = next;
	}
	registered_cmds = NULL;

	memset(hooks_added, 0, sizeof(hooks_added));
}

/* ===================================================================
 * Config block
 * =================================================================== */

static int obbypy_configtest(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	if (type != CONFIG_MAIN) return 0;
	if (!ce || !ce->name || strcmp(ce->name, MYCONF)) return 0;
	for (ConfigEntry *cep = ce->items; cep; cep = cep->next) {
		if (!cep->value) {
			config_error("%s:%d: blank %s::%s",
			             cep->file->filename, cep->line_number,
			             MYCONF, cep->name);
			errors++;
			continue;
		}
		if (!strcmp(cep->name, "scripts-dir")) continue;
		config_error("%s:%d: unknown directive %s::%s",
		             cep->file->filename, cep->line_number,
		             MYCONF, cep->name);
		errors++;
	}
	*errs = errors;
	return errors ? -1 : 1;
}

static int obbypy_configrun(ConfigFile *cf, ConfigEntry *ce, int type)
{
	if (type != CONFIG_MAIN) return 0;
	if (!ce || !ce->name || strcmp(ce->name, MYCONF)) return 0;
	for (ConfigEntry *cep = ce->items; cep; cep = cep->next) {
		if (!strcmp(cep->name, "scripts-dir") && cep->value) {
			safe_strdup(cfg.scripts_dir, cep->value);
		}
	}
	return 1;
}

/* ===================================================================
 * Module lifecycle
 * =================================================================== */

MOD_TEST()
{
	memset(&cfg, 0, sizeof(cfg));
	safe_strdup(cfg.scripts_dir, DEFAULT_SCRIPTS_DIR);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, obbypy_configtest);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	modinfo_ref = modinfo;
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, obbypy_configrun);

	/* Register the `obby` builtin BEFORE Py_Initialize so scripts can
	 * `import obby` straight away. */
	if (PyImport_AppendInittab("obby", PyInit_obby) == -1) {
		config_error("[obbypy] PyImport_AppendInittab failed");
		return MOD_FAILED;
	}

	if (!Py_IsInitialized()) {
		Py_Initialize();
	}

	if (!Py_IsInitialized()) {
		config_error("[obbypy] Py_Initialize failed");
		return MOD_FAILED;
	}

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	load_scripts();
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	cleanup_scripts();
	if (Py_IsInitialized()) {
		Py_Finalize();
	}
	safe_free(cfg.scripts_dir);
	return MOD_SUCCESS;
}
