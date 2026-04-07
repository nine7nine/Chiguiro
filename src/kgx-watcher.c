/* kgx-watcher.c
 *
 * Copyright 2022-2024 Zander Brown
 * Copyright 2026 jordan Johnston
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


#define WATCHER_FG_ACTIVE_MS   500
#define WATCHER_FG_STEADY_MS  1000
#define WATCHER_BG_ACTIVE_MS  2000
#define WATCHER_BG_STEADY_MS  5000
#define WATCHER_BG_IDLE_MS   10000
#define WATCHER_STEADY_POLLS     4
#define WATCHER_IDLE_POLLS      12


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
  guint                     stable_polls;
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
  GTree     *observed_children;
  GPtrArray *dead_shells;
};


struct ReconcileChildren {
  GTree     *observed_children;
  GPtrArray *dead;
};


struct AddObservedChildren {
  KgxWatcher *self;
  guint       added_children;
};


static KgxProcess *
lookup_process_cached (GHashTable *processes,
                       GPid        pid)
{
  KgxProcess *process = NULL;

  process = g_hash_table_lookup (processes, GINT_TO_POINTER (pid));
  if (process)
    return process;

  process = kgx_process_new (pid);
  if (G_UNLIKELY (process == NULL))
    return NULL;

  g_hash_table_insert (processes, GINT_TO_POINTER (pid), process);

  return process;
}


static gboolean
process_matches (KgxProcess *a,
                 KgxProcess *b)
{
  if (kgx_process_get_parent (a) != kgx_process_get_parent (b) ||
      kgx_process_get_session (a) != kgx_process_get_session (b) ||
      kgx_process_get_is_root (a) != kgx_process_get_is_root (b))
    return FALSE;

  return g_strcmp0 (kgx_process_get_name (a),
                    kgx_process_get_name (b)) == 0;
}


static gboolean
process_descends_from_shell (KgxProcess *process,
                             GPid        shell_pid,
                             GHashTable *processes)
{
  g_autoptr (GHashTable) seen = NULL;
  GPid current = 0;
  GPid start_pid = 0;

  g_return_val_if_fail (process != NULL, FALSE);
  g_return_val_if_fail (processes != NULL, FALSE);

  seen = g_hash_table_new (g_direct_hash, g_direct_equal);
  start_pid = kgx_process_get_pid (process);
  current = kgx_process_get_parent (process);

  while (current > 0) {
    KgxProcess *parent_process = NULL;

    if (current == shell_pid)
      return TRUE;

    if (current == start_pid ||
        !g_hash_table_add (seen, GINT_TO_POINTER (current)))
      return FALSE;

    parent_process = lookup_process_cached (processes, current);
    if (G_UNLIKELY (parent_process == NULL))
      return FALSE;

    current = kgx_process_get_parent (parent_process);
  }

  return FALSE;
}


static void
record_observed_child (GTree      *observed_children,
                       KgxTrain   *train,
                       KgxProcess *process)
{
  GPid pid = 0;
  ProcessWatch *existing = NULL;
  ProcessWatch *child = NULL;

  g_return_if_fail (observed_children != NULL);
  g_return_if_fail (KGX_IS_TRAIN (train));
  g_return_if_fail (process != NULL);

  pid = kgx_process_get_pid (process);
  existing = g_tree_lookup (observed_children, GINT_TO_POINTER (pid));

  if (existing) {
    if (existing->train != train) {
      g_debug ("watcher: %i observed under multiple shells, keeping first owner",
               pid);
    }
    return;
  }

  child = process_watch_alloc ();
  child->process = g_rc_box_acquire (process);
  g_set_weak_pointer (&child->train, train);

  g_tree_insert (observed_children, GINT_TO_POINTER (pid), child);
}


static gboolean
scan_session (gpointer key,
              gpointer val,
              gpointer user_data)
{
  struct SessionScan *data = user_data;
  ProcessWatch *shell = val;
  GPid shell_pid = GPOINTER_TO_INT (key);
  GPid session_id = 0;
  g_autofree GPid *pids = NULL;
  g_autoptr (GHashTable) processes = NULL;
  size_t n_pids;

  /* Shell's train was disposed — mark for removal */
  if (G_UNLIKELY (shell->train == NULL)) {
    g_ptr_array_add (data->dead_shells, key);
    return FALSE;
  }

  if (G_UNLIKELY (shell->process == NULL))
    return FALSE;

  session_id = kgx_process_get_session (shell->process);
  if (session_id <= 0)
    return FALSE;

  if (kgx_pids_get_session_pids (session_id, &pids, &n_pids) != KGX_PIDS_OK)
    return FALSE;

  /* Session scans are just the candidate set. Ownership still needs to be
   * traced back to the tab's shell, otherwise sibling shells can borrow each
   * other's status/title updates if they share a session. */
  processes = g_hash_table_new_full (g_direct_hash,
                                     g_direct_equal,
                                     NULL,
                                     (GDestroyNotify) kgx_process_unref);
  g_hash_table_insert (processes,
                       GINT_TO_POINTER (shell_pid),
                       g_rc_box_acquire (shell->process));

  for (size_t i = 0; i < n_pids; i++) {
    KgxProcess *process = NULL;

    /* Don't track the shell itself as a child */
    if (pids[i] == shell_pid)
      continue;

    process = lookup_process_cached (processes, pids[i]);
    if (G_UNLIKELY (process == NULL))
      continue;

    if (!process_descends_from_shell (process, shell_pid, processes))
      continue;

    record_observed_child (data->observed_children, shell->train, process);
  }

  return FALSE;
}


static gboolean
reconcile_existing_child (gpointer pid,
                          gpointer val,
                          gpointer user_data)
{
  struct ReconcileChildren *data = user_data;
  ProcessWatch *existing = val;
  ProcessWatch *observed = NULL;

  observed = g_tree_lookup (data->observed_children, pid);

  if (observed == NULL) {
    g_debug ("watcher: %i marked as dead", GPOINTER_TO_INT (pid));
  } else if (existing->train != observed->train ||
             !process_matches (existing->process, observed->process)) {
    g_debug ("watcher: %i ownership/process info changed, refreshing",
             GPOINTER_TO_INT (pid));
  } else {
    return FALSE;
  }

  if (G_LIKELY (existing->train)) {
    kgx_train_pop_child (existing->train, existing->process);
  }

  g_ptr_array_add (data->dead, pid);

  return FALSE;
}


static gboolean
add_observed_child (gpointer key,
                    gpointer val,
                    gpointer user_data)
{
  struct AddObservedChildren *data = user_data;
  ProcessWatch *observed = val;
  ProcessWatch *child = NULL;
  GPid pid = GPOINTER_TO_INT (key);

  if (g_tree_lookup (data->self->children, key))
    return FALSE;

  child = process_watch_alloc ();
  child->process = g_rc_box_acquire (observed->process);
  g_set_weak_pointer (&child->train, observed->train);

  g_debug ("watcher: Hello %i!", pid);

  g_tree_insert (data->self->children, key, child);

  if (G_LIKELY (observed->train)) {
    kgx_train_push_child (observed->train, observed->process);
  }

  data->added_children++;

  return FALSE;
}


static gboolean watch (gpointer data);

static void
schedule_timeout (KgxWatcher *self,
                  guint       interval_ms)
{
  if (g_tree_nnodes (self->watching) == 0)
    return;

  g_clear_handle_id (&self->timeout, g_source_remove);

  /* /proc scans are maintenance work, not frame-critical UI work.
   * Keep them out of the way of animation/tick handling when the main
   * loop is under pressure. */
  self->timeout = g_timeout_add_full (G_PRIORITY_LOW,
                                      interval_ms,
                                      watch,
                                      g_object_ref (self),
                                      g_object_unref);
  g_source_set_name_by_id (self->timeout, "[kgx] watcher");
}


static guint
get_target_interval (KgxWatcher *self)
{
  if (!self->in_background) {
    if (g_tree_nnodes (self->children) > 0)
      return WATCHER_FG_ACTIVE_MS;

    return self->stable_polls >= WATCHER_STEADY_POLLS
             ? WATCHER_FG_STEADY_MS
             : WATCHER_FG_ACTIVE_MS;
  }

  if (self->stable_polls < WATCHER_STEADY_POLLS)
    return WATCHER_BG_ACTIVE_MS;

  if (g_tree_nnodes (self->children) > 0 || self->stable_polls < WATCHER_IDLE_POLLS)
    return WATCHER_BG_STEADY_MS;

  return WATCHER_BG_IDLE_MS;
}


static inline void
note_watch_activity (KgxWatcher *self,
                     gboolean    changed)
{
  if (changed) {
    self->stable_polls = 0;
  } else if (self->stable_polls < G_MAXUINT) {
    self->stable_polls++;
  }
}


static gboolean
watch (gpointer data)
{
  KgxWatcher *self = KGX_WATCHER (data);
  g_autoptr (GTree) observed_children = NULL;
  struct SessionScan scan;
  struct ReconcileChildren dead;
  struct AddObservedChildren add;
  gboolean changed = FALSE;

  self->timeout = 0;

  if (g_tree_nnodes (self->watching) == 0) {
    return G_SOURCE_REMOVE;
  }

  observed_children = g_tree_new_full (kgx_pid_cmp,
                                       NULL,
                                       NULL,
                                       process_watch_free);

  scan.observed_children = observed_children;
  scan.dead_shells = g_ptr_array_new_full (1, NULL);

  /* Query each shell's session for its child processes */
  g_tree_foreach (self->watching, scan_session, &scan);

  /* Remove shells whose trains have died */
  changed = scan.dead_shells->len > 0;
  for (guint i = 0; i < scan.dead_shells->len; i++)
    g_tree_remove (self->watching, g_ptr_array_index (scan.dead_shells, i));
  g_ptr_array_unref (scan.dead_shells);

  /* Reconcile tracked children against the shell-owned processes observed in
   * this scan. This corrects stale ownership as well as dead children. */
  dead.observed_children = observed_children;
  dead.dead = g_ptr_array_new_full (1, NULL);

  g_tree_foreach (self->children, reconcile_existing_child, &dead);

  /* Can't modify self->children whilst walking it */
  changed = changed || dead.dead->len > 0;
  for (guint i = 0; i < dead.dead->len; i++)
    g_tree_remove (self->children, g_ptr_array_index (dead.dead, i));

  g_ptr_array_unref (dead.dead);

  add.self = self;
  add.added_children = 0;
  g_tree_foreach (observed_children, add_observed_child, &add);
  changed = changed || add.added_children > 0;

  /* Stop polling if all shells are gone */
  if (g_tree_nnodes (self->watching) == 0)
    return G_SOURCE_REMOVE;

  note_watch_activity (self, changed);
  schedule_timeout (self, get_target_interval (self));

  return G_SOURCE_REMOVE;
}


static inline gboolean
update_watcher (KgxWatcher *self, gboolean in_background)
{
  if (self->in_background == in_background) {
    return FALSE;
  }

  self->in_background = in_background;

  g_debug ("watcher: in_background? %s", in_background ? "yes" : "no");

  /* Reschedule with the appropriate cadence for the new state. */
  if (g_tree_nnodes (self->watching) > 0)
    schedule_timeout (self, get_target_interval (self));

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
  if (G_UNLIKELY (watch->process == NULL)) {
    process_watch_free (watch);
    return;
  }

  g_set_weak_pointer (&watch->train, train);

  g_debug ("watcher: tracking %i", pid);

  g_tree_insert (self->watching, GINT_TO_POINTER (pid), watch);

  self->stable_polls = 0;
  schedule_timeout (self, get_target_interval (self));
}
