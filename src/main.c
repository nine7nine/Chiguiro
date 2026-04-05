/* main.c
 *
 * Copyright 2019-2025 Zander Brown
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kgx-config.h"

#include <glib/gi18n.h>

#include <gtk/gtk.h>

#include "kgx-application.h"
#include "kgx-locale.h"


/* Suppress known-harmless GTK/Adw warnings that we can't fix from
 * our side (internal icon lookup at size 0, AdwTabBar layout glitch). */
static GLogWriterOutput
log_writer_func (GLogLevelFlags   log_level,
                 const GLogField *fields,
                 gsize            n_fields,
                 gpointer         user_data)
{
  for (gsize i = 0; i < n_fields; i++) {
    if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 &&
        fields[i].value != NULL) {
      const char *msg = fields[i].value;
      if (strstr (msg, "gtk_icon_theme_lookup_by_gicon") ||
          strstr (msg, "gtk_icon_paintable_is_symbolic") ||
          strstr (msg, "tabboxchild") ||
          strstr (msg, "No property named \"height\""))
        return G_LOG_WRITER_HANDLED;
    }
  }

  return g_log_writer_default (log_level, fields, n_fields, user_data);
}


int
main (int argc, char *argv[])
{
  g_autoptr (GtkApplication) app = NULL;

  g_log_set_writer_func (log_writer_func, NULL, NULL);

  kgx_locale_init (KGX_LOCALE_DYNAMIC);

  g_set_prgname (KGX_BIN_NAME);
  g_set_application_name (KGX_DISPLAY_NAME);
  gtk_window_set_default_icon_name (KGX_APPLICATION_ID);

  app = g_object_new (KGX_TYPE_APPLICATION,
                      "application_id", KGX_APPLICATION_ID,
                      "version", PACKAGE_VERSION,
                      "flags", G_APPLICATION_HANDLES_COMMAND_LINE |
                               G_APPLICATION_HANDLES_OPEN |
                               G_APPLICATION_SEND_ENVIRONMENT |
                               G_APPLICATION_CAN_OVERRIDE_APP_ID,
                      "register-session", TRUE,
                      NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
