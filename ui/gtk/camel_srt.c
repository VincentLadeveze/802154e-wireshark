/* camel_srt.c
 * camel Service Response Time statistics for Wireshark
 * Copyright 2006 Florent Drouin (based on h225_ras_srt.c from Lars Roland)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <string.h>
#include <gtk/gtk.h>

#include <epan/packet_info.h>
#include <epan/epan.h>
#include <epan/value_string.h>
#include <epan/tap.h>
#include <epan/packet.h>
#include <epan/asn1.h>
#include <epan/dissectors/packet-camel.h>

#include "../file.h"
#include "../stat_menu.h"

#include "ui/simple_dialog.h"

#include "ui/gtk/main.h"
#include "ui/gtk/dlg_utils.h"
#include "ui/gtk/gui_utils.h"
#include "ui/gtk/gui_stat_util.h"
#include "ui/gtk/tap_param_dlg.h"
#include "ui/gtk/service_response_time_table.h"

#include "ui/gtk/old-gtk-compat.h"

/* used to keep track of the statistics for an entire program interface */
struct camelsrt_t {
  GtkWidget *win;
  srt_stat_table camel_srt_table;
};

static void camelsrt_set_title(struct camelsrt_t * p_camelsrt);
static void camelsrt_reset(void *phs);
static int camelsrt_packet(void *phs,
			   packet_info *pinfo _U_,
			   epan_dissect_t *edt _U_,
			   const void *phi);

static void camelsrt_draw(void *phs);
static void win_destroy_cb(GtkWindow *win _U_, gpointer data);
static void gtk_camelsrt_init(const char *opt_arg, void *userdata _U_);
void register_tap_listener_gtk_camelsrt(void);

/*
 *
 */
static void camelsrt_set_title(struct camelsrt_t * p_camelsrt)
{
  set_window_title(p_camelsrt->win, "CAMEL Service Response Time statistics");
}

static void camelsrt_reset(void *phs)
{
  struct camelsrt_t *hs=(struct camelsrt_t *)phs;
  reset_srt_table_data(&hs->camel_srt_table);
  camelsrt_set_title(hs);
}

/*
 * Count the delta time between Request and Response
 * As we can make several measurement per message, we use a boolean array for the category
 * Then, if the measurement is provided, check if it is valid, and update the table
 */
static int camelsrt_packet(void *phs,
			   packet_info *pinfo _U_,
			   epan_dissect_t *edt _U_,
			   const void *phi)
{
  struct camelsrt_t *hs=(struct camelsrt_t *)phs;
  const struct camelsrt_info_t * pi=(const struct camelsrt_info_t *)phi;
  int i;

  for (i=1; i<NB_CAMELSRT_CATEGORY; i++) {
    if ( pi->bool_msginfo[i] &&
	 pi->msginfo[i].is_delta_time
	 && pi->msginfo[i].request_available
	 && !pi->msginfo[i].is_duplicate ) {

      add_srt_table_data(&hs->camel_srt_table, i, &pi->msginfo[i].req_time, pinfo);

    }
  } /* category */
  return 1;
}


static void camelsrt_draw(void *phs)
{
  struct camelsrt_t *hs=(struct camelsrt_t *)phs;
  draw_srt_table_data(&hs->camel_srt_table);
}

/*
 * Routine for Display
 */
static void win_destroy_cb(GtkWindow *win _U_, gpointer data)
{
  struct camelsrt_t *hs=(struct camelsrt_t *)data;

  remove_tap_listener(hs);

  free_srt_table_data(&hs->camel_srt_table);
  g_free(hs);
}

static void gtk_camelsrt_init(const char *opt_arg, void *userdata _U_)
{
  struct camelsrt_t * p_camelsrt;
  const char *filter=NULL;

  GtkWidget *cmd_label;
  GtkWidget *main_label;
  GtkWidget *filter_label;
  char *filter_string;
  GString *error_string;
  GtkWidget *vbox;
  GtkWidget *bbox;
  GtkWidget *close_bt;
  int i;

  if(strncmp(opt_arg,"camel,srt,",10) == 0){
    filter=opt_arg+10;
  } else {
    filter=NULL;
  }

  p_camelsrt=(struct camelsrt_t *)g_malloc(sizeof(struct camelsrt_t));

  p_camelsrt->win= dlg_window_new("camel-srt");  /* transient_for top_level */
  gtk_window_set_destroy_with_parent (GTK_WINDOW(p_camelsrt->win), TRUE);

  gtk_window_set_default_size(GTK_WINDOW(p_camelsrt->win), 550, 400);
  camelsrt_set_title(p_camelsrt);

  vbox=ws_gtk_box_new(GTK_ORIENTATION_VERTICAL, 3, FALSE);
  gtk_container_add(GTK_CONTAINER(p_camelsrt->win), vbox);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

  main_label=gtk_label_new("CAMEL Service Response Time statistics");
  gtk_box_pack_start(GTK_BOX(vbox), main_label, FALSE, FALSE, 0);
  gtk_widget_show(main_label);

  filter_string = g_strdup_printf("Filter: %s",filter ? filter : "");
  filter_label=gtk_label_new(filter_string);
  g_free(filter_string);
  gtk_label_set_line_wrap(GTK_LABEL(filter_label), TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), filter_label, FALSE, FALSE, 0);
  gtk_widget_show(filter_label);

  cmd_label=gtk_label_new("CAMEL Commands");
  gtk_box_pack_start(GTK_BOX(vbox), cmd_label, FALSE, FALSE, 0);
  gtk_widget_show(cmd_label);

  /* We must display TOP LEVEL Widget before calling init_srt_table() */
  gtk_widget_show_all(p_camelsrt->win);

  init_srt_table(&p_camelsrt->camel_srt_table, NB_CAMELSRT_CATEGORY, vbox, NULL);
  for(i=0 ;i<NB_CAMELSRT_CATEGORY; i++) {
    init_srt_table_row(&p_camelsrt->camel_srt_table, i,
		       val_to_str(i,camelSRTtype_naming,"Unknown"));
  }

  error_string=register_tap_listener("CAMEL",
				     p_camelsrt,
				     filter,
				     0,
				     camelsrt_reset,
				     camelsrt_packet,
				     camelsrt_draw);

  if(error_string){
    simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK, "%s", error_string->str);
    g_string_free(error_string, TRUE);
    g_free(p_camelsrt);
    return;
  }

  /* Button row. */
  bbox = dlg_button_row_new(GTK_STOCK_CLOSE, NULL);
  gtk_box_pack_end(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);

  close_bt = (GtkWidget *)g_object_get_data(G_OBJECT(bbox), GTK_STOCK_CLOSE);
  window_set_cancel_button(p_camelsrt->win, close_bt, window_cancel_button_cb);

  g_signal_connect(p_camelsrt->win, "delete_event", G_CALLBACK(window_delete_event_cb), NULL);
  g_signal_connect(p_camelsrt->win, "destroy", G_CALLBACK(win_destroy_cb), p_camelsrt);

  gtk_widget_show_all(p_camelsrt->win);
  window_present(p_camelsrt->win);
  cf_retap_packets(&cfile);
  gdk_window_raise(gtk_widget_get_window(p_camelsrt->win));

}

static tap_param camel_srt_params[] = {
  { PARAM_FILTER, "Filter", NULL }
};

static tap_param_dlg camel_srt_dlg = {
  "CAMEL Service Response Time",
  "camel,srt",
  gtk_camelsrt_init,
  -1,
  G_N_ELEMENTS(camel_srt_params),
  camel_srt_params
};

void /* Next line mandatory */
register_tap_listener_gtk_camelsrt(void)
{
  register_param_stat(&camel_srt_dlg, "CAMEL",
			REGISTER_STAT_GROUP_RESPONSE_TIME);
}
