/* kgx-watcher.c
 *
 * Copyright 2022-2024 Zander Brown
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

#include <gio/gio.h>

#include "kgx-utils.h"
#include "kgx-pids.h"

#include "kgx-watcher.h"


/**
 * KgxWatcher:
 * @watching: (element-type GLib.Pid ProcessWatch) the shells running in windows
 * @children: (element-type GLib.Pid ProcessWatch) the processes running in shells
 * @timeout: the current #GSource id of the watcher
 *
 * Used to monitor processes running in pages
 */
struct _KgxWatcher {
  GObject                   parent_instance;

  gboolean                  in_background;

  GTree                    *watching;
  GTree                    *children;

  guint                     timeout;
};


G_DEFINE_TYPE (KgxWatcher, kgx_watcher, G_TYPE_OBJECT)


enum {
  PROP_0,
  PROP_IN_BACKGROUND,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void
kgx_watcher_dispose (GObject *object)
{
  KgxWatcher *self = KGX_WATCHER (object);

  g_clear_handle_id (&self->timeout, g_source_remove);

  g_clear_pointer (&self->watching, g_tree_unref);
  g_clear_pointer (&self->children, g_tree_unref);

  G_OBJECT_CLASS (kgx_watcher_parent_class)->dispose (object);
}


struct _ProcessWatch {
  KgxTrain /*weak*/ *train;
  KgxProcess *process;
};


KGX_DEFINE_DATA (ProcessWatch, process_watch)


static void
process_watch_cleanup (ProcessWatch *watch)
{
  g_clear_pointer (&watch->process, kgx_process_unref);
  g_clear_weak_pointer (&watch->train);
}


struct SessionScan {
  KgxWatcher *self;
  GHashTable *live_pids;
  GPtrArray  *dead_shells;
};


static gboolean
scan_session (gpointer key,
              gpointer val,
              gpointer user_data)
{
  struct SessionScan *data = user_data;
  ProcessWatch *shell = val;
  GPid shell_pid = GPOINTER_TO_INT (key);
  g_autofree GPid *pids = NULL;
  size_t n_pids;

  /* Shell's train was disposed — mark for removal */
  if (G_UNLIKELY (shell->train == NULL)) {
    g_ptr_array_add (data->dead_shells, key);
    return FALSE;
  }

  if (kgx_pids_get_session_pids (shell_pid, &pids, &n_pids) != KGX_PIDS_OK)
    return FALSE;

  for (size_t i = 0; i < n_pids; i++) {
    g_hash_table_add (data->live_pids, GINT_TO_POINTER (pids[i]));

    /* Don't track the shell itself as a child */
    if (pids[i] == shell_pid)
      continue;

    if (!g_tree_lookup (data->self->children, GINT_TO_POINTER (pids[i]))) {
      KgxProcess *process = kgx_process_new (pids[i]);
      ProcessWatch *child;

      if (G_UNLIKELY (process == NULL))
        continue;

      child = process_watch_alloc ();
      child->process = process;
      g_set_weak_pointer (&child->train, shell->train);

      g_debug ("watcher: Hello %i!", pids[i]);

      g_tree_insert (data->self->children,
                     GINT_TO_POINTER (pids[i]),
                     child);

      kgx_train_push_child (shell->train, process);
    }
  }

  return FALSE;
}


struct RemoveDead {
  GHashTable *live_pids;
  GPtrArray  *dead;
};


static gboolean
remove_dead (gpointer pid,
             gpointer val,
             gpointer user_data)
{
  struct RemoveDead *data = user_data;
  ProcessWatch *watch = val;

  if (!g_hash_table_contains (data->live_pids, pid)) {
    g_debug ("watcher: %i marked as dead", GPOINTER_TO_INT (pid));

    if (G_LIKELY (watch->train)) {
      kgx_train_pop_child (watch->train, watch->process);
    }

    g_ptr_array_add (data->dead, pid);
  }

  return FALSE;
}


static gboolean watch (gpointer data);

static void
ensure_timeout (KgxWatcher *self)
{
  if (self->timeout != 0)
    return;

  if (g_tree_nnodes (self->watching) == 0)
    return;

  self->timeout = g_timeout_add_full (G_PRIORITY_DEFAULT_IDLE,
                                      self->in_background ? 2000 : 500,
                                      watch,
                                      g_object_ref (self),
                                      g_object_unref);
  g_source_set_name_by_id (self->timeout, "[kgx] watcher");
}


static gboolean
watch (gpointer data)
{
  KgxWatcher *self = KGX_WATCHER (data);
  g_autoptr (GHashTable) live_pids = NULL;
  struct SessionScan scan;
  struct RemoveDead dead;

  if (g_tree_nnodes (self->watching) == 0) {
    self->timeout = 0;
    return G_SOURCE_REMOVE;
  }

  live_pids = g_hash_table_new (NULL, NULL);

  scan.self = self;
  scan.live_pids = live_pids;
  scan.dead_shells = g_ptr_array_new_full (1, NULL);

  /* Query each shell's session for its child processes */
  g_tree_foreach (self->watching, scan_session, &scan);

  /* Remove shells whose trains have died */
  for (guint i = 0; i < scan.dead_shells->len; i++)
    g_tree_remove (self->watching, g_ptr_array_index (scan.dead_shells, i));
  g_ptr_array_unref (scan.dead_shells);

  /* Detect children that are no longer in any watched session */
  dead.live_pids = live_pids;
  dead.dead = g_ptr_array_new_full (1, NULL);

  g_tree_foreach (self->children, remove_dead, &dead);

  /* Can't modify self->children whilst walking it */
  for (guint i = 0; i < dead.dead->len; i++)
    g_tree_remove (self->children, g_ptr_array_index (dead.dead, i));

  g_ptr_array_unref (dead.dead);

  /* Stop polling if all shells are gone */
  if (g_tree_nnodes (self->watching) == 0) {
    self->timeout = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}


static inline gboolean
update_watcher (KgxWatcher *self, gboolean in_background)
{
  if (self->in_background == in_background) {
    return FALSE;
  }

  self->in_background = in_background;

  g_debug ("watcher: in_background? %s", in_background ? "yes" : "no");

  /* Reschedule with new interval if currently polling */
  g_clear_handle_id (&self->timeout, g_source_remove);
  ensure_timeout (self);

  return TRUE;
}


static void
kgx_watcher_set_property (GObject      *object,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  KgxWatcher *self = KGX_WATCHER (object);

  switch (property_id) {
    case PROP_IN_BACKGROUND:
      if (update_watcher (self, g_value_get_boolean (value))) {
        g_object_notify_by_pspec (object, pspec);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_watcher_get_property (GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  KgxWatcher *self = KGX_WATCHER (object);

  switch (property_id) {
    case PROP_IN_BACKGROUND:
      g_value_set_boolean (value, self->in_background);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_watcher_class_init (KgxWatcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kgx_watcher_dispose;
  object_class->set_property = kgx_watcher_set_property;
  object_class->get_property = kgx_watcher_get_property;

  pspecs[PROP_IN_BACKGROUND] = g_param_spec_boolean ("in-background", NULL, NULL,
                                                     FALSE,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);
}


static void
kgx_watcher_init (KgxWatcher *self)
{
  self->watching = g_tree_new_full (kgx_pid_cmp,
                                    NULL,
                                    NULL,
                                    process_watch_free);
  self->children = g_tree_new_full (kgx_pid_cmp,
                                    NULL,
                                    NULL,
                                    process_watch_free);
}


/**
 * kgx_watcher_watch:
 * @self: the #KgxWatcher
 * @train: the #KgxTrain to watch
 *
 * Registers a new shell process with the pid watcher
 */
void
kgx_watcher_watch (KgxWatcher *self,
                   KgxTrain   *train)
{
  ProcessWatch *watch;
  GPid pid;

  g_return_if_fail (KGX_IS_WATCHER (self));
  g_return_if_fail (KGX_IS_TRAIN (train));

  pid = kgx_train_get_pid (train);

  watch = process_watch_alloc ();
  watch->process = kgx_process_new (pid);
  g_set_weak_pointer (&watch->train, train);

  g_debug ("watcher: tracking %i", pid);

  g_tree_insert (self->watching, GINT_TO_POINTER (pid), watch);

  ensure_timeout (self);
}
