/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007-2008 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 */

#include <config.h>

#include <string.h>

#include <glib/gi18n-lib.h>
#include <dbus/dbus-glib.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/util.h>
#include <libmissioncontrol/mc-enum-types.h>

#include "empathy-idle.h"
#include "empathy-utils.h"
#include "empathy-connectivity.h"

#define DEBUG_FLAG EMPATHY_DEBUG_OTHER
#include "empathy-debug.h"

/* Number of seconds before entering extended autoaway. */
#define EXT_AWAY_TIME (30*60)

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyIdle)
typedef struct {
	MissionControl *mc;
	DBusGProxy     *gs_proxy;
	EmpathyConnectivity *connectivity;

	TpConnectionPresenceType      state;
	gchar          *status;
	TpConnectionPresenceType      flash_state;
	gboolean        auto_away;

	TpConnectionPresenceType      away_saved_state;
	TpConnectionPresenceType      saved_state;
	gchar          *saved_status;

	gboolean        is_idle;
	guint           ext_away_timeout;
} EmpathyIdlePriv;

typedef enum {
	SESSION_STATUS_AVAILABLE,
	SESSION_STATUS_INVISIBLE,
	SESSION_STATUS_BUSY,
	SESSION_STATUS_IDLE,
	SESSION_STATUS_UNKNOWN
} SessionStatus;

enum {
	PROP_0,
	PROP_STATE,
	PROP_STATUS,
	PROP_FLASH_STATE,
	PROP_AUTO_AWAY
};

G_DEFINE_TYPE (EmpathyIdle, empathy_idle, G_TYPE_OBJECT);

static EmpathyIdle * idle_singleton = NULL;

static void
idle_presence_changed_cb (MissionControl *mc,
			  TpConnectionPresenceType state,
			  gchar          *status,
			  EmpathyIdle    *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	if (state == TP_CONNECTION_PRESENCE_TYPE_UNSET)
		/* Assume our presence is offline if MC reports UNSET */
		state = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;

	DEBUG ("Presence changed to '%s' (%d)", status, state);

	g_free (priv->status);
	priv->state = state;
	priv->status = NULL;
	if (!EMP_STR_EMPTY (status)) {
		priv->status = g_strdup (status);
	}

	g_object_notify (G_OBJECT (idle), "state");
	g_object_notify (G_OBJECT (idle), "status");
}

static gboolean
idle_ext_away_cb (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	DEBUG ("Going to extended autoaway");
	empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY);
	priv->ext_away_timeout = 0;

	return FALSE;
}

static void
idle_ext_away_stop (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	if (priv->ext_away_timeout) {
		g_source_remove (priv->ext_away_timeout);
		priv->ext_away_timeout = 0;
	}
}

static void
idle_ext_away_start (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	if (priv->ext_away_timeout != 0) {
		return;
	}
	priv->ext_away_timeout = g_timeout_add_seconds (EXT_AWAY_TIME,
							(GSourceFunc) idle_ext_away_cb,
							idle);
}

static void
idle_session_status_changed_cb (DBusGProxy    *gs_proxy,
				SessionStatus  status,
				EmpathyIdle   *idle)
{
	EmpathyIdlePriv *priv;
	gboolean is_idle;

	priv = GET_PRIV (idle);

	is_idle = (status == SESSION_STATUS_IDLE);

	DEBUG ("Session idle state changed, %s -> %s",
		priv->is_idle ? "yes" : "no",
		is_idle ? "yes" : "no");

	if (!priv->auto_away ||
	    (priv->saved_state == TP_CONNECTION_PRESENCE_TYPE_UNSET &&
	     (priv->state <= TP_CONNECTION_PRESENCE_TYPE_OFFLINE ||
	      priv->state == TP_CONNECTION_PRESENCE_TYPE_HIDDEN))) {
		/* We don't want to go auto away OR we explicitely asked to be
		 * offline, nothing to do here */
		priv->is_idle = is_idle;
		return;
	}

	if (is_idle && !priv->is_idle) {
		TpConnectionPresenceType new_state;
		/* We are now idle */

		idle_ext_away_start (idle);

		if (priv->saved_state != TP_CONNECTION_PRESENCE_TYPE_UNSET) {
		    	/* We are disconnected, when coming back from away
		    	 * we want to restore the presence before the
		    	 * disconnection. */
			priv->away_saved_state = priv->saved_state;
		} else {
			priv->away_saved_state = priv->state;
		}

		new_state = TP_CONNECTION_PRESENCE_TYPE_AWAY;
		if (priv->state == TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY) {
			new_state = TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY;
		}

		DEBUG ("Going to autoaway. Saved state=%d, new state=%d",
			priv->away_saved_state, new_state);
		empathy_idle_set_state (idle, new_state);
	} else if (!is_idle && priv->is_idle) {
		const gchar *new_status;
		/* We are no more idle, restore state */

		idle_ext_away_stop (idle);

		if (priv->away_saved_state == TP_CONNECTION_PRESENCE_TYPE_AWAY ||
		    priv->away_saved_state == TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY) {
			priv->away_saved_state = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;
			new_status = NULL;
		} else {
			new_status = priv->status;
		}

		DEBUG ("Restoring state to %d, reset status to %s",
			priv->away_saved_state, new_status);

		empathy_idle_set_presence (idle,
					   priv->away_saved_state,
					   new_status);

		priv->away_saved_state = TP_CONNECTION_PRESENCE_TYPE_UNSET;
	}

	priv->is_idle = is_idle;
}

static void
idle_state_change_cb (EmpathyConnectivity *connectivity,
		      gboolean old_online,
		      gboolean new_online,
		      EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	if (old_online && !new_online) {
		/* We are no longer connected */
		DEBUG ("Disconnected: Save state %d (%s)",
				priv->state, priv->status);
		priv->saved_state = priv->state;
		g_free (priv->saved_status);
		priv->saved_status = g_strdup (priv->status);
		empathy_idle_set_state (idle, TP_CONNECTION_PRESENCE_TYPE_OFFLINE);
	}
	else if (!old_online && new_online
			&& priv->saved_state != TP_CONNECTION_PRESENCE_TYPE_UNSET) {
		/* We are now connected */
		DEBUG ("Reconnected: Restore state %d (%s)",
				priv->saved_state, priv->saved_status);
		empathy_idle_set_presence (idle,
				priv->saved_state,
				priv->saved_status);
		priv->saved_state = TP_CONNECTION_PRESENCE_TYPE_UNSET;
		g_free (priv->saved_status);
		priv->saved_status = NULL;
	}
}

static void
idle_use_conn_cb (GObject *object,
		  GParamSpec *pspec,
		  EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv = GET_PRIV (idle);

	if (!empathy_connectivity_get_use_conn (priv->connectivity)) {
		if (priv->saved_state != TP_CONNECTION_PRESENCE_TYPE_UNSET) {
			empathy_idle_set_state (idle, priv->saved_state);
		}

		priv->saved_state = TP_CONNECTION_PRESENCE_TYPE_UNSET;
	}
}

static void
idle_finalize (GObject *object)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (object);

	g_free (priv->status);
	g_object_unref (priv->mc);

	if (priv->gs_proxy) {
		g_object_unref (priv->gs_proxy);
	}

	g_signal_handlers_disconnect_by_func (priv->connectivity,
					      idle_state_change_cb, object);
	g_signal_handlers_disconnect_by_func (priv->connectivity,
					      idle_use_conn_cb, object);

	g_object_unref (priv->connectivity);

	idle_ext_away_stop (EMPATHY_IDLE (object));
}

static GObject *
idle_constructor (GType type,
		  guint n_props,
		  GObjectConstructParam *props)
{
	GObject *retval;

	if (idle_singleton) {
		retval = g_object_ref (idle_singleton);
	} else {
		retval = G_OBJECT_CLASS (empathy_idle_parent_class)->constructor
			(type, n_props, props);

		idle_singleton = EMPATHY_IDLE (retval);
		g_object_add_weak_pointer (retval, (gpointer) &idle_singleton);
	}

	return retval;
}

static void
idle_get_property (GObject    *object,
		   guint       param_id,
		   GValue     *value,
		   GParamSpec *pspec)
{
	EmpathyIdlePriv *priv;
	EmpathyIdle     *idle;

	priv = GET_PRIV (object);
	idle = EMPATHY_IDLE (object);

	switch (param_id) {
	case PROP_STATE:
		g_value_set_enum (value, empathy_idle_get_state (idle));
		break;
	case PROP_STATUS:
		g_value_set_string (value, empathy_idle_get_status (idle));
		break;
	case PROP_FLASH_STATE:
		g_value_set_enum (value, empathy_idle_get_flash_state (idle));
		break;
	case PROP_AUTO_AWAY:
		g_value_set_boolean (value, empathy_idle_get_auto_away (idle));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static void
idle_set_property (GObject      *object,
		   guint         param_id,
		   const GValue *value,
		   GParamSpec   *pspec)
{
	EmpathyIdlePriv *priv;
	EmpathyIdle     *idle;

	priv = GET_PRIV (object);
	idle = EMPATHY_IDLE (object);

	switch (param_id) {
	case PROP_STATE:
		empathy_idle_set_state (idle, g_value_get_enum (value));
		break;
	case PROP_STATUS:
		empathy_idle_set_status (idle, g_value_get_string (value));
		break;
	case PROP_FLASH_STATE:
		empathy_idle_set_flash_state (idle, g_value_get_enum (value));
		break;
	case PROP_AUTO_AWAY:
		empathy_idle_set_auto_away (idle, g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	};
}

static void
empathy_idle_class_init (EmpathyIdleClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = idle_finalize;
	object_class->constructor = idle_constructor;
	object_class->get_property = idle_get_property;
	object_class->set_property = idle_set_property;

	g_object_class_install_property (object_class,
					 PROP_STATE,
					 g_param_spec_uint ("state",
							    "state",
							    "state",
							    0, NUM_TP_CONNECTION_PRESENCE_TYPES,
							    TP_CONNECTION_PRESENCE_TYPE_UNSET,
							    G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_STATUS,
					 g_param_spec_string ("status",
							      "status",
							      "status",
							      NULL,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_FLASH_STATE,
					 g_param_spec_uint ("flash-state",
							    "flash-state",
							    "flash-state",
							    0, NUM_TP_CONNECTION_PRESENCE_TYPES,
							    TP_CONNECTION_PRESENCE_TYPE_UNSET,
							    G_PARAM_READWRITE));

	 g_object_class_install_property (object_class,
					  PROP_AUTO_AWAY,
					  g_param_spec_boolean ("auto-away",
								"Automatic set presence to away",
								"Should it set presence to away if inactive",
								FALSE,
								G_PARAM_READWRITE));

	g_type_class_add_private (object_class, sizeof (EmpathyIdlePriv));
}

static TpConnectionPresenceType
empathy_idle_get_actual_presence (EmpathyIdle *idle, GError **error)
{
	McPresence presence;
	EmpathyIdlePriv *priv = GET_PRIV (idle);

	presence = mission_control_get_presence_actual (priv->mc, error);

	switch (presence) {
	case MC_PRESENCE_OFFLINE:
		return TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
	case MC_PRESENCE_AVAILABLE:
		return TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;
	case MC_PRESENCE_AWAY:
		return TP_CONNECTION_PRESENCE_TYPE_AWAY;
	case MC_PRESENCE_EXTENDED_AWAY:
		return TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY;
	case MC_PRESENCE_HIDDEN:
		return TP_CONNECTION_PRESENCE_TYPE_HIDDEN;
	case MC_PRESENCE_DO_NOT_DISTURB:
		return TP_CONNECTION_PRESENCE_TYPE_BUSY;
	default:
		return TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
	}
}

static void
empathy_idle_init (EmpathyIdle *idle)
{
	GError          *error = NULL;
	EmpathyIdlePriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (idle,
		EMPATHY_TYPE_IDLE, EmpathyIdlePriv);

	idle->priv = priv;
	priv->is_idle = FALSE;
	priv->mc = empathy_mission_control_dup_singleton ();
	priv->state = empathy_idle_get_actual_presence (idle, &error);
	if (error) {
		DEBUG ("Error getting actual presence: %s", error->message);

		/* Fallback to OFFLINE as that's what mission_control_get_presence_actual
		does. This also ensure to always display the status icon (there is no
		unset presence icon). */
		priv->state = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
		g_clear_error (&error);
	}
	priv->status = mission_control_get_presence_message_actual (priv->mc, &error);
	if (error || EMP_STR_EMPTY (priv->status)) {
		g_free (priv->status);
		priv->status = NULL;

		if (error) {
			DEBUG ("Error getting actual presence message: %s", error->message);
			g_clear_error (&error);
		}
	}

	dbus_g_proxy_connect_signal (DBUS_G_PROXY (priv->mc),
				     "PresenceChanged",
				     G_CALLBACK (idle_presence_changed_cb),
				     idle, NULL);

	priv->gs_proxy = dbus_g_proxy_new_for_name (tp_get_bus (),
						    "org.gnome.SessionManager",
						    "/org/gnome/SessionManager/Presence",
						    "org.gnome.SessionManager.Presence");
	if (priv->gs_proxy) {
		dbus_g_proxy_add_signal (priv->gs_proxy, "StatusChanged",
					 G_TYPE_UINT, G_TYPE_INVALID);
		dbus_g_proxy_connect_signal (priv->gs_proxy, "StatusChanged",
					     G_CALLBACK (idle_session_status_changed_cb),
					     idle, NULL);
	} else {
		DEBUG ("Failed to get gs proxy");
	}

	priv->connectivity = empathy_connectivity_dup_singleton ();
	g_signal_connect (priv->connectivity, "state-change",
	    G_CALLBACK (idle_state_change_cb), idle);
	g_signal_connect (priv->connectivity, "notify::use-conn",
	    G_CALLBACK (idle_use_conn_cb), idle);
}

EmpathyIdle *
empathy_idle_dup_singleton (void)
{
	return g_object_new (EMPATHY_TYPE_IDLE, NULL);
}

TpConnectionPresenceType
empathy_idle_get_state (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	return priv->state;
}

void
empathy_idle_set_state (EmpathyIdle *idle,
			TpConnectionPresenceType   state)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	empathy_idle_set_presence (idle, state, priv->status);
}

const gchar *
empathy_idle_get_status (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	if (!priv->status) {
		return empathy_presence_get_default_message (priv->state);
	}

	return priv->status;
}

void
empathy_idle_set_status (EmpathyIdle *idle,
			 const gchar *status)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	empathy_idle_set_presence (idle, priv->state, status);
}

TpConnectionPresenceType
empathy_idle_get_flash_state (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	return priv->flash_state;
}

void
empathy_idle_set_flash_state (EmpathyIdle *idle,
			      TpConnectionPresenceType   state)
{
	EmpathyIdlePriv *priv;

	priv = GET_PRIV (idle);

	priv->flash_state = state;

	if (state == TP_CONNECTION_PRESENCE_TYPE_UNSET) {
	}

	g_object_notify (G_OBJECT (idle), "flash-state");
}

static void
empathy_idle_do_set_presence (EmpathyIdle *idle,
			   TpConnectionPresenceType   state,
			   const gchar *status)
{
	McPresence mc_state = MC_PRESENCE_UNSET;
	EmpathyIdlePriv *priv = GET_PRIV (idle);

	switch (state) {
		case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
			mc_state = MC_PRESENCE_OFFLINE;
			break;
		case TP_CONNECTION_PRESENCE_TYPE_AVAILABLE:
			mc_state = MC_PRESENCE_AVAILABLE;
			break;
		case TP_CONNECTION_PRESENCE_TYPE_AWAY:
			mc_state = MC_PRESENCE_AWAY;
			break;
		case TP_CONNECTION_PRESENCE_TYPE_EXTENDED_AWAY:
			mc_state = MC_PRESENCE_EXTENDED_AWAY;
			break;
		case TP_CONNECTION_PRESENCE_TYPE_HIDDEN:
			mc_state = MC_PRESENCE_HIDDEN;
			break;
		case TP_CONNECTION_PRESENCE_TYPE_BUSY:
			mc_state = MC_PRESENCE_DO_NOT_DISTURB;
			break;
		default:
			g_assert_not_reached ();
	}

	mission_control_set_presence (priv->mc, mc_state, status, NULL, NULL);
}

void
empathy_idle_set_presence (EmpathyIdle *idle,
			   TpConnectionPresenceType   state,
			   const gchar *status)
{
	EmpathyIdlePriv *priv;
	const gchar     *default_status;

	priv = GET_PRIV (idle);

	DEBUG ("Changing presence to %s (%d)", status, state);

	/* Do not set translated default messages */
	default_status = empathy_presence_get_default_message (state);
	if (!tp_strdiff (status, default_status)) {
		status = NULL;
	}

	if (!empathy_connectivity_is_online (priv->connectivity)) {
		DEBUG ("Empathy is not online");

		if (tp_strdiff (priv->status, status)) {
			g_free (priv->status);
			priv->status = NULL;
			if (!EMP_STR_EMPTY (status)) {
				priv->status = g_strdup (status);
			}
			g_object_notify (G_OBJECT (idle), "status");
		}
	}

	empathy_idle_do_set_presence (idle, state, status);
}

gboolean
empathy_idle_get_auto_away (EmpathyIdle *idle)
{
	EmpathyIdlePriv *priv = GET_PRIV (idle);

	return priv->auto_away;
}

void
empathy_idle_set_auto_away (EmpathyIdle *idle,
			    gboolean     auto_away)
{
	EmpathyIdlePriv *priv = GET_PRIV (idle);

	priv->auto_away = auto_away;

	g_object_notify (G_OBJECT (idle), "auto-away");
}

