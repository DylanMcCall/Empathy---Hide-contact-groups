/*
 * Copyright (C) 2007-2010 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Xavier Claessens <xclaesse@gmail.com>
 *          Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <telepathy-glib/debug-sender.h>

#include <libempathy/empathy-call-factory.h>
#include <libempathy-gtk/empathy-ui-utils.h>

#include "empathy-call-window.h"

#define DEBUG_FLAG EMPATHY_DEBUG_VOIP
#include <libempathy/empathy-debug.h>

#include <gst/gst.h>

/* Exit after $TIMEOUT seconds if not displaying any call window */
#define TIMEOUT 60

#define EMPATHY_AV_DBUS_NAME "org.gnome.Empathy.AudioVideo"

static GtkApplication *app = NULL;
static gboolean app_held = FALSE;
static guint nb_windows = 0;
static gboolean use_timer = TRUE;

static void
start_timer (void)
{
  if (!use_timer)
    return;

  DEBUG ("Start timer");

  if (app_held)
    g_application_release (G_APPLICATION (app));
}

static void
stop_timer (void)
{
  DEBUG ("Stop timer");

  g_application_hold (G_APPLICATION (app));
  app_held = TRUE;
}

static void
call_window_destroy_cb (EmpathyCallWindow *window,
    gpointer user_data)
{
  nb_windows--;

  if (nb_windows > 0)
    return;

  start_timer ();
}

static void
new_call_handler_cb (EmpathyCallFactory *factory,
    EmpathyCallHandler *handler,
    gboolean outgoing,
    gpointer user_data)
{
  EmpathyCallWindow *window;

  DEBUG ("Create a new call window");

  window = empathy_call_window_new (handler);

  nb_windows++;
  stop_timer ();

  g_signal_connect (window, "destroy",
      G_CALLBACK (call_window_destroy_cb), NULL);

  gtk_widget_show (GTK_WIDGET (window));
}

int
main (int argc,
    char *argv[])
{
  GOptionContext *optcontext;
  GOptionEntry options[] = {
      { NULL }
  };
#ifdef ENABLE_DEBUG
  TpDebugSender *debug_sender;
#endif
  EmpathyCallFactory *call_factory;
  GError *error = NULL;

  /* Init */
  g_thread_init (NULL);

  optcontext = g_option_context_new (N_("- Empathy Audio/Video Client"));
  g_option_context_add_group (optcontext, gst_init_get_option_group ());
  g_option_context_add_group (optcontext, gtk_get_option_group (TRUE));
  g_option_context_add_main_entries (optcontext, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (optcontext, &argc, &argv, &error)) {
    g_print ("%s\nRun '%s --help' to see a full list of available command "
        "line options.\n",
        error->message, argv[0]);
    g_warning ("Error in empathy-av init: %s", error->message);
    return EXIT_FAILURE;
  }

  g_option_context_free (optcontext);

  empathy_gtk_init ();
  g_set_application_name (_("Empathy Audio/Video Client"));
  g_setenv ("PULSE_PROP_media.role", "phone", TRUE);

  gtk_window_set_default_icon_name ("empathy");
  textdomain (GETTEXT_PACKAGE);

  app = gtk_application_new (EMPATHY_AV_DBUS_NAME, G_APPLICATION_IS_SERVICE);

#ifdef ENABLE_DEBUG
  /* Set up debug sender */
  debug_sender = tp_debug_sender_dup ();
  g_log_set_default_handler (tp_debug_sender_log_handler, G_LOG_DOMAIN);
#endif

  call_factory = empathy_call_factory_initialise ();

  g_signal_connect (G_OBJECT (call_factory), "new-call-handler",
      G_CALLBACK (new_call_handler_cb), NULL);

  if (!empathy_call_factory_register (call_factory, &error))
    {
      g_critical ("Failed to register Handler: %s", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  if (g_getenv ("EMPATHY_PERSIST") != NULL)
    {
      DEBUG ("Disable timer");

      use_timer = FALSE;
    }

  /* the inactivity timeout can only be set while the application is held */
  g_application_hold (G_APPLICATION (app));
  g_application_set_inactivity_timeout (G_APPLICATION (app), TIMEOUT * 1000);
  if (use_timer)
    {
      g_application_release (G_APPLICATION (app));
      app_held = FALSE;
    }
  else
    app_held = TRUE;

  start_timer ();

  g_application_run (G_APPLICATION (app), argc, argv);

  g_object_unref (app);
  g_object_unref (call_factory);

#ifdef ENABLE_DEBUG
  g_object_unref (debug_sender);
#endif

  return EXIT_SUCCESS;
}
