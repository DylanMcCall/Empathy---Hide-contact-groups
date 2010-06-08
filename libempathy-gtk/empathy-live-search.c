/*
 * Copyright (C) 2010 Collabora Ltd.
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
 * Authors: Felix Kaser <felix.kaser@collabora.co.uk>
 */

#include <config.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <libempathy/empathy-utils.h>

#include "empathy-live-search.h"

G_DEFINE_TYPE (EmpathyLiveSearch, empathy_live_search, GTK_TYPE_HBOX)

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyLiveSearch)

typedef struct
{
  GtkWidget *search_entry;
  GtkWidget *hook_widget;

  gunichar *text_stripped;
} EmpathyLiveSearchPriv;

enum
{
  PROP_0,
  PROP_HOOK_WIDGET,
  PROP_TEXT
};

static void live_search_hook_widget_destroy_cb (GtkObject *object,
    gpointer user_data);

/**
 * stripped_char:
 *
 * Returns a stripped version of @ch, removing any case, accentuation
 * mark, or any special mark on it.
 **/
static gunichar
stripped_char (gunichar ch)
{
  gunichar retval = 0;
  GUnicodeType utype;
  gunichar *decomp;
  gsize dlen;

  utype = g_unichar_type (ch);

  switch (utype)
    {
    case G_UNICODE_CONTROL:
    case G_UNICODE_FORMAT:
    case G_UNICODE_UNASSIGNED:
    case G_UNICODE_COMBINING_MARK:
      /* Ignore those */
      break;
    default:
      ch = g_unichar_tolower (ch);
      decomp = g_unicode_canonical_decomposition (ch, &dlen);
      if (decomp != NULL)
        {
          retval = decomp[0];
          g_free (decomp);
        }
    }

  return retval;
}

static gunichar *
strip_utf8_string (const gchar *string)
{
  gunichar *ret;
  gint ret_len;
  const gchar *p;

  if (EMP_STR_EMPTY (string))
    return NULL;

  ret = g_malloc (sizeof (gunichar) * (strlen (string) + 1));
  ret_len = 0;

  for (p = string; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar sc;

      sc = stripped_char (g_utf8_get_char (p));
      if (sc != 0)
        ret[ret_len++] = sc;
    }

  ret[ret_len] = 0;

  return ret;
}

static gboolean
live_search_entry_key_pressed_cb (GtkEntry *entry,
    GdkEventKey *event,
    gpointer user_data)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (user_data);

  /* if esc key pressed, hide the search */
  if (event->keyval == GDK_Escape)
    {
      gtk_widget_hide (GTK_WIDGET (self));
      return TRUE;
    }

  return FALSE;
}

static void
live_search_text_changed (GtkEntry *entry,
    gpointer user_data)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (user_data);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);
  const gchar *text;

  text = gtk_entry_get_text (entry);

  if (EMP_STR_EMPTY (text))
    gtk_widget_hide (GTK_WIDGET (self));
  else
    gtk_widget_show (GTK_WIDGET (self));

  g_free (priv->text_stripped);
  priv->text_stripped = strip_utf8_string (text);
  g_object_notify (G_OBJECT (self), "text");
}

static void
live_search_close_pressed (GtkEntry *entry,
    GtkEntryIconPosition icon_pos,
    GdkEvent *event,
    gpointer user_data)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (user_data);

  gtk_widget_hide (GTK_WIDGET (self));
}

static gboolean
live_search_key_press_event_cb (GtkWidget *widget,
    GdkEventKey *event,
    gpointer user_data)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (user_data);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);
  GdkEvent *new_event;
  gboolean ret;

  /* dont forward this event to the entry, else the event is consumed by the
   * entry and does not close the window */
  if (!gtk_widget_get_visible (GTK_WIDGET (self)) &&
      event->keyval == GDK_Escape)
    return FALSE;

  /* do not show the search if CTRL and/or ALT are pressed with a key
   * this is needed, because otherwise the CTRL + F accel would not work,
   * because the entry consumes it */
  if (event->state & (GDK_MOD1_MASK | GDK_CONTROL_MASK) ||
      event->keyval == GDK_Control_L ||
      event->keyval == GDK_Control_R)
    return FALSE;

  /* realize the widget if it is not realized yet */
  gtk_widget_realize (priv->search_entry);
  if (!gtk_widget_has_focus (priv->search_entry))
    {
      gtk_widget_grab_focus (priv->search_entry);
      gtk_editable_set_position (GTK_EDITABLE (priv->search_entry), -1);
    }

  /* forward the event to the search entry */
  new_event = gdk_event_copy ((GdkEvent *) event);
  ret = gtk_widget_event (priv->search_entry, new_event);
  gdk_event_free (new_event);

  return ret;
}

static void
live_search_release_hook_widget (EmpathyLiveSearch *self)
{
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  /* remove old handlers if old source was not null */
  if (priv->hook_widget != NULL)
    {
      g_signal_handlers_disconnect_by_func (priv->hook_widget,
          live_search_key_press_event_cb, self);
      g_signal_handlers_disconnect_by_func (priv->hook_widget,
          live_search_hook_widget_destroy_cb, self);
      g_object_unref (priv->hook_widget);
      priv->hook_widget = NULL;
    }
}

static void
live_search_hook_widget_destroy_cb (GtkObject *object,
    gpointer user_data)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (user_data);

  /* unref the hook widget and hide search */
  live_search_release_hook_widget (self);
  gtk_widget_hide (GTK_WIDGET (self));
}

static void
live_search_dispose (GObject *obj)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (obj);

  live_search_release_hook_widget (self);

  if (G_OBJECT_CLASS (empathy_live_search_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (empathy_live_search_parent_class)->dispose (obj);
}

static void
live_search_finalize (GObject *obj)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (obj);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  g_free (priv->text_stripped);

  if (G_OBJECT_CLASS (empathy_live_search_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (empathy_live_search_parent_class)->finalize (obj);
}

static void
live_search_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (object);

  switch (param_id)
    {
    case PROP_HOOK_WIDGET:
      g_value_set_object (value, empathy_live_search_get_hook_widget (self));
      break;
    case PROP_TEXT:
      g_value_set_string (value, empathy_live_search_get_text (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
    }
}

static void
live_search_set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (object);

  switch (param_id) {
  case PROP_HOOK_WIDGET:
    empathy_live_search_set_hook_widget (self, g_value_get_object (value));
    break;
  case PROP_TEXT:
    empathy_live_search_set_text (self, g_value_get_string (value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
    break;
  };
}

static void
live_search_hide (GtkWidget *widget)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (widget);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  GTK_WIDGET_CLASS (empathy_live_search_parent_class)->hide (widget);

  gtk_entry_set_text (GTK_ENTRY (priv->search_entry), "");
  gtk_widget_grab_focus (priv->hook_widget);
}

static void
live_search_show (GtkWidget *widget)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (widget);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  if (!gtk_widget_has_focus (priv->search_entry))
    gtk_widget_grab_focus (priv->search_entry);

  GTK_WIDGET_CLASS (empathy_live_search_parent_class)->show (widget);
}

static void
live_search_grab_focus (GtkWidget *widget)
{
  EmpathyLiveSearch *self = EMPATHY_LIVE_SEARCH (widget);
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  if (!gtk_widget_has_focus (priv->search_entry))
    gtk_widget_grab_focus (priv->search_entry);
}

static void
empathy_live_search_class_init (EmpathyLiveSearchClass *klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;
  GtkWidgetClass *widget_class = (GtkWidgetClass *) klass;
  GParamSpec *param_spec;

  object_class->finalize = live_search_finalize;
  object_class->dispose = live_search_dispose;
  object_class->get_property = live_search_get_property;
  object_class->set_property = live_search_set_property;

  widget_class->hide = live_search_hide;
  widget_class->show = live_search_show;
  widget_class->grab_focus = live_search_grab_focus;

  param_spec = g_param_spec_object ("hook-widget", "Live Searchs Hook Widget",
      "The live search catches key-press-events on this widget",
      GTK_TYPE_WIDGET, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_HOOK_WIDGET,
      param_spec);

  param_spec = g_param_spec_string ("text", "Live Search Text",
      "The text of the live search entry",
      "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TEXT, param_spec);

  g_type_class_add_private (klass, sizeof (EmpathyLiveSearchPriv));
}

static void
empathy_live_search_init (EmpathyLiveSearch *self)
{
  EmpathyLiveSearchPriv *priv =
    G_TYPE_INSTANCE_GET_PRIVATE ((self), EMPATHY_TYPE_LIVE_SEARCH,
        EmpathyLiveSearchPriv);

  gtk_widget_set_no_show_all (GTK_WIDGET (self), TRUE);

  priv->search_entry = gtk_entry_new ();
  gtk_entry_set_icon_from_stock (GTK_ENTRY (priv->search_entry),
      GTK_ENTRY_ICON_SECONDARY, GTK_STOCK_CLOSE);
  gtk_entry_set_icon_activatable (GTK_ENTRY (priv->search_entry),
      GTK_ENTRY_ICON_SECONDARY, TRUE);
  gtk_entry_set_icon_sensitive (GTK_ENTRY (priv->search_entry),
      GTK_ENTRY_ICON_SECONDARY, TRUE);
  gtk_widget_show (priv->search_entry);

  gtk_box_pack_start (GTK_BOX (self), priv->search_entry, TRUE, TRUE, 0);

  g_signal_connect (priv->search_entry, "icon_release",
      G_CALLBACK (live_search_close_pressed), self);
  g_signal_connect (priv->search_entry, "changed",
      G_CALLBACK (live_search_text_changed), self);
  g_signal_connect (priv->search_entry, "key-press-event",
      G_CALLBACK (live_search_entry_key_pressed_cb), self);

  priv->hook_widget = NULL;

  self->priv = priv;
}

GtkWidget *
empathy_live_search_new (GtkWidget *hook)
{
  g_return_val_if_fail (hook == NULL || GTK_IS_WIDGET (hook), NULL);

  return g_object_new (EMPATHY_TYPE_LIVE_SEARCH,
      "hook-widget", hook,
      NULL);
}

/* public methods */

GtkWidget *
empathy_live_search_get_hook_widget (EmpathyLiveSearch *self)
{
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (EMPATHY_IS_LIVE_SEARCH (self), NULL);

  return priv->hook_widget;
}

void
empathy_live_search_set_hook_widget (EmpathyLiveSearch *self,
    GtkWidget *hook)
{
  EmpathyLiveSearchPriv *priv;

  g_return_if_fail (EMPATHY_IS_LIVE_SEARCH (self));
  g_return_if_fail (hook == NULL || GTK_IS_WIDGET (hook));

  priv = GET_PRIV (self);

  /* release the actual widget */
  live_search_release_hook_widget (self);

  /* connect handlers if new source is not null */
  if (hook != NULL)
    {
      priv->hook_widget = g_object_ref (hook);
      g_signal_connect (priv->hook_widget, "key-press-event",
          G_CALLBACK (live_search_key_press_event_cb),
          self);
      g_signal_connect (priv->hook_widget, "destroy",
          G_CALLBACK (live_search_hook_widget_destroy_cb),
          self);
    }
}

const gchar *
empathy_live_search_get_text (EmpathyLiveSearch *self)
{
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  g_return_val_if_fail (EMPATHY_IS_LIVE_SEARCH (self), NULL);

  return gtk_entry_get_text (GTK_ENTRY (priv->search_entry));
}

void
empathy_live_search_set_text (EmpathyLiveSearch *self,
    const gchar *text)
{
  EmpathyLiveSearchPriv *priv = GET_PRIV (self);

  g_return_if_fail (EMPATHY_IS_LIVE_SEARCH (self));
  g_return_if_fail (text != NULL);

  gtk_entry_set_text (GTK_ENTRY (priv->search_entry), text);
}

static gboolean
live_search_match_string (const gchar *string,
    const gunichar *prefix)
{
  const gchar *p;

  if (prefix == NULL || prefix[0] == 0)
    return TRUE;

  if (EMP_STR_EMPTY (string))
    return FALSE;

  for (p = string; *p != '\0'; p = g_utf8_next_char (p))
    {
      guint i = 0;

      /* Search the start of the word (skip non alpha-num chars) */
      while (*p != '\0' && !g_unichar_isalnum (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);

      /* Check if this word match prefix */
      while (*p != '\0')
        {
          gunichar sc;

          sc = stripped_char (g_utf8_get_char (p));
          if (sc != 0)
            {
              /* If the char does not match, stop */
              if (sc != prefix[i])
                break;

              /* The char matched. If it was the last of prefix, stop */
              if (prefix[++i] == 0)
                return TRUE;
            }

          p = g_utf8_next_char (p);
        }

      /* This word didn't match, go to next one (skip alpha-num chars) */
      while (*p != '\0' && g_unichar_isalnum (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);

      if (*p == '\0')
        break;
    }

  return FALSE;
}

/**
 * empathy_live_search_match:
 * @self: a #EmpathyLiveSearch
 * @string: a string where to search, must be valid UTF-8.
 *
 * Search if one of the words in @string string starts with the current text
 * of @self.
 *
 * Searching for "aba" in "Abasto" will match, searching in "Moraba" will not,
 * and searching in "A tool (abacus)" will do.
 *
 * The match is not case-sensitive, and regardless of the accentuation marks.
 *
 * Returns: %TRUE if a match is found, %FALSE otherwise.
 *
 **/
gboolean
empathy_live_search_match (EmpathyLiveSearch *self,
    const gchar *string)
{
  EmpathyLiveSearchPriv *priv;

  g_return_val_if_fail (EMPATHY_IS_LIVE_SEARCH (self), FALSE);

  priv = GET_PRIV (self);

  return live_search_match_string (string, priv->text_stripped);
}

gboolean
empathy_live_search_match_string (const gchar *string,
    const gchar *prefix)
{
  gunichar *stripped;
  gboolean match;

  stripped = strip_utf8_string (prefix);
  match = live_search_match_string (string, stripped);
  g_free (stripped);

  return match;
}
