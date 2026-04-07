/* kgx-train.c
 *
 * Copyright 2021-2025 Zander Brown
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

#include "kgx-enums.h"
#include "kgx-marshals.h"
#include "kgx-playbox.h"
#include "kgx-remote.h"
#include "kgx-utils.h"

#include "kgx-train.h"


typedef struct _KgxTrainPrivate KgxTrainPrivate;
struct _KgxTrainPrivate {
  char *uuid;
  char *tag;
  char *fallback_title;
  GPid pid;
  KgxStatus status;
  char *last_child_name;

  GHashTable *root;
  GHashTable *remote;
  GHashTable *playbox;
  GHashTable *children;
};

static void kgx_train_async_initiable_iface_init (GAsyncInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (KgxTrain, kgx_train, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (KgxTrain)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                                                kgx_train_async_initiable_iface_init))


enum {
  PROP_0,
  PROP_UUID,
  PROP_TAG,
  PROP_FALLBACK_TITLE,
  PROP_PID,
  PROP_STATUS,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


enum {
  PID_DIED,
  CHILD_ADDED,
  CHILD_REMOVED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];


static void
kgx_train_dispose (GObject *object)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (KGX_TRAIN (object));

  g_clear_pointer (&priv->uuid, g_free);
  g_clear_pointer (&priv->tag, g_free);
  g_clear_pointer (&priv->fallback_title, g_free);
  g_clear_pointer (&priv->last_child_name, g_free);

  g_clear_pointer (&priv->root, g_hash_table_unref);
  g_clear_pointer (&priv->remote, g_hash_table_unref);
  g_clear_pointer (&priv->playbox, g_hash_table_unref);
  g_clear_pointer (&priv->children, g_hash_table_unref);

  G_OBJECT_CLASS (kgx_train_parent_class)->dispose (object);
}


static void
kgx_train_get_property (GObject    *object,
                        guint       property_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (KGX_TRAIN (object));

  switch (property_id) {
    case PROP_UUID:
      g_value_set_string (value, priv->uuid);
      break;
    case PROP_TAG:
      g_value_set_string (value, priv->tag);
      break;
    case PROP_FALLBACK_TITLE:
      g_value_set_string (value, priv->fallback_title);
      break;
    case PROP_PID:
      g_value_set_int (value, priv->pid);
      break;
    case PROP_STATUS:
      g_value_set_flags (value, priv->status);
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static gint
compare_process_pid (gconstpointer a,
                     gconstpointer b)
{
  GPid pa = kgx_process_get_pid (*(KgxProcess **) a);
  GPid pb = kgx_process_get_pid (*(KgxProcess **) b);

  return pa < pb ? -1 : pa > pb ? 1 : 0;
}


static char *
build_fallback_title (KgxTrain *self)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (self);
  const char *shell_name = priv->tag;

  if (g_hash_table_size (priv->children) > 0) {
    g_autoptr (GPtrArray) children = g_ptr_array_new ();
    GHashTableIter iter;
    gpointer pid, process;

    g_hash_table_iter_init (&iter, priv->children);
    while (g_hash_table_iter_next (&iter, &pid, &process)) {
      g_ptr_array_add (children, process);
    }

    if (children->len > 0) {
      g_autoptr (GString) chain = g_string_new (NULL);
      g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash,
                                                           g_str_equal,
                                                           g_free,
                                                           NULL);

      g_ptr_array_sort (children, compare_process_pid);

      if (shell_name && shell_name[0] != '\0') {
        g_string_append_printf (chain, ">%s", shell_name);
        g_hash_table_add (seen, g_strdup (shell_name));
      }

      for (guint i = 0; i < children->len; i++) {
        KgxProcess *child = g_ptr_array_index (children, i);
        const char *name = kgx_process_get_name (child);

        if (name && name[0] != '\0' &&
            !g_hash_table_contains (seen, name)) {
          g_hash_table_add (seen, g_strdup (name));
          g_string_append_printf (chain, ">%s", name);
        }
      }

      if (chain->len > 0) {
        return g_string_free (g_steal_pointer (&chain), FALSE);
      }
    }
  }

  if (priv->last_child_name && priv->last_child_name[0] != '\0') {
    if (shell_name && shell_name[0] != '\0') {
      return g_strdup_printf (">%s>%s", shell_name, priv->last_child_name);
    }

    return g_strdup_printf (">%s", priv->last_child_name);
  }

  if (shell_name && shell_name[0] != '\0') {
    return g_strdup_printf (">%s", shell_name);
  }

  return g_strdup ("Terminal");
}


static void
update_fallback_title (KgxTrain *self)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (self);
  g_autofree char *title = build_fallback_title (self);

  if (g_strcmp0 (priv->fallback_title, title) == 0) {
    return;
  }

  g_free (priv->fallback_title);
  priv->fallback_title = g_steal_pointer (&title);

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_FALLBACK_TITLE]);
}


static void
kgx_train_set_property (GObject      *object,
                        guint         property_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (KGX_TRAIN (object));

  switch (property_id) {
    case PROP_TAG:
      if (g_set_str (&priv->tag, g_value_get_string (value))) {
        update_fallback_title (KGX_TRAIN (object));
        g_object_notify_by_pspec (object, pspec);
      }
      break;
    case PROP_PID:
      priv->pid = g_value_get_int (value);
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static void
kgx_train_class_init (KgxTrainClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kgx_train_dispose;
  object_class->get_property = kgx_train_get_property;
  object_class->set_property = kgx_train_set_property;

  pspecs[PROP_UUID] =
    g_param_spec_string ("uuid", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_TAG] =
    g_param_spec_string ("tag", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_FALLBACK_TITLE] =
    g_param_spec_string ("fallback-title", NULL, NULL,
                         "Terminal",
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /* We've assumed that GPid fits in GParamSpecInt */
  { G_STATIC_ASSERT (sizeof (GPid) == sizeof (int)); }

  pspecs[PROP_PID] =
    g_param_spec_int ("pid", NULL, NULL,
                      G_MININT, G_MAXINT, 0,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_STATUS] =
    g_param_spec_flags ("status", NULL, NULL,
                        KGX_TYPE_STATUS,
                        KGX_NONE,
                        G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  signals[PID_DIED] = g_signal_new ("pid-died",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL,
                                    kgx_marshals_VOID__INT,
                                    G_TYPE_NONE,
                                    1, G_TYPE_INT);
  g_signal_set_va_marshaller (signals[PID_DIED],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_VOID__INTv);

  signals[CHILD_ADDED] = g_signal_new ("child-added",
                                       G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST,
                                       0,
                                       NULL, NULL,
                                       kgx_marshals_VOID__BOXED,
                                       G_TYPE_NONE,
                                       1,
                                       KGX_TYPE_PROCESS | G_SIGNAL_TYPE_STATIC_SCOPE);
  g_signal_set_va_marshaller (signals[CHILD_ADDED],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_VOID__BOXEDv);

  signals[CHILD_REMOVED] = g_signal_new ("child-removed",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL, NULL,
                                         kgx_marshals_VOID__BOXED,
                                         G_TYPE_NONE,
                                         1,
                                         KGX_TYPE_PROCESS | G_SIGNAL_TYPE_STATIC_SCOPE);
  g_signal_set_va_marshaller (signals[CHILD_REMOVED],
                              G_TYPE_FROM_CLASS (klass),
                              kgx_marshals_VOID__BOXEDv);
}


struct _WaitData {
  KgxTrain *self;
};


KGX_DEFINE_DATA (WaitData, wait_data)


static void
wait_data_cleanup (WaitData *self)
{
  g_clear_weak_pointer (&self->self);
}


static void
process_died (GPid     pid,
              int      status,
              gpointer user_data)

{
  WaitData *data = user_data;

  if (!data->self) {
    return; /* Train destroyed before the process actually died */
  }

  g_spawn_close_pid (pid);

  g_signal_emit (data->self, signals[PID_DIED], 0, status);
}


static void
kgx_train_async_initiable_init_async (GAsyncInitable      *initable,
                                      int                  io_priority,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  g_autoptr (WaitData) wait_data = wait_data_alloc ();
  GTask *task = g_task_new (initable, cancellable, callback, user_data);
  KgxTrain *self = KGX_TRAIN (initable);
  KgxTrainPrivate *priv = kgx_train_get_instance_private (self);

  g_task_set_source_tag (task, kgx_train_async_initiable_init_async);

  g_set_weak_pointer (&wait_data->self, self);

  g_child_watch_add_full (G_PRIORITY_HIGH_IDLE,
                          priv->pid,
                          process_died,
                          g_steal_pointer (&wait_data),
                          wait_data_free);

  g_task_return_boolean (task, TRUE);
}


static gboolean
kgx_train_async_initiable_init_finish (GAsyncInitable  *initable,
                                       GAsyncResult    *res,
                                       GError         **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}


static void
kgx_train_async_initiable_iface_init (GAsyncInitableIface *iface)
{
  iface->init_async = kgx_train_async_initiable_init_async;
  iface->init_finish = kgx_train_async_initiable_init_finish;
}


static void
kgx_train_init (KgxTrain *self)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (self);

  priv->uuid = g_uuid_string_random ();
  priv->fallback_title = g_strdup ("Terminal");

  priv->root = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->remote = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->playbox = g_hash_table_new (g_direct_hash, g_direct_equal);
  priv->children = g_hash_table_new_full (g_direct_hash,
                                          g_direct_equal,
                                          NULL,
                                          (GDestroyNotify) kgx_process_unref);
}


const char *
kgx_train_get_uuid (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), NULL);

  priv = kgx_train_get_instance_private (self);

  return priv->uuid;
}


const char *
kgx_train_get_tag (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), NULL);

  priv = kgx_train_get_instance_private (self);

  return priv->tag;
}


const char *
kgx_train_get_fallback_title (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), "Terminal");

  priv = kgx_train_get_instance_private (self);

  return priv->fallback_title;
}


GPid
kgx_train_get_pid (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), -1);

  priv = kgx_train_get_instance_private (self);

  return priv->pid;
}


const char *
kgx_train_get_last_child_name (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), NULL);

  priv = kgx_train_get_instance_private (self);

  return priv->last_child_name;
}


guint
kgx_train_get_child_count (KgxTrain *self)
{
  KgxTrainPrivate *priv;

  g_return_val_if_fail (KGX_IS_TRAIN (self), 0);

  priv = kgx_train_get_instance_private (self);

  return g_hash_table_size (priv->children);
}


/**
 * kgx_train_get_children:
 * @self: the #KgxTrain
 *
 * Get a list of child process running in @self
 *
 * NOTE: This doesn't include the shell/root itself
 *
 * Returns: (element-type Kgx.Process) (transfer full): the list of #KgxProcess
 */
GPtrArray *
kgx_train_get_children (KgxTrain *self)
{
  KgxTrainPrivate *priv;
  GPtrArray *children;
  GHashTableIter iter;
  gpointer pid, process;

  g_return_val_if_fail (KGX_IS_TRAIN (self), NULL);

  priv = kgx_train_get_instance_private (self);

  children = g_ptr_array_new_full (3, (GDestroyNotify) kgx_process_unref);

  g_hash_table_iter_init (&iter, priv->children);
  while (g_hash_table_iter_next (&iter, &pid, &process)) {
    g_ptr_array_add (children, g_rc_box_acquire (process));
  }

  return children;
}


static inline void
set_status (KgxTrain  *self,
            KgxStatus  status)
{
  KgxTrainPrivate *priv = kgx_train_get_instance_private (self);

  if (priv->status == status) {
    return;
  }

  priv->status = status;

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_STATUS]);
}


static inline gpointer
pid_as_key (pid_t pid)
{
  return GINT_TO_POINTER (pid);
}


static inline KgxStatus
push_type (GHashTable *table,
           GPid        pid,
           KgxProcess *process,
           KgxStatus   status)
{
  g_hash_table_insert (table,
                       pid_as_key (pid),
                       process != NULL ? g_rc_box_acquire (process) : NULL);

  g_debug ("train: Now %i %X", g_hash_table_size (table), status);

  return status;
}


void
kgx_train_push_child (KgxTrain   *self,
                      KgxProcess *process)
{
  GPid pid = 0;
  GStrv argv;
  KgxStatus new_status = KGX_NONE;
  KgxTrainPrivate *priv;

  g_return_if_fail (KGX_IS_TRAIN (self));

  priv = kgx_train_get_instance_private (self);

  pid = kgx_process_get_pid (process);
  argv = kgx_process_get_argv (process);

  if (G_LIKELY (argv[0] != NULL)) {
    const char *program = kgx_process_get_name (process);

    /* Remember the last child process name for tab title fallback */
    g_free (priv->last_child_name);
    priv->last_child_name = g_strdup (program);

    if (G_UNLIKELY (kgx_is_remote (program, argv))) {
      push_type (priv->remote, pid, NULL, KGX_REMOTE);
    }

    if (G_UNLIKELY (kgx_is_playbox (program, argv))) {
      push_type (priv->playbox, pid, NULL, KGX_PLAYBOX);
    }
  }

  if (G_UNLIKELY (kgx_process_get_is_root (process))) {
    push_type (priv->root, pid, NULL, KGX_PRIVILEGED);
  }

  /* Compute status from the ground truth (hash table occupancy),
   * not just this child's classification. */
  if (g_hash_table_size (priv->remote) > 0)
    new_status |= KGX_REMOTE;
  if (g_hash_table_size (priv->playbox) > 0)
    new_status |= KGX_PLAYBOX;
  if (g_hash_table_size (priv->root) > 0)
    new_status |= KGX_PRIVILEGED;

  set_status (self, new_status);

  if (g_hash_table_contains (priv->children, pid_as_key (pid))) {
    return;
  }

  push_type (priv->children, pid, process, KGX_NONE);
  update_fallback_title (self);

  g_signal_emit (self, signals[CHILD_ADDED], 0, process);
}


inline static KgxStatus
pop_type (GHashTable *table,
          GPid        pid,
          KgxStatus   status)
{
  guint size = 0;

  g_hash_table_remove (table, GINT_TO_POINTER (pid));

  size = g_hash_table_size (table);

  if (G_LIKELY (size <= 0)) {
    g_debug ("train: No longer %X", status);

    return KGX_NONE;
  } else {
    g_debug ("train: %i %X remaining", size, status);

    return status;
  }
}


void
kgx_train_pop_child (KgxTrain   *self,
                     KgxProcess *process)
{
  GPid pid = 0;
  KgxStatus new_status = KGX_NONE;
  KgxTrainPrivate *priv;

  g_return_if_fail (KGX_IS_TRAIN (self));

  priv = kgx_train_get_instance_private (self);

  pid = kgx_process_get_pid (process);

  if (!g_hash_table_contains (priv->children, pid_as_key (pid))) {
    return;
  }

  new_status |= pop_type (priv->remote, pid, KGX_REMOTE);
  new_status |= pop_type (priv->playbox, pid, KGX_PLAYBOX);
  new_status |= pop_type (priv->root, pid, KGX_PRIVILEGED);
  pop_type (priv->children, pid, KGX_NONE);

  set_status (self, new_status);
  update_fallback_title (self);

  g_signal_emit (self, signals[CHILD_REMOVED], 0, process);
}
