/* kgx-tab-strip.c
 *
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

#include <glib/gi18n.h>

#include "kgx-shared-closures.h"
#include "kgx-tab.h"
#include "kgx-utils.h"

#include "kgx-tab-strip.h"


typedef struct {
  KgxTabStrip  *strip;
  AdwTabPage   *page; /* owned by strip hash-table key */
  GtkWidget    *row;
  GtkWidget    *select_button;
  GtkWidget    *spinner;
  GtkWidget    *icon;
  GtkWidget    *label;
  GtkWidget    *indicator;
  GtkWidget    *close_button;
  GSignalGroup *page_signals;
  GSignalGroup *tab_signals;
  GSignalGroup *train_signals;
} KgxTabStripItem;


struct _KgxTabStrip {
  GtkBox        parent_instance;

  AdwTabView   *view;
  GSignalGroup *view_signals;
  GHashTable   *items;
  GtkSettings  *gtk_settings;
  gulong        decoration_layout_changed_id;
  gboolean      close_buttons_start;

  GtkWidget    *scroller;
  GtkWidget    *box;
};


G_DEFINE_FINAL_TYPE (KgxTabStrip, kgx_tab_strip, GTK_TYPE_BOX)


enum {
  PROP_0,
  PROP_VIEW,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void kgx_tab_strip_rebuild (KgxTabStrip *self);


static void
kgx_tab_strip_item_sync_order (KgxTabStripItem *item)
{
  GtkBox *row = GTK_BOX (item->row);

  if (item->strip->close_buttons_start) {
    gtk_box_reorder_child_after (row, item->close_button, NULL);
    gtk_box_reorder_child_after (row, item->select_button, item->close_button);
    gtk_widget_add_css_class (item->row, "close-start");
    gtk_widget_remove_css_class (item->row, "close-end");
  } else {
    gtk_box_reorder_child_after (row, item->select_button, NULL);
    gtk_box_reorder_child_after (row, item->close_button, item->select_button);
    gtk_widget_add_css_class (item->row, "close-end");
    gtk_widget_remove_css_class (item->row, "close-start");
  }
}


static void
kgx_tab_strip_item_free (gpointer data)
{
  KgxTabStripItem *item = data;

  if (!item)
    return;

  if (item->page_signals)
    g_signal_group_set_target (item->page_signals, NULL);
  if (item->tab_signals)
    g_signal_group_set_target (item->tab_signals, NULL);
  if (item->train_signals)
    g_signal_group_set_target (item->train_signals, NULL);

  g_clear_object (&item->page_signals);
  g_clear_object (&item->tab_signals);
  g_clear_object (&item->train_signals);
  g_free (item);
}


static void
kgx_tab_strip_clear_children (KgxTabStrip *self)
{
  GtkWidget *child = gtk_widget_get_first_child (self->box);

  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling (child);

    gtk_box_remove (GTK_BOX (self->box), child);
    child = next;
  }
}


static void
kgx_tab_strip_sync_selected (KgxTabStrip *self)
{
  GHashTableIter iter;
  gpointer key, value;
  AdwTabPage *selected;

  if (!self->view)
    return;

  selected = adw_tab_view_get_selected_page (self->view);

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    KgxTabStripItem *item = value;
    gboolean is_selected = item->page == selected;

    if (is_selected) {
      gtk_widget_add_css_class (item->row, "selected");
      gtk_widget_add_css_class (item->select_button, "selected");
    } else {
      gtk_widget_remove_css_class (item->row, "selected");
      gtk_widget_remove_css_class (item->select_button, "selected");
    }
  }
}


static KgxTab *
kgx_tab_strip_item_get_tab (KgxTabStripItem *item)
{
  GtkWidget *child = adw_tab_page_get_child (item->page);

  if (!KGX_IS_TAB (child))
    return NULL;

  return KGX_TAB (child);
}


static void
kgx_tab_strip_item_sync_train_target (KgxTabStripItem *item)
{
  KgxTab *tab = kgx_tab_strip_item_get_tab (item);
  g_autoptr (KgxTrain) train = NULL;

  if (tab)
    g_object_get (tab, "train", &train, NULL);

  g_signal_group_set_target (item->train_signals, train);
}


static char *
kgx_tab_strip_item_dup_title (KgxTabStripItem *item)
{
  KgxTab *tab = kgx_tab_strip_item_get_tab (item);

  if (tab) {
    g_autoptr (KgxTrain) train = NULL;
    g_autofree char *initial_title = NULL;
    g_autofree char *tab_title = NULL;
    const char *fallback_title = NULL;

    g_object_get (tab,
                  "train", &train,
                  "initial-title", &initial_title,
                  "tab-title", &tab_title,
                  NULL);

    if (train)
      fallback_title = kgx_train_get_fallback_title (train);

    if (kgx_str_non_empty (fallback_title))
      return g_strdup (fallback_title);

    if (kgx_str_non_empty (initial_title))
      return g_steal_pointer (&initial_title);

    if (kgx_str_non_empty (tab_title))
      return g_steal_pointer (&tab_title);
  }

  return g_strdup (adw_tab_page_get_title (item->page));
}


static void
kgx_tab_strip_item_update (KgxTabStripItem *item)
{
  KgxTabStrip *self = item->strip;
  const char *tooltip;
  GIcon *icon;
  GIcon *indicator;
  gboolean loading;
  gboolean needs_attention;
  gboolean show_close;
  g_autofree char *title = NULL;

  title = kgx_tab_strip_item_dup_title (item);
  tooltip = adw_tab_page_get_tooltip (item->page);
  icon = adw_tab_page_get_icon (item->page);
  indicator = adw_tab_page_get_indicator_icon (item->page);
  loading = adw_tab_page_get_loading (item->page);
  needs_attention = adw_tab_page_get_needs_attention (item->page);

  gtk_label_set_label (GTK_LABEL (item->label),
                       (title && title[0] != '\0') ? title : _("Terminal"));
  gtk_widget_set_tooltip_text (item->select_button, tooltip);

  if (icon)
    gtk_image_set_from_gicon (GTK_IMAGE (item->icon), icon);
  gtk_widget_set_visible (item->icon, !loading && icon != NULL);

  if (loading) {
    gtk_spinner_start (GTK_SPINNER (item->spinner));
  } else {
    gtk_spinner_stop (GTK_SPINNER (item->spinner));
  }
  gtk_widget_set_visible (item->spinner, loading);

  if (indicator)
    gtk_image_set_from_gicon (GTK_IMAGE (item->indicator), indicator);
  gtk_widget_set_visible (item->indicator, indicator != NULL);

  if (needs_attention)
    gtk_widget_add_css_class (item->row, "needs-attention");
  else
    gtk_widget_remove_css_class (item->row, "needs-attention");

  show_close = self->view &&
               adw_tab_view_get_n_pages (self->view) > 1 &&
               !adw_tab_page_get_pinned (item->page);
  gtk_widget_set_visible (item->close_button, show_close);
}


static void
kgx_tab_strip_item_changed (GObject    *object,
                            GParamSpec *pspec,
                            gpointer    data)
{
  KgxTabStripItem *item = data;

  kgx_tab_strip_item_update (item);
  kgx_tab_strip_sync_selected (item->strip);
}


static void
kgx_tab_strip_item_tab_changed (GObject    *object,
                                GParamSpec *pspec,
                                gpointer    data)
{
  KgxTabStripItem *item = data;

  kgx_tab_strip_item_sync_train_target (item);
  kgx_tab_strip_item_update (item);
  kgx_tab_strip_sync_selected (item->strip);
}


static void
kgx_tab_strip_select_clicked (GtkButton *button,
                              gpointer   data)
{
  KgxTabStripItem *item = data;
  KgxTab *tab;

  if (!item->strip->view)
    return;

  adw_tab_view_set_selected_page (item->strip->view, item->page);

  tab = KGX_TAB (adw_tab_page_get_child (item->page));
  gtk_widget_grab_focus (GTK_WIDGET (tab));
}


static void
kgx_tab_strip_close_clicked (GtkButton *button,
                             gpointer   data)
{
  KgxTabStripItem *item = data;

  if (!item->strip->view)
    return;

  adw_tab_view_close_page (item->strip->view, item->page);
}


static gboolean
kgx_tab_strip_drop (GtkDropTarget *target,
                    const GValue  *value,
                    double         x,
                    double         y,
                    gpointer       data)
{
  KgxTabStripItem *item = data;
  KgxTab *tab;

  tab = KGX_TAB (adw_tab_page_get_child (item->page));
  kgx_tab_extra_drop (tab, value);

  return TRUE;
}


static KgxTabStripItem *
kgx_tab_strip_item_new (KgxTabStrip *self,
                        AdwTabPage  *page)
{
  KgxTabStripItem *item = g_new0 (KgxTabStripItem, 1);
  GtkWidget *content;
  GtkWidget *drop_widget;
  GtkDropTarget *file_target;
  GtkDropTarget *string_target;

  item->strip = self;
  item->page = page;

  item->row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (item->row, "tab-strip-item");
  gtk_widget_set_halign (item->row, GTK_ALIGN_START);

  item->select_button = gtk_button_new ();
  gtk_widget_add_css_class (item->select_button, "flat");
  gtk_widget_add_css_class (item->select_button, "tab-strip-select");
  g_object_set (item->select_button, "focus-on-click", FALSE, NULL);
  gtk_widget_set_halign (item->select_button, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (item->row), item->select_button);

  content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (content, GTK_ALIGN_START);
  gtk_widget_set_valign (content, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (content, 12);
  gtk_widget_set_margin_end (content, 10);
  gtk_widget_set_margin_top (content, 6);
  gtk_widget_set_margin_bottom (content, 6);
  gtk_button_set_child (GTK_BUTTON (item->select_button), content);

  item->spinner = gtk_spinner_new ();
  gtk_widget_set_size_request (item->spinner, 12, 12);
  gtk_box_append (GTK_BOX (content), item->spinner);

  item->icon = gtk_image_new ();
  gtk_widget_set_size_request (item->icon, 16, 16);
  gtk_box_append (GTK_BOX (content), item->icon);

  item->label = gtk_label_new (NULL);
  gtk_widget_add_css_class (item->label, "tab-strip-label");
  gtk_label_set_ellipsize (GTK_LABEL (item->label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (item->label), 0.0f);
  gtk_label_set_max_width_chars (GTK_LABEL (item->label), 28);
  gtk_box_append (GTK_BOX (content), item->label);

  item->indicator = gtk_image_new ();
  gtk_widget_add_css_class (item->indicator, "tab-strip-indicator");
  gtk_widget_set_size_request (item->indicator, 16, 16);
  gtk_box_append (GTK_BOX (content), item->indicator);

  item->close_button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (item->close_button, "flat");
  gtk_widget_add_css_class (item->close_button, "tab-strip-close");
  g_object_set (item->close_button, "focus-on-click", FALSE, NULL);
  gtk_widget_set_tooltip_text (item->close_button, _("Close Tab"));
  gtk_box_append (GTK_BOX (item->row), item->close_button);
  kgx_tab_strip_item_sync_order (item);

  g_signal_connect (item->select_button,
                    "clicked",
                    G_CALLBACK (kgx_tab_strip_select_clicked),
                    item);
  g_signal_connect (item->close_button,
                    "clicked",
                    G_CALLBACK (kgx_tab_strip_close_clicked),
                    item);

  item->page_signals = g_signal_group_new (ADW_TYPE_TAB_PAGE);
  g_signal_group_connect (item->page_signals,
                          "notify::title",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::tooltip",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::icon",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::indicator-icon",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::loading",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::needs-attention",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_connect (item->page_signals,
                          "notify::pinned",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  g_signal_group_set_target (item->page_signals, item->page);

  item->tab_signals = g_signal_group_new (KGX_TYPE_TAB);
  g_signal_group_connect (item->tab_signals,
                          "notify::train",
                          G_CALLBACK (kgx_tab_strip_item_tab_changed),
                          item);
  g_signal_group_set_target (item->tab_signals, kgx_tab_strip_item_get_tab (item));

  item->train_signals = g_signal_group_new (KGX_TYPE_TRAIN);
  g_signal_group_connect (item->train_signals,
                          "notify::fallback-title",
                          G_CALLBACK (kgx_tab_strip_item_changed),
                          item);
  kgx_tab_strip_item_sync_train_target (item);

  drop_widget = item->row;
  file_target = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (file_target,
                    "drop",
                    G_CALLBACK (kgx_tab_strip_drop),
                    item);
  gtk_widget_add_controller (drop_widget, GTK_EVENT_CONTROLLER (file_target));

  string_target = gtk_drop_target_new (G_TYPE_STRING, GDK_ACTION_COPY);
  g_signal_connect (string_target,
                    "drop",
                    G_CALLBACK (kgx_tab_strip_drop),
                    item);
  gtk_widget_add_controller (drop_widget, GTK_EVENT_CONTROLLER (string_target));

  kgx_tab_strip_item_update (item);

  return item;
}


static void
kgx_tab_strip_selected_page_changed (GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    data)
{
  kgx_tab_strip_sync_selected (KGX_TAB_STRIP (data));
}


static void
kgx_tab_strip_pages_changed (AdwTabView   *view,
                             AdwTabPage   *page,
                             int           position,
                             KgxTabStrip  *self)
{
  kgx_tab_strip_rebuild (self);
}


static void
kgx_tab_strip_rebuild (KgxTabStrip *self)
{
  int n_pages;

  kgx_tab_strip_clear_children (self);
  g_hash_table_remove_all (self->items);

  if (!self->view)
    return;

  n_pages = adw_tab_view_get_n_pages (self->view);
  for (int i = 0; i < n_pages; i++) {
    AdwTabPage *page = adw_tab_view_get_nth_page (self->view, i);
    KgxTabStripItem *item = kgx_tab_strip_item_new (self, page);

    g_hash_table_insert (self->items, g_object_ref (page), item);
    gtk_box_append (GTK_BOX (self->box), item->row);
  }

  kgx_tab_strip_sync_selected (self);
}


static void
kgx_tab_strip_set_close_buttons_start (KgxTabStrip *self,
                                       gboolean     close_buttons_start)
{
  GHashTableIter iter;
  gpointer key, value;

  if (self->close_buttons_start == close_buttons_start)
    return;

  self->close_buttons_start = close_buttons_start;

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    kgx_tab_strip_item_sync_order (value);
  }
}


static void
kgx_tab_strip_sync_decoration_layout (KgxTabStrip *self)
{
  g_autofree char *layout = NULL;
  gboolean close_buttons_start = FALSE;

  if (self->gtk_settings) {
    g_object_get (self->gtk_settings,
                  "gtk-decoration-layout", &layout,
                  NULL);
    close_buttons_start = kgx_decoration_layout_is_inverted (NULL, layout);
  }

  kgx_tab_strip_set_close_buttons_start (self, close_buttons_start);
}


static void
kgx_tab_strip_decoration_layout_changed (GObject    *object,
                                         GParamSpec *pspec,
                                         gpointer    data)
{
  kgx_tab_strip_sync_decoration_layout (KGX_TAB_STRIP (data));
}


static void
kgx_tab_strip_root (GtkWidget *widget)
{
  KgxTabStrip *self = KGX_TAB_STRIP (widget);
  GdkDisplay *display = NULL;

  GTK_WIDGET_CLASS (kgx_tab_strip_parent_class)->root (widget);

  display = gtk_widget_get_display (widget);
  if (display) {
    GtkSettings *settings = gtk_settings_get_for_display (display);

    if (self->decoration_layout_changed_id != 0 && self->gtk_settings) {
      g_signal_handler_disconnect (self->gtk_settings,
                                   self->decoration_layout_changed_id);
      self->decoration_layout_changed_id = 0;
    }

    g_set_object (&self->gtk_settings, settings);

    if (self->gtk_settings) {
      self->decoration_layout_changed_id =
        g_signal_connect (self->gtk_settings,
                          "notify::gtk-decoration-layout",
                          G_CALLBACK (kgx_tab_strip_decoration_layout_changed),
                          self);
    }
  }

  kgx_tab_strip_sync_decoration_layout (self);
}


static void
kgx_tab_strip_unroot (GtkWidget *widget)
{
  KgxTabStrip *self = KGX_TAB_STRIP (widget);

  if (self->decoration_layout_changed_id != 0 && self->gtk_settings) {
    g_signal_handler_disconnect (self->gtk_settings,
                                 self->decoration_layout_changed_id);
    self->decoration_layout_changed_id = 0;
  }

  g_clear_object (&self->gtk_settings);

  GTK_WIDGET_CLASS (kgx_tab_strip_parent_class)->unroot (widget);
}


static void
kgx_tab_strip_set_view (KgxTabStrip *self,
                        AdwTabView  *view)
{
  if (!g_set_object (&self->view, view))
    return;

  g_signal_group_set_target (self->view_signals, self->view);
  kgx_tab_strip_rebuild (self);

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_VIEW]);
}


static void
kgx_tab_strip_dispose (GObject *object)
{
  KgxTabStrip *self = KGX_TAB_STRIP (object);

  if (self->view_signals)
    g_signal_group_set_target (self->view_signals, NULL);

  kgx_tab_strip_clear_children (self);
  g_clear_pointer (&self->items, g_hash_table_unref);
  g_clear_object (&self->view_signals);
  g_clear_object (&self->view);

  if (self->decoration_layout_changed_id != 0 && self->gtk_settings) {
    g_signal_handler_disconnect (self->gtk_settings,
                                 self->decoration_layout_changed_id);
    self->decoration_layout_changed_id = 0;
  }
  g_clear_object (&self->gtk_settings);

  G_OBJECT_CLASS (kgx_tab_strip_parent_class)->dispose (object);
}


static void
kgx_tab_strip_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  KgxTabStrip *self = KGX_TAB_STRIP (object);

  switch (property_id) {
    case PROP_VIEW:
      g_value_set_object (value, self->view);
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static void
kgx_tab_strip_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  KgxTabStrip *self = KGX_TAB_STRIP (object);

  switch (property_id) {
    case PROP_VIEW:
      kgx_tab_strip_set_view (self, g_value_get_object (value));
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static void
kgx_tab_strip_class_init (KgxTabStripClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = kgx_tab_strip_dispose;
  object_class->get_property = kgx_tab_strip_get_property;
  object_class->set_property = kgx_tab_strip_set_property;
  widget_class->root = kgx_tab_strip_root;
  widget_class->unroot = kgx_tab_strip_unroot;

  pspecs[PROP_VIEW] =
    g_param_spec_object ("view", NULL, NULL,
                         ADW_TYPE_TAB_VIEW,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  gtk_widget_class_set_css_name (widget_class, "tabbar");
}


static void
kgx_tab_strip_init (KgxTabStrip *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class (GTK_WIDGET (self), "kgx-tab-strip");

  self->items = g_hash_table_new_full (g_direct_hash,
                                       g_direct_equal,
                                       g_object_unref,
                                       kgx_tab_strip_item_free);

  self->view_signals = g_signal_group_new (ADW_TYPE_TAB_VIEW);
  g_signal_group_connect (self->view_signals,
                          "notify::selected-page",
                          G_CALLBACK (kgx_tab_strip_selected_page_changed),
                          self);
  g_signal_group_connect (self->view_signals,
                          "page-attached",
                          G_CALLBACK (kgx_tab_strip_pages_changed),
                          self);
  g_signal_group_connect (self->view_signals,
                          "page-detached",
                          G_CALLBACK (kgx_tab_strip_pages_changed),
                          self);
  g_signal_group_connect (self->view_signals,
                          "page-reordered",
                          G_CALLBACK (kgx_tab_strip_pages_changed),
                          self);

  self->scroller = gtk_scrolled_window_new ();
  gtk_widget_set_hexpand (self->scroller, TRUE);
  gtk_widget_set_vexpand (self->scroller, FALSE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroller),
                                  GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_NEVER);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (self->scroller), FALSE);
  gtk_box_append (GTK_BOX (self), self->scroller);

  self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->box, "tab-strip-box");
  gtk_widget_set_halign (self->box, GTK_ALIGN_START);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroller), self->box);
}
