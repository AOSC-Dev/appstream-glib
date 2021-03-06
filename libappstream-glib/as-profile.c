/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2015 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#include <glib/gi18n.h>

#include "as-profile.h"

struct _AsProfile
{
	GObject		 parent_instance;
	GPtrArray	*current;
	GPtrArray	*archived;
	GMutex		 mutex;
	GThread		*unthreaded;
	guint		 autodump_id;
};

typedef struct {
	gchar		*id;
	gint64		 time_start;
	gint64		 time_stop;
} AsProfileItem;

G_DEFINE_TYPE (AsProfile, as_profile, G_TYPE_OBJECT)

struct _AsProfileTask
{
	AsProfile	*profile;
	gchar		*id;
};

static gpointer as_profile_object = NULL;

/**
 * as_profile_item_free:
 **/
static void
as_profile_item_free (AsProfileItem *item)
{
	g_free (item->id);
	g_free (item);
}

/**
 * as_profile_item_find:
 **/
static AsProfileItem *
as_profile_item_find (GPtrArray *array, const gchar *id)
{
	AsProfileItem *tmp;
	guint i;

	g_return_val_if_fail (id != NULL, NULL);

	for (i = 0; i < array->len; i++) {
		tmp = g_ptr_array_index (array, i);
		if (g_strcmp0 (tmp->id, id) == 0)
			return tmp;
	}
	return NULL;
}

/**
 * as_profile_start:
 * @profile: A #AsProfile
 * @fmt: Format string
 * @...: varargs
 *
 * Starts profiling a section of code.
 *
 * Since: 0.2.2
 *
 * Returns: A #AsProfileTask, free with as_profile_task_free()
 **/
AsProfileTask *
as_profile_start (AsProfile *profile, const gchar *fmt, ...)
{
	va_list args;
	g_autofree gchar *tmp = NULL;
	va_start (args, fmt);
	tmp = g_strdup_vprintf (fmt, args);
	va_end (args);
	return as_profile_start_literal (profile, tmp);
}

/**
 * as_profile_start_literal:
 * @profile: A #AsProfile
 * @id: ID string
 *
 * Starts profiling a section of code.
 *
 * Since: 0.2.2
 *
 * Returns: A #AsProfileTask, free with as_profile_task_free()
 **/
AsProfileTask *
as_profile_start_literal (AsProfile *profile, const gchar *id)
{
	GThread *self;
	AsProfileItem *item;
	AsProfileTask *ptask = NULL;
	g_autofree gchar *id_thr = NULL;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&profile->mutex);

	g_return_val_if_fail (AS_IS_PROFILE (profile), NULL);
	g_return_val_if_fail (id != NULL, NULL);

	/* only use the thread ID when not using the main thread */
	self = g_thread_self ();
	if (self != profile->unthreaded) {
		id_thr = g_strdup_printf ("%p~%s", self, id);
	} else {
		id_thr = g_strdup (id);
	}

	/* already started */
	item = as_profile_item_find (profile->current, id_thr);
	if (item != NULL) {
		as_profile_dump (profile);
		g_warning ("Already a started task for %s", id_thr);
		return NULL;
	}

	/* add new item */
	item = g_new0 (AsProfileItem, 1);
	item->id = g_strdup (id_thr);
	item->time_start = g_get_real_time ();
	g_ptr_array_add (profile->current, item);
	g_debug ("run %s", id_thr);

	/* create token */
	ptask = g_new0 (AsProfileTask, 1);
	ptask->profile = g_object_ref (profile);
	ptask->id = g_strdup (id);
	return ptask;
}

/**
 * as_profile_task_free_internal:
 **/
static void
as_profile_task_free_internal (AsProfile *profile, const gchar *id)
{
	GThread *self;
	AsProfileItem *item;
	gdouble elapsed_ms;
	g_autofree gchar *id_thr = NULL;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&profile->mutex);

	g_return_if_fail (AS_IS_PROFILE (profile));
	g_return_if_fail (id != NULL);

	/* only use the thread ID when not using the main thread */
	self = g_thread_self ();
	if (self != profile->unthreaded) {
		id_thr = g_strdup_printf ("%p~%s", self, id);
	} else {
		id_thr = g_strdup (id);
	}

	/* already started */
	item = as_profile_item_find (profile->current, id_thr);
	if (item == NULL) {
		g_warning ("Not already a started task for %s", id_thr);
		return;
	}

	/* debug */
	elapsed_ms = (item->time_stop - item->time_start) / 1000;
	if (elapsed_ms > 5)
		g_debug ("%s took %.0fms", id_thr, elapsed_ms);

	/* update */
	item->time_stop = g_get_real_time ();

	/* move to archive */
	g_ptr_array_remove (profile->current, item);
	g_ptr_array_add (profile->archived, item);
}

/**
 * as_profile_task_free:
 * @ptask: A #AsProfileTask
 *
 * Frees a profile token, and marks the portion of code complete.
 *
 * Since: 0.2.2
 **/
void
as_profile_task_free (AsProfileTask *ptask)
{
	if (ptask == NULL)
		return;
	g_assert (ptask->id != NULL);
	as_profile_task_free_internal (ptask->profile, ptask->id);
	g_free (ptask->id);
	g_object_unref (ptask->profile);
	g_free (ptask);
}

/**
 * as_profile_sort_cb:
 **/
static gint
as_profile_sort_cb (gconstpointer a, gconstpointer b)
{
	AsProfileItem *item_a = *((AsProfileItem **) a);
	AsProfileItem *item_b = *((AsProfileItem **) b);
	if (item_a->time_start < item_b->time_start)
		return -1;
	if (item_a->time_start > item_b->time_start)
		return 1;
	return 0;
}

/**
 * as_profile_clear:
 * @profile: A #AsProfile
 *
 * Clears the list of profiled events.
 *
 * Since: 0.2.2
 **/
void
as_profile_clear (AsProfile *profile)
{
	g_ptr_array_set_size (profile->archived, 0);
}

/**
 * as_profile_dump:
 * @profile: A #AsProfile
 *
 * Dumps the current profiling table to stdout.
 *
 * Since: 0.2.2
 **/
void
as_profile_dump (AsProfile *profile)
{
	AsProfileItem *item;
	gint64 time_start = G_MAXINT64;
	gint64 time_stop = 0;
	gint64 time_ms;
	guint console_width = 86;
	guint i;
	guint j;
	gdouble scale;
	guint bar_offset;
	guint bar_length;

	g_return_if_fail (AS_IS_PROFILE (profile));

	/* nothing to show */
	if (profile->archived->len == 0)
		return;

	/* get the start and end times */
	for (i = 0; i < profile->archived->len; i++) {
		item = g_ptr_array_index (profile->archived, i);
		if (item->time_start < time_start)
			time_start = item->time_start;
		if (item->time_stop > time_stop)
			time_stop = item->time_stop;
	}
	scale = (gdouble) console_width / (gdouble) ((time_stop - time_start) / 1000);

	/* sort the list */
	g_ptr_array_sort (profile->archived, as_profile_sort_cb);

	/* dump a list of what happened when */
	for (i = 0; i < profile->archived->len; i++) {
		item = g_ptr_array_index (profile->archived, i);
		time_ms = (item->time_stop - item->time_start) / 1000;
		if (time_ms < 5)
			continue;

		/* print a timechart of what we've done */
		bar_offset = scale * (item->time_start - time_start) / 1000;
		for (j = 0; j < bar_offset; j++)
			g_print (" ");
		bar_length = scale * time_ms;
		if (bar_length == 0)
			bar_length = 1;
		for (j = 0; j < bar_length; j++)
			g_print ("#");
		for (j = bar_offset + bar_length; j < console_width + 1; j++)
			g_print (" ");
		g_print ("@%04" G_GINT64_FORMAT "ms ",
			 (item->time_stop - time_start) / 1000);
		g_print ("%s %" G_GINT64_FORMAT "ms\n", item->id, time_ms);
	}

	/* not all complete */
	if (profile->current->len > 0) {
		for (i = 0; i < profile->current->len; i++) {
			item = g_ptr_array_index (profile->current, i);
			item->time_stop = g_get_real_time ();
			for (j = 0; j < console_width; j++)
				g_print ("$");
			time_ms = (item->time_stop - item->time_start) / 1000;
			g_print (" @????ms %s %" G_GINT64_FORMAT "ms\n",
				 item->id, time_ms);
		}
	}
}

/**
 * as_profile_autodump_cb:
 **/
static gboolean
as_profile_autodump_cb (gpointer user_data)
{
	AsProfile *profile = AS_PROFILE (user_data);
	as_profile_dump (profile);
	profile->autodump_id = 0;
	return G_SOURCE_REMOVE;
}

/**
 * as_profile_set_autodump:
 * @profile: A #AsProfile
 *
 * Dumps the current profiling table to stdout on a set interval.
 *
 * Since: 0.2.2
 **/
void
as_profile_set_autodump (AsProfile *profile, guint delay)
{
	if (profile->autodump_id != 0)
		g_source_remove (profile->autodump_id);
	profile->autodump_id = g_timeout_add (delay, as_profile_autodump_cb, profile);
}

/**
 * as_profile_finalize:
 **/
static void
as_profile_finalize (GObject *object)
{
	AsProfile *profile = AS_PROFILE (object);

	if (profile->autodump_id != 0)
		g_source_remove (profile->autodump_id);
	g_ptr_array_foreach (profile->current, (GFunc) as_profile_item_free, NULL);
	g_ptr_array_unref (profile->current);
	g_ptr_array_unref (profile->archived);

	G_OBJECT_CLASS (as_profile_parent_class)->finalize (object);
}

/**
 * as_profile_class_init:
 **/
static void
as_profile_class_init (AsProfileClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = as_profile_finalize;
}

/**
 * as_profile_init:
 **/
static void
as_profile_init (AsProfile *profile)
{
	profile->current = g_ptr_array_new ();
	profile->unthreaded = g_thread_self ();
	profile->archived = g_ptr_array_new_with_free_func ((GDestroyNotify) as_profile_item_free);
	g_mutex_init (&profile->mutex);
}

/**
 * as_profile_new:
 *
 * Creates a new #AsProfile.
 *
 * Returns: (transfer full): a #AsProfile
 *
 * Since: 0.2.2
 **/
AsProfile *
as_profile_new (void)
{
	if (as_profile_object != NULL) {
		g_object_ref (as_profile_object);
	} else {
		as_profile_object = g_object_new (AS_TYPE_PROFILE, NULL);
		g_object_add_weak_pointer (as_profile_object, &as_profile_object);
	}
	return AS_PROFILE (as_profile_object);
}
