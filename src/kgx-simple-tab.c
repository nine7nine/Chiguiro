/* kgx-local-tab.c
 *
 * Copyright 2019-2023 Zander Brown
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

#include "kgx-depot.h"
#include "kgx-edge.h"
#include "kgx-window.h"
#include "kgx-settings.h"
#include "kgx-train.h"
#include "kgx-utils.h"
#include "kgx-file-closures.h"

#include "kgx-simple-tab.h"


struct _KgxSimpleTab {
  KgxTab        parent_instance;

  char         *title;
  GFile        *path;

  char         *initial_work_dir;
  GStrv         command;
  GStrv         environ;
  KgxDepot     *depot;

  GCancellable *spawn_cancellable;

  GtkWidget    *scrolled_window;
};


G_DEFINE_FINAL_TYPE (KgxSimpleTab, kgx_simple_tab, KGX_TYPE_TAB)


enum {
  PROP_0,
  PROP_INITIAL_WORK_DIR,
  PROP_COMMAND,
  PROP_ENVIRON,
  PROP_DEPOT,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void
kgx_simple_tab_dispose (GObject *object)
{
  KgxSimpleTab *self = KGX_SIMPLE_TAB (object);

  g_signal_handlers_disconnect_by_data (self, self);

  self->scrolled_window = NULL;

  g_clear_pointer (&self->initial_work_dir, g_free);
  g_clear_pointer (&self->command, g_strfreev);
  g_clear_pointer (&self->environ, g_strfreev);
  g_clear_object (&self->depot);

  g_cancellable_cancel (self->spawn_cancellable);
  g_clear_object (&self->spawn_cancellable);

  gtk_widget_dispose_template (GTK_WIDGET (self), KGX_TYPE_SIMPLE_TAB);

  G_OBJECT_CLASS (kgx_simple_tab_parent_class)->dispose (object);
}


static void
kgx_simple_tab_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  KgxSimpleTab *self = KGX_SIMPLE_TAB (object);

  switch (property_id) {
    case PROP_INITIAL_WORK_DIR:
      self->initial_work_dir = g_value_dup_string (value);
      break;
    case PROP_COMMAND:
      self->command = g_value_dup_boxed (value);
      break;
    case PROP_ENVIRON:
      self->environ = g_value_dup_boxed (value);
      break;
    case PROP_DEPOT:
      g_set_object (&self->depot, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_simple_tab_get_property (GObject    *object,
                             guint       property_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  KgxSimpleTab *self = KGX_SIMPLE_TAB (object);

  switch (property_id) {
    case PROP_INITIAL_WORK_DIR:
      g_value_set_string (value, self->initial_work_dir);
      break;
    case PROP_COMMAND:
      g_value_set_boxed (value, self->command);
      break;
    case PROP_ENVIRON:
      g_value_set_boxed (value, self->environ);
      break;
    case PROP_DEPOT:
      g_value_set_object (value, self->depot);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


struct _StartData {
  KgxSimpleTab *self;
};


KGX_DEFINE_DATA (StartData, start_data)


static void
start_data_cleanup (StartData *self)
{
  g_clear_weak_pointer (&self->self);
}


static void
spawned (GObject      *source,
         GAsyncResult *res,
         gpointer      user_data)

{
  g_autoptr (GTask) task = user_data;
  g_autoptr (KgxTrain) train = NULL;
  g_autoptr (GError) error = NULL;
  StartData *start_data = kgx_task_get_start_data (task);

  train = kgx_depot_spawn_finish (KGX_DEPOT (source), res, &error);

  if (!start_data->self) {
    return; /* The tab went away whilst we were spawning */
  }

  if (error) {
    g_autofree char *message = NULL;

    /* Translators: <b> </b> marks the text as bold, ensure they are matched please! */
    message = g_strdup_printf (_("<b>Failed to start</b> — %s"),
                               error->message);

    kgx_tab_died (KGX_TAB (start_data->self),
                  GTK_MESSAGE_ERROR,
                  message,
                  FALSE);

    g_task_return_error (task, g_steal_pointer (&error));

    return;
  }

  g_task_return_pointer (task,
                         g_steal_pointer (&train),
                         g_object_unref);
}


static void
kgx_simple_tab_start (KgxTab              *page,
                      GAsyncReadyCallback  callback,
                      gpointer             callback_data)
{
  g_autoptr (KgxSettings) settings = NULL;
  g_autoptr (VtePty) pty = NULL;
  g_autoptr (VteTerminal) terminal = NULL;
  g_autoptr (StartData) data = start_data_alloc ();
  g_autoptr (GTask) task = NULL;
  g_autoptr (GError) error = NULL;
  KgxSimpleTab *self;

  g_return_if_fail (KGX_IS_SIMPLE_TAB (page));

  self = KGX_SIMPLE_TAB (page);

  if (self->spawn_cancellable) {
    g_cancellable_reset (self->spawn_cancellable);
  } else {
    self->spawn_cancellable = g_cancellable_new ();
  }

  g_set_weak_pointer (&data->self, self);

  task = g_task_new (self, self->spawn_cancellable, callback, callback_data);
  g_task_set_source_tag (task, kgx_simple_tab_start);
  kgx_task_set_start_data (task, g_steal_pointer (&data));

  pty = vte_pty_new_sync (VTE_PTY_DEFAULT, self->spawn_cancellable, &error);
  if (error) {
    g_task_return_error (task, g_steal_pointer (&error));
    g_clear_object (&self->spawn_cancellable);

    return;
  }

  g_object_get (self, "terminal", &terminal, "settings", &settings, NULL);

  vte_terminal_set_pty (terminal, pty);

  kgx_depot_spawn (self->depot,
                   settings,
                   pty,
                   self->initial_work_dir,
                   (const char *const *) self->command,
                   (const char *const *) self->environ,
                   self->spawn_cancellable,
                   spawned,
                   g_steal_pointer (&task));
}


static KgxTrain *
kgx_simple_tab_start_finish (KgxTab        *page,
                             GAsyncResult  *res,
                             GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (res, page), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}


static gboolean
edge_scroll (GtkEventControllerScroll *controller,
             double                    dx,
             double                    dy,
             KgxSimpleTab             *self)
{
  GtkAdjustment *vadj;
  double value, lower, upper, page;
  GdkModifierType mods;

  mods = gtk_event_controller_get_current_event_state (
           GTK_EVENT_CONTROLLER (controller));

  /* Don't interfere with Ctrl+scroll (zoom). */
  if (mods & GDK_CONTROL_MASK)
    return FALSE;

  if (!self->scrolled_window)
    return FALSE;

  vadj  = gtk_scrolled_window_get_vadjustment (
            GTK_SCROLLED_WINDOW (self->scrolled_window));
  value = gtk_adjustment_get_value (vadj);
  lower = gtk_adjustment_get_lower (vadj);
  upper = gtk_adjustment_get_upper (vadj);
  page  = gtk_adjustment_get_page_size (vadj);

  if (dy < 0 && value <= lower) {
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    if (KGX_IS_WINDOW (root))
      kgx_window_fire_overscroll (KGX_WINDOW (root), GTK_POS_TOP);
    return FALSE;
  }

  if (dy > 0 && value >= upper - page) {
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    if (KGX_IS_WINDOW (root))
      kgx_window_fire_overscroll (KGX_WINDOW (root), GTK_POS_BOTTOM);
    return FALSE;
  }

  return FALSE;
}


static void
kgx_simple_tab_class_init (KgxSimpleTabClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS   (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  KgxTabClass    *tab_class    = KGX_TAB_CLASS    (klass);

  object_class->dispose = kgx_simple_tab_dispose;
  object_class->set_property = kgx_simple_tab_set_property;
  object_class->get_property = kgx_simple_tab_get_property;

  tab_class->start = kgx_simple_tab_start;
  tab_class->start_finish = kgx_simple_tab_start_finish;

  /**
   * KgxSimpleTab:initial-work-dir:
   *
   * Used to handle --working-dir
   */
  pspecs[PROP_INITIAL_WORK_DIR] =
    g_param_spec_string ("initial-work-dir", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /**
   * KgxSimpleTab:command:
   *
   * Used to handle -e
   */
  pspecs[PROP_COMMAND] =
    g_param_spec_boxed ("command", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_ENVIRON] =
    g_param_spec_boxed ("environ", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_DEPOT] =
    g_param_spec_object ("depot", NULL, NULL,
                         KGX_TYPE_DEPOT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               KGX_APPLICATION_PATH "kgx-simple-tab.ui");

  gtk_widget_class_bind_template_child (widget_class, KgxSimpleTab, scrolled_window);

  gtk_widget_class_bind_template_callback (widget_class, kgx_file_as_display_or_uri);
}


static void
kgx_simple_tab_init (KgxSimpleTab *self)
{
  GtkEventController *scroll_ctrl;

  gtk_widget_init_template (GTK_WIDGET (self));

  scroll_ctrl = gtk_event_controller_scroll_new (
                  GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll_ctrl, "scroll",
                    G_CALLBACK (edge_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled_window), scroll_ctrl);
}
